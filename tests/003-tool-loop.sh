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
