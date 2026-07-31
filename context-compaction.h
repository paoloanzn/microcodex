// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "conversation.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microcodex {

    struct CompactionConfig {
        // A zero limit disables automatic compaction.
        std::size_t context_limit_tokens = 0;
        std::size_t compact_at_tokens = 0;
        std::size_t retained_context_tokens = 0;
        std::size_t maximum_summary_bytes = 32 * 1024;
    };

    struct ContextUsage {
        std::size_t reported_input_tokens = 0;
        std::size_t estimated_tokens = 0;
    };

    struct ContextView {
        std::span<const std::string> items;
        std::span<const TurnBoundary> completed_turns;
        // Items at or after this index belong to the active turn and must remain exact.
        std::size_t protected_start;
        bool has_summary;
        std::uint64_t generation;
    };

    struct CompactionPlan {
        std::size_t summary_end;
        std::size_t retained_start;
        std::size_t first_retained_turn;
        std::uint64_t through_turn;
        std::uint64_t keep_from_turn;
        std::uint64_t generation;
    };

    struct PreparedCompaction {
        CompactionCheckpoint checkpoint;
        std::vector<std::string> input_items;
        std::vector<TurnBoundary> completed_turns;
    };

    // ContextCompactor owns policy and deterministic context transformations.
    // Network requests, durable writes, and live-state installation remain in CodexApi.
    class ContextCompactor {
    public:
        explicit ContextCompactor(CompactionConfig config);

        [[nodiscard]] bool needed(const ContextUsage &usage) const;

        std::expected<CompactionPlan, std::string> plan(const ContextView &context, bool retain_recent_turns = true) const;

        std::expected<PreparedCompaction, std::string> prepare(const ContextView &context, const CompactionPlan &plan, std::string summary) const;

        [[nodiscard]] std::string_view summaryInstructions() const;

    private:
        CompactionConfig config_;
    };

    std::size_t estimateContextTokens(std::span<const std::string> items);

} // namespace microcodex
