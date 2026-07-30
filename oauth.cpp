// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "oauth.h"

#include "api.h"
#include "json.h"

#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

    constexpr std::size_t maximum_http_header_size = std::size_t{32} * 1024;
    constexpr std::size_t maximum_token_response_size = std::size_t{1024} * 1024;

    // Keeps every early-return path from leaking a socket or file descriptor.
    // OAuth has many error exits, so centralizing close() here makes the rest of
    // the flow much easier to audit.
    class FileDescriptor {
    public:
        FileDescriptor() = default;
        explicit FileDescriptor(const int descriptor) : descriptor_(descriptor) {}
        FileDescriptor(const FileDescriptor &) = delete;
        FileDescriptor &operator=(const FileDescriptor &) = delete;

        FileDescriptor(FileDescriptor &&other) noexcept
            : descriptor_(std::exchange(other.descriptor_, -1)) {}

        FileDescriptor &operator=(FileDescriptor &&other) noexcept {
            if (this != &other) {
                reset();
                descriptor_ = std::exchange(other.descriptor_, -1);
            }
            return *this;
        }

        ~FileDescriptor() { reset(); }

        [[nodiscard]] int get() const { return descriptor_; }
        [[nodiscard]] explicit operator bool() const { return descriptor_ >= 0; }

        void reset(const int descriptor = -1) {
            if (descriptor_ >= 0) {
                ::close(descriptor_);
            }
            descriptor_ = descriptor;
        }

    private:
        int descriptor_ = -1;
    };

    std::string systemError(const std::string_view operation, const int error_number = errno) {
        return std::string(operation) + ": " + std::strerror(error_number);
    }

    bool containsNewline(const std::string_view value) {
        return value.find_first_of("\r\n") != std::string_view::npos;
    }

    // PKCE and JWTs use the URL-safe Base64 alphabet without '=' padding. These
    // helpers live here instead of pulling in another encoding dependency.
    std::string base64UrlEncode(const std::span<const unsigned char> bytes) {
        constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string encoded;
        encoded.reserve((bytes.size() * 4 + 2) / 3);

        std::size_t position = 0;
        while (bytes.size() - position >= 3) {
            const std::uint32_t block = (static_cast<std::uint32_t>(bytes[position]) << 16) |
                                        (static_cast<std::uint32_t>(bytes[position + 1]) << 8) |
                                        bytes[position + 2];
            encoded += alphabet[(block >> 18) & 0x3f];
            encoded += alphabet[(block >> 12) & 0x3f];
            encoded += alphabet[(block >> 6) & 0x3f];
            encoded += alphabet[block & 0x3f];
            position += 3;
        }

        const std::size_t remaining = bytes.size() - position;
        if (remaining == 1) {
            const std::uint32_t block = static_cast<std::uint32_t>(bytes[position]) << 16;
            encoded += alphabet[(block >> 18) & 0x3f];
            encoded += alphabet[(block >> 12) & 0x3f];
        } else if (remaining == 2) {
            const std::uint32_t block = (static_cast<std::uint32_t>(bytes[position]) << 16) |
                                        (static_cast<std::uint32_t>(bytes[position + 1]) << 8);
            encoded += alphabet[(block >> 18) & 0x3f];
            encoded += alphabet[(block >> 12) & 0x3f];
            encoded += alphabet[(block >> 6) & 0x3f];
        }
        return encoded;
    }

    int base64UrlDigit(const char character) {
        if (character >= 'A' && character <= 'Z') {
            return character - 'A';
        }
        if (character >= 'a' && character <= 'z') {
            return character - 'a' + 26;
        }
        if (character >= '0' && character <= '9') {
            return character - '0' + 52;
        }
        if (character == '-') {
            return 62;
        }
        if (character == '_') {
            return 63;
        }
        return -1;
    }

    std::expected<std::string, std::string> base64UrlDecode(std::string_view encoded) {
        while (!encoded.empty() && encoded.back() == '=') {
            encoded.remove_suffix(1);
        }
        if (encoded.size() % 4 == 1) {
            return std::unexpected("Invalid base64url length");
        }

        std::string decoded;
        decoded.reserve(encoded.size() * 3 / 4);
        std::uint32_t accumulator = 0;
        int available_bits = 0;
        int last_digit = 0;

        for (const char character : encoded) {
            const int digit = base64UrlDigit(character);
            if (digit < 0) {
                return std::unexpected("Invalid base64url character");
            }
            last_digit = digit;
            accumulator = (accumulator << 6) | static_cast<std::uint32_t>(digit);
            available_bits += 6;
            if (available_bits >= 8) {
                available_bits -= 8;
                decoded += static_cast<char>((accumulator >> available_bits) & 0xff);
            }
        }

        // A canonical base64url value has zeroes in bits which only served as
        // padding.
        if ((available_bits == 4 && (last_digit & 0x0f) != 0) ||
            (available_bits == 2 && (last_digit & 0x03) != 0)) {
            return std::unexpected("Invalid base64url padding bits");
        }
        return decoded;
    }

    // Generates the high-entropy PKCE verifier and anti-CSRF state value.
    std::string randomBase64Url(const std::size_t byte_count) {
        std::string bytes(byte_count, '\0');
        // arc4random_buf is the OS CSPRNG on macOS and needs no caller-managed
        // state.
        ::arc4random_buf(bytes.data(), bytes.size());
        return base64UrlEncode(
            std::span(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()));
    }

    // RFC 7636 defines an S256 challenge as BASE64URL(SHA256(verifier)).
    std::string sha256Base64Url(const std::string_view value) {
        std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
        CC_SHA256(value.data(), static_cast<CC_LONG>(value.size()), digest.data());
        return base64UrlEncode(digest);
    }

    // OAuth query strings and form bodies both need RFC 3986 percent encoding.
    // Spaces deliberately become %20; the authorization server accepts this in
    // application/x-www-form-urlencoded bodies as well as in URLs.
    std::string percentEncode(const std::string_view value) {
        constexpr char hex[] = "0123456789ABCDEF";
        std::string encoded;
        encoded.reserve(value.size());
        for (const unsigned char character : value) {
            if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '-' || character == '.' ||
                character == '_' || character == '~') {
                encoded += static_cast<char>(character);
            } else {
                encoded += '%';
                encoded += hex[character >> 4];
                encoded += hex[character & 0x0f];
            }
        }
        return encoded;
    }

    int hexDigit(const char character) {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    }

    std::expected<std::string, std::string> percentDecode(const std::string_view encoded) {
        std::string decoded;
        decoded.reserve(encoded.size());
        for (std::size_t position = 0; position < encoded.size(); ++position) {
            if (encoded[position] == '+') {
                decoded += ' ';
                continue;
            }
            if (encoded[position] != '%') {
                decoded += encoded[position];
                continue;
            }
            if (encoded.size() - position < 3) {
                return std::unexpected("Incomplete percent escape in OAuth callback");
            }
            const int high = hexDigit(encoded[position + 1]);
            const int low = hexDigit(encoded[position + 2]);
            if (high < 0 || low < 0) {
                return std::unexpected("Invalid percent escape in OAuth callback");
            }
            decoded += static_cast<char>((high << 4) | low);
            position += 2;
        }
        return decoded;
    }

    using QueryParameters = std::map<std::string, std::string>;

    // Decode the callback query once and reject duplicate keys. Rejecting
    // duplicates avoids ambiguous state/code values being interpreted
    // differently by this client and an intermediary.
    std::expected<QueryParameters, std::string> parseQuery(const std::string_view query) {
        QueryParameters parameters;
        std::size_t position = 0;
        while (position < query.size()) {
            const std::size_t separator = query.find('&', position);
            const std::string_view pair =
                query.substr(position, separator == std::string_view::npos ? query.size() - position
                                                                           : separator - position);
            const std::size_t equals = pair.find('=');
            auto key = percentDecode(pair.substr(0, equals));
            auto value = percentDecode(equals == std::string_view::npos ? std::string_view{}
                                                                        : pair.substr(equals + 1));
            if (!key || !value) {
                return std::unexpected(key ? value.error() : key.error());
            }
            if (!parameters.emplace(std::move(key.value()), std::move(value.value())).second) {
                return std::unexpected("Duplicate parameter in OAuth callback");
            }
            if (separator == std::string_view::npos) {
                break;
            }
            position = separator + 1;
        }
        return parameters;
    }

    // State is a secret nonce. Avoid returning early at the first differing byte.
    bool constantTimeEqual(const std::string_view left, const std::string_view right) {
        if (left.size() != right.size()) {
            return false;
        }
        unsigned char difference = 0;
        for (std::size_t position = 0; position < left.size(); ++position) {
            difference |= static_cast<unsigned char>(left[position] ^ right[position]);
        }
        return difference == 0;
    }

    struct SocketError {
        std::string message;
        int error_number;
    };

    struct BoundListener {
        FileDescriptor socket;
        std::uint16_t port;
    };

    // Bind only to loopback: authorization codes must never be exposed on a LAN
    // interface. Port zero is supported for tests; production uses the two
    // redirect ports registered for the Codex OAuth client.
    std::expected<BoundListener, SocketError> bindListener(const std::uint16_t port) {
        FileDescriptor listener(::socket(AF_INET, SOCK_STREAM, 0));
        if (!listener) {
            const int error = errno;
            return std::unexpected(
                SocketError{systemError("Could not create OAuth listener", error), error});
        }

        const int enabled = 1;
        ::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#ifdef SO_NOSIGPIPE
        ::setsockopt(listener.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::bind(listener.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) <
            0) {
            const int error = errno;
            return std::unexpected(
                SocketError{systemError("Could not bind OAuth listener", error), error});
        }
        if (::listen(listener.get(), 4) < 0) {
            const int error = errno;
            return std::unexpected(
                SocketError{systemError("Could not listen for OAuth callback", error), error});
        }

        socklen_t address_size = sizeof(address);
        if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&address), &address_size) <
            0) {
            const int error = errno;
            return std::unexpected(
                SocketError{systemError("Could not determine OAuth callback port", error), error});
        }
        return BoundListener{std::move(listener), ntohs(address.sin_port)};
    }

    // select() lets the entire login attempt share one absolute deadline. EINTR
    // is retried without extending that deadline.
    std::expected<FileDescriptor, std::string> acceptBefore(const int listener, const std::chrono::steady_clock::time_point deadline) {
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return std::unexpected("Timed out waiting for the OAuth browser callback");
            }

            const auto remaining =
                std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
            timeval timeout{static_cast<time_t>(remaining.count() / 1'000'000),
                            static_cast<suseconds_t>(remaining.count() % 1'000'000)};
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener, &readable);
            const int selected = ::select(listener + 1, &readable, nullptr, nullptr, &timeout);
            if (selected == 0) {
                return std::unexpected("Timed out waiting for the OAuth browser callback");
            }
            if (selected < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(systemError("Could not wait for OAuth callback"));
            }

            FileDescriptor connection(::accept(listener, nullptr, nullptr));
            if (!connection) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(systemError("Could not accept OAuth callback"));
            }
