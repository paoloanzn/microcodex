#!/bin/sh

tool_home=$TEST_WORKDIR/tool-home
write_test_credentials "$tool_home" || exit 1

expect_process "T9.1: a sub-agent tool call returns the child response" 0 \
    run_with_mock sub-agent env CODEX_HOME="$tool_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Ask a child agent <<'STDOUT' 3<<'STDERR'
Parent received child result
STDOUT

[tool sub_agent] {"prompt":"Child task","timeout_ms":1000}
[tool sub_agent completed] Child result
STDERR
