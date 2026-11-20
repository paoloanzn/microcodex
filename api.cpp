// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "api.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <exception>
#include <expected>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "context-compaction.h"
#include "conversation.h"
#include "event-emitter.h"
#include "http.h"
#include "json.h"
#include "model-catalog.h"
#include "response-item.h"
#include "tool.h"

namespace {

using microcodex::json::appendJsonString;
using microcodex::json::findJsonMember;
using microcodex::json::jsonStringMember;
using microcodex::json::requiredJsonString;

constexpr std::string_view interrupted_message = "Codex turn was interrupted";
constexpr std::string_view incomplete_response_prefix = "The Codex response was incomplete: ";

// Accepted effort values mirror the official Codex client's ReasoningEffort
// set (openai/codex codex-rs/protocol/src/openai_models.rs).
constexpr auto reasoning_effort_values = std::to_array<std::string_view>({
    "none",
    "minimal",
    "low",
    "medium",
    "high",
    "xhigh",
    "max",
    "ultra",
    "persistent",
});

// Wire value for the reasoning effort. The configuration keeps the value the
// user chose, so it can be shown back to them, and only the request carries
// the mapped one. The Responses API calls "persistent" "disabled". "ultra" is
// not a sampling effort of its own: Codex uses it to also run sub-agents, and
// sends the model's highest single-agent effort. MicroCodex has no sub-agents,
// so "ultra" is an alias for "max", which every GPT-6 model supports.
std::string_view wireReasoningEffort(const std::string_view effort) {
    if (effort == "persistent") return "disabled";
    if (effort == "ultra") return "max";
    return effort;
}
constexpr std::string_view turn_usage_limit_error = "The Codex response was incomplete: max_output_tokens";
constexpr std::string_view tool_round_limit_error = "Codex exceeded the maximum number of tool rounds";
std::string turnAbortedItem(const std::string_view error) {
    return microcodex::userMessageItem("<turn_aborted>\n" + std::string(error) + "\n</turn_aborted>");
}

std::string makeUuid() {
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (unsigned char& byte : bytes) {
        byte = static_cast<unsigned char>(random());
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);

    constexpr std::string_view hex = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(36);
    std::size_t index = 0;
    for (const unsigned char byte : bytes) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            uuid += '-';
        }
        uuid += hex[byte >> 4];
        uuid += hex[byte & 0x0f];
        ++index;
    }
    return uuid;
}

bool containsNewline(const std::string_view value) { return value.find_first_of("\r\n") != std::string_view::npos; }

std::optional<std::size_t> unsignedJsonValue(const std::string_view value) {
    std::size_t result = 0;
    const char* end = std::to_address(value.cend());
    const auto [parsed_end, error] = std::from_chars(std::to_address(value.cbegin()), end, result);
    if (error != std::errc{} || parsed_end != end) return std::nullopt;
    return result;
}

std::string currentTimestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    const bool converted = gmtime_s(&utc, &now) == 0;
#else
    const bool converted = gmtime_r(&now, &utc) != nullptr;
#endif
    std::array<char, 32> timestamp{};
    if (!converted || std::strftime(timestamp.data(), timestamp.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return {};
    }
    return timestamp.data();
}

std::string boundedToolOutput(std::string output, const std::size_t maximum_bytes) {
    if (maximum_bytes == 0 || output.size() <= maximum_bytes) return output;

    const std::string marker = "\n... [tool output truncated from " + std::to_string(output.size()) + " bytes] ...\n";
    if (marker.size() >= maximum_bytes) {
        return marker.substr(0, maximum_bytes);
    }
    const std::size_t remaining = maximum_bytes - marker.size();
    const std::size_t head = remaining / 2;
    const std::size_t tail = remaining - head;
    return output.substr(0, head) + marker + output.substr(output.size() - tail);
}

bool isContextLimitError(const std::string_view error) {
    return error.find("context window") != std::string_view::npos ||
           error.find("context_length") != std::string_view::npos ||
           error.find("maximum context") != std::string_view::npos;
}

bool isTurnUsageLimitError(const std::string_view error) { return error == turn_usage_limit_error; }

bool isToolRoundLimitError(const std::string_view error) { return error == tool_round_limit_error; }

// Remote/transport failures (HTTP 5xx, curl-level transport errors) are
// resumable like interruptions and usage limits: the turn keeps whatever
// partial output arrived before the failure, gains a <turn_aborted>
// marker, and the user can continue from there. 4xx responses and
// terminal stream verdicts stay transactional.
bool isTransportFailureError(const std::string_view error) { return error.starts_with("HTTP request failed: "); }

bool isServerErrorStatus(const long status) { return status >= 500 && status < 600; }

bool isRemoteFailureError(const std::string_view error) {
    return isTransportFailureError(error) || error.starts_with("Codex API returned HTTP 5");
}

struct StreamState {
    microcodex::CodexApiResponse response;
    std::vector<std::string> output_items;
    std::string fallback_text;
    std::string line_buffer;
    std::string event_data;
    std::string turn_state;
    std::string turn_id;
    microcodex::CodexEventEmitter* events = nullptr;
    bool completed = false;
};

void emitTextDelta(StreamState& state, const std::string_view text) {
    if (state.events == nullptr || text.empty()) {
        return;
    }
    state.events->emit({
        .type = microcodex::CodexEventType::TextDelta,
        .turn_id = state.turn_id,
        .call_id = {},
        .tool_name = {},
        .text = std::string(text),
        .edit = {},
    });
}

// Returns object[key], or nullopt when the member is missing or the object
// cannot be parsed. Used where a missing value only selects a fallback.
std::optional<std::string_view> optionalMember(const std::string_view object, const std::string_view key) {
    auto member = findJsonMember(object, key);
    if (!member) return std::nullopt;
    return *member;
}

// Returns the string object[key], or nullopt when it is missing or invalid.
std::optional<std::string> optionalStringMember(const std::string_view object, const std::string_view key) {
    auto member = jsonStringMember(object, key);
    if (!member) return std::nullopt;
    return std::move(*member);
}

// Names a string member of a nested object: object[outer][key].
struct NestedMember {
    std::string_view outer;
    std::string_view key;
};

// Returns object[outer][key] as a string when every level is present.
std::optional<std::string> nestedStringMember(const std::string_view object, const NestedMember& member) {
    const std::optional<std::string_view> nested = optionalMember(object, member.outer);
    if (!nested) return std::nullopt;
    return optionalStringMember(*nested, member.key);
}

