# Tests

The suite is a black-box contract test for the built `microcodex` executable.
It uses an isolated credentials directory and a pure-Ruby loopback HTTP server,
so it never reads the user's Codex credentials or contacts a remote API. The
fixture uses only Ruby's standard library and has no package dependencies.

Run all tests:

```sh
make test
```

Run one numbered test file:

```sh
sh tests/run.sh tests/003-tool-loop.sh
```

Keep the disposable work directory after a run:

```sh
MICROCODEX_TEST_KEEP=1 sh tests/run.sh
```

`MICROCODEX_TEST_WORKDIR=/path` selects a reusable parent directory. Each run
creates and keeps a new `run.XXXXXX` child beneath it.

## What the harness tests

`tests/run.sh` builds the production executable and starts
`tests/mock-server.rb` for API scenarios. Every case runs the real CLI and
compares all three observable process results:

- exit status;
- stdout, byte for byte;
- stderr, byte for byte.

The mock server also validates the authenticated model-catalog and Responses
requests before replying. Its scenarios cover dynamic context limits, streamed
text, an HTTP error, and a complete two-request tool loop in which the model
asks the real `write` tool to change the test working directory. This verifies
the CLI, credential loading, request construction, SSE handling, tool
execution, transcript replay, and final output together.
The tool cases also verify that bash sees exports loaded from the user's shell
startup file. PTY-driven interruption cases exercise the real termbox UI and
prove that both partial assistant output and interrupted function calls are
replayed when the user sends `continue`.
Conversation scenarios also cover durable JSONL turns, listing and walking
saved history, exact replay after resume, compaction checkpoints, and the
invariant that compaction never removes the durable transcript.
Sub-agent scenarios run the real `agent` tool: the mock server answers the
parent and the MicroCodex process it starts in turn, through a first task and
a follow-up that resumes the sub-agent. Another case sends `SIGTERM` to a
headless parent while its sub-agent runs a command, and checks that the
command is gone when the parent exits.

The production endpoint is unchanged by default. The runner sets
`MICROCODEX_API_ENDPOINT` only for child processes that use fake credentials.
Do not point that variable at an untrusted server while using real credentials:
the configured endpoint receives the bearer token.

## Writing a case

Numbered test files are sourced by `tests/run.sh`. A case supplies expected
stdout on standard input and expected stderr on file descriptor 3:

```sh
expect_process "T4.1: example" 0 command arg <<'STDOUT' 3<<'STDERR'
normal output
STDOUT
diagnostic output
STDERR
```

Use `run_with_mock SCENARIO ...` when the command should talk to the local API
fixture. Add the scenario to `tests/mock-server.rb`, validate stable request
properties there, and avoid matching UUIDs or other per-run values.

## Footprint check

`make footprint` checks the size of the executable and the peak memory of a
session against the budgets at the top of `tests/footprint.sh`. It reuses the
mock server's scenarios, and `tests/peak-rss.c` measures the memory.
`make footprint FOOTPRINT_BASE=origin/main` also builds that commit in a
temporary worktree and fails on too much growth over it. CI runs the check on
every push and pull request; see
[footprint budgets](../docs/development/ci-and-releases.md#footprint-budgets).