#ifdef SO_NOSIGPIPE
            const int enabled = 1;
            ::setsockopt(connection.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
            return connection;
        }
    }

    // Read only through the end of the HTTP headers. The callback is a GET with
    // no body, and the fixed size limit prevents a local peer from growing memory
    // without bound or holding finish() forever.
    std::expected<std::string, std::string> readHttpRequest(const int connection, const std::chrono::steady_clock::time_point deadline) {
        std::string request;
        std::array<char, 4096> buffer{};
        while (request.find("\r\n\r\n") == std::string::npos) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return std::unexpected("Timed out reading the OAuth browser callback");
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
            timeval timeout{static_cast<time_t>(remaining.count() / 1'000'000),
                            static_cast<suseconds_t>(remaining.count() % 1'000'000)};
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(connection, &readable);
            const int selected = ::select(connection + 1, &readable, nullptr, nullptr, &timeout);
            if (selected == 0) {
                return std::unexpected("Timed out reading the OAuth browser callback");
            }
            if (selected < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(systemError("Could not wait for OAuth callback data"));
            }

            const ssize_t received = ::recv(connection, buffer.data(), buffer.size(), 0);
            if (received == 0) {
                return std::unexpected("Browser closed the OAuth callback connection early");
            }
            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(systemError("Could not read OAuth callback"));
            }
            if (request.size() + static_cast<std::size_t>(received) > maximum_http_header_size) {
                return std::unexpected("OAuth callback headers are too large");
            }
            request.append(buffer.data(), static_cast<std::size_t>(received));
        }
        return request;
    }

    struct HttpRequestLine {
        std::string method;
        std::string target;
    };

    // We only need method and request-target; a general HTTP parser would add a
    // large dependency for a single localhost GET.
    std::expected<HttpRequestLine, std::string> parseHttpRequestLine(const std::string_view request) {
        const std::size_t line_end = request.find("\r\n");
        const std::string_view line = request.substr(0, line_end);
        const std::size_t first_space = line.find(' ');
        const std::size_t second_space = first_space == std::string_view::npos
                                             ? std::string_view::npos
                                             : line.find(' ', first_space + 1);
        if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
            line.substr(second_space + 1).find(' ') != std::string_view::npos ||
            !line.substr(second_space + 1).starts_with("HTTP/")) {
            return std::unexpected("Malformed OAuth callback request line");
        }
        return HttpRequestLine{
            std::string(line.substr(0, first_space)),
            std::string(line.substr(first_space + 1, second_space - first_space - 1))};
    }

    // Always close the connection so browser keep-alive cannot leave the login
    // waiting on an already-handled request. SIGPIPE is suppressed below because
    // browsers are allowed to close the tab before reading our final page.
    void sendHttpResponse(const int connection, const int status, const std::string_view reason, const std::string_view body) {
        const std::string response =
            "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
            "\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: "
            "no-store\r\nConnection: "
            "close\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + std::string(body);

        std::size_t sent = 0;
        while (sent < response.size()) {
#ifdef MSG_NOSIGNAL
            const ssize_t count =
                ::send(connection, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
#else
            const ssize_t count =
                ::send(connection, response.data() + sent, response.size() - sent, 0);
#endif
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(count);
        }
    }

    struct HttpResponse {
        long status = 0;
        std::string body;
        std::string callback_error;
    };

    // libcurl invokes this through a C ABI. Catch every exception here so none
    // can cross that boundary, and cap the response before appending it.
    std::size_t receiveHttpBody(char *data, const std::size_t size, const std::size_t count, void *user_data) {
        const std::size_t byte_count = size * count;
        auto &response = *static_cast<HttpResponse *>(user_data);
        try {
            if (byte_count > maximum_token_response_size -
                                 std::min(response.body.size(), maximum_token_response_size)) {
                response.callback_error = "OAuth token response is too large";
                return 0;
            }
            response.body.append(data, byte_count);
        } catch (...) {
            response.callback_error = "Could not store OAuth token response";
            return 0;
        }
        return byte_count;
    }

    // Shared token-endpoint transport. The initial code exchange is form encoded,
    // while refresh follows Codex and sends JSON, so content_type stays explicit.
    std::expected<HttpResponse, std::string> postBody(const std::string &url, const std::string &body, const std::string &content_type, const long timeout_seconds) {
        if (timeout_seconds <= 0) {
            return std::unexpected("OAuth token request timeout must be greater than zero");
        }

        static const CURLcode curl_initialization = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (curl_initialization != CURLE_OK) {
            return std::unexpected(std::string("Could not initialize HTTP client: ") +
                                   curl_easy_strerror(curl_initialization));
        }
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                 &curl_easy_cleanup);
        if (!curl) {
            return std::unexpected("Could not create OAuth token request");
        }

        HttpResponse response;
        std::array<char, CURL_ERROR_SIZE> curl_error{};
        curl_slist *raw_headers = curl_slist_append(nullptr, content_type.c_str());
        if (raw_headers == nullptr) {
            return std::unexpected("Could not allocate OAuth request headers");
        }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers,
                                                                            &curl_slist_free_all);

        const auto setOption = [&curl](const CURLoption option, const auto value) {
            return curl_easy_setopt(curl.get(), option, value) == CURLE_OK;
        };
        if (!setOption(CURLOPT_URL, url.c_str()) || !setOption(CURLOPT_HTTPHEADER, headers.get()) ||
            !setOption(CURLOPT_POST, 1L) || !setOption(CURLOPT_POSTFIELDS, body.data()) ||
            !setOption(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size())) ||
            !setOption(CURLOPT_WRITEFUNCTION, &receiveHttpBody) ||
            !setOption(CURLOPT_WRITEDATA, &response) ||
            !setOption(CURLOPT_ERRORBUFFER, curl_error.data()) ||
            !setOption(CURLOPT_USERAGENT, "microcodex") ||
            !setOption(CURLOPT_CONNECTTIMEOUT, timeout_seconds) ||
            !setOption(CURLOPT_TIMEOUT, timeout_seconds) || !setOption(CURLOPT_NOSIGNAL, 1L)) {
            return std::unexpected("Could not configure OAuth token request");
        }

        const CURLcode result = curl_easy_perform(curl.get());
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
        if (!response.callback_error.empty()) {
            return std::unexpected(response.callback_error);
        }
        if (result != CURLE_OK) {
            const std::string detail = curl_error[0] == '\0'
                                           ? std::string(curl_easy_strerror(result))
                                           : std::string(curl_error.data());
            return std::unexpected("OAuth token request failed: " + detail);
        }
        return response;
    }

    // Extract only the documented error fields. Never echo a successful token
    // response or arbitrary body because those may contain credentials.
    std::string tokenEndpointError(const std::string_view body, const long status) {
        auto description = microcodex::json::jsonStringMember(body, "error_description");
        if (description && description.value()) {
            return "OAuth token endpoint returned HTTP " + std::to_string(status) + ": " +
                   description.value().value();
        }
        auto code = microcodex::json::jsonStringMember(body, "error");
        if (code && code.value()) {
            return "OAuth token endpoint returned HTTP " + std::to_string(status) + ": " +
                   code.value().value();
        }
        return "OAuth token endpoint returned HTTP " + std::to_string(status);
    }

    // Refresh responses may omit a token or explicitly return JSON null. Treat
    // both forms as "keep the previous token" while rejecting other JSON types.
    std::expected<std::optional<std::string>, std::string> optionalStringOrNull(const std::string_view object, const std::string_view name) {
        auto member = microcodex::json::findJsonMember(object, name);
        if (!member) {
            return std::unexpected(member.error());
        }
        if (!member.value() || member.value().value() == "null") {
            return std::optional<std::string>{};
        }
        auto value = microcodex::json::string(member.value().value());
        if (!value) {
            return std::unexpected("JSON member '" + std::string(name) +
                                   "' is not a string or null");
        }
        return std::optional<std::string>{std::move(value.value())};
    }

    // The token endpoint authenticates the JWT; this routine only decodes its
    // payload to obtain routing metadata for CodexApi. OpenAI namespaces these
    // claims under "https://api.openai.com/auth" rather than at the JWT root.
    std::expected<std::string, std::string> accountIdFromIdToken(const std::string_view id_token) {
        const std::size_t first_dot = id_token.find('.');
        const std::size_t second_dot = first_dot == std::string_view::npos
                                           ? std::string_view::npos
                                           : id_token.find('.', first_dot + 1);
        if (first_dot == std::string_view::npos || second_dot == std::string_view::npos ||
            id_token.find('.', second_dot + 1) != std::string_view::npos || first_dot == 0 ||
            second_dot == first_dot + 1 || second_dot + 1 == id_token.size()) {
            return std::unexpected("OAuth token endpoint returned a malformed ID token");
        }

        auto payload = base64UrlDecode(id_token.substr(first_dot + 1, second_dot - first_dot - 1));
        if (!payload) {
            return std::unexpected("Could not decode OAuth ID token: " + payload.error());
        }
        auto auth =
            microcodex::json::findJsonMember(payload.value(), "https://api.openai.com/auth");
        if (!auth) {
            return std::unexpected("Could not parse OAuth ID token: " + auth.error());
        }
        if (!auth.value()) {
            return std::string{};
        }
        auto account_id =
            microcodex::json::jsonStringMember(auth.value().value(), "chatgpt_account_id");
        if (!account_id) {
            return std::unexpected("Could not parse OAuth account ID: " + account_id.error());
        }
        return account_id.value().value_or(std::string{});
    }

    // Complete RFC 7636: the short-lived browser code is useless without the
    // verifier kept inside OAuthLogin. The exact redirect URI must match the one
    // sent to /oauth/authorize, including the callback port.
    std::expected<microcodex::OAuthCredentials, std::string> exchangeAuthorizationCode(const microcodex::OAuthOptions &options, const std::string_view redirect_uri, const std::string_view code_verifier, const std::string_view code) {
        const std::string endpoint = options.issuer + "/oauth/token";
        const std::string form = "grant_type=authorization_code&code=" + percentEncode(code) +
                                 "&redirect_uri=" + percentEncode(redirect_uri) +
                                 "&client_id=" + percentEncode(options.client_id) +
                                 "&code_verifier=" + percentEncode(code_verifier);
        auto response = postBody(endpoint, form, "Content-Type: application/x-www-form-urlencoded",
                                 options.token_request_timeout_seconds);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(tokenEndpointError(response->body, response->status));
        }

        auto access_token = microcodex::json::requiredJsonString(response->body, "access_token");
        auto id_token = microcodex::json::requiredJsonString(response->body, "id_token");
        auto refresh_token = microcodex::json::requiredJsonString(response->body, "refresh_token");
        if (!access_token || !id_token || !refresh_token) {
            return std::unexpected("OAuth token endpoint returned an incomplete token set");
        }
        if (access_token->empty() || id_token->empty() || refresh_token->empty()) {
            return std::unexpected("OAuth token endpoint returned an empty token");
        }
        auto account_id = accountIdFromIdToken(id_token.value());
        if (!account_id) {
            return std::unexpected(account_id.error());
        }
        return microcodex::OAuthCredentials{
            std::move(access_token.value()), std::move(account_id.value()),
            std::move(id_token.value()), std::move(refresh_token.value())};
    }

    std::expected<void, std::string> validateOAuthOptions(const microcodex::OAuthOptions &options) {
        if (options.issuer.empty() || options.client_id.empty() || options.originator.empty()) {
            return std::unexpected("OAuth issuer, client ID, and originator cannot be empty");
        }
        if (containsNewline(options.issuer) || containsNewline(options.client_id) ||
            containsNewline(options.originator)) {
            return std::unexpected("OAuth options cannot contain a newline");
        }
        if ((!options.issuer.starts_with("https://") && !options.issuer.starts_with("http://")) ||
            options.issuer.find_first_of("?#") != std::string::npos) {
            return std::unexpected("OAuth issuer must be an HTTP(S) origin "
                                   "without query or fragment");
        }
        if (options.token_request_timeout_seconds <= 0) {
            return std::unexpected("OAuth token request timeout must be greater than zero");
        }
        return {};
    }

    std::string normalizedIssuer(std::string issuer) {
        while (issuer.size() > std::string_view("https://").size() && issuer.ends_with('/')) {
            issuer.pop_back();
        }
        return issuer;
    }

    // Keep these parameters in lockstep with the upstream Codex login flow. The
    // offline_access scope is what permits refreshes in later sessions.
    std::string buildAuthorizationUrl(const microcodex::OAuthOptions &options, const std::string_view redirect_uri, const std::string_view code_challenge, const std::string_view state) {
        const std::array<std::pair<std::string_view, std::string_view>, 10> parameters{{
            {"response_type", "code"},
            {"client_id", options.client_id},
            {"redirect_uri", redirect_uri},
            {"scope", "openid profile email offline_access api.connectors.read "
                      "api.connectors.invoke"},
            {"code_challenge", code_challenge},
            {"code_challenge_method", "S256"},
            {"id_token_add_organizations", "true"},
            {"codex_cli_simplified_flow", "true"},
            {"state", state},
            {"originator", options.originator},
        }};

        std::string url = options.issuer + "/oauth/authorize?";
        for (std::size_t position = 0; position < parameters.size(); ++position) {
            if (position != 0) {
                url += '&';
            }
            url += parameters[position].first;
            url += '=';
            url += percentEncode(parameters[position].second);
        }
        return url;
    }

    std::expected<void, std::string> validateCredentials(const microcodex::OAuthCredentials &credentials) {
        if (credentials.access_token.empty() || credentials.id_token.empty() ||
            credentials.refresh_token.empty()) {
            return std::unexpected("OAuth access token, ID token, and refresh "
                                   "token cannot be empty");
        }
        return {};
    }

    std::string utcTimestamp() {
        const std::time_t now = std::time(nullptr);
        std::tm utc{};
        gmtime_r(&now, &utc);
        std::array<char, 32> text{};
        std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return text.data();
    }

    // Write beside the final file, fsync it, then rename it into place. Because
    // rename is atomic on one filesystem, a crash can leave either the old or
    // new auth.json but never a half-written credential file. O_EXCL plus 0600
    // also prevents following a pre-created temporary-file symlink.
    std::expected<void, std::string> writeFileAtomically(const std::filesystem::path &path, const std::string_view contents) {
        std::error_code filesystem_error;
        const std::filesystem::path parent = path.has_parent_path() ? path.parent_path() : ".";
        const bool created = std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            return std::unexpected("Could not create OAuth credentials directory: " +
                                   filesystem_error.message());
        }
        if (created) {
            std::filesystem::permissions(parent, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, filesystem_error);
            if (filesystem_error) {
                return std::unexpected("Could not protect OAuth credentials directory: " +
                                       filesystem_error.message());
            }
        }

        std::filesystem::path temporary;
        FileDescriptor file;
        for (int attempt = 0; attempt < 10; ++attempt) {
            temporary = path.string() + ".tmp." + randomBase64Url(9);
            file.reset(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600));
            if (file) {
                break;
            }
            if (errno != EEXIST) {
                return std::unexpected(systemError("Could not create temporary credentials file"));
            }
        }
        if (!file) {
            return std::unexpected("Could not choose a temporary credentials filename");
        }

        const auto discardTemporary = [&temporary]() {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        };
        std::size_t written = 0;
        while (written < contents.size()) {
            const ssize_t count =
                ::write(file.get(), contents.data() + written, contents.size() - written);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                const std::string error = systemError("Could not write OAuth credentials");
                file.reset();
                discardTemporary();
                return std::unexpected(error);
            }
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(file.get()) < 0) {
            const std::string error = systemError("Could not flush OAuth credentials");
            file.reset();
            discardTemporary();
            return std::unexpected(error);
        }
        file.reset();
        if (::rename(temporary.c_str(), path.c_str()) < 0) {
            const std::string error = systemError("Could not install OAuth credentials file");
            discardTemporary();
            return std::unexpected(error);
        }
        return {};
    }

    // auth.json should be tiny. Bound reads even when the file changes after
    // fstat(), so a corrupted or hostile file cannot allocate unlimited memory.
    std::expected<std::optional<std::string>, std::string> readSmallFile(const std::filesystem::path &path) {
        FileDescriptor file(::open(path.c_str(), O_RDONLY));
        if (!file) {
            if (errno == ENOENT) {
                return std::optional<std::string>{};
            }
            return std::unexpected(systemError("Could not open OAuth credentials"));
        }

        struct stat information{};
        if (::fstat(file.get(), &information) < 0) {
            return std::unexpected(systemError("Could not inspect OAuth credentials"));
        }
        if (information.st_size < 0 ||
            static_cast<std::uintmax_t>(information.st_size) > maximum_token_response_size) {
            return std::unexpected("OAuth credentials file is too large");
        }

        std::string contents;
        contents.reserve(static_cast<std::size_t>(information.st_size));
        std::array<char, 4096> buffer{};
        while (true) {
            const ssize_t count = ::read(file.get(), buffer.data(), buffer.size());
            if (count == 0) {
                break;
            }
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(systemError("Could not read OAuth credentials"));
            }
            if (contents.size() + static_cast<std::size_t>(count) > maximum_token_response_size) {
                return std::unexpected("OAuth credentials file is too large");
            }
            contents.append(buffer.data(), static_cast<std::size_t>(count));
        }
        return std::optional<std::string>{std::move(contents)};
    }

} // namespace

namespace microcodex {

    // Everything that must survive between start() and finish() lives here:
    // the listening socket, the exact redirect URI, and the two per-login
    // secrets. Keeping it behind the PImpl also keeps POSIX socket details out
    // of oauth.h.
    struct OAuthLogin::Impl {
        OAuthOptions options;
        FileDescriptor listener;
        std::uint16_t callback_port = 0;
        std::string redirect_uri;
        std::string code_verifier;
        std::string state;
        std::string authorization_url;
        bool finish_called = false;
    };

    OAuthLogin::OAuthLogin(std::unique_ptr<Impl> implementation)
        : implementation_(std::move(implementation)) {}

    OAuthLogin::OAuthLogin(OAuthLogin &&) noexcept = default;
    OAuthLogin &OAuthLogin::operator=(OAuthLogin &&) noexcept = default;
    OAuthLogin::~OAuthLogin() = default;

    // Start listening before returning the URL. Otherwise a fast browser could
    // complete authorization before the callback socket exists.
    std::expected<OAuthLogin, std::string> OAuthLogin::start(OAuthOptions options) {
        options.issuer = normalizedIssuer(std::move(options.issuer));
        auto validation = validateOAuthOptions(options);
        if (!validation) {
            return std::unexpected(validation.error());
        }

        auto bound = bindListener(options.callback_port);
        // The Codex client has two registered redirects. Only fall back when the
        // preferred port is occupied; other bind errors should remain visible.
        if (!bound && bound.error().error_number == EADDRINUSE && options.callback_port != 0 &&
            options.fallback_callback_port != options.callback_port) {
            bound = bindListener(options.fallback_callback_port);
        }
        if (!bound) {
            return std::unexpected(bound.error().message);
        }

        auto implementation = std::make_unique<Impl>();
        implementation->options = std::move(options);
        implementation->listener = std::move(bound->socket);
        implementation->callback_port = bound->port;
        implementation->redirect_uri =
            "http://localhost:" + std::to_string(bound->port) + "/auth/callback";
        implementation->code_verifier = randomBase64Url(64);
        implementation->state = randomBase64Url(32);
        const std::string challenge = sha256Base64Url(implementation->code_verifier);
        implementation->authorization_url =
            buildAuthorizationUrl(implementation->options, implementation->redirect_uri, challenge,
                                  implementation->state);
        return OAuthLogin(std::move(implementation));
    }

