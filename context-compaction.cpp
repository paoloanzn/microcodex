// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "context-compaction.h"

#include "response-item.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace microcodex {

    namespace {

        constexpr std::string_view summary_instructions =
            "Summarize the conversation for another coding agent that must continue the work. "
            "Preserve the user's goal and constraints, decisions and their reasons, files inspected "
            "and changed, important code behavior, commands and test results, unresolved errors, and "
            "remaining work. Do not include conversational filler.";

        std::size_t turnStart(const ContextView &context, const std::size_t turn_index) {
            if (turn_index == 0) return context.has_summary ? 1 : 0;
            return context.completed_turns[turn_index - 1].end;
        }

        std::size_t estimatedTokens(const ContextView &context, const std::size_t begin, const std::size_t end) {
            return estimateContextTokens(context.items.subspan(begin, end - begin));
        }

    } // namespace

    ContextCompactor::ContextCompactor(CompactionConfig config)
        : config_(std::move(config)) {
        if (config_.context_limit_tokens != 0 && config_.compact_at_tokens == 0) {
            config_.compact_at_tokens = config_.context_limit_tokens * 3 / 4;
        }
        if (config_.retained_context_tokens == 0) {
            config_.retained_context_tokens = config_.compact_at_tokens / 5;
        }
    }

    bool ContextCompactor::needed(const ContextUsage &usage) const {
        if (config_.compact_at_tokens == 0) return false;
        return std::max(usage.reported_input_tokens, usage.estimated_tokens) >=
               config_.compact_at_tokens;
    }

    std::expected<CompactionPlan, std::string> ContextCompactor::plan(const ContextView &context, const bool retain_recent_turns) const {
        if (context.protected_start > context.items.size()) {
            return std::unexpected("Active turn begins outside the model context");
        }
        const std::size_t summary_items = context.has_summary ? 1 : 0;
        if (summary_items > context.items.size()) {
            return std::unexpected("Model context is missing its compaction summary");
        }
        if (context.completed_turns.empty()) {
            return std::unexpected("There are no completed turns to compact");
        }

        std::size_t eligible_turns = 0;
        std::size_t previous_end = summary_items;
        for (const TurnBoundary &turn : context.completed_turns) {
            if (turn.end <= previous_end || turn.end > context.items.size()) {
                return std::unexpected("Completed turn boundaries are invalid");
            }
            previous_end = turn.end;
            if (turn.end > context.protected_start) break;
            ++eligible_turns;
        }
        if (eligible_turns == 0) {
            return std::unexpected("There are no complete turns before the active turn");
        }

        std::size_t retained_tokens = 0;
        std::size_t first_retained_turn = eligible_turns;
        std::size_t retained_start = context.protected_start;

        // Walk complete turns from newest to oldest. Retention always follows
        // turn boundaries, so a function call can never be separated from its output.
        for (std::size_t index = retain_recent_turns ? eligible_turns : 0;
             index > 0; --index) {
            const std::size_t turn_index = index - 1;
            const std::size_t begin = turnStart(context, turn_index);
            const std::size_t end = context.completed_turns[turn_index].end;
            const std::size_t tokens = estimatedTokens(context, begin, end);
            if (retained_tokens != 0 &&
                (retained_tokens >= config_.retained_context_tokens ||
                 tokens > config_.retained_context_tokens - retained_tokens)) {
                break;
            }
            if (retained_tokens == 0 && tokens > config_.retained_context_tokens) {
                break;
            }
            retained_tokens += tokens;
            retained_start = begin;
            first_retained_turn = turn_index;
        }

        if (retained_start <= summary_items) {
            return std::unexpected("All completed turns already fit the retained context budget");
        }

        const std::uint64_t through_turn = context.completed_turns[eligible_turns - 1].number;
        const std::uint64_t keep_from_turn =
            first_retained_turn < eligible_turns
                ? context.completed_turns[first_retained_turn].number
                : through_turn + 1;

        return CompactionPlan{
            .summary_end = retained_start,
            .retained_start = retained_start,
            .first_retained_turn = first_retained_turn,
            .through_turn = through_turn,
            .keep_from_turn = keep_from_turn,
            .generation = context.generation + 1,
        };
    }

    std::expected<PreparedCompaction, std::string> ContextCompactor::prepare(const ContextView &context, const CompactionPlan &plan, std::string summary) const {
        if (summary.empty()) {
            return std::unexpected("Compaction produced an empty summary");
        }
        if (summary.size() > config_.maximum_summary_bytes) {
            return std::unexpected("Compaction summary exceeds its size limit");
        }
        if (plan.summary_end != plan.retained_start ||
            plan.retained_start > context.protected_start ||
            plan.retained_start > context.items.size() ||
            plan.first_retained_turn > context.completed_turns.size()) {
            return std::unexpected("Compaction plan no longer matches the model context");
        }

        PreparedCompaction prepared{
            .checkpoint = {
                .generation = plan.generation,
                .through_turn = plan.through_turn,
                .keep_from_turn = plan.keep_from_turn,
                .summary = std::move(summary),
            },
            .input_items = {},
            .completed_turns = {},
        };

        prepared.input_items.reserve(1 + context.items.size() - plan.retained_start);
        prepared.input_items.push_back(userMessageItem(
            "<conversation_summary>\n" + prepared.checkpoint.summary +
            "\n</conversation_summary>"));
        for (std::size_t index = plan.retained_start; index < context.items.size(); ++index) {
            prepared.input_items.push_back(context.items[index]);
        }

        prepared.completed_turns.reserve(
            context.completed_turns.size() - plan.first_retained_turn);
        for (std::size_t index = plan.first_retained_turn;
             index < context.completed_turns.size(); ++index) {
            const TurnBoundary &old = context.completed_turns[index];
            if (old.end < plan.retained_start) {
                return std::unexpected("Compaction retained an invalid turn boundary");
            }
            prepared.completed_turns.push_back({
                .number = old.number,
                .end = 1 + old.end - plan.retained_start,
            });
        }
        return prepared;
    }

    std::string_view ContextCompactor::summaryInstructions() const {
        return summary_instructions;
    }

    std::size_t estimateContextTokens(const std::span<const std::string> items) {
        std::size_t bytes = 0;
        for (const std::string &item : items) {
            if (bytes > std::numeric_limits<std::size_t>::max() - item.size()) {
                return std::numeric_limits<std::size_t>::max();
            }
            bytes += item.size();
        }
        // Code and JSON tend to use more tokens per byte than prose. Three
        // bytes per token is intentionally conservative and avoids a tokenizer.
        return bytes / 3 + (bytes % 3 != 0);
    }

} // namespace microcodex
