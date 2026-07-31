// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "edit.h"
#include "read.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <fstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

    constexpr std::size_t maximum_edit_size = static_cast<std::size_t>(-1);

    bool doesFileExist(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return false;
        }
        return true;
    }

    std::vector<std::size_t> findMatches(const std::string& target, const std::string_view substring) {
        std::vector<std::size_t> matches;
        std::size_t position = target.find(substring);
        while (position != std::string::npos) {
            matches.push_back(position);
            position = target.find(substring, position + substring.size());
        }
        return matches;
    }

    struct Line {
        std::size_t begin;
        std::size_t end;
        std::string_view text;
    };

    std::vector<Line> splitLines(const std::string& text) {
        std::vector<Line> lines;
        for (std::size_t begin = 0; begin < text.size();) {
            const std::size_t newline = text.find('\n', begin);
            const std::size_t text_end = newline == std::string::npos ? text.size() : newline;
            const std::size_t end = newline == std::string::npos ? text.size() : newline + 1;
            std::string_view line{text.data() + begin, text_end - begin};
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            lines.push_back({begin, end, line});
            begin = end;
        }
        return lines;
    }

    std::pair<std::size_t, std::size_t> overlappingLines(
        const std::vector<Line>& lines, const std::size_t begin, const std::size_t end) {
        std::size_t first = 0;
        while (first < lines.size() && lines[first].end <= begin) {
            ++first;
        }
        std::size_t last = first;
        while (last < lines.size() && lines[last].begin < end) {
            ++last;
        }
        return {first, last};
    }

    std::size_t insertionLine(const std::vector<Line>& lines, const std::size_t position) {
        std::size_t index = 0;
        while (index < lines.size() && lines[index].end <= position) {
            ++index;
        }
        return index;
    }

    struct ChangedRange {
        std::size_t old_first;
        std::size_t old_last;
        std::size_t new_first;
        std::size_t new_last;
    };

    std::vector<microcodex::EditHunk> makeHunks(
        const std::string& before, const std::string& after,
        const std::vector<std::pair<std::size_t, std::size_t>>& positions,
        const std::size_t old_length, const std::size_t new_length) {
        if (before == after) {
            return {};
        }

        const std::vector<Line> old_lines = splitLines(before);
        const std::vector<Line> new_lines = splitLines(after);
        std::vector<ChangedRange> ranges;

        for (const auto& [old_begin, new_begin] : positions) {
            const std::size_t old_end = old_begin + old_length;
            const auto [old_first, old_last] = overlappingLines(old_lines, old_begin, old_end);

            std::size_t new_first;
            std::size_t new_last;
            if (new_length != 0) {
                std::tie(new_first, new_last) =
                    overlappingLines(new_lines, new_begin, new_begin + new_length);
            } else {
                new_first = insertionLine(new_lines, new_begin);
                new_last = new_first;

                // Removing complete lines needs no added line. A partial-line
                // removal changes the remaining line, so show that new line.
                const bool removes_complete_lines =
                    old_first < old_last && old_begin == old_lines[old_first].begin &&
                    old_end == old_lines[old_last - 1].end;
                if (!removes_complete_lines && !new_lines.empty()) {
                    if (new_first == new_lines.size()) {
                        new_first = new_lines.size() - 1;
                    }
                    new_last = new_first + 1;
                }
            }

            ChangedRange range{old_first, old_last, new_first, new_last};
            if (!ranges.empty() && range.old_first <= ranges.back().old_last &&
                range.new_first <= ranges.back().new_last) {
                ranges.back().old_last = std::max(ranges.back().old_last, range.old_last);
                ranges.back().new_last = std::max(ranges.back().new_last, range.new_last);
            } else {
                ranges.push_back(range);
            }
        }

        std::vector<microcodex::EditHunk> hunks;
        hunks.reserve(ranges.size());
        for (const ChangedRange& range : ranges) {
            microcodex::EditHunk hunk{
                .old_start = range.old_first + 1,
                .new_start = range.new_first + 1,
                .context_before = range.new_first == 0
                                      ? std::nullopt
                                      : std::optional<std::string>(new_lines[range.new_first - 1].text),
                .removed_lines = {},
                .added_lines = {},
                .context_after = range.new_last < new_lines.size()
                                     ? std::optional<std::string>(new_lines[range.new_last].text)
                                     : std::nullopt,
            };
            for (std::size_t index = range.old_first; index < range.old_last; ++index) {
                hunk.removed_lines.emplace_back(old_lines[index].text);
            }
            for (std::size_t index = range.new_first; index < range.new_last; ++index) {
                hunk.added_lines.emplace_back(new_lines[index].text);
            }
            hunks.push_back(std::move(hunk));
        }
        return hunks;
    }

} // namespace

namespace microcodex {

    std::expected<EditResult, std::string> edit(const std::string& path, const std::string_view old_content, const std::string_view new_content, const bool replaceAll) {
        if (!doesFileExist(path)) {
            return std::unexpected("File not found");
        }

        if (old_content.empty()) {
            return std::unexpected("Content to replace cannot be empty");
        }

        std::expected<std::string, std::string> file_content = read(path, 0, maximum_edit_size);
        if (!file_content) {
            return std::unexpected(file_content.error());
        }

        std::vector<std::size_t> matches = findMatches(*file_content, old_content);
        if (matches.empty()) {
            return std::unexpected("Content not found");
        }
        if (!replaceAll && matches.size() != 1) {
            return std::unexpected("Content occurs more than once; use replace_all to replace every occurrence");
        }
        if (!replaceAll) {
            matches.resize(1);
        }

        // Build the result from left to right. Besides being easier to follow
        // than offset-adjusted replacements, this records each new position for
        // the compact UI diff.
        std::string content;
        content.reserve(file_content->size());
        std::vector<std::pair<std::size_t, std::size_t>> positions;
        std::size_t cursor = 0;
        for (const std::size_t match : matches) {
            content.append(*file_content, cursor, match - cursor);
            const std::size_t new_position = content.size();
            content.append(new_content);
            positions.push_back({match, new_position});
            cursor = match + old_content.size();
        }
        content.append(*file_content, cursor, std::string::npos);

        std::vector<EditHunk> hunks = makeHunks(
            *file_content, content, positions, old_content.size(), new_content.size());

        std::ofstream file(path, std::ios::out  | std::ios::trunc);
        if (!file) {
            return std::unexpected("Could not create file");
        }

        file << content;

        if (!file) {
            return std::unexpected("Failed to write to file");
        }

        return EditResult{path, std::move(hunks)};
    }

} // namespace microcodex
