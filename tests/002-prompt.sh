#!/bin/sh

prompt_home=$TEST_WORKDIR/prompt-home
write_test_credentials "$prompt_home" || exit 1

expect_process "T2.1: prompt arguments and model produce streamed text" 0 \
    run_with_mock text env CODEX_HOME="$prompt_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex --model test-model Say hello from two arguments <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
STDERR

expect_process "T2.2: API errors reach stderr and fail the command" 1 \
    run_with_mock http-error env CODEX_HOME="$prompt_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex trigger-error <<'STDOUT' 3<<'STDERR'
STDOUT
Agent failed: Codex API returned HTTP 429: rate limited
STDERR
