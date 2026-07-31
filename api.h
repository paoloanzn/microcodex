// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "context-compaction.h"
#include "conversation.h"
#include "event-emitter.h"
#include "response-item.h"
#include "tool.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
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
        std::size_t maximum_tool_output_bytes = 64 * 1024;
        // Offline fallback matching Codex's 272K unknown-model descriptor. The
        // real values are queried from /models before an online run. Keeping an
        // explicit fallback lets CodexApi initialize and load saved conversation
        // history without network access, even though it cannot sample a model.
        CompactionConfig compaction{
            .context_limit_tokens = 258'400,
            .compact_at_tokens = 244'800,
            .retained_context_tokens = 20 * 1024,
            .maximum_summary_bytes = 32 * 1024,
        };
        bool persist_conversation = true;
        std::optional<std::filesystem::path> resume_conversation;
        std::vector<std::shared_ptr<const ToolBase>> tools;
    };

    struct CodexApiResponse {
        std::string text;
        // All tool calls executed across the sampling rounds of this turn.
        std::vector<CodexToolCall> tool_calls;
        std::size_t input_tokens = 0;
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

        // Creates or resumes durable conversation state. sendUserMessage() also
        // calls this lazily, while interactive clients call it before rendering.
        std::expected<void, std::string> initializeConversation();

        std::expected<std::vector<SavedTurn>, std::string> readHistoryBefore(std::size_t cursor, std::size_t maximum_bytes) const;
        [[nodiscard]] std::size_t savedTurnCount() const;

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
            std::shared_ptr<const EditResult> edit;
            bool succeeded;
        };

        struct ModelResponse {
            CodexApiResponse response;
            std::vector<std::string> output_items;
            std::string turn_state;
        };

        std::expected<CodexApiResponse, std::string> requestWithToolExecution(std::stop_token stop_token, std::string_view turn_id, std::size_t &turn_start);
        std::expected<CodexApiResponse, std::string> request(std::stop_token stop_token, std::string_view turn_id);
        std::expected<ModelResponse, std::string> performRequest(std::string request_body, std::stop_token stop_token, std::string_view turn_id, bool emit_events) const;
        std::expected<std::string, std::string> requestSummary(std::span<const std::string> items, std::stop_token stop_token);
        std::expected<void, std::string> compactContext(std::stop_token stop_token, std::size_t &protected_start, bool force);
        std::expected<std::vector<ToolExecutionResult>, std::string> executeToolCalls(std::span<const CodexToolCall> calls, std::stop_token stop_token, std::string_view turn_id) const;
        ToolExecutionResult executeToolCall(const CodexToolCall &call, std::stop_token stop_token) const;
        std::expected<std::string, std::string> buildRequestBody(std::span<const std::string> items, std::string_view instructions, bool include_tools, std::string_view final_item = {}) const;
        void emitEvent(CodexEvent event) const noexcept;

        CodexApiConfig config_;
        ContextCompactor compactor_;
        CodexEventEmitter *events_ = nullptr;
        std::vector<std::string> input_items_;
        std::vector<TurnBoundary> completed_turns_;
        std::optional<ConversationFile> conversation_file_;
        bool conversation_initialized_ = false;
        bool has_summary_ = false;
        std::uint64_t compaction_generation_ = 0;
        std::size_t reported_input_tokens_ = 0;
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
