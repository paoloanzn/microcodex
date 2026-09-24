// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "http.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <thread>

namespace microcodex {

    namespace {

        constexpr int max_http_attempts = 4;
        constexpr auto initial_retry_delay = std::chrono::milliseconds(50);

        class CurlHeaders {
        public:
            CurlHeaders() = default;
            CurlHeaders(const CurlHeaders &) = delete;
            CurlHeaders &operator=(const CurlHeaders &) = delete;
            ~CurlHeaders() { curl_slist_free_all(headers_); }

            bool append(const std::string &header) {
                curl_slist *appended = curl_slist_append(headers_, header.c_str());
                if (appended == nullptr) return false;
                headers_ = appended;
                return true;
            }

            curl_slist *get() const { return headers_; }

        private:
            curl_slist *headers_ = nullptr;
        };

        struct RequestState {
            const HttpRequest &request;
            HttpDataHandler body_handler;
            HttpDataHandler header_handler;
            void *user_data;
            std::string body;
            std::string error;
            std::size_t body_bytes_received = 0;
        };

        std::string toLowerAscii(std::string_view text) {
            std::string lowered(text);
            for (char &character : lowered) {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            return lowered;
        }

        bool containsInsensitive(std::string_view haystack, std::string_view needle) {
            if (needle.empty()) return true;
            const std::string lowered_haystack = toLowerAscii(haystack);
            const std::string lowered_needle = toLowerAscii(needle);
            return lowered_haystack.find(lowered_needle) != std::string::npos;
        }

        // Narrow markers only: broad strings like "libressl", "ssl routines",
        // or generic "alert" would also match permanent certificate/handshake failures.
        bool detailLooksLikeTransientTls(std::string_view detail) {
            return containsInsensitive(detail, "ssl_read") ||
                   containsInsensitive(detail, "ssl_write") ||
                   containsInsensitive(detail, "bad record mac") ||
                   containsInsensitive(detail, "decryption_failed") ||
                   containsInsensitive(detail, "decryption failed") ||
                   containsInsensitive(detail, "unexpected eof while reading");
        }

        bool isRetryableCurlCode(const CURLcode result) {
            switch (result) {
            case CURLE_RECV_ERROR:
            case CURLE_SEND_ERROR:
            case CURLE_GOT_NOTHING:
            case CURLE_PARTIAL_FILE:
            case CURLE_OPERATION_TIMEDOUT:
            case CURLE_COULDNT_CONNECT:
            case CURLE_SSL_SHUTDOWN_FAILED:
                return true;
            default:
                return false;
            }
        }

        bool isTransientTransportFailure(const CURLcode result, std::string_view detail) {
            // Handshake/connect TLS errors are often permanent (certs, protocol).
            // Only treat CURLE_SSL_CONNECT_ERROR as transient when detail has a
            // narrow retryable indicator; other CURLcodes keep their own rules.
            if (result == CURLE_SSL_CONNECT_ERROR) {
                return detailLooksLikeTransientTls(detail);
            }
            return isRetryableCurlCode(result);
        }

        std::atomic<int> &testTransientTlsFailuresRemaining() {
            static std::atomic<int> remaining{-1};
            static std::once_flag loaded;
            std::call_once(loaded, [] {
                const char *value = std::getenv("MICROCODEX_TEST_TRANSIENT_TLS_FAILURES");
                if (value == nullptr || value[0] == '\0') {
                    remaining.store(0, std::memory_order_relaxed);
                    return;
                }
                char *end = nullptr;
                const long parsed = std::strtol(value, &end, 10);
                if (end == value || parsed < 0 || parsed > 1000) {
                    remaining.store(0, std::memory_order_relaxed);
                    return;
                }
                remaining.store(static_cast<int>(parsed), std::memory_order_relaxed);
            });
            return remaining;
        }

        bool consumeTestTransientTlsFailure(std::string &detail) {
            const int previous = testTransientTlsFailuresRemaining().fetch_sub(1, std::memory_order_relaxed);
            if (previous <= 0) {
                if (previous == 0) {
                    testTransientTlsFailuresRemaining().fetch_add(1, std::memory_order_relaxed);
                }
                return false;
            }
            // Synthetic macOS LibreSSL failure matching issue #12.
            detail =
                "LibreSSL SSL_read: LibreSSL/3.3.6: error:1404C3FC:SSL routines:ST_OK:sslv3 alert bad record mac, errno 0";
            return true;
        }

        void appendAttemptLog(std::string_view line) {
            const char *path = std::getenv("MICROCODEX_TEST_HTTP_ATTEMPT_LOG");
            if (path == nullptr || path[0] == '\0') return;
            FILE *file = std::fopen(path, "a");
            if (file == nullptr) return;
            std::fwrite(line.data(), 1, line.size(), file);
            std::fputc('\n', file);
            std::fclose(file);
        }

        std::size_t receiveBody(char *data, const std::size_t size, const std::size_t count, void *user_data) {
            const std::size_t byte_count = size * count;
            auto &state = *static_cast<RequestState *>(user_data);
            if (state.request.stop_token.stop_requested()) return 0;

            try {
                state.body_bytes_received += byte_count;
                if (state.body_handler != nullptr) {
                    auto handled = state.body_handler(std::string_view(data, byte_count), state.user_data);
                    if (!handled) {
                        state.error = handled.error();
                        return 0;
                    }
                }

                const std::size_t remaining = state.request.maximum_response_bytes - std::min(state.body.size(), state.request.maximum_response_bytes);
                state.body.append(data, std::min(byte_count, remaining));
                if (state.body_handler == nullptr && byte_count > remaining) {
                    state.error = "HTTP response exceeds its size limit";
                    return 0;
                }
            } catch (const std::exception &error) {
                state.error = std::string("HTTP body callback failed: ") + error.what();
                return 0;
            } catch (...) {
                state.error = "HTTP body callback failed";
                return 0;
            }
            return byte_count;
        }

        std::size_t receiveHeader(char *data, const std::size_t size, const std::size_t count, void *user_data) {
            const std::size_t byte_count = size * count;
            auto &state = *static_cast<RequestState *>(user_data);
            if (state.request.stop_token.stop_requested()) return 0;
            if (state.header_handler == nullptr) return byte_count;

            try {
                auto handled = state.header_handler(std::string_view(data, byte_count), state.user_data);
                if (!handled) {
                    state.error = handled.error();
                    return 0;
                }
            } catch (const std::exception &error) {
                state.error = std::string("HTTP header callback failed: ") + error.what();
                return 0;
            } catch (...) {
                state.error = "HTTP header callback failed";
                return 0;
            }
            return byte_count;
        }

        int transferProgress(void *user_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            const auto &state = *static_cast<const RequestState *>(user_data);
            return state.request.stop_token.stop_requested() ? 1 : 0;
        }

        std::expected<HttpResponse, std::string> performHttpRequestOnce(const HttpRequest &request, const HttpDataHandler body_handler, const HttpDataHandler header_handler, void *user_data, std::string &transport_detail, CURLcode &transport_code, std::size_t &body_bytes_received, bool &request_sent, long attempt_total_timeout_seconds) {
            transport_detail.clear();
            transport_code = CURLE_OK;
            body_bytes_received = 0;
            request_sent = false;

            std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
            if (!curl) return std::unexpected("Could not create HTTP request");

            CurlHeaders headers;
            for (const std::string &header : request.headers) {
                if (!headers.append(header)) return std::unexpected("Could not allocate HTTP headers");
            }

            RequestState state{
                .request = request,
                .body_handler = body_handler,
                .header_handler = header_handler,
                .user_data = user_data,
                .body = {},
                .error = {},
                .body_bytes_received = 0,
            };
            const std::string url(request.url);
            std::array<char, CURL_ERROR_SIZE> curl_error{};
            const auto setOption = [&curl](const CURLoption option, const auto value) {
                return curl_easy_setopt(curl.get(), option, value) == CURLE_OK;
            };

            if (!setOption(CURLOPT_URL, url.c_str()) ||
                !setOption(CURLOPT_HTTPHEADER, headers.get()) ||
                !setOption(CURLOPT_WRITEFUNCTION, &receiveBody) ||
                !setOption(CURLOPT_WRITEDATA, &state) ||
                !setOption(CURLOPT_HEADERFUNCTION, &receiveHeader) ||
                !setOption(CURLOPT_HEADERDATA, &state) ||
                !setOption(CURLOPT_XFERINFOFUNCTION, &transferProgress) ||
                !setOption(CURLOPT_XFERINFODATA, &state) ||
                !setOption(CURLOPT_NOPROGRESS, 0L) ||
                !setOption(CURLOPT_ERRORBUFFER, curl_error.data()) ||
                !setOption(CURLOPT_USERAGENT, "microcodex") ||
                !setOption(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS) ||
                !setOption(CURLOPT_NOSIGNAL, 1L)) {
                return std::unexpected("Could not configure HTTP request");
            }

            if (request.method == HttpMethod::Post &&
                (!setOption(CURLOPT_POST, 1L) ||
                 !setOption(CURLOPT_POSTFIELDS, request.body.data()) ||
                 !setOption(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size())))) {
                return std::unexpected("Could not configure HTTP request body");
            }
            if (request.method == HttpMethod::Get && !setOption(CURLOPT_HTTPGET, 1L)) {
                return std::unexpected("Could not configure HTTP GET request");
            }
            if (request.idle_timeout_seconds > 0 &&
                (!setOption(CURLOPT_LOW_SPEED_LIMIT, 1L) || !setOption(CURLOPT_LOW_SPEED_TIME, request.idle_timeout_seconds))) {
                return std::unexpected("Could not configure HTTP idle timeout");
            }
            if (attempt_total_timeout_seconds > 0 && !setOption(CURLOPT_TIMEOUT, attempt_total_timeout_seconds)) {
                return std::unexpected("Could not configure HTTP timeout");
            }

            if (consumeTestTransientTlsFailure(transport_detail)) {
                // Failure injected before curl_easy_perform: request never left the client.
                transport_code = CURLE_RECV_ERROR;
                body_bytes_received = state.body_bytes_received;
                request_sent = false;
                return std::unexpected("HTTP request failed: " + transport_detail);
            }

            const CURLcode result = curl_easy_perform(curl.get());
            long status = 0;
            curl_off_t uploaded_bytes = 0;
            curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
            curl_easy_getinfo(curl.get(), CURLINFO_SIZE_UPLOAD_T, &uploaded_bytes);
            body_bytes_received = state.body_bytes_received;
            // POST is only safe to auto-retry when nothing was uploaded and the
            // server never produced a status line (request not known to be sent).
            request_sent = uploaded_bytes > 0 || status > 0;

            if (request.stop_token.stop_requested()) return std::unexpected("HTTP request interrupted");
            if (!state.error.empty()) return std::unexpected(std::move(state.error));
            if (result != CURLE_OK) {
                transport_code = result;
                transport_detail = curl_error[0] != '\0' ? std::string(curl_error.data()) : std::string(curl_easy_strerror(result));
                return std::unexpected("HTTP request failed: " + transport_detail);
            }
            return HttpResponse{.status = status, .body = std::move(state.body)};
        }

    } // namespace

    bool isTransientHttpTransportError(const std::string_view detail) {
        return detailLooksLikeTransientTls(detail);
    }

    std::string httpTransportFailureMessage(const std::string_view detail) {
        if (detailLooksLikeTransientTls(detail)) {
            return "HTTP request failed: transient TLS connection error";
        }
        if (detail.empty()) return "HTTP request failed";
        if (detail.starts_with("HTTP request failed:")) return std::string(detail);
        return "HTTP request failed: " + std::string(detail);
    }

    std::expected<HttpResponse, std::string> performHttpRequest(const HttpRequest &request, const HttpDataHandler body_handler, const HttpDataHandler header_handler, void *user_data) {
        static const CURLcode curl_initialization = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (curl_initialization != CURLE_OK) {
            return std::unexpected(std::string("Could not initialize HTTP client: ") + curl_easy_strerror(curl_initialization));
        }

        std::string transport_detail;
        CURLcode transport_code = CURLE_OK;
        std::size_t body_bytes_received = 0;
        bool request_sent = false;
        std::expected<HttpResponse, std::string> last_failure = std::unexpected("HTTP request failed");

        const bool has_total_timeout = request.total_timeout_seconds > 0;
        const auto overall_deadline = has_total_timeout
                                          ? std::chrono::steady_clock::now() + std::chrono::seconds(request.total_timeout_seconds)
                                          : std::chrono::steady_clock::time_point::max();

        for (int attempt = 0; attempt < max_http_attempts; ++attempt) {
            if (request.stop_token.stop_requested()) return std::unexpected("HTTP request interrupted");

            long attempt_timeout_seconds = 0;
            if (has_total_timeout) {
                const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(overall_deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0) break;
                attempt_timeout_seconds = remaining.count();
            }

            appendAttemptLog("attempt " + std::to_string(attempt + 1));
            auto response = performHttpRequestOnce(request, body_handler, header_handler, user_data, transport_detail, transport_code, body_bytes_received, request_sent, attempt_timeout_seconds);
            if (response) return response;

            last_failure = std::unexpected(response.error());
            if (request.stop_token.stop_requested()) return std::unexpected("HTTP request interrupted");
            if (response.error() == "HTTP request interrupted") return last_failure;

            // Callback/configuration errors and partial streams are not retried.
            const bool transport_failure = !transport_detail.empty() || transport_code != CURLE_OK;
            // GET is idempotent when no response body arrived. POST is only
            // retried when curl shows the request was never sent.
            const bool safe_to_replay =
                body_bytes_received == 0 &&
                (request.method == HttpMethod::Get || !request_sent);
            if (!transport_failure || !safe_to_replay) {
                if (transport_failure && detailLooksLikeTransientTls(transport_detail)) {
                    return std::unexpected(httpTransportFailureMessage(transport_detail));
                }
                return last_failure;
            }

            if (!isTransientTransportFailure(transport_code, transport_detail)) {
                if (detailLooksLikeTransientTls(transport_detail)) {
                    return std::unexpected(httpTransportFailureMessage(transport_detail));
                }
                return last_failure;
            }

            if (attempt + 1 >= max_http_attempts) break;

            const auto delay = initial_retry_delay * (1 << attempt);
            appendAttemptLog("retry-backoff-ms " + std::to_string(delay.count()));
            auto backoff_deadline = std::chrono::steady_clock::now() + delay;
            if (has_total_timeout && backoff_deadline > overall_deadline) {
                backoff_deadline = overall_deadline;
            }
            while (std::chrono::steady_clock::now() < backoff_deadline) {
                if (request.stop_token.stop_requested()) return std::unexpected("HTTP request interrupted");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (has_total_timeout && std::chrono::steady_clock::now() >= overall_deadline) break;
        }

        if (detailLooksLikeTransientTls(transport_detail)) {
            return std::unexpected(httpTransportFailureMessage(transport_detail));
        }
        return last_failure;
    }

} // namespace microcodex
