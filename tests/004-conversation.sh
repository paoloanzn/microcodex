#!/bin/sh

conversation_home=$TEST_WORKDIR/conversation-home
write_test_credentials "$conversation_home" || exit 1

expect_process "T4.1: a completed prompt is saved as a conversation" 0 \
    run_with_mock conversation-first env CODEX_HOME="$conversation_home" \
        PATH="$TEST_BIN_DIR:$PATH" microcodex --model test-model Remember alpha \
        <<'STDOUT' 3<<'STDERR'
Alpha stored
STDOUT
STDERR

set -- "$conversation_home"/conversations/*.jsonl
[ "$#" -eq 1 ] && [ -f "$1" ] || exit 1
conversation_id=$(basename "$1" .jsonl)

expect_process "T4.2: show walks the complete saved transcript" 0 \
    env CODEX_HOME="$conversation_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex show "$conversation_id" <<'STDOUT' 3<<'STDERR'
user: Remember alpha
assistant: Alpha stored
STDOUT
STDERR

# Simulate a process dying midway through its next JSONL append. A writable
# resume must discard this tail before committing another complete turn.
printf '%s' '{"type":"turn","number":2' >> "$1" || exit 1

expect_process "T4.3: resume replays exact saved items and commits another turn" 0 \
    run_with_mock conversation-resume env CODEX_HOME="$conversation_home" \
        PATH="$TEST_BIN_DIR:$PATH" microcodex resume "$conversation_id" \
        Recall beta <<'STDOUT' 3<<'STDERR'
Alpha and beta recalled
STDOUT
STDERR

expect_process "T4.4: show remains complete after resume" 0 \
    env CODEX_HOME="$conversation_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex show "$conversation_id" <<'STDOUT' 3<<'STDERR'
user: Remember alpha
assistant: Alpha stored
user: Recall beta
assistant: Alpha and beta recalled
STDOUT
STDERR

expect_process "T4.5: list reads conversation metadata without login" 0 \
    sh -c 'CODEX_HOME="$1" PATH="$2:$PATH" microcodex list | cut -f1,3' \
        sh "$conversation_home" "$TEST_BIN_DIR" <<STDOUT 3<<'STDERR'
$conversation_id	test-model
STDOUT
STDERR

compaction_home=$TEST_WORKDIR/compaction-home
write_test_credentials "$compaction_home" || exit 1

expect_process "T4.6: create context that a later process will compact" 0 \
    run_with_mock compaction-seed env CODEX_HOME="$compaction_home" \
        PATH="$TEST_BIN_DIR:$PATH" microcodex --model test-model \
        Seed compactable context <<'STDOUT' 3<<'STDERR'
Seed response
STDOUT
STDERR

set -- "$compaction_home"/conversations/*.jsonl
[ "$#" -eq 1 ] && [ -f "$1" ] || exit 1
compaction_id=$(basename "$1" .jsonl)

expect_process "T4.7: resume compacts old turns before sending the new prompt" 0 \
    run_with_mock compaction-resume env CODEX_HOME="$compaction_home" \
        MICROCODEX_RETAINED_CONTEXT_TOKENS=1 \
        PATH="$TEST_BIN_DIR:$PATH" microcodex resume "$compaction_id" \
        Continue after compaction <<'STDOUT' 3<<'STDERR'
Continued from compacted state
STDOUT
STDERR

expect_process "T4.8: compaction never removes turns from the durable transcript" 0 \
    env CODEX_HOME="$compaction_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex show "$compaction_id" <<'STDOUT' 3<<'STDERR'
user: Seed compactable context
assistant: Seed response
user: Continue after compaction
assistant: Continued from compacted state
STDOUT
STDERR

context_error_home=$TEST_WORKDIR/context-error-home
write_test_credentials "$context_error_home" || exit 1

expect_process "T4.9: create history for context-limit recovery" 0 \
    run_with_mock context-error-seed env CODEX_HOME="$context_error_home" \
        PATH="$TEST_BIN_DIR:$PATH" microcodex --model test-model \
        Seed context error recovery <<'STDOUT' 3<<'STDERR'
Context error seed response
STDOUT
STDERR

set -- "$context_error_home"/conversations/*.jsonl
[ "$#" -eq 1 ] && [ -f "$1" ] || exit 1
context_error_id=$(basename "$1" .jsonl)

expect_process "T4.10: a provider context error compacts and retries once" 0 \
    run_with_mock context-error-retry env CODEX_HOME="$context_error_home" \
        PATH="$TEST_BIN_DIR:$PATH" microcodex resume "$context_error_id" \
        Recover from context error <<'STDOUT' 3<<'STDERR'
Recovered after retry
STDOUT
STDERR
