#!/bin/sh

# An unsigned JWT whose payload is {"exp":1000000000} (2001-09-09), so it is
# always past its expiry and must trigger a proactive refresh.
expired_jwt=eyJhbGciOiJub25lIn0.eyJleHAiOjEwMDAwMDAwMDB9.c2ln

# expect_process only compares process results; these cases also assert that
# the refreshed token was persisted back to auth.json.
check_saved_token() {
    desc=$1
    file=$2
    tests_run=$((tests_run + 1))
    if grep -q '"access_token": "refreshed-access-token"' "$file"; then
        printf 'ok %03d - %s\n' "$tests_run" "$desc"
    else
        tests_failed=$((tests_failed + 1))
        printf 'not ok %03d - %s\n' "$tests_run" "$desc"
    fi
}

# T9.1: the OPENAI_API_KEY environment variable authenticates the request when
# no auth.json exists. MICROCODEX_TEST_BEARER is exported (not passed through
# `env`) because the mock server process starts before the CLI and only sees
# the exported environment.
key_home=$TEST_WORKDIR/key-home
mkdir -p "$key_home" || exit 1
export MICROCODEX_TEST_BEARER="env-api-key"

expect_process "T9.1: OPENAI_API_KEY environment variable authenticates the request" 0 \
    run_with_mock api-key env CODEX_HOME="$key_home" \
        OPENAI_API_KEY="env-api-key" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Api key prompt <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
STDERR
unset MICROCODEX_TEST_BEARER

# T9.2: an OPENAI_API_KEY stored in auth.json is used when the variable is
# unset, taking precedence over the OAuth token set in the same file.
file_home=$TEST_WORKDIR/file-key-home
mkdir -p "$file_home" || exit 1
cat > "$file_home/auth.json" <<'EOF'
{
  "auth_mode": "apikey",
  "OPENAI_API_KEY": "file-api-key",
  "tokens": {
    "id_token": "test-id-token",
    "access_token": "test-access-token",
    "refresh_token": "test-refresh-token",
    "account_id": "test-account"
  }
}
EOF
chmod 600 "$file_home/auth.json"
export MICROCODEX_TEST_BEARER="file-api-key"

expect_process "T9.2: auth.json OPENAI_API_KEY authenticates the request" 0 \
    run_with_mock api-key env -u OPENAI_API_KEY CODEX_HOME="$file_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Api key prompt <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
STDERR
unset MICROCODEX_TEST_BEARER

# T9.3: an HTTP 401 refreshes the OAuth token and retries the request with the
# new token; the refreshed token is saved back to auth.json.
refresh_home=$TEST_WORKDIR/refresh-home
write_test_credentials "$refresh_home" || exit 1

expect_process "T9.3: HTTP 401 refreshes the token and retries the request" 0 \
    run_with_mock token-refresh-401 env -u OPENAI_API_KEY CODEX_HOME="$refresh_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Refresh the token <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
STDERR
check_saved_token "T9.3: refreshed token is persisted to auth.json" "$refresh_home/auth.json"

# T9.4: an access token that is already past its JWT expiry is refreshed
# before the first request goes out; the model catalog and the turn both use
# the new token, which is saved back to auth.json.
expired_home=$TEST_WORKDIR/expired-home
mkdir -p "$expired_home" || exit 1
cat > "$expired_home/auth.json" <<EOF
{
  "auth_mode": "chatgpt",
  "tokens": {
    "id_token": "test-id-token",
    "access_token": "$expired_jwt",
    "refresh_token": "test-refresh-token",
    "account_id": "test-account"
  }
}
EOF
chmod 600 "$expired_home/auth.json"

expect_process "T9.4: expired access token is refreshed before the request" 0 \
    run_with_mock token-refresh-expired env -u OPENAI_API_KEY CODEX_HOME="$expired_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Refresh the token <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
STDERR
check_saved_token "T9.4: refreshed token is persisted to auth.json" "$expired_home/auth.json"

# T9.5: when the proactive refresh fails (the refresh endpoint is down), the
# turn still goes out with the stored token instead of failing. The stored
# token is left untouched in auth.json; the startup warning is the only trace
# of the failed refresh.
fallback_home=$TEST_WORKDIR/fallback-home
mkdir -p "$fallback_home" || exit 1
cat > "$fallback_home/auth.json" <<EOF
{
  "auth_mode": "chatgpt",
  "tokens": {
    "id_token": "test-id-token",
    "access_token": "$expired_jwt",
    "refresh_token": "test-refresh-token",
    "account_id": "test-account"
  }
}
EOF
chmod 600 "$fallback_home/auth.json"

expect_process "T9.5: failed proactive refresh falls back to the stored token" 0 \
    run_with_mock token-refresh-fallback env -u OPENAI_API_KEY CODEX_HOME="$fallback_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Fallback after failed refresh <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
Warning: OAuth token endpoint returned HTTP 400: refresh_failed
STDERR
tests_run=$((tests_run + 1))
if grep -q '"access_token": "refreshed-access-token"' "$fallback_home/auth.json"; then
    tests_failed=$((tests_failed + 1))
    printf 'not ok %03d - %s\n' "$tests_run" "T9.5: auth.json was overwritten despite the failed refresh"
else
    printf 'ok %03d - %s\n' "$tests_run" "T9.5: auth.json keeps the stored token after a failed refresh"
fi

# T9.6: a failed proactive refresh is not retried on later requests. The
# session performs two model requests (the first triggers a tool call); the
# mock asserts the token endpoint saw exactly two attempts (startup + first
# turn) rather than three.
skip_home=$TEST_WORKDIR/skip-home
mkdir -p "$skip_home" || exit 1
cat > "$skip_home/auth.json" <<EOF
{
  "auth_mode": "chatgpt",
  "tokens": {
    "id_token": "test-id-token",
    "access_token": "$expired_jwt",
    "refresh_token": "test-refresh-token",
    "account_id": "test-account"
  }
}
EOF
chmod 600 "$skip_home/auth.json"

skip_cwd=$case_cwd
case_cwd=$TEST_PROJECT
# 003-tool-loop.sh already created result.txt via the same tool call; the
# write tool refuses to overwrite, so start from a clean slate.
rm -f "$TEST_PROJECT/result.txt"
expect_process "T9.6: failed proactive refresh is not retried on later requests" 0 \
    run_with_mock token-refresh-fallback-skip env -u OPENAI_API_KEY CODEX_HOME="$skip_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Trigger a tool call after failed refresh <<'STDOUT' 3<<'STDERR'
Created result.txt
STDOUT
Warning: OAuth token endpoint returned HTTP 400: refresh_failed

[tool write] {"path":"result.txt","content":"made by tool\n"}
[tool write completed] Created result.txt
STDERR
case_cwd=$skip_cwd

# T9.7: when persisting the refreshed token fails, the turn still goes out
# with the refreshed token instead of discarding it. The failure is injected
# through the deterministic MICROCODEX_TEST_FAIL_CREDENTIAL_SAVE seam in
# oauth.cpp: an LD_PRELOAD/DYLD_INSERT_LIBRARIES open() interposition shim
# cannot reliably intercept the file-open entry point on Darwin, so the
# forced failure would silently never happen there. auth.json must keep the
# stale token afterwards.
savefail_home=$TEST_WORKDIR/savefail-home
mkdir -p "$savefail_home" || exit 1
cat > "$savefail_home/auth.json" <<EOF
{
  "auth_mode": "chatgpt",
  "tokens": {
    "id_token": "test-id-token",
    "access_token": "$expired_jwt",
    "refresh_token": "test-refresh-token",
    "account_id": "test-account"
  }
}
EOF
chmod 600 "$savefail_home/auth.json"

expect_process "T9.7: failed credential save keeps the refreshed token for the session" 0 \
    run_with_mock token-refresh-save-fail env -u OPENAI_API_KEY CODEX_HOME="$savefail_home" \
        MICROCODEX_TEST_FAIL_CREDENTIAL_SAVE=1 PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Refresh despite an unwritable credential file <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
Warning: Could not create temporary credentials file: Permission denied; using the refreshed token for this session only
STDERR
tests_run=$((tests_run + 1))
if grep -q "\"access_token\": \"$expired_jwt\"" "$savefail_home/auth.json"; then
    printf 'ok %03d - %s\n' "$tests_run" "T9.7: auth.json keeps the stale token after the failed save"
else
    tests_failed=$((tests_failed + 1))
    printf 'not ok %03d - %s\n' "$tests_run" "T9.7: auth.json was modified despite the failed save"
fi

# T9.8: when the proactive refresh fails on a request and that same request
# returns 401, the client must NOT start a second refresh for the same
# request: the issuer just proved itself unreachable, so another attempt
# would only wait out a second timeout. The 401 surfaces directly, and the
# mock asserts the token endpoint saw exactly two attempts (startup +
# proactive) rather than three.
fail401_home=$TEST_WORKDIR/fail401-home
mkdir -p "$fail401_home" || exit 1
cat > "$fail401_home/auth.json" <<EOF
{
  "auth_mode": "chatgpt",
  "tokens": {
    "id_token": "test-id-token",
    "access_token": "$expired_jwt",
    "refresh_token": "test-refresh-token",
    "account_id": "test-account"
  }
}
EOF
chmod 600 "$fail401_home/auth.json"

expect_process "T9.8: 401 after a failed proactive refresh does not refresh again" 1 \
    run_with_mock token-refresh-401-after-failed-proactive env -u OPENAI_API_KEY CODEX_HOME="$fail401_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Refresh fails then the request is unauthorized <<'STDOUT' 3<<'STDERR'
STDOUT
Warning: OAuth token endpoint returned HTTP 400: refresh_failed
Agent failed: Codex API returned HTTP 401: token expired
STDERR

# T9.9: a plaintext non-loopback OAuth issuer is rejected before any token
# grant is attempted, so the refresh token never travels over plaintext HTTP.
# The loopback mock issuer used by every other test stays allowed.
badissuer_home=$TEST_WORKDIR/badissuer-home
mkdir -p "$badissuer_home" || exit 1
cat > "$badissuer_home/auth.json" <<EOF2
{
  "auth_mode": "chatgpt",
  "tokens": {
    "id_token": "test-id-token",
    "access_token": "$expired_jwt",
    "refresh_token": "test-refresh-token",
    "account_id": "test-account"
  }
}
EOF2
chmod 600 "$badissuer_home/auth.json"

expect_process "T9.9: plaintext non-loopback OAuth issuer is rejected" 0 \
    run_with_mock token-refresh-fallback env -u OPENAI_API_KEY CODEX_HOME="$badissuer_home" \
        MICROCODEX_OAUTH_ISSUER=http://example.com/ \
        PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Fallback after failed refresh <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
Warning: OAuth issuer must use HTTPS unless it is a loopback address
STDERR

# T9.10: userinfo in a plaintext issuer cannot smuggle a remote host past the
# loopback check. "http://127.0.0.1:8080@issuer.example" parses as host
# issuer.example with userinfo "127.0.0.1:8080", so it must be rejected before
# any token grant is attempted.
userinfo_home=$TEST_WORKDIR/userinfo-home
mkdir -p "$userinfo_home" || exit 1
cat > "$userinfo_home/auth.json" <<EOF2
{
  "auth_mode": "chatgpt",
  "tokens": {
    "id_token": "test-id-token",
    "access_token": "$expired_jwt",
    "refresh_token": "test-refresh-token",
    "account_id": "test-account"
  }
}
EOF2
chmod 600 "$userinfo_home/auth.json"

expect_process "T9.10: plaintext issuer with userinfo is rejected" 0 \
    run_with_mock token-refresh-fallback env -u OPENAI_API_KEY CODEX_HOME="$userinfo_home" \
        MICROCODEX_OAUTH_ISSUER='http://127.0.0.1:8080@issuer.example' \
        PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Fallback after failed refresh <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
Warning: OAuth issuer must not contain userinfo
STDERR
