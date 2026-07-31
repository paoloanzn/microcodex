<h1 align="center">MicroCodex (uCodex)</h1>

<p align="center">
  A small, local-first coding agent for your terminal, written in C++23.
</p>

<p align="center">
  <a href="https://github.com/paoloanzn/microcodex/actions/workflows/ci.yml"><img src="https://github.com/paoloanzn/microcodex/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
</p>

MicroCodex connects to the Codex Responses API and gives the model a focused set
of local tools for reading, creating, editing, searching, and testing code. It
supports one-shot prompts, a full-screen terminal UI, durable conversations,
conversation resumption, streaming output, tool execution, interruption, and
automatic context compaction.

> [!WARNING]
> MicroCodex is not a sandbox. Model-requested shell commands and file operations
> run with the same permissions as the MicroCodex process. Review the working
> directory and use an isolated environment when working with untrusted code.

## Features

- **Local coding tools:** `read`, `write`, `edit`, `glob`, and `bash`
- **Two interfaces:** one-shot command output and an interactive terminal UI
- **Streaming responses:** assistant text and tool progress appear as they arrive
- **Durable sessions:** conversations are stored as append-only JSONL files
- **Resume support:** restores the original model, working directory, and exact
  Responses API items
- **Context management:** summarizes old turns while preserving the complete
  durable transcript
- **Cancellable commands:** interruption propagates to HTTP requests and shell
  process groups
- **Rich terminal rendering:** Markdown, shell highlighting, compact tool output,
  and inline edit diffs
- **Minimal dependencies:** libcurl plus the vendored termbox2 and md4c libraries

## Requirements

MicroCodex supports macOS and Linux and requires:

- A C++23 compiler (`clang++` on macOS or `g++` on Linux)
- `make`
- libcurl development headers and library
- OpenSSL/libcrypto development files on Linux
- Git submodules for termbox2 and md4c

Clone the repository with its submodules, or initialize them after cloning:

```sh
git submodule update --init --recursive
```

## Build

```sh
make
```

The executable is written to:

```text
build/app
```

For the examples below, either invoke `build/app` directly or install/link it as
`microcodex`, for example:

```sh
ln -sf "$(pwd)/build/app" /usr/local/bin/microcodex
```

To remove build artifacts:

```sh
make clean
```

## Authentication

MicroCodex uses OpenAI's OAuth flow and stores credentials in the same format as
Codex.

```sh
microcodex login
```

Open the displayed URL in a browser. MicroCodex waits for the OAuth callback on
localhost and saves the resulting credentials to:

```text
$CODEX_HOME/auth.json
```

When `CODEX_HOME` is unset, it uses:

```text
~/.codex/auth.json
```

To remove the saved credentials:

```sh
microcodex logout
```

## Usage

```text
microcodex login
microcodex logout
microcodex list
microcodex show ID
microcodex [--model MODEL] resume ID [PROMPT]
microcodex [--model MODEL]
microcodex [--model MODEL] PROMPT
```

### One-shot prompt

Pass a prompt as command-line arguments:

```sh
microcodex "Find the failing test, fix it, and run the relevant test suite"
```

Assistant text is streamed to standard output. Tool progress is written to
standard error, which keeps the final answer easy to pipe or capture.

Choose a model explicitly with:

```sh
microcodex --model MODEL "Explain this repository"
```

### Interactive mode

Start without a prompt:

```sh
microcodex
```

Useful keys:

| Key | Action |
| --- | --- |
| `Enter` | Send the current prompt |
| `Ctrl-J` | Insert a newline |
| `Ctrl-C` | Interrupt the active turn, or clear the current input |
| `Ctrl-R` | Reset and start a new conversation |
| `Ctrl-T` | Toggle collapsed/full tool output |
| `Ctrl-Q` | Quit |
| `Ctrl-D` | Quit when the input is empty; otherwise delete at the cursor |
| `Ctrl-U` / `Ctrl-K` | Delete before / after the cursor |
| `Page Up` / `Page Down` | Scroll through the transcript |

### Saved conversations

Completed and explicitly interrupted turns are saved under:

```text
$CODEX_HOME/conversations/
```

or, by default:

```text
~/.codex/conversations/
```

List saved conversations:

```sh
microcodex list
```

Print a conversation as plain text:

```sh
microcodex show CONVERSATION_ID
```

Resume it interactively:

```sh
microcodex resume CONVERSATION_ID
```

Or resume it with an immediate prompt:

```sh
microcodex resume CONVERSATION_ID "Continue from where we stopped"
```

Unless `--model` is supplied, resuming uses the model recorded in the
conversation. It also restores the conversation's original working directory.

## Tools

The model can call the following tools in the directory where MicroCodex runs:

| Tool | Purpose |
| --- | --- |
| `read` | Read a bounded byte range from a file |
| `write` | Create a new file; refuses to overwrite an existing file |
| `edit` | Replace exact text in an existing file; `replace_all` defaults to `false` |
| `glob` | Expand a filesystem glob pattern |
| `bash` | Run a command in the user's shell environment and capture stdout, stderr, and exit status |

Tool calls in the same model response may run in parallel. Avoid asking the
agent to perform overlapping mutations of the same files.

## Context compaction

Before each run, MicroCodex queries the model catalog for the selected model's
context limits. When the conversation approaches that limit, old complete turns
are summarized and recent turns are retained verbatim. Compaction affects only
the context sent to the model: original turns remain in the JSONL transcript and
continue to appear in `show` and saved history.

If model metadata cannot be fetched, MicroCodex warns and uses built-in fallback
limits. The following environment variables override compaction policy:

| Variable | Meaning |
| --- | --- |
| `MICROCODEX_COMPACT_AT_TOKENS` | Token threshold that triggers compaction |
| `MICROCODEX_RETAINED_CONTEXT_TOKENS` | Approximate budget for recent turns retained verbatim |

Both values must be unsigned integers.

## Configuration

| Variable | Purpose |
| --- | --- |
| `CODEX_HOME` | Override the directory containing `auth.json` and `conversations/` |
| `MICROCODEX_COMPACT_AT_TOKENS` | Override the automatic compaction threshold |
| `MICROCODEX_RETAINED_CONTEXT_TOKENS` | Override the recent-context retention budget |
| `MICROCODEX_API_ENDPOINT` | Override the Responses API endpoint |

`MICROCODEX_API_ENDPOINT` is primarily intended for tests and development. The
configured server receives the bearer token, so do not point it at an untrusted
server while using real credentials.

## How it works

A turn is a synchronous model/tool loop:

1. The user message is added to the current context.
2. MicroCodex sends the exact conversation items and tool schemas to the
   Responses API with streaming enabled.
3. Text deltas are forwarded to the CLI or terminal UI.
4. Requested tools execute locally, in parallel when the model returns a batch.
5. Function outputs are appended and the model is sampled again.
6. When no more tools are requested, the complete turn is appended to the
   conversation file.

Normal API failures roll the in-memory turn back. An intentional interruption is
saved instead, including partial output and completed tool activity, so a later
prompt such as `continue` can safely account for partially executed work.

Key implementation areas:

| Files | Responsibility |
| --- | --- |
| `main.cpp` | CLI parsing and top-level setup |
| `agent.cpp`, `tool.cpp` | Agent instructions and tool registration |
| `api.cpp` | Streaming Responses API and model/tool loop |
| `conversation.cpp` | Durable JSONL conversations and crash recovery |
| `context-compaction.cpp` | Context planning and summarization |
| `oauth.cpp` | OAuth login and credential storage |
| `http.cpp` | Cancellable libcurl transport |
| `ui.cpp` | Interactive terminal application |
| `markdown.cpp`, `styled-text.cpp` | Terminal Markdown rendering and wrapping |

## Tests

Run the black-box CLI, API-streaming, tool-loop, conversation, and interruption
suite with:

```sh
make test
```

The suite builds the production executable and uses fake credentials plus a
loopback Ruby API fixture. It does not contact the production service or read
your Codex credentials.

Run one test file with:

```sh
sh tests/run.sh tests/003-tool-loop.sh
```

See [`tests/TESTS.md`](tests/TESTS.md) for details about the harness, covered
scenarios, and test-case format.

## License

Licensed under the Apache License 2.0. See [`LICENSE`](LICENSE) and
[`NOTICE`](NOTICE).
