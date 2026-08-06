// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "api.h"
#include "http.h"
#include "json.h"
#include "response-item.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

    using microcodex::json::appendJsonString;
    using microcodex::json::findJsonMember;
    using microcodex::json::jsonStringMember;
    using microcodex::json::requiredJsonString;

    constexpr std::string_view interrupted_message = "Codex turn was interrupted";
    constexpr std::string_view incomplete_response_prefix = "The Codex response was incomplete: ";
    constexpr std::string_view turn_usage_limit_error =
        "The Codex response was incomplete: max_output_tokens";
    constexpr std::string_view tool_round_limit_error =
        "Codex exceeded the maximum number of tool rounds";
    std::string turnAbortedItem(const std::string_view error) {
        return microcodex::userMessageItem(
            "<turn_aborted>\n" + std::string(error) + "\n</turn_aborted>");
    }

    std::string makeUuid() {
        std::array<unsigned char, 16> bytes{};
        std::random_device random;
        for (unsigned char &byte : bytes) {
            byte = static_cast<unsigned char>(random());
        }
        bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
        bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);

        constexpr char hex[] = "0123456789abcdef";
        std::string uuid;
        uuid.reserve(36);
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) {
                uuid += '-';
            }
            uuid += hex[bytes[index] >> 4];
            uuid += hex[bytes[index] & 0x0f];
        }
        return uuid;
    }

    bool containsNewline(const std::string_view value) {
        return value.find_first_of("\r\n") != std::string_view::npos;
    }

    std::optional<std::size_t> unsignedJsonValue(const std::string_view value) {
        std::size_t result = 0;
        const char *begin = value.data();
        const char *end = begin + value.size();
        const auto [parsed_end, error] = std::from_chars(begin, end, result);
        if (error != std::errc{} || parsed_end != end) return std::nullopt;
        return result;
    }

    std::string currentTimestamp() {
        const std::time_t now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        char timestamp[32]{};
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return timestamp;
    }

    std::string boundedToolOutput(std::string output, const std::size_t maximum_bytes) {
        if (maximum_bytes == 0 || output.size() <= maximum_bytes) return output;

        const std::string marker = "\n... [tool output truncated from " +
                                   std::to_string(output.size()) + " bytes] ...\n";
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

    bool isTurnUsageLimitError(const std::string_view error) {
        return error == turn_usage_limit_error;
    }

    bool isToolRoundLimitError(const std::string_view error) {
        return error == tool_round_limit_error;
    }

    struct StreamState {
        microcodex::CodexApiResponse response;
        std::vector<std::string> output_items;
        std::string fallback_text;
        std::string line_buffer;
        std::string event_data;
        std::string turn_state;
        std::string turn_id;
        microcodex::CodexEventEmitter *events = nullptr;
        bool completed = false;
    };

    void emitTextDelta(StreamState &state, const std::string_view text) {
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

    std::expected<void, std::string> handleEvent(const std::string_view data, StreamState &state) {
        if (data.empty() || data == "[DONE]") {
            return {};
        }

        auto type = requiredJsonString(data, "type");
        if (!type) {
            return std::unexpected("Invalid Responses API event: " + type.error());
        }

        if (*type == "response.output_text.delta") {
            auto delta = requiredJsonString(data, "delta");
            if (!delta) {
                return std::unexpected(delta.error());
            }
            state.response.text += *delta;
            emitTextDelta(state, *delta);
            return {};
        }

        if (*type == "response.output_item.done") {
            auto item = findJsonMember(data, "item");
            if (!item) {
                return std::unexpected(item.error());
            }
            if (!*item) {
                return std::unexpected("response.output_item.done has no item");
            }

            // Keep the exact response item. Since requests use store=false, the
            // next sampling request must replay model messages, reasoning, and
            // function calls exactly as the server returned them.
            const std::string_view item_json = **item;
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
                if (*message) {
                    state.fallback_text += (*message)->text;
                }
            }
            return {};
        }

        if (*type == "response.completed") {
            auto response = findJsonMember(data, "response");
            if (!response) {
                return std::unexpected(response.error());
            }
            if (!*response) {
                return std::unexpected("response.completed has no response");
            }

            // Some compatible endpoints omit output_text.delta and provide
            // only the final message item. Emit that fallback exactly once.
            if (state.response.text.empty() && !state.fallback_text.empty()) {
                state.response.text = state.fallback_text;
                emitTextDelta(state, state.fallback_text);
            }
            auto usage = findJsonMember(**response, "usage");
            if (usage && *usage) {
                auto input_tokens = findJsonMember(**usage, "input_tokens");
                if (input_tokens && *input_tokens) {
                    state.response.input_tokens =
                        unsignedJsonValue(**input_tokens).value_or(0);
                }
            }
            state.completed = true;
            return {};
        }

        if (*type == "response.failed") {
            auto response = findJsonMember(data, "response");
            if (response && *response) {
                auto error = findJsonMember(**response, "error");
                if (error && *error) {
                    auto message = jsonStringMember(**error, "message");
                    if (message && *message) {
                        return std::unexpected(**message);
                    }
                }
            }
            return std::unexpected("The Codex response failed");
        }

        if (*type == "response.incomplete") {
            std::string reason = "unknown reason";
            auto response = findJsonMember(data, "response");
            if (response && *response) {
                auto details = findJsonMember(**response, "incomplete_details");
                if (details && *details) {
                    auto value = jsonStringMember(**details, "reason");
                    if (value && *value) {
                        reason = std::move(**value);
                    }
                }
            }
            return std::unexpected(std::string(incomplete_response_prefix) + reason);
        }

        if (*type == "error") {
            auto error = findJsonMember(data, "error");
            if (error && *error) {
                auto message = jsonStringMember(**error, "message");
                if (message && *message) {
                    return std::unexpected(**message);
                }
            }
            auto message = jsonStringMember(data, "message");
            if (message && *message) {
                return std::unexpected(**message);
            }
            return std::unexpected("The Codex API returned an error event");
        }

        // Creation, progress, and metadata events do not affect this minimal
        // turn runner. Final response items above are the durable state.
        return {};
    }

    std::expected<void, std::string> finishEvent(StreamState &state) {
        if (state.event_data.empty()) {
            return {};
        }
        std::string data = std::move(state.event_data);
        state.event_data.clear();
        return handleEvent(data, state);
    }

    std::expected<void, std::string> handleSseLine(std::string_view line, StreamState &state) {
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

    std::expected<void, std::string> consumeSse(const std::string_view bytes, StreamState &state) {
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

    std::expected<void, std::string> finishSse(StreamState &state) {
        if (!state.line_buffer.empty()) {
            auto line = handleSseLine(state.line_buffer, state);
            state.line_buffer.clear();
            if (!line) {
                return line;
            }
        }
        return finishEvent(state);
    }

    std::expected<void, std::string> receiveResponseBody(const std::string_view data, void *user_data) {
        return consumeSse(data, *static_cast<StreamState *>(user_data));
    }

    std::string trim(std::string_view value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.remove_suffix(1);
        }
        return std::string(value);
    }

    bool equalsIgnoringCase(const std::string_view left, const std::string_view right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (std::tolower(static_cast<unsigned char>(left[index])) != std::tolower(static_cast<unsigned char>(right[index]))) {
                return false;
            }
        }
        return true;
    }

    std::expected<void, std::string> receiveResponseHeader(const std::string_view line, void *user_data) {
        auto &state = *static_cast<StreamState *>(user_data);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos && equalsIgnoringCase(line.substr(0, colon), "x-codex-turn-state")) {
            state.turn_state = trim(line.substr(colon + 1));
        }
        return {};
    }

    std::string responseErrorMessage(const std::string_view body, const long status) {
        auto error = findJsonMember(body, "error");
        if (error && *error) {
            auto message = jsonStringMember(**error, "message");
            if (message && *message) {
                return "Codex API returned HTTP " + std::to_string(status) + ": " + **message;
            }
        }
        auto detail = jsonStringMember(body, "detail");
        if (detail && *detail) {
            return "Codex API returned HTTP " + std::to_string(status) + ": " + **detail;
        }
        return "Codex API returned HTTP " + std::to_string(status);
    }

    // sendUserMessage() installs one stop source for the lifetime of a turn.
    // This guard clears it on every return path, including allocation and
    // std::future exceptions.
    class RunningTurnGuard {
    public:
        RunningTurnGuard(std::mutex &mutex, bool &running, std::optional<std::stop_source> &stop_source) : mutex_(mutex), running_(running), stop_source_(stop_source) {}

        RunningTurnGuard(const RunningTurnGuard &) = delete;
        RunningTurnGuard &operator=(const RunningTurnGuard &) = delete;

        ~RunningTurnGuard() {
            std::lock_guard lock(mutex_);
            stop_source_.reset();
            running_ = false;
        }

    private:
        std::mutex &mutex_;
        bool &running_;
        std::optional<std::stop_source> &stop_source_;
    };

} // namespace

