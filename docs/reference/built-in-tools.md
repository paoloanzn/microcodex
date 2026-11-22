# Built-in tools

MicroCodex gives the model six tools, defined in `makeCodingTools()` in
`agent.cpp`. A sub-agent gets the first five: it has no `agent` tool. This page lists each tool's arguments, its output, and the error
messages it can return. How calls are scheduled, limited and cancelled is
explained in [tools](../concepts/tools.md).

These rules apply to every tool:

- Paths are used as given. Relative paths are relative to MicroCodex's
  working directory, and nothing stops a path from pointing outside it.
- A failed call returns `Error: ` followed by the message listed below. The
  turn continues, and the model sees the error.
- Output longer than 64 KiB is cut in the middle (see
  [tools](../concepts/tools.md#what-a-call-returns)).
- Calls run in parallel, and nothing orders two calls that touch the same
  file.

## `read`

Reads a byte range from a file.

| Argument | Type | Default | Notes |
|---|---|---|---|
| `path` | string | required | |
| `offset` | integer ≥ 0 | required | First byte to read. |
| `limit` | integer ≥ 0 | required | Maximum number of bytes. |

The output is the raw bytes from `offset` to `offset + limit`, or to the end
of the file. The range is counted in bytes, so it can start or end inside a
multi-byte UTF-8 character.

| Error | Cause |
|---|---|
| `Could not open file` | The file does not exist or cannot be opened. |
| `Offset is beyond file size` | `offset` is larger than the file. An offset equal to the size returns an empty output. |
| `Could not get file size`, `Could not seek in file`, `Could not read file` | An I/O error. |

## `write`

Creates a new file.

| Argument | Type | Default | Notes |
|---|---|---|---|
| `path` | string | required | Must not exist. |
| `content` | string | required | Written as is. |

The output is `Created PATH`. Missing parent directories are not created.

| Error | Cause |
|---|---|
| `Overwriting an existing file is not allowed` | `path` already exists. Use `edit` to change it. |
| `Could not create file` | The file cannot be opened for writing, for example because its directory does not exist. |
| `Failed to write to file` | The write failed. |

## `edit`

Replaces exact text in an existing file.

| Argument | Type | Default | Notes |
|---|---|---|---|
| `path` | string | required | Must exist. |
| `old_content` | string | required | Text to find. Must not be empty. |
| `new_content` | string | required | Replacement text. May be empty. |
| `replace_all` | boolean | `false` | Replace every occurrence instead of exactly one. |

The search is a plain substring search: no patterns and no whitespace
normalization. Occurrences are found from left to right without overlapping.
Without `replace_all`, the text must occur exactly once. The whole file is
then rewritten in place.

The output sent to the model is `Edited PATH`. The UI also receives an
`EditResult` with the changed lines and one line of context on each side, and
draws it as a diff. The diff is not sent to the model.

| Error | Cause |
|---|---|
| `File not found` | `path` does not exist. |
| `Content to replace cannot be empty` | `old_content` is empty. |
| `Content not found` | `old_content` does not occur. |
| `Content occurs more than once; use replace_all to replace every occurrence` | Several occurrences without `replace_all`. |
| `Could not create file`, `Failed to write to file` | The file cannot be rewritten. |

## `glob`

Expands a filesystem pattern with POSIX `glob(3)`.

| Argument | Type | Default | Notes |
|---|---|---|---|
| `pattern` | string | required | For example `src/*.cpp`. |

The output is the matching paths, sorted, one per line. The pattern supports
`*`, `?` and `[…]` only. `**` does not search subdirectories, `{a,b}` is not
expanded, and `*` does not match names that start with a dot.

| Error | Cause |
|---|---|
| `Pattern cannot be empty` | `pattern` is empty. |
| `No files matched pattern` | Nothing matches. |
| `Could not read a directory while expanding pattern` | A directory could not be read. |
| `Not enough memory to expand pattern`, `Could not expand pattern` | Other `glob(3)` failures. |

## `bash`

Runs a shell command.

| Argument | Type | Default | Notes |
|---|---|---|---|
| `command` | string | required | Shell source, run with `-c`. |

The output is a JSON object with the command's output and status:

```json
{"stdout":"loaded-from-bashrc","stderr":"","exit_code":0}
```

`exit_code` is the exit status, or 128 plus the signal number when a signal
ended the command. A non-zero status is still a successful call; the model
reads the status from the output.

### Shell environment

Commands run in the user's shell, `$SHELL`, or `/bin/sh` when it is unset or
not a regular file. Each call starts a new shell in MicroCodex's working
directory, so `cd` and unexported variables do not carry over to the next
call.

So that commands see the same `PATH` and exports as the user's terminal, the
first call captures the shell's environment once per process:

1. It runs the shell as a login shell (`-lc`). The script first loads the
   interactive startup file (`$ZDOTDIR/.zshrc` or `~/.zshrc` for zsh,
   `~/.bashrc` for bash unless `BASH_ENV` is set, `$ENV` for other shells),
   then writes `export -p` to a temporary snapshot file.
2. Every command then runs as `$SHELL -c`, loading the snapshot first.

The capture has a 10-second limit. If it fails, each command instead runs as
a login shell that loads the startup file itself, which is slower.

### Process handling

The command runs in its own process group, with stdout and stderr captured
through pipes. Standard input is not redirected. When the call is stopped (by
an interruption or the tool timeout), the whole group gets `SIGINT`, then
`SIGKILL` if it is still running one second later. Background processes the
command started are in the same group, so they are stopped too.

A call ends only when the shell has exited and both pipes are closed. A
background process that keeps stdout or stderr open, such as `sleep 60 &`,
keeps the call running until it exits or the call is stopped. Redirect the
output of background processes (`sleep 60 > /dev/null 2>&1 &`) to avoid this.

The complete output is kept in memory until the command exits, and is then
cut to 64 KiB.

### Denied commands

Before starting a shell, `bash` checks the command against five patterns
(`bash-safety.cpp`). A match returns `command denied: REASON` without running
anything:

| Matches | Reason |
|---|---|
| `rm` with `-f`, `--force` or a flag group containing `f`, such as `-rf` | `forced file removal is blocked` |
| `git reset … --hard` | `git reset --hard is blocked` |
| `git clean` with `-f`, `--force` or a flag group containing `f` | `forced git clean is blocked` |
| `git checkout --` | `git checkout -- is blocked` |
| `mkfs`, `mkfs.*`, `fdisk`, `parted`, `shutdown`, `reboot`, `halt`, `poweroff` | `system or disk destructive command is blocked` |

The patterns are case-insensitive. They match at the start of the command or
after a space, `;`, `|`, `&`, `(` or `)`, so `true && rm -rf build` is
denied too. This is a text match, not a shell parser: a command built at run
time, such as `eval "$cmd"` or a script file, is not inspected. Do not rely on
it as a security boundary.

| Error | Cause |
|---|---|
| `command must not be empty` | `command` is empty. |
| `command denied: …` | The command matched the denylist. |
| `pipe failed: …`, `fork failed: …`, `poll failed: …`, `waitpid failed: …` | A system call failed. |

A stopped call reports the generic `Error: Tool execution interrupted` or
`Error: Tool execution timed out after N seconds` instead of a `bash` message.

## `agent`

Starts a sub-agent: another MicroCodex process that works on a task in the
same working directory, with the same login and the other five tools, and
returns its final answer. The description tells the model to use it only when
the user explicitly asks for sub-agents, delegation or parallel agents.

| Argument | Type | Default | Notes |
|---|---|---|---|
| `task` | string | required | The sub-agent's prompt. It sees none of the parent's conversation. Must not be empty. |
| `agent_id` | string | none | The `agent_id` of an earlier result. The sub-agent resumes that conversation and gets `task` as its next message. |
| `model` | string | the parent's current model | A follow-up without `model` keeps the sub-agent's own model. |
| `effort` | string | the parent's current effort | One of the `--effort` values (see [command line](cli.md#prompt-and-interactive-session)). |

"Current" includes changes made with `/model` and `/effort`: the tool reads
them from the session's `CodexApi`, which `connectAgentTool()` gives it when
the session starts.

The output is a JSON object:

```json
{"agent_id":"4f0c…","answer":"The answer is 42","exit_code":0}
```

- `agent_id` is the sub-agent's conversation ID, or `null` when it ended
  before saving a conversation.
- `answer` is the sub-agent's stdout: its final answer.
- `exit_code` is the sub-agent's exit status. A non-zero status is still a
  successful call, and the object then has an `error` field with the last
  4 KiB of the sub-agent's stderr.

### How a sub-agent runs

The tool runs the MicroCodex executable that is running now (`/proc/self/exe`
on Linux), not the installed launcher, so a sub-agent is always the same
build and skips the update check:

```shell
microcodex --model MODEL --effort EFFORT TASK
microcodex [--model MODEL] --effort EFFORT resume AGENT_ID TASK
```

It runs with `MICROCODEX_SUB_AGENT=1` (see
[command line](cli.md#environment-variables)), so it has no `agent` tool, and
it ends its stderr with `[conversation ID]`, which the tool removes and
returns as `agent_id`. Its tool activity is not shown to the user. Each
sub-agent is a separate process that saves its own conversation, so `list`
and `show` see it, and `resume` can continue it by hand.

Several `agent` calls in one response run in parallel like any other calls,
and there is no limit on how many run at once. Nothing stops two sub-agents
from editing the same file.

The call has no time limit. The sub-agent runs in its own process group with
`/dev/null` as standard input. When the call is stopped, the group gets
`SIGINT`. The sub-agent then interrupts its own turn, which stops its
commands, and exits; if it is still running three seconds later, the group
gets `SIGKILL`.

| Error | Cause |
|---|---|
| `task must not be empty` | `task` is empty. |
| `Missing JSON member 'task'`, `Expected a JSON string` | `task` is missing, or an argument is not a string. |
| `effort: unsupported reasoning effort '…' (…)` | `effort` is not an `--effort` value. |
| `pipe failed: …`, `fork failed: …`, `poll failed: …`, `waitpid failed: …` | A system call failed. |
