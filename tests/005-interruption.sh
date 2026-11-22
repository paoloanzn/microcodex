#!/bin/sh
# shellcheck disable=SC2154 # run.sh sets tests_run before sourcing this file.

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

incomplete_home=$TEST_WORKDIR/incomplete-home
write_test_credentials "$incomplete_home" || exit 1
mock_number=$((tests_run + 2))
mock_dir=$TEST_WORKDIR/mock-$mock_number
expect_process "T5.3: continuing after maximum turn usage retains the partial response" 0 \
    run_with_mock incomplete-output env CODEX_HOME="$incomplete_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/continue-ui.rb" microcodex "Start incomplete response test" \
        "$mock_dir/incomplete-ready" "$mock_dir/continued" <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR

expect_process "T5.4: maximum turn usage persists the partial response" 0 \
    grep -q "Partial limited answer" "$incomplete_home"/conversations/*.jsonl <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR

tool_limit_home=$TEST_WORKDIR/tool-limit-home
write_test_credentials "$tool_limit_home" || exit 1
mock_number=$((tests_run + 2))
mock_dir=$TEST_WORKDIR/mock-$mock_number
expect_process "T5.5: continuing after the tool round limit retains the entire turn" 0 \
    run_with_mock tool-round-limit env CODEX_HOME="$tool_limit_home" \
        PATH="$TEST_BIN_DIR:$PATH" \
        "$RUBY" "$TEST_DIR/continue-ui.rb" microcodex "Start tool round limit test" \
        "$mock_dir/limit-ready" "$mock_dir/continued" <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR

expect_process "T5.6: the tool round limited turn is persisted" 0 \
    grep -q 'call_round_128' "$tool_limit_home"/conversations/*.jsonl <<'STDOUT' 3<<'STDERR'
STDOUT
STDERR

agent_interrupt_home=$TEST_WORKDIR/agent-interrupt-home
write_test_credentials "$agent_interrupt_home" || exit 1
rm -f "$TEST_PROJECT/command.pid"
# The sub-agent's command writes command.pid in the working directory.
# shellcheck disable=SC2034 # run.sh runs each case from case_cwd.
case_cwd=$TEST_PROJECT

# SIGTERM reaches a headless MicroCodex while its sub-agent runs a long
# command. The interruption has to travel down to that command: when the
# parent exits, the command must be gone too. SIGTERM, because a background
# job ignores SIGINT.
# shellcheck disable=SC2016 # The inner sh -c expands its own arguments.
expect_process "T5.7: interrupting a parent stops its sub-agent and the sub-agent's command" 1 \
    run_with_mock sub-agent-interrupt sh -c '
        CODEX_HOME="$1" PATH="$2:$PATH" microcodex Delegate a long command 2>"$1/parent.err" &
        app=$!
        attempt=0
        while [ ! -s command.pid ]; do
            attempt=$((attempt + 1))
            if [ "$attempt" -ge 500 ]; then
                kill "$app"
                exit 96
            fi
            sleep 0.01
        done
        kill -TERM "$app"
        wait "$app"
        status=$?
        if kill -0 "$(cat command.pid)" 2>/dev/null; then
            kill "$(cat command.pid)"
            echo "the command outlived MicroCodex" >&2
            exit 95
        fi
        cat "$1/parent.err" >&2
        exit "$status"' sh "$agent_interrupt_home" "$TEST_BIN_DIR" <<'STDOUT' 3<<'STDERR'
STDOUT

[tool agent] {"task":"Run a long command"}
[tool agent failed] Error: Tool execution interrupted
Agent failed: Codex turn was interrupted
STDERR