namespace microcodex {

    CodexApi::CodexApi(CodexApiConfig config)
        : config_(std::move(config)),
          compactor_(config_.compaction),
          installation_id_(makeUuid()),
          session_id_(makeUuid()) {}

    CodexApi::CodexApi(CodexApiConfig config, CodexEventEmitter &events) : CodexApi(std::move(config)) {
        events_ = &events;
    }

    std::expected<void, std::string> CodexApi::initializeConversation() {
        std::lock_guard lock(turn_mutex_);
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

            session_id_ = resumed->metadata.id;
            input_items_ = std::move(resumed->input_items);
            completed_turns_ = std::move(resumed->completed_turns);
            has_summary_ = resumed->has_summary;
            compaction_generation_ = resumed->compaction_generation;
            turn_number_ = static_cast<std::size_t>(resumed->next_turn_number - 1);
            conversation_file_.emplace(std::move(*file));
            conversation_initialized_ = true;
            return {};
        }

        auto directory = conversationDirectory();
        if (!directory) return std::unexpected(directory.error());
        std::error_code error;
        const std::filesystem::path working_directory =
            std::filesystem::current_path(error);
        if (error) {
            return std::unexpected("Could not determine the working directory: " +
                                   error.message());
        }
        ConversationMetadata metadata{
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

        std::stop_source turn_stop;
        {
            std::lock_guard lock(turn_mutex_);
            if (shutdown_requested_) {
                return std::unexpected("CodexApi has been shut down");
            }
            if (turn_running_) {
                return std::unexpected("A Codex turn is already running");
            }
            turn_running_ = true;
            active_turn_stop_ = turn_stop;
        }
        RunningTurnGuard running_turn(turn_mutex_, turn_running_, active_turn_stop_);

        const std::size_t previous_turn = turn_number_;
        const std::string previous_turn_state = turn_state_;
        const std::string turn_id = makeUuid();

        std::size_t turn_start = input_items_.size();
        auto compacted = compactContext(turn_stop.get_token(), turn_start, false);
        if (!compacted) {
            emitEvent({.type = CodexEventType::Error, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = compacted.error(), .edit = {}, .succeeded = false});
            return std::unexpected(compacted.error());
        }

        turn_state_.clear();
        ++turn_number_;
        input_items_.push_back(userMessageItem(message));
        emitEvent({.type = CodexEventType::TurnStarted, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = {}, .edit = {}});

        compacted = compactContext(turn_stop.get_token(), turn_start, false);
        if (!compacted) {
            input_items_.resize(turn_start);
            turn_number_ = previous_turn;
            turn_state_ = previous_turn_state;
            emitEvent({.type = CodexEventType::Error, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = compacted.error(), .edit = {}, .succeeded = false});
            return std::unexpected(compacted.error());
        }

        auto response = requestWithToolExecution(turn_stop.get_token(), turn_id, turn_start);
        if (!response) {
            const bool interrupted = turn_stop.stop_requested();
            const bool usage_limited = isTurnUsageLimitError(response.error());
            const bool tool_round_limited = isToolRoundLimitError(response.error());
            if (interrupted || usage_limited || tool_round_limited) {
                // Keep every complete response item and partial assistant text.
                // The marker makes the termination explicit to a later
                // "continue" while preserving one durable turn boundary.
                input_items_.push_back(turnAbortedItem(response.error()));
                if (conversation_file_) {
                    const auto turn_items = std::span<const std::string>(input_items_).subspan(
                        turn_start, input_items_.size() - turn_start);
                    auto saved = conversation_file_->appendTurn(turn_number_, turn_id, turn_items);
                    if (!saved) {
                        input_items_.resize(turn_start);
                        turn_number_ = previous_turn;
                        turn_state_ = previous_turn_state;
                        emitEvent({.type = CodexEventType::Error, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = saved.error(), .edit = {}, .succeeded = false});
                        return std::unexpected(saved.error());
                    }
                }
                completed_turns_.push_back({
                    .number = turn_number_,
                    .end = input_items_.size(),
                });
                emitEvent({.type = CodexEventType::TurnInterrupted, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = response.error(), .edit = {}});
                return std::unexpected(response.error());
            }

            // Other failures remain transactional: restore the last terminal
            // turn even after successful intermediate tool rounds.
            input_items_.resize(turn_start);
            turn_number_ = previous_turn;
            turn_state_ = previous_turn_state;
            emitEvent({.type = CodexEventType::Error, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = response.error(), .edit = {}, .succeeded = false});
            return std::unexpected(response.error());
        }

        if (conversation_file_) {
            const auto turn_items = std::span<const std::string>(input_items_).subspan(
                turn_start, input_items_.size() - turn_start);
            auto saved = conversation_file_->appendTurn(turn_number_, turn_id, turn_items);
            if (!saved) {
                input_items_.resize(turn_start);
                turn_number_ = previous_turn;
                turn_state_ = previous_turn_state;
                emitEvent({.type = CodexEventType::Error, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = saved.error(), .edit = {}, .succeeded = false});
                return std::unexpected(saved.error());
            }
        }
        completed_turns_.push_back({
            .number = turn_number_,
            .end = input_items_.size(),
        });

        emitEvent({.type = CodexEventType::TurnCompleted, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = response->text, .edit = {}});
        return response;
    }