std::expected<void, std::string> handleOutputTextDelta(const std::string_view data, StreamState& state) {
    auto delta = requiredJsonString(data, "delta");
    if (!delta) {
        return std::unexpected(delta.error());
    }
    state.response.text += *delta;
    emitTextDelta(state, *delta);
    return {};
}

std::expected<void, std::string> handleOutputItemDone(const std::string_view data, StreamState& state) {
    auto item = findJsonMember(data, "item");
    if (!item) {
        return std::unexpected(item.error());
    }
    const std::optional<std::string_view>& item_member = *item;
    if (!item_member) {
        return std::unexpected("response.output_item.done has no item");
    }

    // Keep the exact response item. Since requests use store=false, the
    // next sampling request must replay model messages, reasoning, and
    // function calls exactly as the server returned them.
    const std::string_view item_json = *item_member;
    state.output_items.emplace_back(item_json);

    auto item_type = requiredJsonString(item_json, "type");
    if (!item_type) {
        return std::unexpected(item_type.error());
    }
    if (*item_type == "function_call") {
        auto call_id = requiredJsonString(item_json, "call_id");
        auto name = requiredJsonString(item_json, "name");
        auto arguments = requiredJsonString(item_json, "arguments");
        if (!call_id || !name || !arguments) {
            return std::unexpected("Invalid function_call response item");
        }
        state.response.tool_calls.push_back({std::move(*call_id), std::move(*name), std::move(*arguments)});
    } else if (*item_type == "message") {
        auto message = microcodex::responseMessage(item_json);
        if (!message) {
            return std::unexpected(message.error());
        }
        const auto& message_value = *message;
        if (message_value) {
            state.fallback_text += message_value->text;
        }
    }
    return {};
}

std::expected<void, std::string> handleResponseCompleted(const std::string_view data, StreamState& state) {
    auto response = findJsonMember(data, "response");
    if (!response) {
        return std::unexpected(response.error());
    }
    const std::optional<std::string_view>& response_json = *response;
    if (!response_json) {
        return std::unexpected("response.completed has no response");
    }

    // Some compatible endpoints omit output_text.delta and provide
    // only the final message item. Emit that fallback exactly once.
    if (state.response.text.empty() && !state.fallback_text.empty()) {
        state.response.text = state.fallback_text;
        emitTextDelta(state, state.fallback_text);
    }
    if (const std::optional<std::string_view> usage = optionalMember(*response_json, "usage")) {
        if (const std::optional<std::string_view> input_tokens = optionalMember(*usage, "input_tokens")) {
            state.response.input_tokens = unsignedJsonValue(*input_tokens).value_or(0);
        }
    }
    state.completed = true;
    return {};
}

std::string responseFailedMessage(const std::string_view data) {
    if (const std::optional<std::string_view> response = optionalMember(data, "response")) {
        if (std::optional<std::string> message = nestedStringMember(*response, {.outer = "error", .key = "message"})) {
            return std::move(*message);
        }
    }
    return "The Codex response failed";
}

std::string responseIncompleteMessage(const std::string_view data) {
    std::string reason = "unknown reason";
    if (const std::optional<std::string_view> response = optionalMember(data, "response")) {
        if (std::optional<std::string> value =
                nestedStringMember(*response, {.outer = "incomplete_details", .key = "reason"})) {
            reason = std::move(*value);
        }
    }
    return std::string(incomplete_response_prefix) + reason;
}

std::string errorEventMessage(const std::string_view data) {
    if (std::optional<std::string> message = nestedStringMember(data, {.outer = "error", .key = "message"})) {
        return std::move(*message);
    }
    if (std::optional<std::string> message = optionalStringMember(data, "message")) {
        return std::move(*message);
    }
    return "The Codex API returned an error event";
}

std::expected<void, std::string> handleEvent(const std::string_view data, StreamState& state) {
    if (data.empty() || data == "[DONE]") {
        return {};
    }

    auto type = requiredJsonString(data, "type");
    if (!type) {
        return std::unexpected("Invalid Responses API event: " + type.error());
    }

    if (*type == "response.output_text.delta") return handleOutputTextDelta(data, state);
    if (*type == "response.output_item.done") return handleOutputItemDone(data, state);
    if (*type == "response.completed") return handleResponseCompleted(data, state);
    if (*type == "response.failed") return std::unexpected(responseFailedMessage(data));
    if (*type == "response.incomplete") return std::unexpected(responseIncompleteMessage(data));
    if (*type == "error") return std::unexpected(errorEventMessage(data));

    // Creation, progress, and metadata events do not affect this minimal
    // turn runner. Final response items above are the durable state.
    return {};
}

std::expected<void, std::string> finishEvent(StreamState& state) {
    if (state.event_data.empty()) {
        return {};
    }
    const std::string data = std::move(state.event_data);
    state.event_data.clear();
    return handleEvent(data, state);
}

std::expected<void, std::string> handleSseLine(std::string_view line, StreamState& state) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    if (line.empty()) {
        return finishEvent(state);
    }
    if (line.starts_with("data:")) {
        line.remove_prefix(5);
        if (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        if (!state.event_data.empty()) {
            state.event_data += '\n';
        }
        state.event_data += line;
    }
    return {};
}

std::expected<void, std::string> consumeSse(const std::string_view bytes, StreamState& state) {
    state.line_buffer += bytes;
    std::size_t line_end = 0;
    while ((line_end = state.line_buffer.find('\n')) != std::string::npos) {
        const std::string line = state.line_buffer.substr(0, line_end);
        state.line_buffer.erase(0, line_end + 1);
        auto result = handleSseLine(line, state);
        if (!result) {
            return result;
        }
    }
    return {};
}

std::expected<void, std::string> finishSse(StreamState& state) {
    if (!state.line_buffer.empty()) {
        auto line = handleSseLine(state.line_buffer, state);
        state.line_buffer.clear();
        if (!line) {
            return line;
        }
    }
    return finishEvent(state);
}

std::expected<void, std::string> receiveResponseBody(const std::string_view data, void* user_data) {
    return consumeSse(data, *static_cast<StreamState*>(user_data));
}

std::string trim(std::string_view value) {
    while (!value.empty() && (std::isspace(static_cast<unsigned char>(value.front())) != 0)) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (std::isspace(static_cast<unsigned char>(value.back())) != 0)) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

