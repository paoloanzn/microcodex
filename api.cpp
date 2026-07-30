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
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    using microcodex::json::appendJsonString;
    using microcodex::json::findJsonMember;
    using microcodex::json::jsonArrayElements;
    using microcodex::json::jsonStringMember;
    using microcodex::json::requiredJsonString;
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
        if (type.value() != "message") {
            return std::string{};
        }

        auto content = findJsonMember(item, "content");
        if (!content) {
            return std::unexpected(content.error());
        }
        if (!content.value()) {
            return std::unexpected("Response message has no content");
        }
        auto parts = jsonArrayElements(content.value().value());
        if (!parts) {
            return std::unexpected(parts.error());
        }

        std::string text;
        for (const std::string_view part : parts.value()) {
            auto part_type = requiredJsonString(part, "type");
            if (!part_type) {
                return std::unexpected(part_type.error());
            }
            if (part_type.value() == "output_text") {
                auto part_text = requiredJsonString(part, "text");
                if (!part_text) {
                    return std::unexpected(part_text.error());
                }
                text += part_text.value();
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
        const microcodex::CodexTextCallback *on_text = nullptr;
        bool completed = false;
    };

    std::expected<void, std::string> handleEvent(const std::string_view data, StreamState &state) {
        if (data.empty() || data == "[DONE]") {
            return {};
        }

        auto type = requiredJsonString(data, "type");
        if (!type) {
            return std::unexpected("Invalid Responses API event: " + type.error());
        }

        if (type.value() == "response.output_text.delta") {
            auto delta = requiredJsonString(data, "delta");
            if (!delta) {
                return std::unexpected(delta.error());
            }
            state.response.text += delta.value();
            if (state.on_text != nullptr && *state.on_text) {
                (*state.on_text)(delta.value());
            }
            return {};
        }

        if (type.value() == "response.output_item.done") {
            auto item = findJsonMember(data, "item");
            if (!item) {
                return std::unexpected(item.error());
            }
            if (!item.value()) {
                return std::unexpected("response.output_item.done has no item");
            }

            const std::string_view item_json = item.value().value();
            state.output_items.emplace_back(item_json);

            auto item_type = requiredJsonString(item_json, "type");
            if (!item_type) {
                return std::unexpected(item_type.error());
            }
            if (item_type.value() == "function_call") {
                auto call_id = requiredJsonString(item_json, "call_id");
                auto name = requiredJsonString(item_json, "name");
                auto arguments = requiredJsonString(item_json, "arguments");
                if (!call_id || !name || !arguments) {
                    return std::unexpected("Invalid function_call response item");
                }
                state.response.tool_calls.push_back({std::move(call_id.value()),
                                                     std::move(name.value()),
                                                     std::move(arguments.value())});
            } else if (item_type.value() == "message") {
                auto text = assistantMessageText(item_json);
                if (!text) {
                    return std::unexpected(text.error());
                }
                state.fallback_text += text.value();
            }
            return {};
        }

        if (type.value() == "response.completed") {
            auto response = findJsonMember(data, "response");
            if (!response) {
                return std::unexpected(response.error());
            }
            if (!response.value()) {
                return std::unexpected("response.completed has no response");
            }
            if (state.response.text.empty()) {
                state.response.text = state.fallback_text;
            }
            state.completed = true;
            return {};
        }

        if (type.value() == "response.failed") {
            auto response = findJsonMember(data, "response");
            if (response && response.value()) {
                auto error = findJsonMember(response.value().value(), "error");
                if (error && error.value()) {
                    auto message = jsonStringMember(error.value().value(), "message");
                    if (message && message.value()) {
                        return std::unexpected(message.value().value());
                    }
                }
            }
            return std::unexpected("The Codex response failed");
        }

        if (type.value() == "response.incomplete") {
            std::string reason = "unknown reason";
            auto response = findJsonMember(data, "response");
            if (response && response.value()) {
                auto details = findJsonMember(response.value().value(), "incomplete_details");
                if (details && details.value()) {
                    auto value = jsonStringMember(details.value().value(), "reason");
                    if (value && value.value()) {
                        reason = std::move(value.value().value());
                    }
                }
            }
            return std::unexpected("The Codex response was incomplete: " + reason);
        }

        if (type.value() == "error") {
            auto error = findJsonMember(data, "error");
            if (error && error.value()) {
                auto message = jsonStringMember(error.value().value(), "message");
                if (message && message.value()) {
                    return std::unexpected(message.value().value());
                }
            }
            auto message = jsonStringMember(data, "message");
            if (message && message.value()) {
                return std::unexpected(message.value().value());
            }
            return std::unexpected("The Codex API returned an error event");
        }

        // Other events contain progress or metadata that this minimal client does
        // not need. The final output items are retained above for the next request.
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

    std::size_t receiveBody(char *data, const std::size_t size, const std::size_t count,
                            void *user_data) {
        const std::size_t byte_count = size * count;
        auto &state = *static_cast<StreamState *>(user_data);

        try {
            // Keep only a bounded copy for useful HTTP error messages.
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
            if (std::tolower(static_cast<unsigned char>(left[index])) !=
                std::tolower(static_cast<unsigned char>(right[index]))) {
                return false;
            }
        }
        return true;
    }

    std::size_t receiveHeader(char *data, const std::size_t size, const std::size_t count,
                              void *user_data) {
        const std::size_t byte_count = size * count;
        auto &state = *static_cast<StreamState *>(user_data);
        try {
            const std::string_view line(data, byte_count);
            const std::size_t colon = line.find(':');
            if (colon != std::string_view::npos &&
                equalsIgnoringCase(line.substr(0, colon), "x-codex-turn-state")) {
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
        if (error && error.value()) {
            auto message = jsonStringMember(error.value().value(), "message");
            if (message && message.value()) {
                return "Codex API returned HTTP " + std::to_string(status) + ": " +
                       message.value().value();
            }
        }
        auto detail = jsonStringMember(body, "detail");
        if (detail && detail.value()) {
            return "Codex API returned HTTP " + std::to_string(status) + ": " +
                   detail.value().value();
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

    std::string userMessageItem(const std::string_view message) {
        std::string item = "{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_"
                           "text\",\"text\":";
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

    CodexApi::CodexApi(CodexApiConfig config)
        : config_(std::move(config)), installation_id_(makeUuid()), session_id_(makeUuid()) {}

    std::expected<CodexApiResponse, std::string>
    CodexApi::sendUserMessage(const std::string_view message, const CodexTextCallback &on_text) {
        if (message.empty()) {
            return std::unexpected("User message cannot be empty");
        }

        const std::size_t previous_size = input_items_.size();
        const std::size_t previous_turn = turn_number_;
        const std::string previous_turn_state = turn_state_;
        turn_state_.clear();
        ++turn_number_;
        input_items_.push_back(userMessageItem(message));

        auto response = requestWithToolExecution(on_text);
        if (!response) {
            input_items_.resize(previous_size);
            turn_number_ = previous_turn;
            turn_state_ = previous_turn_state;
        }
        return response;
    }

    std::expected<CodexApiResponse, std::string>
    CodexApi::sendToolOutputs(const std::span<const CodexToolOutput> outputs,
                              const CodexTextCallback &on_text) {
        if (outputs.empty()) {
            return std::unexpected("At least one tool output is required");
        }
        if (input_items_.empty()) {
            return std::unexpected("Cannot send tool output before a user message");
        }

        const std::size_t previous_size = input_items_.size();
        const std::string previous_turn_state = turn_state_;
        for (const CodexToolOutput &output : outputs) {
            if (output.call_id.empty()) {
                input_items_.resize(previous_size);
                return std::unexpected("Tool call ID cannot be empty");
            }
            input_items_.push_back(toolOutputItem(output));
        }

        auto response = requestWithToolExecution(on_text);
        if (!response) {
            input_items_.resize(previous_size);
            turn_state_ = previous_turn_state;
        }
        return response;
    }

    void CodexApi::resetConversation() {
        input_items_.clear();
        session_id_ = makeUuid();
        turn_state_.clear();
        turn_number_ = 0;
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
        if (containsNewline(config_.access_token) || containsNewline(config_.account_id)) {
            return std::unexpected("Credentials cannot contain a newline");
        }
        if (input_items_.empty()) {
            return std::unexpected("Conversation has no input");
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
            if (config_.tools[index] == nullptr) {
                return std::unexpected("Tools cannot contain null entries");
            }
            if (index != 0) {
                body += ',';
            }
            body += config_.tools[index]->toJsonString();
        }
        body += ']';

        body +=
            ",\"tool_choice\":\"auto\",\"parallel_tool_calls\":false,\"reasoning\":{\"effort\":";
        appendJsonString(body, config_.reasoning_effort);
        body += "},\"store\":false,\"stream\":true,\"include\":[\"reasoning.encrypted_content\"],"
                "\"prompt_cache_key\":";
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

    std::expected<std::vector<CodexToolOutput>, std::string>
    CodexApi::executeToolCalls(const std::span<const CodexToolCall> calls) const {
        std::vector<CodexToolOutput> outputs;
        outputs.reserve(calls.size());

        for (const CodexToolCall &call : calls) {
            const auto tool = std::find_if(config_.tools.begin(), config_.tools.end(),
                                           [&call](const auto &candidate) {
                                               return candidate != nullptr &&
                                                      candidate->name() == call.name;
                                           });
            auto result = tool == config_.tools.end()
                              ? std::expected<std::string, std::string>(
                                    std::unexpected("Unknown tool '" + call.name + "'"))
                              : (*tool)->executeJson(call.arguments);
            outputs.push_back(
                {call.call_id, result ? std::move(result.value()) : "Error: " + result.error()});
        }
        return outputs;
    }

    std::expected<CodexApiResponse, std::string>
    CodexApi::requestWithToolExecution(const CodexTextCallback &on_text) {
        CodexApiResponse complete_response;
        std::size_t tool_rounds = 0;

        while (true) {
            auto response = request(on_text);
            if (!response) {
                return std::unexpected(response.error());
            }

            complete_response.text += response->text;
            complete_response.tool_calls.insert(
                complete_response.tool_calls.end(), response->tool_calls.begin(),
                response->tool_calls.end());

            if (response->tool_calls.empty()) {
                return complete_response;
            }
            if (tool_rounds++ >= config_.maximum_tool_rounds) {
                return std::unexpected("Codex exceeded the maximum number of tool rounds");
            }

            auto outputs = executeToolCalls(response->tool_calls);
            if (!outputs) {
                return std::unexpected(outputs.error());
            }
            for (const CodexToolOutput &output : outputs.value()) {
                input_items_.push_back(toolOutputItem(output));
            }
        }
    }

    std::expected<CodexApiResponse, std::string>
    CodexApi::request(const CodexTextCallback &on_text) {
        auto request_body = buildRequestBody();
        if (!request_body) {
            return std::unexpected(request_body.error());
        }

        static const CURLcode curl_initialization = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (curl_initialization != CURLE_OK) {
            return std::unexpected(std::string("Could not initialize HTTP client: ") +
                                   curl_easy_strerror(curl_initialization));
        }

        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                 &curl_easy_cleanup);
        if (!curl) {
            return std::unexpected("Could not create HTTP request");
        }

        CurlHeaders headers;
        if (!headers.append("Authorization: Bearer " + config_.access_token) ||
            (!config_.account_id.empty() &&
             !headers.append("ChatGPT-Account-ID: " + config_.account_id)) ||
            !headers.append("Content-Type: application/json") ||
            !headers.append("Accept: text/event-stream") || !headers.append("originator: microcodex") ||
            !headers.append("session-id: " + session_id_) ||
            !headers.append("thread-id: " + session_id_) ||
            !headers.append("x-client-request-id: " + session_id_) ||
            !headers.append("x-codex-window-id: " + session_id_ + ":" +
                            std::to_string(turn_number_ - 1)) ||
            (!turn_state_.empty() && !headers.append("x-codex-turn-state: " + turn_state_))) {
            return std::unexpected("Could not allocate HTTP headers");
        }

        StreamState state;
        state.on_text = &on_text;
        std::array<char, CURL_ERROR_SIZE> curl_error{};

        const auto setOption = [&curl](const CURLoption option, const auto value) {
            return curl_easy_setopt(curl.get(), option, value) == CURLE_OK;
        };

        if (!setOption(CURLOPT_URL, config_.endpoint.c_str()) ||
            !setOption(CURLOPT_HTTPHEADER, headers.get()) || !setOption(CURLOPT_POST, 1L) ||
            !setOption(CURLOPT_POSTFIELDS, request_body->data()) ||
            !setOption(CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(request_body->size())) ||
            !setOption(CURLOPT_WRITEFUNCTION, &receiveBody) ||
            !setOption(CURLOPT_WRITEDATA, &state) ||
            !setOption(CURLOPT_HEADERFUNCTION, &receiveHeader) ||
            !setOption(CURLOPT_HEADERDATA, &state) ||
            !setOption(CURLOPT_ERRORBUFFER, curl_error.data()) ||
            !setOption(CURLOPT_USERAGENT, "microcodex") ||
            !setOption(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS) ||
            !setOption(CURLOPT_NOSIGNAL, 1L) || !setOption(CURLOPT_LOW_SPEED_LIMIT, 1L) ||
            !setOption(CURLOPT_LOW_SPEED_TIME, config_.idle_timeout_seconds)) {
            return std::unexpected("Could not configure HTTP request");
        }

        const CURLcode result = curl_easy_perform(curl.get());
        long status = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);

        if (!state.error.empty()) {
            return std::unexpected(state.error);
        }
        if (result != CURLE_OK) {
            const std::string detail = curl_error[0] != '\0'
                                           ? std::string(curl_error.data())
                                           : std::string(curl_easy_strerror(result));
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

} // namespace microcodex

