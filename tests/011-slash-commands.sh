#!/bin/sh
# shellcheck disable=SC2154 # run.sh sets tests_run before sourcing this file.

slash_home=$TEST_WORKDIR/slash-home
write_test_credentials "$slash_home" || exit 1

# expect_process increments tests_run before run_with_mock allocates its fixture.
mock_number=$((tests_run + 2))
mock_dir=$TEST_WORKDIR/mock-$mock_number
expect_process "T11.1: slash commands change the effort and the model, reset, refuse during a turn and quit" 0 \
    run_with_mock slash-commands env CODEX_HOME="$slash_home" PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/slash-commands-ui.rb" microcodex "$mock_dir" <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR

# /model ran after the first conversation's two turns, so only that file has
# a model record. The conversation /new started was created with the new model.
# Two matching files would give two lines, which is not a file.
switched_file=$(grep -l '"type":"model"' "$slash_home"/conversations/*.jsonl)
[ -f "$switched_file" ] || exit 1
switched_id=$(basename "$switched_file" .jsonl)

expect_process "T11.2: resume continues with the model chosen with /model" 0 \
    run_with_mock model-resume env CODEX_HOME="$slash_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex resume "$switched_id" After the switch <<'STDOUT' 3<<'STDERR'
Resumed with the new model
STDOUT
STDERR

expect_process "T11.3: resuming with the saved model adds no model record" 0 \
    grep -c '"type":"model"' "$switched_file" <<'STDOUT' 3<<'STDERR'
1
STDOUT
STDERR
