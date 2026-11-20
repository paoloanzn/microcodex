# Conversation files

Each conversation is saved as one JSONL file. This page defines its records,
the rules a valid file follows, and what happens when a file is damaged. How
the file maps to the in-memory state is explained in
[conversation state](../concepts/conversation-state.md). The code is in
`conversation.cpp`.

## Location

```text
$CODEX_HOME/conversations/<id>.jsonl
```

`CODEX_HOME` defaults to `~/.codex`. `<id>` is the conversation ID, a random
UUID. The directory is created with mode `0700`, and each file is created
with mode `0600`. Creating a file never replaces an existing one.

## Format

A file is a sequence of records. Each record is one JSON object on one line,
followed by `\n`. A record, without its newline, is at most 16 MiB. The first
record is always `conversation`. After it come `turn`, `checkpoint` and
`model` records in the order they were written.

This file has two turns and one compaction between them. It was produced by
test T4.7, and the long path is shortened:

```json
{"type":"conversation","version":1,"id":"f96644a6-fb86-416f-8799-c8effe96549e","created_at":"2026-11-19T13:35:23Z","working_directory":"/…/project","model":"test-model"}
{"type":"turn","number":1,"id":"71e514b5-832d-4aea-b8f3-27438d3bdb09","items":[{"type":"message","role":"user","content":[{"type":"input_text","text":"Seed compactable context"}]},{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Seed response"}]}]}
{"type":"checkpoint","generation":1,"through_turn":1,"keep_from_turn":2,"summary":"The seed established compactable state."}
{"type":"turn","number":2,"id":"26fb5212-165f-4c6f-8d97-0b715ca9cba8","items":[{"type":"message","role":"user","content":[{"type":"input_text","text":"Continue after compaction"}]},{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Continued from compacted state"}]}]}
```

Readers find members by name, so the order of members inside a record does
not matter. Members not listed below are ignored.

### `conversation`

The metadata record, written when the file is created. `list` reads only
this line.

| Field | Type | Notes |
|---|---|---|
| `type` | string | `"conversation"`. |
| `version` | integer | `1`. Any other value is rejected. |
| `id` | string | The conversation ID. It matches the file name. |
| `created_at` | string | UTC time in the form `2026-11-19T13:35:23Z`. `list` sorts by this field. |
| `working_directory` | string | Absolute path of the directory MicroCodex was started in. `resume` changes to it. |
| `model` | string | Model the conversation started with. `resume` uses it unless a [`model`](#model) record or `--model` says otherwise. |

### `turn`

One terminal turn: completed or interrupted. A failed turn is never written.

| Field | Type | Notes |
|---|---|---|
| `type` | string | `"turn"`. |
| `number` | integer | 1 for the first turn, then increasing by exactly 1. |
| `id` | string | A random UUID for this turn. |
| `items` | array | The turn's response items, in order, exactly as they were sent or received. Never empty. |

The items are the turn's user message, every item the model returned
(messages, reasoning, function calls), the `function_call_output` for every
call, and, for an interrupted turn, a final `<turn_aborted>` user message.
Every `function_call` in a saved turn has a matching output.

### `checkpoint`

Written by each [context compaction](../concepts/context-compaction.md).

| Field | Type | Notes |
|---|---|---|
| `type` | string | `"checkpoint"`. |
| `generation` | integer | 1 for the first checkpoint, then increasing by exactly 1. |
| `through_turn` | integer | Number of the last turn written before this checkpoint. |
| `keep_from_turn` | integer | First turn the model still sees word for word. At least 1 and at most `through_turn + 1`, which means no turn is kept. |
| `summary` | string | The model's summary of everything before `keep_from_turn`. Not empty. |

Only the last checkpoint in a file is used. Earlier ones stay as a record of
previous compactions.

### `model`

Written when `/model` switches to another model, and when `resume --model`
names a model other than the saved one. Nothing is written when the model
stays the same.

```json
{"type":"model","model":"gpt-6.2-astra"}
```

| Field | Type | Notes |
|---|---|---|
| `type` | string | `"model"`. |
| `model` | string | Model of the turns that follow. Not empty. |

`resume` uses the model of the last `model` record, or the metadata's `model`
when there is none. `list` still shows the metadata's `model`.

## Rules

`ConversationFile::resume()` checks every record and refuses a file that
breaks a rule. `resume` and `show` then print an error and exit with status 1.
`resume` checks the file before it starts the agent, so the error has no
`Agent failed: ` prefix, even in a one-shot `resume ID PROMPT` run.

| Rule | Error |
|---|---|
| The file has at least one complete line. | `Conversation file is empty` |
| The first record is a `conversation` record. | Usually `Missing JSON member 'version'`, because the metadata fields are read before the type is checked. A record with every metadata field but another `type` gives `Conversation file does not begin with metadata`. |
| The version is 1. | `Unsupported conversation file version N` |
| There is only one `conversation` record. | `Conversation contains more than one metadata record` |
| Turn numbers go 1, 2, 3, … with no gaps. | `Conversation turn numbers are not consecutive` |
| Each checkpoint's generation is one more than the previous one, and its `through_turn` is the last turn written before it. | `Compaction checkpoints are inconsistent` |
| `keep_from_turn` is between 1 and `through_turn + 1`. | `Compaction checkpoint has an invalid retained turn` |
| A `model` record names a model. | `Model record has an empty model` |
| Every turn has at least one item. | `Saved turn contains no response items` |
| No record is larger than 16 MiB. | `Conversation record exceeds the size limit` |
| Every `type` is known. | `Unknown conversation record type 'NAME'` |

## Writing

`ConversationFile` writes with `O_APPEND` and calls `fsync()` after every
record, so a record is on disk before the turn is reported as finished. A turn
is written once, when it ends. A checkpoint is written before the compacted
context is used.

If a write or `fsync()` fails, the file is truncated back to its size before
that record, the file is closed, and the turn fails. Every later write in the
same process then fails with `Conversation is not open for writing`. Resume
the conversation from a new process to continue.

Nothing ever rewrites or deletes earlier records. Compaction only adds a
checkpoint, and `/model` only adds a `model` record.

## Damaged files

A crash during a write can leave a last line without its newline. Such a line
never counts as a record:

- `resume` removes it from the file before saving the next turn;
- `show` ignores it and leaves the file unchanged.

Test T4.3 checks this by appending `{"type":"turn","number":2` to a saved file
and resuming it.

Any other invalid record makes the file unusable, with the errors listed
under [rules](#rules). MicroCodex does not repair such files.
