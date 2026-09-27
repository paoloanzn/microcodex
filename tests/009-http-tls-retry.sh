#!/bin/sh

# Unit checks for LibreSSL/TLS error classification and sanitization.
tls_unit=$TEST_WORKDIR/http-transient-tls-test
printf '%s\n' "building http-transient-tls-test"
(cd "$ROOT_DIR" && c++ -std=c++23 -O2 -Wall -Wextra -pthread \
    -I. -o "$tls_unit" tests/http-transient-tls-test.cpp http.cpp -lcurl -pthread) || exit 1

expect_process "T9.1: TLS error classifier and sanitizer" 0 \
    "$tls_unit" <<'STDOUT' 3<<'STDERR'
http-transient-tls-test: ok
STDOUT
STDERR

tls_home=$TEST_WORKDIR/tls-retry-home
write_test_credentials "$tls_home" || exit 1
mkdir -p "$tls_home/skills/test-skill" || exit 1
cat > "$tls_home/skills/test-skill/SKILL.md" <<'EOF_SKILL'
---
name: test-skill
description: Test shared Codex skill discovery.
---

# Test skill
EOF_SKILL

attempt_log=$TEST_WORKDIR/http-attempt.log
rm -f "$attempt_log"

# Inject two synthetic LibreSSL SSL_read failures across the process, then allow
# real requests. Success proves performHttpRequest retried with backoff.
expect_process "T9.2: transient TLS failures are retried until the request succeeds" 0 \
    run_with_mock text \
    env MICROCODEX_TEST_TRANSIENT_TLS_FAILURES=2 \
        MICROCODEX_TEST_HTTP_ATTEMPT_LOG="$attempt_log" \
        CODEX_HOME="$tls_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex --model test-model Say hello from two arguments <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
STDERR

expect_process "T9.3: retry attempts used backoff" 0 \
    sh -c 'grep -q "attempt 2" "$1" && grep -q "retry-backoff-ms" "$1"' \
    sh "$attempt_log" <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR

# Exhaust retries with only synthetic TLS failures against a loopback endpoint.
# Raw LibreSSL text must not appear; the stable sanitized message should.
expect_process "T9.4: exhausted TLS retries surface a sanitized error" 1 \
    env MICROCODEX_TEST_TRANSIENT_TLS_FAILURES=20 \
        MICROCODEX_API_ENDPOINT="http://127.0.0.1:1/responses" \
        CODEX_HOME="$tls_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex --model test-model trigger-tls-exhaustion <<'STDOUT' 3<<'STDERR'
STDOUT
Warning: Could not retrieve model context limits: HTTP request failed: transient TLS connection error. Using built-in context limits.
Agent failed: HTTP request failed: transient TLS connection error
STDERR
