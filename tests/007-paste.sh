#!/bin/sh

paste_home=$TEST_WORKDIR/paste-home
write_test_credentials "$paste_home" || exit 1

expect_process "T7.1: multiline pastes stay in the composer and large pastes render compactly" 0 \
    run_with_mock paste env CODEX_HOME="$paste_home" PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/paste-ui.rb" microcodex <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR
