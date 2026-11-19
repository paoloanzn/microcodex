# Command line

This page lists every command, option, environment variable and file that
MicroCodex uses, and the keys of the interactive UI. Installing is covered in
the [root README](../../README.md#quickstart), and the installer and launcher
variables are covered in [CI and releases](../development/ci-and-releases.md#installer-and-launcher).

## Usage

```text
$ microcodex --help
Usage:
  microcodex login [--device-auth]
  microcodex logout
  microcodex list
  microcodex show ID
  microcodex [--model MODEL] [--effort EFFORT] resume ID [PROMPT]
  microcodex [--model MODEL] [--effort EFFORT]
  microcodex [--model MODEL] [--effort EFFORT] PROMPT

Effort values: none, minimal, low, medium, high, xhigh, max, ultra, persistent (default: medium)
  MICROCODEX_EFFORT selects the effort when --effort is absent.
```

Every command exits with status 0 on success and 1 on failure. Errors are
printed to stderr.

## Commands

### Prompt and interactive session

```shell
microcodex [--model MODEL] [--effort EFFORT] [PROMPT]
```

With `PROMPT`, MicroCodex runs one turn, prints the answer to stdout and
exits. Tool activity goes to stderr (see
[turn lifecycle](../concepts/turn-lifecycle.md#events)). All remaining words
are joined with spaces into one prompt, so quoting is optional.

Without `PROMPT`, it starts the interactive UI. Both forms start a new saved
conversation and record the current directory as its working directory.

The options must come before the prompt:

| Option | Default | Notes |
|---|---|---|
| `--model MODEL` | `gpt-6-sol` | Any model the account can use. A value that starts with `--` is rejected. |
| `--effort EFFORT` | `medium` | One of the values below. Overrides `MICROCODEX_EFFORT`. |

The effort is sent as the request's `reasoning.effort`. In an interactive
session, `/effort` changes it for the following turns. Two values are renamed
on the way:

| Value | Sent as |
|---|---|
| `none`, `minimal`, `low`, `medium`, `high`, `xhigh`, `max` | unchanged |
| `ultra` | `max` |
| `persistent` | `disabled` |

An empty or unknown value is an error, reported before any network request:

```text
$ microcodex --effort bogus hi
--effort: unsupported reasoning effort 'bogus' (expected one of: none, minimal, low, medium, high, xhigh, max, ultra, persistent)
```

The usage text follows the error.

### `resume`

```shell
microcodex [--model MODEL] [--effort EFFORT] resume ID [PROMPT]
```

Continues a saved conversation. `ID` is the conversation ID printed by
`list`, with or without the `.jsonl` suffix. MicroCodex changes to the
conversation's saved working directory and uses its saved model unless
`--model` is given. With `PROMPT` it runs one more turn; without it, it opens
the interactive UI with the recent history shown.

A damaged last line, left by a crash during a write, is removed before the
new turn is saved. See
[conversation files](conversation-files.md#damaged-files).

### `list`

Prints one line per saved conversation, newest first, with four
tab-separated fields: ID, creation time (UTC), model and working directory.
It needs no login. Files whose first line cannot be read are skipped.

```text
$ microcodex list
f96644a6-fb86-416f-8799-c8effe96549e	2026-11-19T13:35:23Z	test-model	/home/user/project
```

### `show`

```shell
microcodex show ID
```

Prints every saved turn of a conversation, including turns that compaction
removed from the model's context. Each item becomes one line:

```text
$ microcodex show 2f1c…
user: Remember alpha
assistant: Alpha stored
```

Tool calls print as `tool NAME: ARGUMENTS` and their outputs as
`tool output CALL_ID: OUTPUT`. Other item types are skipped. An unknown ID
prints `Conversation 'ID' was not found`.

### `login`

```shell
microcodex login [--device-auth]
```

Signs in with a ChatGPT account through OAuth and saves the tokens to
`auth.json`.

- **Browser login** (the default) prints an authorization URL and waits up to
  5 minutes for the browser to return to a listener on `127.0.0.1`, port
  1455. Port 1457 is used when 1455 is busy. The listener only binds to the
  loopback interface. The login uses PKCE and checks the `state` value.
- **Device login** (`--device-auth`) prints a URL and a one-time code to
  enter there. It needs no local port, so it works over SSH. It polls for up
  to 15 minutes.

MicroCodex does not refresh the access token on its own. When the saved token
expires, requests fail with HTTP 401; run `microcodex login` again.

### `logout`

Deletes `auth.json`. It prints `Logged out.`, or `Already logged out.` when
there was no file.

## Environment variables

| Variable | Default | Effect |
|---|---|---|
| `CODEX_HOME` | `~/.codex` | Directory for `auth.json`, `AGENTS.md`, `conversations/` and `skills/`. |
| `HOME` | | Used for `~/.codex` when `CODEX_HOME` is unset. If neither is set, every command that needs `auth.json` fails with `Cannot find OAuth credentials path: HOME is not set`. |
| `MICROCODEX_EFFORT` | `medium` | Reasoning effort when `--effort` is absent. Same values as `--effort`. It is checked after the credentials are loaded, and an error starts with `MICROCODEX_EFFORT:` and has no usage text. |
| `MICROCODEX_TOOL_EXECUTION_TIMEOUT_SECONDS` | `120` | Time limit for each tool call. `0` disables it. The maximum is 31536000 (one year). |
| `MICROCODEX_COMPACT_AT_TOKENS` | from the model catalog | Context size that triggers compaction. `0` disables automatic compaction. |
| `MICROCODEX_RETAINED_CONTEXT_TOKENS` | `20480` | Token budget for the recent turns kept word for word by compaction. |
| `MICROCODEX_API_ENDPOINT` | `https://chatgpt.com/backend-api/codex/responses` | Responses endpoint. The model catalog is read from the same base URL, with `/responses` replaced by `/models`. |
| `SHELL` | `/bin/sh` | Shell used by the `bash` tool. See [built-in tools](built-in-tools.md#bash). |

The numeric variables must be unsigned integers; an empty value counts as
unset. They are checked after the model catalog lookup, so a warning about the
catalog can come first. An invalid value stops the command before the first
model request:

```text
MICROCODEX_TOOL_EXECUTION_TIMEOUT_SECONDS must be an unsigned integer
```

`MICROCODEX_API_ENDPOINT` exists so the test suite can run against a local
mock server. The endpoint receives the access token, so never point it at a
server you do not trust while you are logged in.

## Files

All paths are relative to `CODEX_HOME`. MicroCodex uses the same layout as
the Codex CLI, so the two share credentials, global instructions and skills.

| Path | Contents |
|---|---|
| `auth.json` | OAuth tokens, written atomically with mode `0600`: `{"auth_mode":"chatgpt","OPENAI_API_KEY":null,"tokens":{"id_token","access_token","refresh_token","account_id"},"last_refresh"}`. |
| `AGENTS.md` | Instructions for every project, read at startup and sent as the first message of every request. At most 32 KiB is used; a longer file ends with a `<truncated>` note to the model. See [the global AGENTS.md](../concepts/turn-lifecycle.md#the-global-agentsmd). |
| `AGENTS.override.md` | Used instead of `AGENTS.md` when it exists and is not blank. |
| `conversations/` | One `<id>.jsonl` file per conversation. The directory has mode `0700` and each file `0600`. Format: [conversation files](conversation-files.md). |
| `skills/` | Skills, read at startup. Every `SKILL.md` up to 6 directory levels deep whose YAML front matter has a `name` (at most 128 characters) and a `description` (at most 1024) is listed in the model's instructions with its absolute path. Files over 1 MiB are skipped. |

The `bash` tool also creates one temporary file per process,
`microcodex-shell-snapshot.XXXXXX` in the system temporary directory, and
deletes it on exit.

## Interactive keys

The footer shows the most common keys. All of them:

| Key | Action |
|---|---|
| Enter | Send the message, or run a [slash command](#slash-commands). |
| Ctrl-J | Insert a newline. |
| Esc, Ctrl-C | Interrupt the running turn, or clear the composer when no turn is running. |
| Ctrl-D | Quit when no turn is running and the composer is empty; otherwise delete the character under the cursor. |
| Ctrl-Q | Quit. A running turn is interrupted first. |
| Ctrl-R | Start a new conversation. Refused while a turn is running. The old conversation stays saved. |
| Ctrl-T | Show or collapse long tool output. |
| Ctrl-L | Redraw the screen. |
| Left, Right | Move by one character. With Alt or Ctrl, move by one word. |
| Alt-B, Alt-F | Move one word back or forward. |
| Up, Down | Move between composer lines. With Alt, go to the start or end of the line. |
| Ctrl-A, Ctrl-E | Go to the start or end of the line. |
| Home, End | Go to the start or end of the whole message. |
| Backspace, Delete | Delete the character before or under the cursor. Alt-Backspace deletes the word before it. |
| Ctrl-U, Ctrl-K | Delete to the start or the end of the message. |
| Page Up, Page Down | Scroll the transcript by one screen. |
| Mouse wheel | Scroll the transcript by three rows. |

Large pastes appear as a `[Pasted Content N chars]` label; see
[terminal UI](../concepts/terminal-ui.md#input).

## Slash commands

Typing `/` as the first character of the composer opens a menu of commands
below it, and further letters filter it by name. While the menu is open, Up
and Down select a command, Tab completes its name, and Enter runs the selected
command, so `/st` followed by Enter runs `/status`.

| Command | During a turn | Action |
|---|---|---|
| `/effort` | refused | Open a picker of the effort values, with the current one selected. |
| `/effort LEVEL` | refused | Set the effort for the following turns. `LEVEL` is one of the [effort values](#prompt-and-interactive-session). |
| `/new` | refused | Start a new conversation, like Ctrl-R. |
| `/status` | refused | Show the model, the effort, the number of saved turns and the context size. |
| `/quit` | allowed | Quit, like Ctrl-Q. |

The footer shows the current model and effort. A command never reaches the
model. Text that starts with `/` but does not name a command, such as
`/etc/hosts is wrong`, is sent as a normal message. A command that is refused
or given unexpected arguments shows an error:

```text
✗ '/new' is not available while a turn is running
✗ '/new' takes no arguments
✗ /effort: unsupported reasoning effort 'turbo' (expected one of: none, minimal, low, medium, high, xhigh, max, ultra, persistent)
```

`/status` prints, for example:

```text
• Model: gpt-6-sol
  Reasoning effort: medium
  Saved turns: 0
  Context: 0 of 258,400 tokens, compacts at 244,800
```
