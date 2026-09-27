#!/bin/sh

paste_home=$TEST_WORKDIR/invisible-paste-home
write_test_credentials "$paste_home" || exit 1

expect_process "T10.1: invisible pasted characters stay out of terminal rendering" 0 \
    run_with_mock invisible-paste env CODEX_HOME="$paste_home" PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/invisible-paste-ui.rb" microcodex <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR

expect_process "T10.2: editing a paste label removes the whole paste" 0 \
    run_with_mock invisible-paste-edited env CODEX_HOME="$paste_home" PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/invisible-paste-ui.rb" microcodex edit <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR
