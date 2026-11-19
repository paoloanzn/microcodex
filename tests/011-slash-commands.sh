#!/bin/sh
# shellcheck disable=SC2154 # run.sh sets tests_run before sourcing this file.

slash_home=$TEST_WORKDIR/slash-home
write_test_credentials "$slash_home" || exit 1

# expect_process increments tests_run before run_with_mock allocates its fixture.
mock_number=$((tests_run + 2))
mock_dir=$TEST_WORKDIR/mock-$mock_number
expect_process "T11.1: slash commands change the effort, reset, refuse during a turn and quit" 0 \
    run_with_mock slash-commands env CODEX_HOME="$slash_home" PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/slash-commands-ui.rb" microcodex "$mock_dir" <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR
