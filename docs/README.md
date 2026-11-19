# MicroCodex documentation

These pages explain how MicroCodex works inside: the data structures it is
built around, how a turn moves through the code, and how conversations are
stored. They are written for contributors and maintainers. To install and run
MicroCodex, start with the [root README](../README.md).

The concept pages build on each other, so read them in the order listed. The
reference pages can be read in any order.

## Concepts

Read these first. Together they give the mental model the rest of the code
assumes.

| Page | Covers |
|---|---|
| [Architecture](concepts/architecture.md) | The modules and their layers, the threads that run at the same time, and the design rules that hold across the code. |
| [Turn lifecycle](concepts/turn-lifecycle.md) | What `CodexApi::sendUserMessage()` does: sampling requests, tool rounds, streaming, and the three ways a turn can end. |
| [Conversation state](concepts/conversation-state.md) | Response items, the in-memory `input_items_` array with its turn boundaries, and how that state maps to the file on disk. |
| [Context compaction](concepts/context-compaction.md) | When the context is summarized, which turns are kept word for word, and how the summary is saved without rewriting history. |
| [Tools](concepts/tools.md) | `ToolBase` and the `Tool<>` adapter, parallel execution, output limits, timeouts and cancellation. |
| [Terminal UI](concepts/terminal-ui.md) | The state-reducer model the UI is built on, the event queue between the agent and the UI, the main loop, how transcript entries become terminal rows, and how slash commands work. |

## Reference

Exact formats, flags and limits, for when you need the precise value.

| Page | Covers |
|---|---|
| [Command line](reference/cli.md) | Subcommands, options, environment variables, files under `CODEX_HOME`, interactive keys and slash commands. |
| [Conversation files](reference/conversation-files.md) | The JSONL record types, their fields, the rules a file must satisfy, and how damaged files are handled. |
| [Built-in tools](reference/built-in-tools.md) | The arguments, output and failure messages of `read`, `write`, `edit`, `glob` and `bash`. |

## Development

For changing MicroCodex. The build, lint and pull request rules are in
[CONTRIBUTING.md](../CONTRIBUTING.md), and the test harness is in
[tests/TESTS.md](../tests/TESTS.md).

| Page | Covers |
|---|---|
| [Adding a tool](development/adding-a-tool.md) | A step-by-step walkthrough: implement a tool, register it, and test it against the mock server. |
| [CI and releases](development/ci-and-releases.md) | What each CI job checks, how a release is cut and published, and how the installer and launcher use the release assets. |

## Conventions used in these pages

- Source references name the file and the symbol, such as
  `api.cpp` `CodexApi::compactContext()`, so they survive line changes.
- Conversation records and command output are copied from real runs of the
  test suite. Long paths and UUIDs are shortened with `…`.
- Default values are the ones in the source at the time of writing. The source
  is authoritative when the two disagree.
