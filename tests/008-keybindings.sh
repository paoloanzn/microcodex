#!/bin/sh

keybindings_home=$TEST_WORKDIR/keybindings-home
write_test_credentials "$keybindings_home" || exit 1

expect_process "T8.1: composer keybindings navigate lines and delete words" 0 \
    run_with_mock keybindings env CODEX_HOME="$keybindings_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/keybindings-ui.rb" microcodex <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR
