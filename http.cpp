// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "http.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <string>

namespace microcodex {

    namespace {

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
        };

        std::size_t receiveBody(char *data, const std::size_t size, const std::size_t count, void *user_data) {
            const std::size_t byte_count = size * count;
            auto &state = *static_cast<RequestState *>(user_data);
            if (state.request.stop_token.stop_requested()) return 0;

            try {
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

    } // namespace

    std::expected<HttpResponse, std::string> performHttpRequest(const HttpRequest &request, const HttpDataHandler body_handler, const HttpDataHandler header_handler, void *user_data) {
        static const CURLcode curl_initialization = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (curl_initialization != CURLE_OK) {
            return std::unexpected(std::string("Could not initialize HTTP client: ") + curl_easy_strerror(curl_initialization));
        }

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
        if (request.total_timeout_seconds > 0 && !setOption(CURLOPT_TIMEOUT, request.total_timeout_seconds)) {
            return std::unexpected("Could not configure HTTP timeout");
        }

        const CURLcode result = curl_easy_perform(curl.get());
        long status = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);

        if (request.stop_token.stop_requested()) return std::unexpected("HTTP request interrupted");
        if (!state.error.empty()) return std::unexpected(std::move(state.error));
        if (result != CURLE_OK) {
            const std::string detail = curl_error[0] != '\0' ? std::string(curl_error.data()) : std::string(curl_easy_strerror(result));
            return std::unexpected("HTTP request failed: " + detail);
        }
        return HttpResponse{.status = status, .body = std::move(state.body)};
    }

} // namespace microcodex
