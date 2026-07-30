// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "edit.h"
#include "read.h"

#include <cstddef>
#include <expected>
#include <fstream> 
#include <iostream>
#include <string_view>
#include <vector>

#define MAX_EDIT_LIMIT 0xFFFFFFFFFFFFFFFF

namespace {

    bool doesFileExist(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return false;
        }
        return true;
    }

    std::vector<size_t> findMatches(const std::string& target, const std::string_view substring, bool findAll) {
        std::vector<size_t> matches {};

        // Searching for an empty string would otherwise repeatedly find the
        // same position when collecting all matches.
        if (substring.empty()) {
            return matches;
        }

        size_t position = target.find(substring);
        while (position != std::string::npos) {
            matches.push_back(position);

            if (!findAll || position > target.size() - substring.size()) {
                break;
            }

            // Advance by the matched length so adjacent matches are found and
            // overlapping matches are intentionally excluded.
            position = target.find(substring, position + substring.size());
        }

        return matches;
    }

    int replaceSubstringAtPositions(std::string& target, const std::vector<size_t>& matches, const std::string_view replacement, const size_t matched_length) {
        // A replacement can invalidate offsets that occur after it, so process
        // the matches from right to left.  This keeps every supplied position
        // anchored to the original target string.
        for (auto match = matches.rbegin(); match != matches.rend(); ++match) {
            if (*match > target.size() || matched_length > target.size() - *match) {
                return -1;
            }

            target.replace(*match, matched_length, replacement);
        }

        return 0;
    }

} // namespace

namespace microcodex {

    std::expected<int, std::string> edit(const std::string& path, std::string_view old_content, std::string_view new_content, bool replaceAll) {
        if (!doesFileExist(path)) {
            return std::unexpected("File not found");
        }

        if (old_content.empty()) {
            return std::unexpected("Content to replace cannot be empty");
        }

        std::expected<std::string, std::string> file_content = read(path, 0, MAX_EDIT_LIMIT);
        if(!file_content) {
            return std::unexpected(file_content.error());
        }

        std::vector<size_t> matches = findMatches(file_content.value(), old_content, replaceAll);
        if (matches.empty()) {
            return std::unexpected("Content not found");
        }

        std::string content = std::move(file_content.value());
        if (replaceSubstringAtPositions(content, matches, new_content, old_content.size()) != 0) {
            return std::unexpected("Invalid match position");
        }

        std::ofstream file(path, std::ios::out  | std::ios::trunc);
        if (!file) {
            return std::unexpected("Could not create file");
        }

        file << content;

        if (!file) {
            return std::unexpected("Failed to write to file");
        }

        return 0;
    }

} // namespace microcodex