    const std::string &OAuthLogin::authorizationUrl() const {
        static const std::string empty;
        return implementation_ ? implementation_->authorization_url : empty;
    }

    std::uint16_t OAuthLogin::callbackPort() const {
        return implementation_ ? implementation_->callback_port : 0;
    }

    std::expected<OAuthCredentials, std::string> OAuthLogin::finish(const std::chrono::seconds timeout) {
        if (!implementation_) {
            return std::unexpected("OAuth login object has been moved from");
        }
        if (implementation_->finish_called) {
            return std::unexpected("OAuth login has already been finished");
        }
        if (timeout <= std::chrono::seconds::zero()) {
            return std::unexpected("OAuth callback timeout must be greater than zero");
        }
        implementation_->finish_called = true;
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        // Browsers may request unrelated paths, and a local process may send a
        // callback with the wrong state. Respond to those requests but keep
        // waiting; only a callback carrying our state is terminal.
        while (implementation_->listener) {
            auto connection = acceptBefore(implementation_->listener.get(), deadline);
            if (!connection) {
                cancel();
                return std::unexpected(connection.error());
            }
            auto raw_request = readHttpRequest(connection->get(), deadline);
            if (!raw_request) {
                sendHttpResponse(connection->get(), 400, "Bad Request", raw_request.error());
                continue;
            }
            auto request = parseHttpRequestLine(raw_request.value());
            if (!request || request->method != "GET") {
                sendHttpResponse(connection->get(), 400, "Bad Request",
                                 request ? "OAuth callback must use GET" : request.error());
                continue;
            }

            const std::size_t question = request->target.find('?');
            const std::string_view path = std::string_view(request->target).substr(0, question);
            const std::string_view query =
                question == std::string::npos
                    ? std::string_view{}
                    : std::string_view(request->target).substr(question + 1);
            if (path == "/cancel") {
                sendHttpResponse(connection->get(), 200, "OK", "Login cancelled.");
                cancel();
                return std::unexpected("OAuth login was cancelled");
            }
            if (path != "/auth/callback") {
                sendHttpResponse(connection->get(), 404, "Not Found", "Not Found");
                continue;
            }

            auto parameters = parseQuery(query);
            if (!parameters) {
                sendHttpResponse(connection->get(), 400, "Bad Request", parameters.error());
                continue;
            }
            // State authenticates the localhost callback and prevents another
            // page or process from completing this login attempt with its code.
            const auto state = parameters->find("state");
            if (state == parameters->end() ||
                !constantTimeEqual(state->second, implementation_->state)) {
                sendHttpResponse(connection->get(), 400, "Bad Request", "OAuth state mismatch");
                continue;
            }

            const auto oauth_error = parameters->find("error");
            if (oauth_error != parameters->end()) {
                const auto description = parameters->find("error_description");
                const std::string message =
                    description == parameters->end() || description->second.empty()
                        ? "OAuth authorization failed: " + oauth_error->second
                        : "OAuth authorization failed: " + description->second;
                sendHttpResponse(connection->get(), 400, "Bad Request", message);
                cancel();
                return std::unexpected(message);
            }

            const auto code = parameters->find("code");
            if (code == parameters->end() || code->second.empty()) {
                const std::string message = "OAuth callback has no authorization code";
                sendHttpResponse(connection->get(), 400, "Bad Request", message);
                cancel();
                return std::unexpected(message);
            }

            // The callback has been authenticated by state. Stop accepting
            // requests before the network exchange so there is only one
            // terminal callback.
            implementation_->listener.reset();
            auto credentials =
                exchangeAuthorizationCode(implementation_->options, implementation_->redirect_uri,
                                          implementation_->code_verifier, code->second);
            implementation_->code_verifier.clear();
            implementation_->state.clear();
            if (!credentials) {
                sendHttpResponse(connection->get(), 500, "Login Failed", credentials.error());
                return std::unexpected(credentials.error());
            }
            sendHttpResponse(connection->get(), 200, "OK",
                             "Login complete. You can close this browser window.");
            return credentials;
        }
        return std::unexpected("OAuth login was cancelled");
    }