bool equalsIgnoringCase(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

std::expected<void, std::string> receiveResponseHeader(const std::string_view line, void* user_data) {
    auto& state = *static_cast<StreamState*>(user_data);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos && equalsIgnoringCase(line.substr(0, colon), "x-codex-turn-state")) {
        state.turn_state = trim(line.substr(colon + 1));
    }
    return {};
}

std::string responseErrorMessage(const std::string_view body, const long status) {
    std::string prefix = "Codex API returned HTTP " + std::to_string(status);
    if (const std::optional<std::string> message = nestedStringMember(body, {.outer = "error", .key = "message"})) {
        return prefix + ": " + *message;
    }
    if (const std::optional<std::string> detail = optionalStringMember(body, "detail")) {
        return prefix + ": " + *detail;
    }
    return prefix;
}

// sendUserMessage() installs one stop source for the lifetime of a turn.
// This guard clears it on every return path, including allocation and
// std::future exceptions.
class RunningTurnGuard {
public:
    RunningTurnGuard(std::mutex& mutex, bool& running, std::optional<std::stop_source>& stop_source)
        : mutex_(mutex), running_(running), stop_source_(stop_source) {}

    RunningTurnGuard(const RunningTurnGuard&) = delete;
    RunningTurnGuard& operator=(const RunningTurnGuard&) = delete;
    RunningTurnGuard(RunningTurnGuard&&) = delete;
    RunningTurnGuard& operator=(RunningTurnGuard&&) = delete;

    ~RunningTurnGuard() {
        const std::lock_guard lock(mutex_);
        stop_source_.reset();
        running_ = false;
    }

private:
    std::mutex& mutex_;
    bool& running_;
    std::optional<std::stop_source>& stop_source_;
};

// Rejects request settings that would produce an invalid request.
std::expected<void, std::string> validateRequestConfig(const microcodex::CodexApiConfig& config) {
    if (config.access_token.empty()) {
        return std::unexpected("Codex access token cannot be empty");
    }
    if (config.model.empty()) {
        return std::unexpected("Codex model cannot be empty");
    }
    if (config.endpoint.empty()) {
        return std::unexpected("Codex endpoint cannot be empty");
    }
    if (config.reasoning_effort.empty()) {
        return std::unexpected("Reasoning effort cannot be empty");
    }
    if (config.idle_timeout_seconds <= 0) {
        return std::unexpected("Idle timeout must be greater than zero");
    }
    if (config.maximum_tool_rounds == 0) {
        return std::unexpected("Maximum tool rounds must be greater than zero");
    }
    if (config.maximum_parallel_tool_calls == 0) {
        return std::unexpected("Maximum parallel tool calls must be greater than zero");
    }
    if (containsNewline(config.access_token) || containsNewline(config.account_id)) {
        return std::unexpected("Credentials cannot contain a newline");
    }
    return {};
}

std::expected<void, std::string> validateRequestTools(
    const std::span<const std::shared_ptr<const microcodex::ToolBase>> tools) {
    std::unordered_set<std::string> tool_names;
    for (const auto& tool : tools) {
        if (tool == nullptr) {
            return std::unexpected("Tools cannot contain null entries");
        }
        if (tool->name().empty()) {
            return std::unexpected("Tool names cannot be empty");
        }
        if (!tool_names.emplace(tool->name()).second) {
            return std::unexpected("Duplicate tool name '" + std::string(tool->name()) + "'");
        }
    }
    return {};
}

// Appends leading_item, the input items, then final_item as JSON array
// elements. Empty leading and final items are skipped.
void appendInputItems(std::string& body, const std::string_view leading_item, const std::span<const std::string> items,
                      const std::string_view final_item) {
    const std::size_t start = body.size();
    const auto append = [&body, start](const std::string_view item) {
        if (body.size() != start) body += ',';
        body += item;
    };
    if (!leading_item.empty()) append(leading_item);
    for (const std::string& item : items) append(item);
    if (!final_item.empty()) append(final_item);
}

// The global AGENTS.md text, wrapped the way the model expects user
// instructions.
std::string userInstructionsItem(const std::string_view text) {
    if (text.empty()) return {};
    return microcodex::userMessageItem("# AGENTS.md instructions\n\n<INSTRUCTIONS>\n" + std::string(text) +
                                       "\n</INSTRUCTIONS>");
}

void appendToolDefinitions(std::string& body,
                           const std::span<const std::shared_ptr<const microcodex::ToolBase>> tools) {
    bool first = true;
    for (const auto& tool : tools) {
        if (!first) {
            body += ',';
        }
        body += tool->toJsonString();
        first = false;
    }
}

// Pairs each function call that will not run with an explicit
// non-execution result, so the saved turn remains valid input for a later
// continuation.
void appendUnexecutedToolOutputs(std::vector<std::string>& items,
                                 const std::span<const microcodex::CodexToolCall> calls) {
    for (const microcodex::CodexToolCall& call : calls) {
        items.push_back(microcodex::toolOutputItem({
            .call_id = call.call_id,
            .output = "Error: Tool call was not executed because Codex exceeded the maximum number of tool rounds",
        }));
    }
}

std::expected<void, std::string> validateToolCalls(const std::span<const microcodex::CodexToolCall> calls,
                                                   const std::size_t maximum_parallel_calls) {
    if (calls.empty()) {
        return std::unexpected("At least one tool call is required");
    }
    if (calls.size() > maximum_parallel_calls) {
        return std::unexpected("Codex requested more than the configured maximum number of parallel tool calls");
    }

    std::unordered_set<std::string> call_ids;
    for (const microcodex::CodexToolCall& call : calls) {
        if (call.call_id.empty()) {
            return std::unexpected("Tool call ID cannot be empty");
        }
        if (call.name.empty()) {
            return std::unexpected("Tool name cannot be empty");
        }
        if (!call_ids.emplace(call.call_id).second) {
            return std::unexpected("Duplicate tool call ID '" + call.call_id + "'");
        }
    }
    return {};
}

// One launched tool call. Result is CodexApi's private result type; the
// helpers below deduce it instead of naming it.
template <typename Result>
struct ToolTask {
    std::future<Result> future;
    std::stop_source stop_source;
    // Anchors the task's timeout deadline: with several stuck tools,
    // waiting a fresh `timeout` per task would let the turn take up to
    // N x timeout, so each task waits only until its own deadline.
    std::chrono::steady_clock::time_point start_time;
    // Set when the task is stopped for timing out. Overdue tasks are
    // stopped together and share one grace period, so a batch of stuck
    // tools cannot stack their grace periods either.
    std::chrono::steady_clock::time_point grace_deadline;
};

