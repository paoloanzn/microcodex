// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "tool.h"

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microcodex {

    struct CodexApiConfig {
        std::string access_token;
        std::string account_id;
        std::string model;
        std::string instructions;
        std::string reasoning_effort = "medium";
        std::string endpoint = "https://chatgpt.com/backend-api/codex/responses";
        long idle_timeout_seconds = 300;
        std::size_t maximum_tool_rounds = 64;
        std::vector<std::shared_ptr<const ToolBase>> tools;
    };

    struct CodexToolCall {
        std::string call_id;
        std::string name;
        // The Responses API sends function arguments as JSON encoded in a string.
        std::string arguments;
    };

    struct CodexToolOutput {
        std::string call_id;
        std::string output;
    };

    struct CodexApiResponse {
        std::string text;
        // Configured tools are executed automatically. This contains the calls
        // that were executed while producing the final response.
        std::vector<CodexToolCall> tool_calls;
    };

    // Called as assistant text arrives. The view is valid only for the duration
    // of the callback.
    using CodexTextCallback = std::function<void(std::string_view)>;

    // A synchronous, stateful conversation with the Codex Responses endpoint.
    // The object retains response and tool-call items because the endpoint is
    // called with store=false and therefore expects the full conversation each time.
    class CodexApi {
    public:
        explicit CodexApi(CodexApiConfig config);

        std::expected<CodexApiResponse, std::string>
        sendUserMessage(std::string_view message, const CodexTextCallback &on_text = {});

        std::expected<CodexApiResponse, std::string>
        sendToolOutputs(std::span<const CodexToolOutput> outputs,
                        const CodexTextCallback &on_text = {});

        void resetConversation();

    private:
        std::expected<CodexApiResponse, std::string> request(const CodexTextCallback &on_text);
        std::expected<CodexApiResponse, std::string>
        requestWithToolExecution(const CodexTextCallback &on_text);
        std::expected<std::vector<CodexToolOutput>, std::string>
        executeToolCalls(std::span<const CodexToolCall> calls) const;
        std::expected<std::string, std::string> buildRequestBody() const;

        CodexApiConfig config_;
        std::vector<std::string> input_items_;
        std::string installation_id_;
        std::string session_id_;
        std::string turn_state_;
        std::size_t turn_number_ = 0;
    };

} // namespace microcodex