    void OAuthLogin::cancel() {
        if (implementation_) {
            implementation_->listener.reset();
            implementation_->code_verifier.clear();
            implementation_->state.clear();
        }
    }

    // Match Codex's path resolution so both programs can consume the same
    // auth.json when the user opts into CODEX_HOME.
    std::expected<std::filesystem::path, std::string> defaultOAuthCredentialsPath() {
        if (const char *codex_home = std::getenv("CODEX_HOME");
            codex_home != nullptr && codex_home[0] != '\0') {
            return std::filesystem::path(codex_home) / "auth.json";
        }
        if (const char *user_home = std::getenv("HOME");
            user_home != nullptr && user_home[0] != '\0') {
            return std::filesystem::path(user_home) / ".codex" / "auth.json";
        }
        return std::unexpected("Cannot find OAuth credentials path: HOME is not set");
    }

    std::expected<std::optional<OAuthCredentials>, std::string> loadOAuthCredentials(const std::filesystem::path &path) {
        auto contents = readSmallFile(path);
        if (!contents) {
            return std::unexpected(contents.error());
        }
        if (!contents.value()) {
            return std::optional<OAuthCredentials>{};
        }

        // Ignore unrelated Codex fields and read only the token object this API
        // owns. That keeps the loader compatible as auth.json grows new fields.
        auto tokens = json::findJsonMember(contents.value().value(), "tokens");
        if (!tokens) {
            return std::unexpected("Could not parse OAuth credentials: " + tokens.error());
        }
        if (!tokens.value()) {
            return std::unexpected("OAuth credentials file has no token set");
        }

        auto access_token = json::requiredJsonString(tokens.value().value(), "access_token");
        auto id_token = json::requiredJsonString(tokens.value().value(), "id_token");
        auto refresh_token = json::requiredJsonString(tokens.value().value(), "refresh_token");
        if (!access_token || !id_token || !refresh_token) {
            return std::unexpected("OAuth credentials file has an incomplete token set");
        }

        auto account = optionalStringOrNull(tokens.value().value(), "account_id");
        if (!account) {
            return std::unexpected("Could not parse OAuth credentials account ID: " +
                                   account.error());
        }

        OAuthCredentials credentials{std::move(access_token.value()),
                                     account.value().value_or(std::string{}),
                                     std::move(id_token.value()), std::move(refresh_token.value())};
        auto validation = validateCredentials(credentials);
        if (!validation) {
            return std::unexpected(validation.error());
        }
        return std::optional<OAuthCredentials>{std::move(credentials)};
    }