// How long a stopped tool gets to observe the stop and exit.
constexpr std::chrono::seconds tool_stop_grace_period(2);

// The deadline is anchored at the task's launch, not at the moment the
// caller reaches it: otherwise parallel stuck tools would each consume a
// full timeout and the turn could take N x timeout. A task already stopped
// for timing out counts as timed out even if it exited cooperatively, so it
// is not reported as interrupted.
template <typename Result>
bool toolTaskTimedOut(ToolTask<Result>& task, const std::chrono::seconds timeout) {
    return timeout.count() > 0 && (task.grace_deadline != std::chrono::steady_clock::time_point{} ||
                                   task.future.wait_until(task.start_time + timeout) == std::future_status::timeout);
}

// Asks every overdue, unfinished tool in tasks to stop at once, so they all
// share one grace period.
template <typename Result>
void stopOverdueToolTasks(const std::span<ToolTask<Result>> tasks, const std::chrono::seconds timeout) {
    const auto now = std::chrono::steady_clock::now();
    for (ToolTask<Result>& task : tasks) {
        if (task.start_time + timeout <= now &&
            task.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            task.stop_source.request_stop();
            task.grace_deadline = now + tool_stop_grace_period;
        }
    }
}

// Gives a cooperative tool its grace period to observe the stop and exit
// (the bash tool kills its child process on stop). A tool that ignores the
// stop is left to a detached reaper thread: futures from std::async block
// in their destructor until the task finishes, so a still-running task must
// never be destroyed here. Its result is discarded either way.
template <typename Result>
void releaseTimedOutToolTask(ToolTask<Result>& task) {
    if (task.future.wait_until(task.grace_deadline) != std::future_status::ready) {
        std::thread([future = std::move(task.future)] { future.wait(); }).detach();
    }
}

// Reads a task that finished within its timeout, turning a task failure
// into an error result for that call.
template <typename Result>
Result finishedToolTaskResult(std::future<Result>& future, const microcodex::CodexToolCall& call) {
    try {
        return future.get();
    } catch (const std::exception& error) {
        return Result{
            .output = {.call_id = call.call_id, .output = std::string("Error: Tool task failed: ") + error.what()},
            .edit = {},
            .succeeded = false};
    } catch (...) {
        return Result{
            .output = {.call_id = call.call_id, .output = "Error: Tool task failed"}, .edit = {}, .succeeded = false};
    }
}

}  // namespace

