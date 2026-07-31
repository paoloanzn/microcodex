// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microcodex {

    // An edit hunk contains only the changed lines and one unchanged line on
    // either side. This is enough for the UI without retaining whole files.
    struct EditHunk {
        std::size_t old_start;
        std::size_t new_start;
        std::optional<std::string> context_before;
        std::vector<std::string> removed_lines;
        std::vector<std::string> added_lines;
        std::optional<std::string> context_after;
    };

    struct EditResult {
        std::string path;
        std::vector<EditHunk> hunks;
    };

    std::expected<EditResult, std::string> edit(const std::string& path, std::string_view old_content, std::string_view new_content, bool replaceAll);

} // namespace microcodex