    std::expected<std::optional<OAuthCredentials>, std::string> loadOAuthCredentials() {
        auto path = defaultOAuthCredentialsPath();
        if (!path) {
            return std::unexpected(path.error());
        }
        return loadOAuthCredentials(path.value());
    }

    std::expected<void, std::string> saveOAuthCredentials(const OAuthCredentials &credentials, const std::filesystem::path &path) {
        auto validation = validateCredentials(credentials);
        if (!validation) {
            return validation;
        }

        // Preserve Codex's auth.json shape rather than inventing a second
        // credential format. account_id is nullable in the upstream schema.
        std::string contents = "{\n  \"auth_mode\": \"chatgpt\",\n  \"OPENAI_API_KEY\": null,\n  "
                               "\"tokens\": {\n    \"id_token\": ";
        json::appendJsonString(contents, credentials.id_token);
        contents += ",\n    \"access_token\": ";
        json::appendJsonString(contents, credentials.access_token);
        contents += ",\n    \"refresh_token\": ";
        json::appendJsonString(contents, credentials.refresh_token);
        contents += ",\n    \"account_id\": ";
        if (credentials.account_id.empty()) {
            contents += "null";
        } else {
            json::appendJsonString(contents, credentials.account_id);
        }
        contents += "\n  },\n  \"last_refresh\": ";
        json::appendJsonString(contents, utcTimestamp());
        contents += "\n}\n";
        return writeFileAtomically(path, contents);
    }

