#!/bin/sh

queue_home=$TEST_WORKDIR/queue-home
write_test_credentials "$queue_home" || exit 1

# expect_process increments tests_run before run_with_mock allocates its fixture.
mock_number=$((tests_run + 2))
mock_dir=$TEST_WORKDIR/mock-$mock_number
expect_process "T9.1: messages typed mid-turn are queued and processed in order after the turn completes" 0 \
    run_with_mock message-queue env CODEX_HOME="$queue_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/queue-ui.rb" microcodex "Start message queue test" \
        "$mock_dir/first-request" "$mock_dir/queued-done" <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR

# T9.2: quitting while a turn is executing discards queued messages instead
# of sending them. The driver queues one message mid-turn and quits; the
# mock serves exactly one request in this scenario, so a drained queue
# would leave a second request file behind.
quit_home=$TEST_WORKDIR/quit-home
write_test_credentials "$quit_home" || exit 1

mock_number=$((tests_run + 2))
mock_dir=$TEST_WORKDIR/mock-$mock_number
expect_process "T9.2: quitting mid-turn discards queued messages" 0 \
    run_with_mock message-queue-quit env CODEX_HOME="$quit_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/queue-ui.rb" microcodex "Start message queue test" \
        "$mock_dir/first-request" "$mock_dir/queued-done" quit <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR
tests_run=$((tests_run + 1))
if [ -e "$mock_dir/request-2.txt" ]; then
    tests_failed=$((tests_failed + 1))
    printf 'not ok %03d - %s\n' "$tests_run" "T9.2: a second request was sent after quit"
else
    printf 'ok %03d - %s\n' "$tests_run" "T9.2: no request was sent after quit"
fi
