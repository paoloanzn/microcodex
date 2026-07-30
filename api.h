// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "event-emitter.h"
#include "tool.h"

#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
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
        std::size_t maximum_parallel_tool_calls = 32;
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
        // All tool calls executed across the sampling rounds of this turn.
        std::vector<CodexToolCall> tool_calls;
    };

    // A synchronous, stateful Codex conversation. sendUserMessage() owns the
    // complete model/tool loop and returns only when the turn completes, fails,
    // or is interrupted. A future UI should call it on one worker thread.
    class CodexApi {
    public:
        explicit CodexApi(CodexApiConfig config);

        // The emitter must outlive this CodexApi. It is output-only: commands
        // and cancellation always travel through explicit methods below.
        CodexApi(CodexApiConfig config, CodexEventEmitter &events);

        CodexApi(const CodexApi &) = delete;
        CodexApi &operator=(const CodexApi &) = delete;

        std::expected<CodexApiResponse, std::string> sendUserMessage(std::string_view message);

        // interrupt() is the only operation intended to be called from another
        // thread while sendUserMessage() is running.
        void interrupt() noexcept;

        // Reset is rejected while a turn is active so conversation history is
        // always owned and modified by a single thread.
        std::expected<void, std::string> resetConversation();

        // Prevents future turns and interrupts one that is currently active.
        // CodexApi owns no worker thread, so the caller remains responsible for
        // joining the thread that called sendUserMessage().
        void shutdown() noexcept;

        [[nodiscard]] bool turnInProgress() const noexcept;

    private:
        struct ToolExecutionResult {
            CodexToolOutput output;
            bool succeeded;
        };

        std::expected<CodexApiResponse, std::string> requestWithToolExecution(std::stop_token stop_token, std::string_view turn_id);
        std::expected<CodexApiResponse, std::string> request(std::stop_token stop_token, std::string_view turn_id);
        std::expected<std::vector<ToolExecutionResult>, std::string> executeToolCalls(std::span<const CodexToolCall> calls, std::stop_token stop_token, std::string_view turn_id) const;
        ToolExecutionResult executeToolCall(const CodexToolCall &call, std::stop_token stop_token) const;
        std::expected<std::string, std::string> buildRequestBody() const;
        void emitEvent(CodexEvent event) const noexcept;

        CodexApiConfig config_;
        CodexEventEmitter *events_ = nullptr;
        std::vector<std::string> input_items_;
        std::string installation_id_;
        std::string session_id_;
        std::string turn_state_;
        std::size_t turn_number_ = 0;

        mutable std::mutex turn_mutex_;
        bool turn_running_ = false;
        bool shutdown_requested_ = false;
        std::optional<std::stop_source> active_turn_stop_;
    };

} // namespace microcodex