    std::expected<void, std::string> saveOAuthCredentials(const OAuthCredentials &credentials) {
        auto path = defaultOAuthCredentialsPath();
        if (!path) {
            return std::unexpected(path.error());
        }
        return saveOAuthCredentials(credentials, path.value());
    }

    std::expected<OAuthCredentials, std::string> refreshOAuthCredentials(const OAuthCredentials &credentials, OAuthOptions options) {
        auto credential_validation = validateCredentials(credentials);
        if (!credential_validation) {
            return std::unexpected(credential_validation.error());
        }
        options.issuer = normalizedIssuer(std::move(options.issuer));
        auto option_validation = validateOAuthOptions(options);
        if (!option_validation) {
            return std::unexpected(option_validation.error());
        }

        std::string body = "{\"client_id\":";
        json::appendJsonString(body, options.client_id);
        body += ",\"grant_type\":\"refresh_token\",\"refresh_token\":";
        json::appendJsonString(body, credentials.refresh_token);
        body += '}';
        auto response =
            postBody(options.issuer + "/oauth/token", body, "Content-Type: application/json",
                     options.token_request_timeout_seconds);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(tokenEndpointError(response->body, response->status));
        }

        auto access_token = json::requiredJsonString(response->body, "access_token");
        if (!access_token || access_token->empty()) {
            return std::unexpected("OAuth refresh response has no access token");
        }
        // OpenAI can rotate any subset of the tokens. Start from the old set and
        // replace only non-null, non-empty values returned by the server.
        OAuthCredentials refreshed = credentials;
        refreshed.access_token = std::move(access_token.value());

