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

The mock server also validates the outbound HTTP request before replying. Its
scenarios cover streamed text, an HTTP error, and a complete two-request tool
loop in which the model asks the real `write` tool to change the test working
directory. This verifies the CLI, credential loading, request construction,
SSE handling, tool execution, transcript replay, and final output together.

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
