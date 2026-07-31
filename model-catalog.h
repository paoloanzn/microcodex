// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "context-compaction.h"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microcodex {

    struct ModelContextLimits {
        std::string name;
        std::size_t context_window_tokens;
        std::size_t maximum_context_window_tokens;
        std::size_t effective_context_window_percent;
        std::size_t effective_context_window_tokens;
        std::size_t compact_at_tokens;
    };

    std::expected<std::vector<ModelContextLimits>, std::string> parseModelContextLimits(std::string_view response);
    std::expected<std::vector<ModelContextLimits>, std::string> fetchModelContextLimits(std::string_view responses_endpoint, std::string_view access_token, std::string_view account_id);
    const ModelContextLimits *findModelContextLimits(std::span<const ModelContextLimits> models, std::string_view name) noexcept;
    CompactionConfig compactionConfigForModel(const ModelContextLimits &model, std::size_t retained_context_tokens, std::size_t maximum_summary_bytes);

} // namespace microcodex
