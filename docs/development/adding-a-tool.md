# Adding a tool

This guide adds a tool named `file_size` that returns the size of a file in
bytes. It covers the full path: the function, the adapter, registration, and
a black-box test against the mock server. Read [tools](../concepts/tools.md)
first for the interface this guide uses.

Every snippet below was built with `make WERROR=1`, and passes `make lint`,
`make format-check` and its test.

## Before you start

- A working build (`make`) and test run (`make test`). See
  [CONTRIBUTING.md](../../CONTRIBUTING.md#development-setup).
- Ruby, which the test suite's mock server needs.

A tool must follow these rules, because of how
[calls are run](../concepts/tools.md#running-calls):

- **It is thread-safe.** Calls run in parallel, so two calls of the same tool
  can run at once. Avoid mutable global state.
- **It stops when asked.** Long work should check the `std::stop_token` and
  return early. A tool that ignores it is left running on a reaper thread
  after its timeout.
- **It reports failures as messages.** Return `std::unexpected("…")` with a
  sentence the model can act on. The model sees it as `Error: …`.
- **Its output is small.** Anything beyond 64 KiB is cut in the middle.

## 1. Write the function

A tool is an ordinary function that knows nothing about JSON. Put it in its
own module at the repository root. The `Makefile` compiles every `*.cpp` file
there, so no build change is needed.

`file-size.h`:

```cpp
// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace microcodex {

// Returns the size of a regular file in bytes.
std::expected<std::uintmax_t, std::string> fileSize(const std::string& path);

}  // namespace microcodex
```

`file-size.cpp`:

```cpp
// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "file-size.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

namespace microcodex {

std::expected<std::uintmax_t, std::string> fileSize(const std::string& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::unexpected("Not a regular file");
    }
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected("Could not read file size: " + error.message());
    }
    return size;
}

}  // namespace microcodex
```

The `std::error_code` overloads keep `std::filesystem` from throwing. A
thrown exception would still be caught and reported, but as
`Tool threw an exception`, which tells the model less.

## 2. Write the adapter

The adapter turns the JSON arguments into a call and the result into a
`ToolResult`. Adapters live in the anonymous namespace of `agent.cpp`, next
to the others. Add `#include "file-size.h"` and `#include <cstdint>` to that
file, then:

```cpp
std::expected<microcodex::ToolResult, std::string> executeFileSize(
    std::expected<std::uintmax_t, std::string> (*callable)(const std::string&), const ToolArguments& arguments,
    const std::stop_token stop_token) {
    auto path = arguments.string("path");
    if (!path) return std::unexpected(path.error());
    if (stop_token.stop_requested()) return std::unexpected("Tool execution interrupted");
    auto size = callable(*path);
    if (!size) return std::unexpected(size.error());
    return microcodex::ToolResult{.output = std::to_string(*size), .edit = {}};
}
```

Next to the other `using` aliases, name the tool type. The template arguments
are the function's return type, then its parameter types:

```cpp
using FileSizeTool = microcodex::Tool<std::expected<std::uintmax_t, std::string>, const std::string&>;
```

## 3. Register the tool

Add the tool to `makeCodingTools()` in `agent.cpp`, before `return tools;`:

```cpp
tools.emplace_back(std::make_shared<FileSizeTool>(
    "file_size", "Return the size of a regular file in bytes.", fileSize,
    R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})",
    executeFileSize));
```

The three strings are what the model sees, so write them for the model:

- the **name** it calls (unique, `snake_case` like the others);
- a **description** of what the tool does and how it fails;
- the **parameters**, a JSON Schema object. Keep it flat with scalar members,
  because `ToolArguments` only reads top-level strings, integers and
  booleans.

A sixth, optional argument gives the tool its own time limit, such as
`std::chrono::minutes(30)`, or `std::chrono::seconds(0)` for none. Leave it
out to use the session's limit, which is right for most tools. See
[timeouts](../concepts/tools.md#timeouts).

Every request now includes the new definition, and the model can call it.

## 4. Add a test

Tests run the real executable against `tests/mock-server.rb`, a scripted
Responses API server. See [tests/TESTS.md](../../tests/TESTS.md) for the
harness. A tool test needs a two-request **scenario**: the first reply calls
the tool, and the second checks that the tool's output came back.

In `tests/mock-server.rb`, add a reply that calls the tool, next to the other
`…_call_response` methods:

```ruby
def file_size_call_response
  sse(
    {type: "response.output_item.done",
     item: {type: "function_call", call_id: "call_size", name: "file_size",
            arguments: JSON.generate(path: "five.txt")}},
    completed
  )
end
```

Then add the scenario in three places:

```ruby
# validate_scenario!: check the second request carries the real output
when "tool-file-size"
  validate_coding_tools!(payload)
  if request_number.positive?
    assert(payload.fetch("input").include?({"type" => "function_call_output",
                                            "call_id" => "call_size", "output" => "5"}),
           "second request did not contain the file size")
  end

# response_for: call the tool first, then answer
when "tool-file-size"
  [200, "OK", "text/event-stream",
   request_number.zero? ? file_size_call_response : message_response("The file has 5 bytes")]

# expected_requests: add "tool-file-size" to the list of two-request scenarios
```

Finally add a case to `tests/003-tool-loop.sh`. Expected stdout comes on
standard input and expected stderr on file descriptor 3:

```sh
printf 'hello' > "$TEST_PROJECT/five.txt"

expect_process "T3.9: file_size reports the size of a file" 0 \
    run_with_mock tool-file-size env CODEX_HOME="$tool_home" PATH="$TEST_BIN_DIR:$PATH" \
        microcodex How big is five.txt <<'STDOUT' 3<<'STDERR'
The file has 5 bytes
STDOUT

[tool file_size] {"path":"five.txt"}
[tool file_size completed] 5
STDERR
```

Run the file on its own:

```text
$ sh tests/run.sh tests/003-tool-loop.sh
…
ok 009 - T3.9: file_size reports the size of a file
9 tests, 0 failures
```

The case passes only if the whole loop works: the tool is advertised, the
model's call reaches the function, and the output is sent back in the next
request.

## 5. Check and document

Run the same checks as CI:

```shell
make WERROR=1
make test
make lint
make format-check
```

Then add the tool to [built-in tools](../reference/built-in-tools.md) with its
arguments, output and errors.

## Showing custom output in the UI

The UI shows a tool's output as plain text. The `edit` tool is the one
exception: it also sets `ToolResult::edit`, which the UI draws as a diff. That
field is specific to edits. A tool that needs its own display should replace
it with a small generic type rather than add another tool-specific field, as
the note in `tool.h` says.
