#!/bin/sh

tool_home=$TEST_WORKDIR/tool-home
write_test_credentials "$tool_home" || exit 1
case_cwd=$TEST_PROJECT

expect_process "T3.1: a model tool call mutates the working tree and returns its result" 0 \
    run_with_mock tool-write env CODEX_HOME="$tool_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Create the requested file <<'STDOUT' 3<<'STDERR'
Created result.txt
STDOUT

[tool write] {"path":"result.txt","content":"made by tool\n"}
[tool write completed] Created result.txt
STDERR

expect_process "T3.2: the tool changed the real file on disk" 0 \
    /bin/cat result.txt <<'STDOUT' 3<<'STDERR'
made by tool
STDOUT
STDERR

printf 'line one\nline two\nline three\n' > "$TEST_PROJECT/edit-target.txt"

expect_process "T3.3: an edit tool call returns its compact model output" 0 \
    run_with_mock tool-edit env CODEX_HOME="$tool_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Edit the requested file <<'STDOUT' 3<<'STDERR'
Edited edit-target.txt
STDOUT

[tool edit] {"path":"edit-target.txt","old_content":"line two","new_content":"line two changed","replace_all":false}
[tool edit completed] Edited edit-target.txt
STDERR

expect_process "T3.4: the edit changed the real file on disk" 0 \
    /bin/cat edit-target.txt <<'STDOUT' 3<<'STDERR'
line one
line two changed
line three
STDOUT
STDERR
