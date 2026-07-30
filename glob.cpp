// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "glob.h"

#include <glob.h>

#include <expected>
#include <string>

namespace {

class GlobResult {
public:
    GlobResult() = default;
    GlobResult(const GlobResult&) = delete;
    GlobResult& operator=(const GlobResult&) = delete;

    ~GlobResult() {
        globfree(&value_);
    }

    glob_t* get() {
        return &value_;
    }

    const glob_t& value() const {
        return value_;
    }

private:
    glob_t value_{};
};

std::string globErrorMessage(const int error_code) {
    switch (error_code) {
        case GLOB_NOSPACE:
            return "Not enough memory to expand pattern";
        case GLOB_ABORTED:
            return "Could not read a directory while expanding pattern";
        case GLOB_NOMATCH:
            return "No files matched pattern";
        default:
            return "Could not expand pattern";
    }
}

std::string joinMatches(const glob_t& matches) {
    std::string result;

    for (std::size_t index = 0; index < matches.gl_pathc; ++index) {
        if (!result.empty()) {
            result += '\n';
        }

        result += matches.gl_pathv[index];
    }

    return result;
}

} // namespace

namespace microcodex {

    std::expected<std::string, std::string> glob(const std::string& pattern) {
        if (pattern.empty()) {
            return std::unexpected("Pattern cannot be empty");
        }

        GlobResult matches;
        const int error_code = ::glob(pattern.c_str(), 0, nullptr, matches.get());

        if (error_code != 0) {
            return std::unexpected(globErrorMessage(error_code));
        }

        return joinMatches(matches.value());
    }

} // namespace microcodex
