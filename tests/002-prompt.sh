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

expect_process "T2.10: --effort ultra sends the model wire top 'max'" 0 \
    run_with_mock effort-ultra env CODEX_HOME="$prompt_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex --effort ultra Say hello with ultra effort <<'STDOUT' 3<<'STDERR'
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

agents_home=$TEST_WORKDIR/agents-home
write_test_credentials "$agents_home" || exit 1
printf '\n  Answer tersely.\n\n' > "$agents_home/AGENTS.md" || exit 1

expect_process "T2.11: the global AGENTS.md is sent before the prompt" 0 \
    run_with_mock agents-md env CODEX_HOME="$agents_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Follow the global instructions <<'STDOUT' 3<<'STDERR'
Instructions received
STDOUT
STDERR

set -- "$agents_home"/conversations/*.jsonl
[ "$#" -eq 1 ] && [ -f "$1" ] || exit 1

expect_process "T2.12: the global AGENTS.md is not saved with the conversation" 0 \
    env CODEX_HOME="$agents_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex show "$(basename "$1" .jsonl)" <<'STDOUT' 3<<'STDERR'
user: Follow the global instructions
assistant: Instructions received
STDOUT
STDERR

override_home=$TEST_WORKDIR/override-home
write_test_credentials "$override_home" || exit 1
printf 'Ignored global rule.\n' > "$override_home/AGENTS.md" || exit 1
{ printf 'Override rule.\n' && head -c 39985 /dev/zero | tr '\0' x; } > "$override_home/AGENTS.override.md" || exit 1

expect_process "T2.13: AGENTS.override.md wins and is cut at 32 KiB with a note" 0 \
    run_with_mock agents-md-override env CODEX_HOME="$override_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Follow the override <<'STDOUT' 3<<'STDERR'
Instructions received
STDOUT
STDERR
