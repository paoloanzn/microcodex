// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "http.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

    int failures = 0;

    void expect(const bool condition, const std::string_view message) {
        if (condition) return;
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }

} // namespace

int main() {
    using microcodex::httpTransportFailureMessage;
    using microcodex::isTransientHttpTransportError;

    constexpr std::string_view libressl_bad_mac =
        "LibreSSL SSL_read: LibreSSL/3.3.6: error:1404C3FC:SSL routines:ST_OK:sslv3 alert bad record mac, errno 0";

    expect(isTransientHttpTransportError(libressl_bad_mac),
           "LibreSSL SSL_read bad record mac should be transient");
    expect(isTransientHttpTransportError("error:1404C3FC:SSL routines:ST_OK:sslv3 alert bad record mac"),
           "OpenSSL-style bad record mac should be transient");
    expect(isTransientHttpTransportError("SSL_write: broken pipe"),
           "SSL_write failures should be transient");
    expect(!isTransientHttpTransportError("HTTP 401 unauthorized"),
           "application HTTP errors should not be classified as transient TLS");
    expect(!isTransientHttpTransportError("Could not resolve host"),
           "unrelated curl errors without TLS markers should not match TLS classifier");
    expect(!isTransientHttpTransportError("error:0A000086:SSL routines:tls_process_server_certificate:certificate verify failed"),
           "certificate verify failures must not match via broad SSL routines text");
    expect(!isTransientHttpTransportError("LibreSSL/3.3.6: handshake failure"),
           "mere LibreSSL backend mention must not be classified as transient");
    expect(!isTransientHttpTransportError("sslv3 alert handshake failure"),
           "generic sslv3 alert without a narrow transient marker must not match");

    const std::string sanitized = httpTransportFailureMessage(libressl_bad_mac);
    expect(sanitized == "HTTP request failed: transient TLS connection error",
           "transient TLS failures should use a stable sanitized message");
    expect(sanitized.find("LibreSSL") == std::string::npos,
           "sanitized message must not contain LibreSSL");
    expect(sanitized.find("SSL_read") == std::string::npos,
           "sanitized message must not contain SSL_read");
    expect(sanitized.find("bad record mac") == std::string::npos,
           "sanitized message must not contain raw TLS alert text");

    const std::string plain = httpTransportFailureMessage("Connection reset by peer");
    expect(plain == "HTTP request failed: Connection reset by peer",
           "non-TLS transport details should remain visible");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "http-transient-tls-test: ok\n";
    return 0;
}
