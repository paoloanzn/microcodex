// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

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

#include "context-compaction.h"
#include "conversation.h"
#include "event-emitter.h"
#include "model-catalog.h"
#include "response-item.h"
#include "tool.h"

namespace microcodex {

// The user-facing reasoning effort values, in the order they are listed.
std::span<const std::string_view> reasoningEffortValues() noexcept;

// Checks a user-facing effort value. `source` names where the value came from
// ("--effort", "MICROCODEX_EFFORT" or "/effort") in the error message.
std::expected<void, std::string> validateReasoningEffort(std::string_view effort, std::string_view source);

struct CodexApiConfig {
    std::string access_token;
    std::string account_id;
    std::string model;
    std::string instructions;
    // The global AGENTS.md text. When set, every sampling request starts its
    // input with it as a user message. It is never added to the history or
    // saved, so a resumed conversation always uses the current file.
    std::string user_instructions;
    // User-facing thinking effort, one of reasoningEffortValues(). It is
    // sent as "reasoning": {"effort": ...}, except that "persistent" is sent
    // as "disabled" and "ultra" as "max".
    std::string reasoning_effort = "medium";
    std::string endpoint = "https://chatgpt.com/backend-api/codex/responses";
    long idle_timeout_seconds = 300;
    std::size_t maximum_tool_rounds = 128;
    std::size_t maximum_parallel_tool_calls = 32;
    std::size_t maximum_tool_output_bytes = std::size_t{64} * 1024;
    // Maximum wall-clock time for a single tool call, measured from when the
    // call starts, so parallel stuck calls cannot stack their timeouts.
    // When a call exceeds it, the call is reported as a timeout error and
    // its worker is asked to stop, so a stuck tool can never wedge the
    // turn. Zero disables the timeout. Tunable at runtime with MICROCODEX_TOOL_EXECUTION_TIMEOUT_SECONDS.
    std::size_t tool_execution_timeout_seconds = 120;
    // Offline fallback matching Codex's 272K unknown-model descriptor. The
    // real values are queried from /models before an online run. Keeping an
    // explicit fallback lets CodexApi initialize and load saved conversation
    // history without network access, even though it cannot sample a model.
    CompactionConfig compaction{
        .context_limit_tokens = 258'400,
        .compact_at_tokens = 244'800,
        .retained_context_tokens = std::size_t{20} * 1024,
        .maximum_summary_bytes = std::size_t{32} * 1024,
    };
    // The /models catalog read at startup. setModel() accepts only these
    // models and takes their context limits from it. Empty when the catalog
    // could not be read, which makes setModel() refuse every model.
    std::vector<ModelContextLimits> models;
    // MICROCODEX_COMPACT_AT_TOKENS. When set, it replaces the catalog's
    // compaction threshold for every model, including after setModel().
    std::optional<std::size_t> compact_at_tokens_override;
    bool persist_conversation = true;
    std::optional<std::filesystem::path> resume_conversation;
    std::vector<std::shared_ptr<const ToolBase>> tools;
};

// Settings and context usage reported by CodexApi::status().
struct CodexApiStatus {
    std::string model;
    std::string reasoning_effort;
    std::size_t saved_turns = 0;
    // The larger of the last reported input token count and the byte-based
    // estimate, the same size compaction compares against its threshold.
    std::size_t context_tokens = 0;
    std::size_t context_limit_tokens = 0;
    // Zero when automatic compaction is off.
    std::size_t compact_at_tokens = 0;
    // Whether the next turn compacts the context before its request, for
    // example after switching to a model with a smaller context window.
    bool compaction_pending = false;
    // The models setModel() accepts, in catalog order.
    std::vector<std::string> models;
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
    CodexApi(CodexApiConfig config, CodexEventEmitter& events);

    ~CodexApi() = default;
    CodexApi(const CodexApi&) = delete;
    CodexApi& operator=(const CodexApi&) = delete;
    CodexApi(CodexApi&&) = delete;
    CodexApi& operator=(CodexApi&&) = delete;