        auto id_token = optionalStringOrNull(response->body, "id_token");
        auto refresh_token = optionalStringOrNull(response->body, "refresh_token");
        if (!id_token || !refresh_token) {
            return std::unexpected("OAuth refresh response contains an invalid token");
        }
        if (id_token.value() && !id_token.value()->empty()) {
            refreshed.id_token = std::move(id_token.value().value());
            auto account_id = accountIdFromIdToken(refreshed.id_token);
            if (!account_id) {
                return std::unexpected(account_id.error());
            }
            refreshed.account_id = std::move(account_id.value());
        }
        if (refresh_token.value() && !refresh_token.value()->empty()) {
            refreshed.refresh_token = std::move(refresh_token.value().value());
        }
        return refreshed;
    }

    // unlink() removes only a file or symlink; unlike filesystem::remove it will
    // never delete an empty directory accidentally passed as the auth path.
    std::expected<bool, std::string> logoutOAuth(const std::filesystem::path &path) {
        if (::unlink(path.c_str()) == 0) {
            return true;
        }
        if (errno == ENOENT) {
            return false;
        }
        return std::unexpected(systemError("Could not delete OAuth credentials"));
    }

    std::expected<bool, std::string> logoutOAuth() {
        auto path = defaultOAuthCredentialsPath();
        if (!path) {
            return std::unexpected(path.error());
        }
        return logoutOAuth(path.value());
    }

    // These are the only OAuth values consumed by api.cpp request headers.
    void applyOAuthCredentials(CodexApiConfig &config, const OAuthCredentials &credentials) {
        config.access_token = credentials.access_token;
        config.account_id = credentials.account_id;
    }

} // namespace microcodex
