#!/bin/sh

tool_home=$TEST_WORKDIR/tool-home
write_test_credentials "$tool_home" || exit 1
# shellcheck disable=SC2034 # run.sh runs each case from case_cwd.
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

expect_process "T3.3: edit defaults replace_all to false when omitted" 0 \
    run_with_mock tool-edit env CODEX_HOME="$tool_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Edit the requested file <<'STDOUT' 3<<'STDERR'
Edited edit-target.txt
STDOUT

[tool edit] {"path":"edit-target.txt","old_content":"line two","new_content":"line two changed"}
[tool edit completed] Edited edit-target.txt
STDERR

expect_process "T3.4: the edit changed the real file on disk" 0 \
    /bin/cat edit-target.txt <<'STDOUT' 3<<'STDERR'
line one
line two changed
line three
STDOUT
STDERR

shell_home=$TEST_WORKDIR/shell-home
mkdir -p "$shell_home" || exit 1
printf '%s\n' 'export MICROCODEX_RC_VALUE=loaded-from-bashrc' > "$shell_home/.bashrc" || exit 1

expect_process "T3.5: bash restores exports from the user's interactive shell rc" 0 \
    run_with_mock tool-shell-env env HOME="$shell_home" SHELL=/bin/bash \
        CODEX_HOME="$tool_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Read the shell environment <<'STDOUT' 3<<'STDERR'
Shell environment loaded
STDOUT

[tool bash] {"command":"printf '%s' \"$MICROCODEX_RC_VALUE\""}
[tool bash completed] {"stdout":"loaded-from-bashrc","stderr":"","exit_code":0}
STDERR

expect_process "T3.6: bash rejects a destructive command before execution" 0 \
    run_with_mock tool-bash-denied env CODEX_HOME="$tool_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Try a denied shell command <<'STDOUT' 3<<'STDERR'
Dangerous command blocked
STDOUT

[tool bash] {"command":"rm -rf denylist-sentinel"}
[tool bash failed] Error: command denied: forced file removal is blocked
STDERR

expect_process "T3.7: a stuck tool call times out instead of hanging the turn" 0 \
    run_with_mock tool-timeout env MICROCODEX_TOOL_EXECUTION_TIMEOUT_SECONDS=2 \
        CODEX_HOME="$tool_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Run a slow shell command <<'STDOUT' 3<<'STDERR'
Slow command timed out
STDOUT

[tool bash] {"command":"sleep 30"}
[tool bash failed] Error: Tool execution timed out after 2 seconds
STDERR

# Fails a case whose command outlives LIMIT seconds, so timing regressions show
# up even when the output is correct.
run_within() {
    limit=$1
    shift
    started=$(date +%s)
    "$@"
    status=$?
    elapsed=$(($(date +%s) - started))
    if [ "$elapsed" -gt "$limit" ]; then
        printf 'took %ss, limit %ss\n' "$elapsed" "$limit" >&2
        return 97
    fi
    return "$status"
}

for fifo in 1 2 3 4; do
    mkfifo "$TEST_PROJECT/fifo-$fifo" || exit 1
done

# Four reads that ignore their stop token time out together after 1s. They share
# one 2s grace period, so the turn finishes in about 3s; a grace period per call
# would take about 9s.
expect_process "T3.8: stuck tools that ignore stop share one grace period" 0 \
    run_within 6 run_with_mock tool-timeout-batch env MICROCODEX_TOOL_EXECUTION_TIMEOUT_SECONDS=1 \
        CODEX_HOME="$tool_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Read the blocked pipes <<'STDOUT' 3<<'STDERR'
Blocked reads timed out
STDOUT

[tool read] {"path":"fifo-1","offset":0,"limit":16}

[tool read] {"path":"fifo-2","offset":0,"limit":16}

[tool read] {"path":"fifo-3","offset":0,"limit":16}

[tool read] {"path":"fifo-4","offset":0,"limit":16}
[tool read failed] Error: Tool execution timed out after 1 seconds
[tool read failed] Error: Tool execution timed out after 1 seconds
[tool read failed] Error: Tool execution timed out after 1 seconds
[tool read failed] Error: Tool execution timed out after 1 seconds
STDERR

rm -f "$TEST_PROJECT"/fifo-[1-4]

# The first call sleeps, so the second one finishes first and is reported
# first. The model still receives both outputs in call order.
expect_process "T3.9: a tool call is reported as soon as it finishes" 0 \
    run_with_mock tool-finish-order env CODEX_HOME="$tool_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex Run a slow and a fast command <<'STDOUT' 3<<'STDERR'
Both commands finished
STDOUT

[tool bash] {"command":"sleep 1; printf slow"}

[tool bash] {"command":"printf fast"}
[tool bash completed] {"stdout":"fast","stderr":"","exit_code":0}
[tool bash completed] {"stdout":"slow","stderr":"","exit_code":0}
STDERR
