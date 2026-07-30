// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "api.h"
#include "json.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <exception>
#include <expected>
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
    using microcodex::json::jsonArrayElements;
    using microcodex::json::jsonStringMember;
    using microcodex::json::requiredJsonString;

    constexpr std::string_view interrupted_message = "Codex turn was interrupted";

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

    std::expected<std::string, std::string> assistantMessageText(const std::string_view item) {
        auto type = requiredJsonString(item, "type");
        if (!type) {
            return std::unexpected(type.error());
        }
        if (*type != "message") {
            return std::string{};
        }

        auto content = findJsonMember(item, "content");
        if (!content) {
            return std::unexpected(content.error());
        }
        if (!*content) {
            return std::unexpected("Response message has no content");
        }
        auto parts = jsonArrayElements(**content);
        if (!parts) {
            return std::unexpected(parts.error());
        }

        std::string text;
        for (const std::string_view part : *parts) {
            auto part_type = requiredJsonString(part, "type");
            if (!part_type) {
                return std::unexpected(part_type.error());
            }
            if (*part_type == "output_text") {
                auto part_text = requiredJsonString(part, "text");
                if (!part_text) {
                    return std::unexpected(part_text.error());
                }
                text += *part_text;
            }
        }
        return text;
    }

    struct StreamState {
        microcodex::CodexApiResponse response;
        std::vector<std::string> output_items;
        std::string fallback_text;
        std::string line_buffer;
        std::string event_data;
        std::string received_body;
        std::string error;
        std::string turn_state;
        std::string turn_id;
        microcodex::CodexEventEmitter *events = nullptr;
        std::stop_token stop_token;
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
                auto text = assistantMessageText(item_json);
                if (!text) {
                    return std::unexpected(text.error());
                }
                state.fallback_text += *text;
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
            return std::unexpected("The Codex response was incomplete: " + reason);
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

    std::size_t receiveBody(char *data, const std::size_t size, const std::size_t count, void *user_data) {
        const std::size_t byte_count = size * count;
        auto &state = *static_cast<StreamState *>(user_data);
        if (state.stop_token.stop_requested()) {
            return 0;
        }

        try {
            // Keep only a bounded copy for useful non-2xx HTTP error messages.
            constexpr std::size_t maximum_error_body = 64 * 1024;
            if (state.received_body.size() < maximum_error_body) {
                const std::size_t remaining = maximum_error_body - state.received_body.size();
                state.received_body.append(data, std::min(byte_count, remaining));
            }

            auto result = consumeSse(std::string_view(data, byte_count), state);
            if (!result) {
                state.error = result.error();
                return 0;
            }
        } catch (const std::exception &error) {
            state.error = std::string("Response callback failed: ") + error.what();
            return 0;
        } catch (...) {
            state.error = "Response callback failed";
            return 0;
        }

        return byte_count;
    }

    int transferProgress(void *user_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        const auto &state = *static_cast<const StreamState *>(user_data);
        return state.stop_token.stop_requested() ? 1 : 0;
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

    std::size_t receiveHeader(char *data, const std::size_t size, const std::size_t count, void *user_data) {
        const std::size_t byte_count = size * count;
        auto &state = *static_cast<StreamState *>(user_data);
        if (state.stop_token.stop_requested()) {
            return 0;
        }

        try {
            const std::string_view line(data, byte_count);
            const std::size_t colon = line.find(':');
            if (colon != std::string_view::npos && equalsIgnoringCase(line.substr(0, colon), "x-codex-turn-state")) {
                state.turn_state = trim(line.substr(colon + 1));
            }
        } catch (const std::exception &error) {
            state.error = std::string("Response header callback failed: ") + error.what();
            return 0;
        } catch (...) {
            state.error = "Response header callback failed";
            return 0;
        }
        return byte_count;
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

    class CurlHeaders {
    public:
        CurlHeaders() = default;
        CurlHeaders(const CurlHeaders &) = delete;
        CurlHeaders &operator=(const CurlHeaders &) = delete;
        ~CurlHeaders() { curl_slist_free_all(headers_); }

        bool append(const std::string &header) {
            curl_slist *appended = curl_slist_append(headers_, header.c_str());
            if (appended == nullptr) {
                return false;
            }
            headers_ = appended;
            return true;
        }

        curl_slist *get() const { return headers_; }

    private:
        curl_slist *headers_ = nullptr;
    };

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

    std::string userMessageItem(const std::string_view message) {
        std::string item = "{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":";
        appendJsonString(item, message);
        item += "}]}";
        return item;
    }

    std::string toolOutputItem(const microcodex::CodexToolOutput &output) {
        std::string item = "{\"type\":\"function_call_output\",\"call_id\":";
        appendJsonString(item, output.call_id);
        item += ",\"output\":";
        appendJsonString(item, output.output);
        item += '}';
        return item;
    }

} // namespace

namespace microcodex {

    CodexApi::CodexApi(CodexApiConfig config) : config_(std::move(config)), installation_id_(makeUuid()), session_id_(makeUuid()) {}

    CodexApi::CodexApi(CodexApiConfig config, CodexEventEmitter &events) : CodexApi(std::move(config)) {
        events_ = &events;
    }

    std::expected<CodexApiResponse, std::string> CodexApi::sendUserMessage(const std::string_view message) {
        if (message.empty()) {
            return std::unexpected("User message cannot be empty");
        }

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

        const std::size_t previous_size = input_items_.size();
        const std::size_t previous_turn = turn_number_;
        const std::string previous_turn_state = turn_state_;
        const std::string turn_id = makeUuid();

        turn_state_.clear();
        ++turn_number_;
        input_items_.push_back(userMessageItem(message));
        emitEvent({.type = CodexEventType::TurnStarted, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = {}});

        auto response = requestWithToolExecution(turn_stop.get_token(), turn_id);
        if (!response) {
            // The API transcript is append-only during a turn. Truncating to
            // this checkpoint restores the last complete conversation even if
            // the failure happened after several successful tool rounds.
            input_items_.resize(previous_size);
            turn_number_ = previous_turn;
            turn_state_ = previous_turn_state;

            if (turn_stop.stop_requested()) {
                emitEvent({.type = CodexEventType::TurnInterrupted, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = std::string(interrupted_message)});
                return std::unexpected(std::string(interrupted_message));
            }

            emitEvent({.type = CodexEventType::Error, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = response.error(), .succeeded = false});
            return std::unexpected(response.error());
        }

        emitEvent({.type = CodexEventType::TurnCompleted, .turn_id = turn_id, .call_id = {}, .tool_name = {}, .text = response->text});
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

    std::expected<std::string, std::string> CodexApi::buildRequestBody() const {
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
        if (input_items_.empty()) {
            return std::unexpected("Conversation has no input");
        }

        std::unordered_set<std::string> tool_names;
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

        const std::string window_id = session_id_ + ":" + std::to_string(turn_number_ - 1);
        std::string body = "{\"model\":";
        appendJsonString(body, config_.model);
        body += ",\"instructions\":";
        appendJsonString(body, config_.instructions);
        body += ",\"input\":[";
        for (std::size_t index = 0; index < input_items_.size(); ++index) {
            if (index != 0) {
                body += ',';
            }
            body += input_items_[index];
        }
        body += R"(],"tools":[)";
        for (std::size_t index = 0; index < config_.tools.size(); ++index) {
            if (index != 0) {
                body += ',';
            }
            body += config_.tools[index]->toJsonString();
        }
        body += ']';

        body += ",\"tool_choice\":\"auto\",\"parallel_tool_calls\":true,\"reasoning\":{\"effort\":";
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
                return {{call.call_id, "Error: Tool execution interrupted"}, false};
            }

            const auto tool = std::find_if(config_.tools.begin(), config_.tools.end(), [&call](const auto &candidate) {
                return candidate != nullptr && candidate->name() == call.name;
            });
            auto result = tool == config_.tools.end()
                              ? std::expected<std::string, std::string>(std::unexpected("Unknown tool '" + call.name + "'"))
                              : (*tool)->executeJson(call.arguments, stop_token);
            if (stop_token.stop_requested()) {
                return {{call.call_id, "Error: Tool execution interrupted"}, false};
            }
            if (!result) {
                return {{call.call_id, "Error: " + result.error()}, false};
            }
            return {{call.call_id, std::move(*result)}, true};
        } catch (const std::exception &error) {
            return {{call.call_id, std::string("Error: Tool threw an exception: ") + error.what()}, false};
        } catch (...) {
            return {{call.call_id, "Error: Tool threw an unknown exception"}, false};
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
                result = {{calls[index].call_id, std::string("Error: Tool task failed: ") + error.what()}, false};
            } catch (...) {
                result = {{calls[index].call_id, "Error: Tool task failed"}, false};
            }

            emitEvent({
                .type = CodexEventType::ToolFinished,
                .turn_id = std::string(turn_id),
                .call_id = calls[index].call_id,
                .tool_name = calls[index].name,
                .text = result.output.output,
                .succeeded = result.succeeded,
            });
            results.push_back(std::move(result));
        }
        return results;
    }

    std::expected<CodexApiResponse, std::string> CodexApi::requestWithToolExecution(const std::stop_token stop_token, const std::string_view turn_id) {
        CodexApiResponse complete_response;
        std::size_t tool_rounds = 0;

        while (true) {
            if (stop_token.stop_requested()) {
                return std::unexpected(std::string(interrupted_message));
            }

            auto response = request(stop_token, turn_id);
            if (!response) {
                return std::unexpected(response.error());
            }

            complete_response.text += response->text;
            complete_response.tool_calls.insert(complete_response.tool_calls.end(), response->tool_calls.begin(), response->tool_calls.end());
            if (response->tool_calls.empty()) {
                return complete_response;
            }

            ++tool_rounds;
            if (tool_rounds > config_.maximum_tool_rounds) {
                return std::unexpected("Codex exceeded the maximum number of tool rounds");
            }

            auto results = executeToolCalls(response->tool_calls, stop_token, turn_id);
            if (!results) {
                return std::unexpected(results.error());
            }
            if (stop_token.stop_requested()) {
                return std::unexpected(std::string(interrupted_message));
            }

            // Append the complete batch before the next request. No partial
            // batch is ever exposed to the model, even when tools finish in a
            // different order.
            for (const ToolExecutionResult &result : *results) {
                input_items_.push_back(toolOutputItem(result.output));
            }
        }
    }

    std::expected<CodexApiResponse, std::string> CodexApi::request(const std::stop_token stop_token, const std::string_view turn_id) {
        auto request_body = buildRequestBody();
        if (!request_body) {
            return std::unexpected(request_body.error());
        }

        static const CURLcode curl_initialization = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (curl_initialization != CURLE_OK) {
            return std::unexpected(std::string("Could not initialize HTTP client: ") + curl_easy_strerror(curl_initialization));
        }

        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
        if (!curl) {
            return std::unexpected("Could not create HTTP request");
        }

        CurlHeaders headers;
        if (!headers.append("Authorization: Bearer " + config_.access_token) ||
            (!config_.account_id.empty() && !headers.append("ChatGPT-Account-ID: " + config_.account_id)) ||
            !headers.append("Content-Type: application/json") ||
            !headers.append("Accept: text/event-stream") ||
            !headers.append("originator: microcodex") ||
            !headers.append("session-id: " + session_id_) ||
            !headers.append("thread-id: " + session_id_) ||
            !headers.append("x-client-request-id: " + session_id_) ||
            !headers.append("x-codex-window-id: " + session_id_ + ":" + std::to_string(turn_number_ - 1)) ||
            (!turn_state_.empty() && !headers.append("x-codex-turn-state: " + turn_state_))) {
            return std::unexpected("Could not allocate HTTP headers");
        }

        StreamState state;
        state.turn_id = std::string(turn_id);
        state.events = events_;
        state.stop_token = stop_token;
        std::array<char, CURL_ERROR_SIZE> curl_error{};

        // libcurl's C callbacks receive StreamState through void*. Keeping all
        // request-local callback state in this one object avoids global state
        // and makes simultaneous tool threads irrelevant to HTTP parsing.
        const auto setOption = [&curl](const CURLoption option, const auto value) {
            return curl_easy_setopt(curl.get(), option, value) == CURLE_OK;
        };

        if (!setOption(CURLOPT_URL, config_.endpoint.c_str()) ||
            !setOption(CURLOPT_HTTPHEADER, headers.get()) ||
            !setOption(CURLOPT_POST, 1L) ||
            !setOption(CURLOPT_POSTFIELDS, request_body->data()) ||
            !setOption(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request_body->size())) ||
            !setOption(CURLOPT_WRITEFUNCTION, &receiveBody) ||
            !setOption(CURLOPT_WRITEDATA, &state) ||
            !setOption(CURLOPT_HEADERFUNCTION, &receiveHeader) ||
            !setOption(CURLOPT_HEADERDATA, &state) ||
            !setOption(CURLOPT_XFERINFOFUNCTION, &transferProgress) ||
            !setOption(CURLOPT_XFERINFODATA, &state) ||
            !setOption(CURLOPT_NOPROGRESS, 0L) ||
            !setOption(CURLOPT_ERRORBUFFER, curl_error.data()) ||
            !setOption(CURLOPT_USERAGENT, "microcodex") ||
            !setOption(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS) ||
            !setOption(CURLOPT_NOSIGNAL, 1L) ||
            !setOption(CURLOPT_LOW_SPEED_LIMIT, 1L) ||
            !setOption(CURLOPT_LOW_SPEED_TIME, config_.idle_timeout_seconds)) {
            return std::unexpected("Could not configure HTTP request");
        }

        const CURLcode result = curl_easy_perform(curl.get());
        long status = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);

        if (stop_token.stop_requested()) {
            return std::unexpected(std::string(interrupted_message));
        }
        if (!state.error.empty()) {
            return std::unexpected(state.error);
        }
        if (result != CURLE_OK) {
            const std::string detail = curl_error[0] != '\0' ? std::string(curl_error.data()) : std::string(curl_easy_strerror(result));
            return std::unexpected("Codex API request failed: " + detail);
        }
        if (status < 200 || status >= 300) {
            return std::unexpected(responseErrorMessage(state.received_body, status));
        }

        auto final_event = finishSse(state);
        if (!final_event) {
            return std::unexpected(final_event.error());
        }
        if (!state.completed) {
            return std::unexpected("Codex stream closed before response.completed");
        }

        turn_state_ = std::move(state.turn_state);
        for (std::string &item : state.output_items) {
            input_items_.push_back(std::move(item));
        }
        return std::move(state.response);
    }

    void CodexApi::emitEvent(CodexEvent event) const noexcept {
        if (events_ != nullptr) {
            events_->emit(event);
        }
    }

} // namespace microcodex
