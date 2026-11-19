#!/bin/sh

# Remote/transport failures are resumable, never retried: the turn keeps its
# partial output, gains a <turn_aborted> marker, is saved, and the user can
# continue from there with "continue". 4xx responses and terminal stream
# verdicts stay transactional.

remote_home=$TEST_WORKDIR/remote-home
write_test_credentials "$remote_home" || exit 1

expect_process "T9.1: an HTTP 503 aborts the turn instead of retrying" 1 \
    run_with_mock remote-503 env CODEX_HOME="$remote_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex --model test-model Trigger remote failure <<'STDOUT' 3<<'STDERR'
STDOUT
Agent failed: Codex API returned HTTP 503: service unavailable
STDERR

set -- "$remote_home"/conversations/*.jsonl
[ "$#" -eq 1 ] && [ -f "$1" ] || exit 1
remote_id=$(basename "$1" .jsonl)

expect_process "T9.2: the aborted 503 turn is saved with its marker" 0 \
    grep -q "<turn_aborted>" "$remote_home"/conversations/*.jsonl <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR

expect_process "T9.3: continuing after the 503 resumes from the aborted turn" 0 \
    run_with_mock remote-continue env CODEX_HOME="$remote_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex resume "$remote_id" continue <<'STDOUT' 3<<'STDERR'
Continued after remote failure
STDOUT
STDERR

drop_home=$TEST_WORKDIR/drop-home
write_test_credentials "$drop_home" || exit 1

expect_process "T9.4: a mid-stream transport drop aborts the turn without retrying" 1 \
    run_with_mock stream-drop env CODEX_HOME="$drop_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex --model test-model Survive a mid-stream drop <<'STDOUT' 3<<'STDERR'
Partial then drop
STDOUT
Agent failed: HTTP request failed: transfer closed with 512 bytes remaining to read
STDERR

set -- "$drop_home"/conversations/*.jsonl
[ "$#" -eq 1 ] && [ -f "$1" ] || exit 1
drop_id=$(basename "$1" .jsonl)

# shellcheck disable=SC2016 # The inner sh -c expands its own arguments.
expect_process "T9.5: the dropped turn keeps its partial text and marker" 0 \
    sh -c 'grep -q "Partial then drop" "$0"/conversations/*.jsonl && grep -q "<turn_aborted>" "$0"/conversations/*.jsonl' "$drop_home" <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR

expect_process "T9.6: continuing after the drop resumes from the partial text" 0 \
    run_with_mock remote-continue env CODEX_HOME="$drop_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex resume "$drop_id" continue <<'STDOUT' 3<<'STDERR'
Continued after remote failure
STDOUT
STDERR

stream_home=$TEST_WORKDIR/stream-home
write_test_credentials "$stream_home" || exit 1

expect_process "T9.7: terminal stream verdicts stay transactional" 1 \
    run_with_mock stream-error env CODEX_HOME="$stream_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex --model test-model Trigger stream failure <<'STDOUT' 3<<'STDERR'
doomed
STDOUT
Agent failed: boom
STDERR

# shellcheck disable=SC2016 # The inner sh -c expands its own arguments.
expect_process "T9.8: the transactional stream failure saves no aborted turn" 0 \
    sh -c '! grep -q "<turn_aborted>" "$0"/conversations/*.jsonl' "$stream_home" <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR
