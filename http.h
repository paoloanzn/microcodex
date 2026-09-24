// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace microcodex {

    enum class HttpMethod {
        Get,
        Post,
    };

    struct HttpRequest {
        HttpMethod method;
        std::string_view url;
        std::span<const std::string> headers;
        std::string_view body;
        long idle_timeout_seconds;
        long total_timeout_seconds;
        std::size_t maximum_response_bytes;
        std::stop_token stop_token;
    };

    struct HttpResponse {
        long status;
        std::string body;
    };

    using HttpDataHandler = std::expected<void, std::string> (*)(std::string_view data, void *user_data);

    // True when a libcurl/LibreSSL/OpenSSL transport error detail describes a
    // transient TLS or network failure that is safe to retry (for example
    // SSL_read / sslv3 alert bad record mac on macOS LibreSSL).
    bool isTransientHttpTransportError(std::string_view detail);

    // Stable failure text for transport errors. Transient TLS failures are
    // mapped to a generic message so raw LibreSSL/OpenSSL strings are not
    // surfaced as fatal turn failures.
    std::string httpTransportFailureMessage(std::string_view detail);

    // Performs one synchronous request. Callers may consume body and header
    // chunks as they arrive; a bounded body copy is retained for errors and
    // non-streaming responses. Transient TLS/network failures are retried with
    // exponential backoff within the configured total timeout: GET when no
    // response body bytes have arrived yet, and POST only when the request is
    // known not to have been sent.
    std::expected<HttpResponse, std::string> performHttpRequest(const HttpRequest &request, HttpDataHandler body_handler = nullptr, HttpDataHandler header_handler = nullptr, void *user_data = nullptr);

} // namespace microcodex