namespace microcodex {

std::span<const std::string_view> reasoningEffortValues() noexcept { return reasoning_effort_values; }

std::expected<void, std::string> validateReasoningEffort(const std::string_view effort, const std::string_view source) {
    std::string list;
    for (const std::string_view value : reasoning_effort_values) {
        if (value == effort) return {};
        if (!list.empty()) list += ", ";
        list += value;
    }
    if (effort.empty()) {
        return std::unexpected(std::string(source) + " requires an effort value (expected one of: " + list + ")");
    }
    return std::unexpected(std::string(source) + ": unsupported reasoning effort '" + std::string(effort) +
                           "' (expected one of: " + list + ")");
}

CodexApi::CodexApi(CodexApiConfig config)
    : config_(std::move(config)),
      user_instructions_item_(userInstructionsItem(config_.user_instructions)),
      compactor_(config_.compaction),
      installation_id_(makeUuid()),
      session_id_(makeUuid()) {}

CodexApi::CodexApi(CodexApiConfig config, CodexEventEmitter& events) : CodexApi(std::move(config)) {
    events_ = &events;
}

std::expected<void, std::string> CodexApi::initializeConversation() {
    const std::lock_guard lock(turn_mutex_);
    if (conversation_initialized_) return {};
    if (turn_running_) {
        return std::unexpected("Cannot initialize a conversation while a turn is running");
    }
    if (!config_.persist_conversation) {
        conversation_initialized_ = true;
        return {};
    }

    if (config_.resume_conversation) {
        auto file = ConversationFile::open(*config_.resume_conversation);
        if (!file) return std::unexpected(file.error());
        auto resumed = file->resume();
        if (!resumed) return std::unexpected(resumed.error());
        // Resuming with another model, through --model, is a model change too.
        if (resumed->metadata.model != config_.model) {
            auto saved = file->appendModel(config_.model);
            if (!saved) return std::unexpected(saved.error());
        }

        session_id_ = resumed->metadata.id;
        input_items_ = std::move(resumed->input_items);
        completed_turns_ = std::move(resumed->completed_turns);
        has_summary_ = resumed->has_summary;
        compaction_generation_ = resumed->compaction_generation;
        turn_number_ = resumed->next_turn_number - 1;
        conversation_file_.emplace(std::move(*file));
        conversation_initialized_ = true;
        return {};
    }

    auto directory = conversationDirectory();
    if (!directory) return std::unexpected(directory.error());
    std::error_code error;
    const std::filesystem::path working_directory = std::filesystem::current_path(error);
    if (error) {
        return std::unexpected("Could not determine the working directory: " + error.message());
    }
    const ConversationMetadata metadata{
        .version = 1,
        .id = session_id_,
        .created_at = currentTimestamp(),
        .working_directory = working_directory.string(),
        .model = config_.model,
    };
    auto file = ConversationFile::create(*directory, metadata);
    if (!file) return std::unexpected(file.error());
    conversation_file_.emplace(std::move(*file));
    conversation_initialized_ = true;
    return {};
}

std::expected<CodexApiResponse, std::string> CodexApi::sendUserMessage(const std::string_view message) {
    if (message.empty()) {
        return std::unexpected("User message cannot be empty");
    }
    auto initialized = initializeConversation();
    if (!initialized) return std::unexpected(initialized.error());

    const std::stop_source turn_stop;
    {
        const std::lock_guard lock(turn_mutex_);
        if (shutdown_requested_) {
            return std::unexpected("CodexApi has been shut down");
        }
        if (turn_running_) {
            return std::unexpected("A Codex turn is already running");
        }
        turn_running_ = true;
        active_turn_stop_ = turn_stop;
    }
    const RunningTurnGuard running_turn(turn_mutex_, turn_running_, active_turn_stop_);

    const std::size_t previous_turn = turn_number_;
    const std::string previous_turn_state = turn_state_;
    const std::string turn_id = makeUuid();

    std::size_t turn_start = input_items_.size();
    auto compacted = compactContext(turn_stop.get_token(), turn_start, false);
    if (!compacted) {
        emitEvent({.type = CodexEventType::Error,
                   .turn_id = turn_id,
                   .call_id = {},
                   .tool_name = {},
                   .text = compacted.error(),
                   .edit = {},
                   .succeeded = false});
        return std::unexpected(compacted.error());
    }

    turn_state_.clear();
    ++turn_number_;
    input_items_.push_back(userMessageItem(message));
    emitEvent({.type = CodexEventType::TurnStarted,
               .turn_id = turn_id,
               .call_id = {},
               .tool_name = {},
               .text = {},
               .edit = {}});

    compacted = compactContext(turn_stop.get_token(), turn_start, false);
    if (!compacted) {
        input_items_.resize(turn_start);
        turn_number_ = previous_turn;
        turn_state_ = previous_turn_state;
        emitEvent({.type = CodexEventType::Error,
                   .turn_id = turn_id,
                   .call_id = {},
                   .tool_name = {},
                   .text = compacted.error(),
                   .edit = {},
                   .succeeded = false});
        return std::unexpected(compacted.error());
    }

    auto response = requestWithToolExecution(turn_stop.get_token(), turn_id, turn_start);
    if (!response) {
        const bool interrupted = turn_stop.stop_requested();
        const bool usage_limited = isTurnUsageLimitError(response.error());
        const bool tool_round_limited = isToolRoundLimitError(response.error());
        const bool remote_failed = isRemoteFailureError(response.error());
        if (interrupted || usage_limited || tool_round_limited || remote_failed) {
            // Keep every complete response item and partial assistant text.
            // The marker makes the termination explicit to a later
            // "continue" while preserving one durable turn boundary.
            input_items_.push_back(turnAbortedItem(response.error()));
            if (conversation_file_) {
                const auto turn_items =
                    std::span<const std::string>(input_items_).subspan(turn_start, input_items_.size() - turn_start);
                auto saved = conversation_file_->appendTurn(turn_number_, turn_id, turn_items);
                if (!saved) {
                    input_items_.resize(turn_start);
                    turn_number_ = previous_turn;
                    turn_state_ = previous_turn_state;
                    emitEvent({.type = CodexEventType::Error,
                               .turn_id = turn_id,
                               .call_id = {},
                               .tool_name = {},
                               .text = saved.error(),
                               .edit = {},
                               .succeeded = false});
                    return std::unexpected(saved.error());
                }
            }
            completed_turns_.push_back({
                .number = turn_number_,
                .end = input_items_.size(),
            });
            emitEvent({.type = CodexEventType::TurnInterrupted,
                       .turn_id = turn_id,
                       .call_id = {},
                       .tool_name = {},
                       .text = response.error(),
                       .edit = {}});
            return std::unexpected(response.error());
        }

        // Other failures remain transactional: restore the last terminal
        // turn even after successful intermediate tool rounds.
        input_items_.resize(turn_start);
        turn_number_ = previous_turn;
        turn_state_ = previous_turn_state;
        emitEvent({.type = CodexEventType::Error,
                   .turn_id = turn_id,
                   .call_id = {},
                   .tool_name = {},
                   .text = response.error(),
                   .edit = {},
                   .succeeded = false});
        return std::unexpected(response.error());
    }

    if (conversation_file_) {
        const auto turn_items =
            std::span<const std::string>(input_items_).subspan(turn_start, input_items_.size() - turn_start);
        auto saved = conversation_file_->appendTurn(turn_number_, turn_id, turn_items);
        if (!saved) {
            input_items_.resize(turn_start);
            turn_number_ = previous_turn;
            turn_state_ = previous_turn_state;
            emitEvent({.type = CodexEventType::Error,
                       .turn_id = turn_id,
                       .call_id = {},
                       .tool_name = {},
                       .text = saved.error(),
                       .edit = {},
                       .succeeded = false});
            return std::unexpected(saved.error());
        }
    }
    completed_turns_.push_back({
        .number = turn_number_,
        .end = input_items_.size(),
    });

    emitEvent({.type = CodexEventType::TurnCompleted,
               .turn_id = turn_id,
               .call_id = {},
               .tool_name = {},
               .text = response->text,
               .edit = {}});
    return response;
}

void CodexApi::interrupt() noexcept {
    const std::lock_guard lock(turn_mutex_);
    if (active_turn_stop_) {
        active_turn_stop_->request_stop();
    }
}

std::expected<void, std::string> CodexApi::resetConversation() {
    const std::lock_guard lock(turn_mutex_);
    if (turn_running_) {
        return std::unexpected("Cannot reset the conversation while a turn is running");
    }
    if (shutdown_requested_) {
        return std::unexpected("CodexApi has been shut down");
    }

    input_items_.clear();
    completed_turns_.clear();
    conversation_file_.reset();
    conversation_initialized_ = false;
    config_.resume_conversation.reset();
    has_summary_ = false;
    compaction_generation_ = 0;
    reported_input_tokens_ = 0;
    session_id_ = makeUuid();
    turn_state_.clear();
    turn_number_ = 0;
    return {};
}

void CodexApi::shutdown() noexcept {
    const std::lock_guard lock(turn_mutex_);
    shutdown_requested_ = true;
    if (active_turn_stop_) {
        active_turn_stop_->request_stop();
    }
}

std::expected<CodexApiStatus, std::string> CodexApi::status() const {
    const std::lock_guard lock(turn_mutex_);
    if (turn_running_) {
        return std::unexpected("Cannot read the agent status while a turn is running");
    }
    const ContextUsage usage{
        .reported_input_tokens = reported_input_tokens_,
        .estimated_tokens = estimatedContextTokens(),
    };
    std::vector<std::string> models;
    models.reserve(config_.models.size());
    for (const ModelContextLimits& model : config_.models) models.push_back(model.name);
    return CodexApiStatus{
        .model = config_.model,
        .reasoning_effort = config_.reasoning_effort,
        .saved_turns = conversation_file_ ? conversation_file_->turnCount() : 0,
        .context_tokens = std::max(usage.reported_input_tokens, usage.estimated_tokens),
        .context_limit_tokens = config_.compaction.context_limit_tokens,
        .compact_at_tokens = config_.compaction.compact_at_tokens,
        .compaction_pending = compactor_.needed(usage),
        .models = std::move(models),
    };
}

std::expected<void, std::string> CodexApi::setReasoningEffort(std::string effort) {
    auto valid = validateReasoningEffort(effort, "Reasoning effort");
    if (!valid) return valid;
    const std::lock_guard lock(turn_mutex_);
    if (turn_running_) {
        return std::unexpected("Cannot change the reasoning effort while a turn is running");
    }
    config_.reasoning_effort = std::move(effort);
    return {};
}

std::expected<void, std::string> CodexApi::setModel(std::string model) {
    const std::lock_guard lock(turn_mutex_);
    if (turn_running_) {
        return std::unexpected("Cannot change the model while a turn is running");
    }
    if (config_.models.empty()) {
        return std::unexpected("Cannot change the model without the models catalog");
    }
    const ModelContextLimits* limits = findModelContextLimits(config_.models, model);
    if (limits == nullptr) {
        return std::unexpected("Model '" + model + "' is not in the models catalog");
    }
    // Save first, so a failed write leaves the model unchanged. Before the
    // first turn there is no file yet, and it is created with the new model.
    if (conversation_file_ && model != config_.model) {
        auto saved = conversation_file_->appendModel(model);
        if (!saved) return saved;
    }
    CompactionConfig compaction = compactionConfigForModel(*limits, config_.compaction.retained_context_tokens,
                                                           config_.compaction.maximum_summary_bytes);
    if (config_.compact_at_tokens_override) compaction.compact_at_tokens = *config_.compact_at_tokens_override;
    config_.compaction = compaction;
    compactor_ = ContextCompactor(compaction);
    config_.model = std::move(model);
    return {};
}

bool CodexApi::turnInProgress() const noexcept {
    const std::lock_guard lock(turn_mutex_);
    return turn_running_;
}

std::expected<std::vector<SavedTurn>, std::string> CodexApi::readHistoryBefore(const std::size_t cursor,
                                                                               const std::size_t maximum_bytes) const {
    const std::lock_guard lock(turn_mutex_);
    if (turn_running_) {
        return std::unexpected("Cannot read saved history while a turn is running");
    }
    if (!conversation_file_) return std::vector<SavedTurn>{};
    return conversation_file_->readTurnsBefore(cursor, maximum_bytes);
}

std::size_t CodexApi::savedTurnCount() const {
    const std::lock_guard lock(turn_mutex_);
    return conversation_file_ ? conversation_file_->turnCount() : 0;
}

std::size_t CodexApi::estimatedContextTokens() const {
    const std::size_t history = estimateContextTokens(input_items_);
    const std::size_t instructions = estimateContextTokens(std::span(&user_instructions_item_, 1));
    return history > std::numeric_limits<std::size_t>::max() - instructions ? std::numeric_limits<std::size_t>::max()
                                                                            : history + instructions;
}

std::expected<std::string, std::string> CodexApi::buildRequestBody(const std::span<const std::string> items,
                                                                   const std::string_view instructions,
                                                                   const bool include_tools,
                                                                   const std::string_view leading_item,
                                                                   const std::string_view final_item) const {
    auto valid_config = validateRequestConfig(config_);
    if (!valid_config) return std::unexpected(valid_config.error());
    if (items.empty() && final_item.empty()) {
        return std::unexpected("Conversation has no input");
    }
    if (include_tools) {
        auto valid_tools = validateRequestTools(config_.tools);
        if (!valid_tools) return std::unexpected(valid_tools.error());
    }

    const std::string window_id = session_id_ + ":" + std::to_string(compaction_generation_);
    std::string body = "{\"model\":";
    appendJsonString(body, config_.model);
    body += ",\"instructions\":";
    appendJsonString(body, instructions);
    body += ",\"input\":[";
    appendInputItems(body, leading_item, items, final_item);
    body += R"(],"tools":[)";
    if (include_tools) {
        appendToolDefinitions(body, config_.tools);
    }
    body += ']';

    if (include_tools) {
        body += R"(,"tool_choice":"auto","parallel_tool_calls":true)";
    }
    body += R"(,"reasoning":{"effort":)";
    appendJsonString(body, wireReasoningEffort(config_.reasoning_effort));
    body += R"(},"store":false,"stream":true,"include":["reasoning.encrypted_content"],"prompt_cache_key":)";
    appendJsonString(body, session_id_);
    body += R"(,"client_metadata":{"x-codex-installation-id":)";
    appendJsonString(body, installation_id_);
    body += ",\"session_id\":";
    appendJsonString(body, session_id_);
    body += ",\"thread_id\":";
    appendJsonString(body, session_id_);
    body += ",\"x-codex-window-id\":";
    appendJsonString(body, window_id);
    body += "}}";
    return body;
}

CodexApi::ToolExecutionResult CodexApi::executeToolCall(const std::span<const std::shared_ptr<const ToolBase>> tools,
                                                        const CodexToolCall& call, const std::stop_token stop_token,
                                                        const std::size_t maximum_output_bytes) {
    try {
        if (stop_token.stop_requested()) {
            return {.output = {.call_id = call.call_id, .output = "Error: Tool execution interrupted"},
                    .edit = {},
                    .succeeded = false};
        }

        const auto tool = std::ranges::find_if(
            tools, [&call](const auto& candidate) { return candidate != nullptr && candidate->name() == call.name; });
        auto result = tool == tools.end()
                          ? std::expected<ToolResult, std::string>(std::unexpected("Unknown tool '" + call.name + "'"))
                          : (*tool)->executeJson(call.arguments, stop_token);
        if (stop_token.stop_requested()) {
            return {.output = {.call_id = call.call_id, .output = "Error: Tool execution interrupted"},
                    .edit = {},
                    .succeeded = false};
        }
        if (!result) {
            return {.output = {.call_id = call.call_id, .output = "Error: " + result.error()},
                    .edit = {},
                    .succeeded = false};
        }
        return {.output = {.call_id = call.call_id,
                           .output = boundedToolOutput(std::move(result->output), maximum_output_bytes)},
                .edit = std::move(result->edit),
                .succeeded = true};
    } catch (const std::exception& error) {
        return {.output = {.call_id = call.call_id,
                           .output = std::string("Error: Tool threw an exception: ") + error.what()},
                .edit = {},
                .succeeded = false};
    } catch (...) {
        return {.output = {.call_id = call.call_id, .output = "Error: Tool threw an unknown exception"},
                .edit = {},
                .succeeded = false};
    }
}

std::expected<std::vector<CodexApi::ToolExecutionResult>, std::string> CodexApi::executeToolCalls(
    const std::span<const CodexToolCall> calls, const std::stop_token stop_token,
    const std::string_view turn_id) const {
    auto valid_calls = validateToolCalls(calls, config_.maximum_parallel_tool_calls);
    if (!valid_calls) return std::unexpected(valid_calls.error());

    std::vector<ToolTask<ToolExecutionResult>> tasks;
    tasks.reserve(calls.size());
    try {
        // Captured by value: a task that hits the execution timeout keeps
        // running on a reaper thread after this function returns, so it
        // must never touch CodexApi state through `this`.
        const auto tools = config_.tools;
        const std::size_t maximum_output_bytes = config_.maximum_tool_output_bytes;
        for (const CodexToolCall& call : calls) {
            emitEvent({
                .type = CodexEventType::ToolStarted,
                .turn_id = std::string(turn_id),
                .call_id = call.call_id,
                .tool_name = call.name,
                .text = call.arguments,
                .edit = {},
            });
            ToolTask<ToolExecutionResult> task;
            const std::stop_token task_token = task.stop_source.get_token();
            task.start_time = std::chrono::steady_clock::now();
            task.future = std::async(std::launch::async, [tools, call, task_token, maximum_output_bytes] {
                return executeToolCall(tools, call, task_token, maximum_output_bytes);
            });
            tasks.push_back(std::move(task));
        }
    } catch (const std::exception& error) {
        return std::unexpected(std::string("Could not start parallel tool execution: ") + error.what());
    } catch (...) {
        return std::unexpected("Could not start parallel tool execution");
    }

    // A cancelled turn stops every in-flight tool, including ones already
    // past their wait below.
    auto propagate_stop = [&tasks] {
        for (auto& task : tasks) {
            task.stop_source.request_stop();
        }
    };
    const std::stop_callback<decltype(propagate_stop)> stop_propagation(stop_token, std::move(propagate_stop));

    const std::chrono::seconds timeout(config_.tool_execution_timeout_seconds);
    std::vector<ToolExecutionResult> results;
    results.reserve(calls.size());
    for (std::size_t index = 0; index < tasks.size(); ++index) {
        ToolTask<ToolExecutionResult>& task = tasks[index];
        ToolExecutionResult result;
        if (toolTaskTimedOut(task, timeout)) {
            // Ask every overdue tool to stop at once, then report the
            // timeout without waiting for it: the turn must never wedge
            // behind a hung tool call.
            if (task.grace_deadline == std::chrono::steady_clock::time_point{}) {
                stopOverdueToolTasks(std::span(tasks).subspan(index), timeout);
            }
            result = {.output = {.call_id = calls[index].call_id,
                                 .output = "Error: Tool execution timed out after " +
                                           std::to_string(config_.tool_execution_timeout_seconds) + " seconds"},
                      .edit = {},
                      .succeeded = false};
            releaseTimedOutToolTask(task);
        } else {
            // All futures have already been launched. Reading them in this
            // order preserves the model's call order without serializing
            // their actual execution.
            result = finishedToolTaskResult(task.future, calls[index]);
        }

        emitEvent({
            .type = CodexEventType::ToolFinished,
            .turn_id = std::string(turn_id),
            .call_id = calls[index].call_id,
            .tool_name = calls[index].name,
            .text = result.output.output,
            .edit = result.edit,
            .succeeded = result.succeeded,
        });
        results.push_back(std::move(result));
    }
    return results;
}

std::expected<CodexApiResponse, std::string> CodexApi::requestWithToolExecution(const std::stop_token stop_token,
                                                                                const std::string_view turn_id,
                                                                                std::size_t& turn_start) {
    CodexApiResponse complete_response;
    std::size_t tool_rounds = 0;
    bool retried_context_limit = false;

    while (true) {
        if (stop_token.stop_requested()) {
            return std::unexpected(std::string(interrupted_message));
        }

        auto response = request(stop_token, turn_id);
        if (!response) {
            if (!retried_context_limit && isContextLimitError(response.error())) {
                auto compacted = compactContext(stop_token, turn_start, true);
                if (!compacted) return std::unexpected(compacted.error());
                retried_context_limit = true;
                continue;
            }
            return std::unexpected(response.error());
        }

        complete_response.text += response->text;
        complete_response.input_tokens = response->input_tokens;
        complete_response.tool_calls.insert(complete_response.tool_calls.end(), response->tool_calls.begin(),
                                            response->tool_calls.end());
        if (response->tool_calls.empty()) {
            return complete_response;
        }

        ++tool_rounds;
        if (tool_rounds > config_.maximum_tool_rounds) {
            // request() has already retained these function-call items.
            appendUnexecutedToolOutputs(input_items_, response->tool_calls);
            return std::unexpected(std::string(tool_round_limit_error));
        }

        auto results = executeToolCalls(response->tool_calls, stop_token, turn_id);
        if (!results) {
            return std::unexpected(results.error());
        }

        // Append the complete batch before observing cancellation. Function
        // calls must stay paired with outputs, including explicit
        // interruption outputs, so a later turn can safely continue.
        for (const ToolExecutionResult& result : *results) {
            input_items_.push_back(toolOutputItem(result.output));
        }
        if (stop_token.stop_requested()) {
            return std::unexpected(std::string(interrupted_message));
        }
        auto compacted = compactContext(stop_token, turn_start, false);
        if (!compacted) return std::unexpected(compacted.error());
    }
}

std::expected<CodexApiResponse, std::string> CodexApi::request(const std::stop_token stop_token,
                                                               const std::string_view turn_id) {
    auto request_body = buildRequestBody(input_items_, config_.instructions, true, user_instructions_item_);
    if (!request_body) {
        return std::unexpected(request_body.error());
    }

    ModelResponse partial;
    auto sampled = performRequest(*request_body, stop_token, turn_id, true, &partial);
    if (!sampled) {
        if (stop_token.stop_requested() || isTurnUsageLimitError(sampled.error()) ||
            isRemoteFailureError(sampled.error())) {
            // User cancellation, API response limits, and remote/transport
            // failures leave useful model output that the next turn must
            // see in order to continue.
            turn_state_ = std::move(partial.turn_state);
            reported_input_tokens_ = partial.response.input_tokens;
            bool has_assistant_message = false;
            for (std::string& item : partial.output_items) {
                auto type = responseItemType(item);
                if (type && *type == "message") has_assistant_message = true;
                input_items_.push_back(std::move(item));
            }
            if (!partial.response.text.empty() && !has_assistant_message) {
                input_items_.push_back(assistantMessageItem(partial.response.text));
            }
        }
        return std::unexpected(sampled.error());
    }

    turn_state_ = std::move(sampled->turn_state);
    reported_input_tokens_ = sampled->response.input_tokens;
    for (std::string& item : sampled->output_items) {
        input_items_.push_back(std::move(item));
    }
    return std::move(sampled->response);
}

std::expected<CodexApi::ModelResponse, std::string> CodexApi::performRequest(const std::string& request_body,
                                                                             const std::stop_token stop_token,
                                                                             const std::string_view turn_id,
                                                                             const bool emit_events,
                                                                             ModelResponse* partial_response) const {
    std::vector<std::string> headers{
        "Authorization: Bearer " + config_.access_token,
        "Content-Type: application/json",
        "Accept: text/event-stream",
        "originator: codex_cli_rs",
        "session-id: " + session_id_,
        "thread-id: " + session_id_,
        "x-client-request-id: " + session_id_,
        "x-codex-window-id: " + session_id_ + ":" + std::to_string(turn_number_ - 1),
    };
    if (!config_.account_id.empty()) headers.push_back("ChatGPT-Account-ID: " + config_.account_id);
    if (emit_events && !turn_state_.empty()) headers.push_back("x-codex-turn-state: " + turn_state_);

    StreamState state;
    state.turn_id = std::string(turn_id);
    state.events = emit_events ? events_ : nullptr;
    auto response = performHttpRequest(
        {
            .method = HttpMethod::Post,
            .url = config_.endpoint,
            .headers = headers,
            .body = request_body,
            .idle_timeout_seconds = config_.idle_timeout_seconds,
            .total_timeout_seconds = 0,
            .maximum_response_bytes = std::size_t{64} * 1024,
            .stop_token = stop_token,
        },
        receiveResponseBody, receiveResponseHeader, &state);
    if (!response) {
        const bool interrupted = stop_token.stop_requested();
        const bool usage_limited = isTurnUsageLimitError(response.error());
        const bool transport_failed = isTransportFailureError(response.error());
        if ((interrupted || usage_limited || transport_failed) && partial_response != nullptr) {
            // Cancellation, response limits, and transport failures are
            // resumable: preserve all complete items and streamed text
            // received before termination.
            *partial_response = ModelResponse{
                .response = std::move(state.response),
                .output_items = std::move(state.output_items),
                .turn_state = std::move(state.turn_state),
            };
        }
        if (interrupted) {
            return std::unexpected(std::string(interrupted_message));
        }
        return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        if (isServerErrorStatus(response->status) && partial_response != nullptr) {
            // Remote 5xx failures are resumable like interruptions: keep
            // whatever the model produced before the failure so the next
            // turn can continue from it.
            *partial_response = ModelResponse{
                .response = std::move(state.response),
                .output_items = std::move(state.output_items),
                .turn_state = std::move(state.turn_state),
            };
        }
        return std::unexpected(responseErrorMessage(response->body, response->status));
    }

    auto final_event = finishSse(state);
    if (!final_event) {
        return std::unexpected(final_event.error());
    }
    if (!state.completed) {
        return std::unexpected("Codex stream closed before response.completed");
    }

    return ModelResponse{
        .response = std::move(state.response),
        .output_items = std::move(state.output_items),
        .turn_state = std::move(state.turn_state),
    };
}

std::expected<std::string, std::string> CodexApi::requestSummary(const std::span<const std::string> items,
                                                                 const std::stop_token stop_token) {
    const std::string prompt = userMessageItem("Produce the conversation summary now.");
    auto body = buildRequestBody(items, ContextCompactor::summaryInstructions(), false, {}, prompt);
    if (!body) return std::unexpected(body.error());

    auto sampled = performRequest(*body, stop_token, makeUuid(), false);
    if (!sampled) return std::unexpected(sampled.error());
    if (sampled->response.text.empty()) {
        return std::unexpected("Compaction returned no summary text");
    }
    return std::move(sampled->response.text);
}

std::expected<void, std::string> CodexApi::compactContext(const std::stop_token stop_token,
                                                          std::size_t& protected_start, const bool force) {
    const ContextUsage usage{
        .reported_input_tokens = reported_input_tokens_,
        .estimated_tokens = estimatedContextTokens(),
    };
    if (!force && !compactor_.needed(usage)) return {};

    const ContextView view{
        .items = input_items_,
        .completed_turns = completed_turns_,
        .protected_start = protected_start,
        .has_summary = has_summary_,
        .generation = compaction_generation_,
    };
    auto plan = compactor_.plan(view, !force);
    if (!plan) {
        // Automatic checks can run again immediately after a successful
        // compaction. If no complete prefix can be reduced further, let the
        // request proceed; a real context-limit response will retry with force.
        if (!force) return {};
        return std::unexpected("Could not compact context: " + plan.error());
    }

    auto summary = requestSummary(std::span<const std::string>(input_items_).first(plan->summary_end), stop_token);
    if (!summary) return std::unexpected(summary.error());

    auto prepared = compactor_.prepare(view, *plan, std::move(*summary));
    if (!prepared) return std::unexpected(prepared.error());

    if (conversation_file_) {
        auto saved = conversation_file_->appendCheckpoint(prepared->checkpoint);
        if (!saved) return std::unexpected(saved.error());
    }

    const std::size_t protected_items = input_items_.size() - protected_start;
    input_items_ = std::move(prepared->input_items);
    completed_turns_ = std::move(prepared->completed_turns);
    protected_start = input_items_.size() - protected_items;
    has_summary_ = true;
    compaction_generation_ = prepared->checkpoint.generation;
    reported_input_tokens_ = 0;
    turn_state_.clear();
    return {};
}

void CodexApi::emitEvent(const CodexEvent& event) const noexcept {
    if (events_ != nullptr) {
        events_->emit(event);
    }
}

}  // namespace microcodex