    std::expected<CodexApiResponse, std::string> sendUserMessage(std::string_view message);

    // Creates or resumes durable conversation state. sendUserMessage() also
    // calls this lazily, while interactive clients call it before rendering.
    std::expected<void, std::string> initializeConversation();

    std::expected<std::vector<SavedTurn>, std::string> readHistoryBefore(std::size_t cursor,
                                                                         std::size_t maximum_bytes) const;
    [[nodiscard]] std::size_t savedTurnCount() const;

    // interrupt() is the only operation intended to be called from another
    // thread while sendUserMessage() is running.
    void interrupt() noexcept;

    // Reset is rejected while a turn is active so conversation history is
    // always owned and modified by a single thread.
    std::expected<void, std::string> resetConversation();

    // These are rejected while a turn is running, because the worker thread
    // reads the configuration and the context during a turn.
    std::expected<CodexApiStatus, std::string> status() const;
    std::expected<void, std::string> setReasoningEffort(std::string effort);
    // Switches the following turns to a catalog model and applies its context
    // limits. The change is saved in the conversation file, so a resumed
    // conversation continues with it.
    std::expected<void, std::string> setModel(std::string model);

    // Prevents future turns and interrupts one that is currently active.
    // CodexApi owns no worker thread, so the caller remains responsible for
    // joining the thread that called sendUserMessage().
    void shutdown() noexcept;

    [[nodiscard]] bool turnInProgress() const noexcept;

private:
    struct ToolExecutionResult {
        CodexToolOutput output;
        std::shared_ptr<const EditResult> edit;
        bool succeeded = false;
    };

    struct ModelResponse {
        CodexApiResponse response;
        std::vector<std::string> output_items;
        std::string turn_state;
    };

    std::expected<CodexApiResponse, std::string> requestWithToolExecution(std::stop_token stop_token,
                                                                          std::string_view turn_id,
                                                                          std::size_t& turn_start);
    std::expected<CodexApiResponse, std::string> request(std::stop_token stop_token, std::string_view turn_id);
    std::expected<ModelResponse, std::string> performRequest(const std::string& request_body,
                                                             std::stop_token stop_token, std::string_view turn_id,
                                                             bool emit_events,
                                                             ModelResponse* partial_response = nullptr) const;
    std::expected<std::string, std::string> requestSummary(std::span<const std::string> items,
                                                           std::stop_token stop_token);
    std::expected<void, std::string> compactContext(std::stop_token stop_token, std::size_t& protected_start,
                                                    bool force);
    std::expected<std::vector<ToolExecutionResult>, std::string> executeToolCalls(std::span<const CodexToolCall> calls,
                                                                                  std::stop_token stop_token,
                                                                                  std::string_view turn_id) const;
    // Runs one tool call without touching CodexApi state. A call that hits
    // the execution timeout keeps running on a reaper thread after
    // executeToolCalls returns, so it must never capture `this`.
    static ToolExecutionResult executeToolCall(std::span<const std::shared_ptr<const ToolBase>> tools,
                                               const CodexToolCall& call, std::stop_token stop_token,
                                               std::size_t maximum_output_bytes);
    // The input is leading_item, then items, then final_item; empty ones are skipped.
    std::expected<std::string, std::string> buildRequestBody(std::span<const std::string> items,
                                                             std::string_view instructions, bool include_tools,
                                                             std::string_view leading_item,
                                                             std::string_view final_item = {}) const;
    // The history's size estimate, including the user instructions message.
    [[nodiscard]] std::size_t estimatedContextTokens() const;
    void emitEvent(const CodexEvent& event) const noexcept;

    CodexApiConfig config_;
    // config_.user_instructions as a user message item, or empty.
    std::string user_instructions_item_;
    ContextCompactor compactor_;
    CodexEventEmitter* events_ = nullptr;
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

}  // namespace microcodex
