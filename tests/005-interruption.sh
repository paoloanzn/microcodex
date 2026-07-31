#!/bin/sh

interrupt_home=$TEST_WORKDIR/interrupt-home
write_test_credentials "$interrupt_home" || exit 1

# expect_process increments tests_run before run_with_mock allocates its fixture.
mock_number=$((tests_run + 2))
mock_dir=$TEST_WORKDIR/mock-$mock_number
expect_process "T5.1: continuing after output interruption retains partial assistant context" 0 \
    run_with_mock interrupt-output env CODEX_HOME="$interrupt_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/interrupt-ui.rb" microcodex "Start interrupt test" \
        "$mock_dir/interrupt-ready" "$mock_dir/continued" <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR

tool_interrupt_home=$TEST_WORKDIR/tool-interrupt-home
write_test_credentials "$tool_interrupt_home" || exit 1
mock_number=$((tests_run + 2))
mock_dir=$TEST_WORKDIR/mock-$mock_number
expect_process "T5.2: continuing after tool interruption retains the call and cancellation output" 0 \
    run_with_mock interrupt-tool env CODEX_HOME="$tool_interrupt_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/interrupt-ui.rb" microcodex "Start interrupted tool test" \
        "$mock_dir/interrupt-ready" "$mock_dir/continued" <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR
