#!/bin/sh

prompt_home=$TEST_WORKDIR/prompt-home
write_test_credentials "$prompt_home" || exit 1
mkdir -p "$prompt_home/skills/test-skill" || exit 1
cat > "$prompt_home/skills/test-skill/SKILL.md" <<'EOF'
---
name: test-skill
description: Test shared Codex skill discovery.
---

# Test skill
EOF

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

expect_process "T2.3: no --model sends the GPT-6 default" 0 \
    run_with_mock default-model env CODEX_HOME="$prompt_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Say hello with the default model <<'STDOUT' 3<<'STDERR'
Hello from the default model!
STDOUT
STDERR

expect_process "T2.4: --effort high reaches the request body" 0 \
    run_with_mock effort env CODEX_HOME="$prompt_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex --effort high Say hello with high effort <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
STDERR

expect_process "T2.5: MICROCODEX_EFFORT reaches the request body" 0 \
    run_with_mock effort-env env CODEX_HOME="$prompt_home" MICROCODEX_EFFORT=low PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Say hello with env effort <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
STDERR

expect_process "T2.6: --effort persistent sends the Responses API 'disabled' value" 0 \
    run_with_mock effort-persistent env CODEX_HOME="$prompt_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex --effort persistent Say hello with persistent effort <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
STDERR

expect_process "T2.7: an invalid MICROCODEX_EFFORT fails with a clear error" 1 \
    env CODEX_HOME="$prompt_home" MICROCODEX_EFFORT=turbo PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Say hello <<'STDOUT' 3<<'STDERR'
STDOUT
MICROCODEX_EFFORT: unsupported reasoning effort 'turbo' (expected one of: none, minimal, low, medium, high, xhigh, max, ultra, persistent)
STDERR

expect_process "T2.8: an empty MICROCODEX_EFFORT fails with a clear error" 1 \
    env CODEX_HOME="$prompt_home" MICROCODEX_EFFORT= PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Say hello <<'STDOUT' 3<<'STDERR'
STDOUT
MICROCODEX_EFFORT requires an effort value (expected one of: none, minimal, low, medium, high, xhigh, max, ultra, persistent)
STDERR

expect_process "T2.9: --effort wins over MICROCODEX_EFFORT" 0 \
    run_with_mock effort env CODEX_HOME="$prompt_home" MICROCODEX_EFFORT=low PATH="$TEST_BIN_DIR:$PATH" \
        microcodex --effort high Say hello with high effort <<'STDOUT' 3<<'STDERR'
Hello, world!
STDOUT
STDERR