    void CodexApi::interrupt() noexcept {
        std::lock_guard lock(turn_mutex_);
        if (active_turn_stop_) {
            active_turn_stop_->request_stop();
        }
    }

    std::expected<void, std::string> CodexApi::resetConversation() {
        std::lock_guard lock(turn_mutex_);
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
        std::lock_guard lock(turn_mutex_);
        shutdown_requested_ = true;
        if (active_turn_stop_) {
            active_turn_stop_->request_stop();
        }
    }

    bool CodexApi::turnInProgress() const noexcept {
        std::lock_guard lock(turn_mutex_);
        return turn_running_;
    }

    std::expected<std::vector<SavedTurn>, std::string> CodexApi::readHistoryBefore(const std::size_t cursor, const std::size_t maximum_bytes) const {
        std::lock_guard lock(turn_mutex_);
        if (turn_running_) {
            return std::unexpected("Cannot read saved history while a turn is running");
        }
        if (!conversation_file_) return std::vector<SavedTurn>{};
        return conversation_file_->readTurnsBefore(cursor, maximum_bytes);
    }

    std::size_t CodexApi::savedTurnCount() const {
        std::lock_guard lock(turn_mutex_);
        return conversation_file_ ? conversation_file_->turnCount() : 0;
    }

    std::expected<std::string, std::string> CodexApi::buildRequestBody(const std::span<const std::string> items, const std::string_view instructions, const bool include_tools, const std::string_view final_item) const {
        if (config_.access_token.empty()) {
            return std::unexpected("Codex access token cannot be empty");
        }
        if (config_.model.empty()) {
            return std::unexpected("Codex model cannot be empty");
        }
        if (config_.endpoint.empty()) {
            return std::unexpected("Codex endpoint cannot be empty");
        }
        if (config_.reasoning_effort.empty()) {
            return std::unexpected("Reasoning effort cannot be empty");
        }
        if (config_.idle_timeout_seconds <= 0) {
            return std::unexpected("Idle timeout must be greater than zero");
        }
        if (config_.maximum_tool_rounds == 0) {
            return std::unexpected("Maximum tool rounds must be greater than zero");
        }
        if (config_.maximum_parallel_tool_calls == 0) {
            return std::unexpected("Maximum parallel tool calls must be greater than zero");
        }
        if (containsNewline(config_.access_token) || containsNewline(config_.account_id)) {
            return std::unexpected("Credentials cannot contain a newline");
        }
        if (items.empty() && final_item.empty()) {
            return std::unexpected("Conversation has no input");
        }

        std::unordered_set<std::string> tool_names;
        if (include_tools) {
            for (const auto &tool : config_.tools) {
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
        }

        const std::string window_id =
            session_id_ + ":" + std::to_string(compaction_generation_);
        std::string body = "{\"model\":";
        appendJsonString(body, config_.model);
        body += ",\"instructions\":";
        appendJsonString(body, instructions);
        body += ",\"input\":[";
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (index != 0) {
                body += ',';
            }
            body += items[index];
        }
        if (!final_item.empty()) {
            if (!items.empty()) body += ',';
            body += final_item;
        }
        body += R"(],"tools":[)";
        if (include_tools) {
            for (std::size_t index = 0; index < config_.tools.size(); ++index) {
                if (index != 0) {
                    body += ',';
                }
                body += config_.tools[index]->toJsonString();
            }
        }
        body += ']';

        if (include_tools) {
            body += ",\"tool_choice\":\"auto\",\"parallel_tool_calls\":true";
        }
        body += ",\"reasoning\":{\"effort\":";
        appendJsonString(body, config_.reasoning_effort);
        body += "},\"store\":false,\"stream\":true,\"include\":[\"reasoning.encrypted_content\"],\"prompt_cache_key\":";
        appendJsonString(body, session_id_);
        body += ",\"client_metadata\":{\"x-codex-installation-id\":";
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

    CodexApi::ToolExecutionResult CodexApi::executeToolCall(const CodexToolCall &call, const std::stop_token stop_token) const {
        try {
            if (stop_token.stop_requested()) {
                return {{call.call_id, "Error: Tool execution interrupted"}, {}, false};
            }

            const auto tool = std::find_if(config_.tools.begin(), config_.tools.end(), [&call](const auto &candidate) {
                return candidate != nullptr && candidate->name() == call.name;
            });
            auto result = tool == config_.tools.end()
                              ? std::expected<ToolResult, std::string>(std::unexpected("Unknown tool '" + call.name + "'"))
                              : (*tool)->executeJson(call.arguments, stop_token);
            if (stop_token.stop_requested()) {
                return {{call.call_id, "Error: Tool execution interrupted"}, {}, false};
            }
            if (!result) {
                return {{call.call_id, "Error: " + result.error()}, {}, false};
            }
            return {{call.call_id,
                     boundedToolOutput(std::move(result->output),
                                       config_.maximum_tool_output_bytes)},
                    std::move(result->edit),
                    true};
        } catch (const std::exception &error) {
            return {{call.call_id, std::string("Error: Tool threw an exception: ") + error.what()}, {}, false};
        } catch (...) {
            return {{call.call_id, "Error: Tool threw an unknown exception"}, {}, false};
        }
    }

    std::expected<std::vector<CodexApi::ToolExecutionResult>, std::string> CodexApi::executeToolCalls(const std::span<const CodexToolCall> calls, const std::stop_token stop_token, const std::string_view turn_id) const {
        if (calls.empty()) {
            return std::unexpected("At least one tool call is required");
        }
        if (calls.size() > config_.maximum_parallel_tool_calls) {
            return std::unexpected("Codex requested more than the configured maximum number of parallel tool calls");
        }

        std::unordered_set<std::string> call_ids;
        for (const CodexToolCall &call : calls) {
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

        std::vector<std::future<ToolExecutionResult>> futures;
        futures.reserve(calls.size());
        try {
            for (const CodexToolCall &call : calls) {
                emitEvent({
                    .type = CodexEventType::ToolStarted,
                    .turn_id = std::string(turn_id),
                    .call_id = call.call_id,
                    .tool_name = call.name,
                    .text = call.arguments,
                    .edit = {},
                });
                futures.push_back(std::async(std::launch::async, [this, call, stop_token] {
                    return executeToolCall(call, stop_token);
                }));
            }
        } catch (const std::exception &error) {
            return std::unexpected(std::string("Could not start parallel tool execution: ") + error.what());
        } catch (...) {
            return std::unexpected("Could not start parallel tool execution");
        }

        std::vector<ToolExecutionResult> results;
        results.reserve(calls.size());
        for (std::size_t index = 0; index < futures.size(); ++index) {
            ToolExecutionResult result;
            try {
                // All futures have already been launched. Reading them in this
                // order preserves the model's call order without serializing
                // their actual execution.
                result = futures[index].get();
            } catch (const std::exception &error) {
                result = {{calls[index].call_id, std::string("Error: Tool task failed: ") + error.what()}, {}, false};
            } catch (...) {
                result = {{calls[index].call_id, "Error: Tool task failed"}, {}, false};
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

    std::expected<CodexApiResponse, std::string> CodexApi::requestWithToolExecution(const std::stop_token stop_token, const std::string_view turn_id, std::size_t &turn_start) {
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
            complete_response.tool_calls.insert(complete_response.tool_calls.end(), response->tool_calls.begin(), response->tool_calls.end());
            if (response->tool_calls.empty()) {
                return complete_response;
            }

            ++tool_rounds;
            if (tool_rounds > config_.maximum_tool_rounds) {
                // request() has already retained these function-call items. Pair
                // each one with an explicit non-execution result so the saved
                // turn remains valid input for a later continuation.
                for (const CodexToolCall &call : response->tool_calls) {
                    input_items_.push_back(toolOutputItem({
                        call.call_id,
                        "Error: Tool call was not executed because Codex exceeded the maximum number of tool rounds",
                    }));
                }
                return std::unexpected(std::string(tool_round_limit_error));
            }

            auto results = executeToolCalls(response->tool_calls, stop_token, turn_id);
            if (!results) {
                return std::unexpected(results.error());
            }

            // Append the complete batch before observing cancellation. Function
            // calls must stay paired with outputs, including explicit
            // interruption outputs, so a later turn can safely continue.
            for (const ToolExecutionResult &result : *results) {
                input_items_.push_back(toolOutputItem(result.output));
            }
            if (stop_token.stop_requested()) {
                return std::unexpected(std::string(interrupted_message));
            }
            auto compacted = compactContext(stop_token, turn_start, false);
            if (!compacted) return std::unexpected(compacted.error());
        }
    }

    std::expected<CodexApiResponse, std::string> CodexApi::request(const std::stop_token stop_token, const std::string_view turn_id) {
        auto request_body = buildRequestBody(input_items_, config_.instructions, true);
        if (!request_body) {
            return std::unexpected(request_body.error());
        }

        ModelResponse partial;
        auto sampled = performRequest(std::move(*request_body), stop_token, turn_id, true, &partial);
        if (!sampled) {
            if (stop_token.stop_requested() || isTurnUsageLimitError(sampled.error())) {
                // Both user cancellation and an API response limit leave useful
                // model output that the next turn must see in order to continue.
                turn_state_ = std::move(partial.turn_state);
                reported_input_tokens_ = partial.response.input_tokens;
                bool has_assistant_message = false;
                for (std::string &item : partial.output_items) {
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
        for (std::string &item : sampled->output_items) {
            input_items_.push_back(std::move(item));
        }
        return std::move(sampled->response);
    }

    std::expected<CodexApi::ModelResponse, std::string> CodexApi::performRequest(std::string request_body, const std::stop_token stop_token, const std::string_view turn_id, const bool emit_events, ModelResponse *partial_response) const {
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
        auto response = performHttpRequest({
            .method = HttpMethod::Post,
            .url = config_.endpoint,
            .headers = headers,
            .body = request_body,
            .idle_timeout_seconds = config_.idle_timeout_seconds,
            .total_timeout_seconds = 0,
            .maximum_response_bytes = 64 * 1024,
            .stop_token = stop_token,
        }, receiveResponseBody, receiveResponseHeader, &state);
        if (!response) {
            const bool interrupted = stop_token.stop_requested();
            const bool usage_limited = isTurnUsageLimitError(response.error());
            if ((interrupted || usage_limited) && partial_response != nullptr) {
                // Cancellation and response limits are resumable: preserve all
                // complete items and streamed text received before termination.
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
        if (response->status < 200 || response->status >= 300) return std::unexpected(responseErrorMessage(response->body, response->status));

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

    std::expected<std::string, std::string> CodexApi::requestSummary(const std::span<const std::string> items, const std::stop_token stop_token) {
        const std::string prompt = userMessageItem("Produce the conversation summary now.");
        auto body = buildRequestBody(items, compactor_.summaryInstructions(), false, prompt);
        if (!body) return std::unexpected(body.error());

        auto sampled = performRequest(std::move(*body), stop_token, makeUuid(), false);
        if (!sampled) return std::unexpected(sampled.error());
        if (sampled->response.text.empty()) {
            return std::unexpected("Compaction returned no summary text");
        }
        return std::move(sampled->response.text);
    }

    std::expected<void, std::string> CodexApi::compactContext(const std::stop_token stop_token, std::size_t &protected_start, const bool force) {
        const ContextUsage usage{
            .reported_input_tokens = reported_input_tokens_,
            .estimated_tokens = estimateContextTokens(input_items_),
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

        auto summary = requestSummary(
            std::span<const std::string>(input_items_).first(plan->summary_end),
            stop_token);
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

    void CodexApi::emitEvent(CodexEvent event) const noexcept {
        if (events_ != nullptr) {
            events_->emit(event);
        }
    }

} // namespace microcodex
