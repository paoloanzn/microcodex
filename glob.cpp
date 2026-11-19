// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "glob.h"

// The POSIX header, not this project's "glob.h".
#include <glob.h>  // NOLINT(readability-duplicate-include) IWYU pragma: keep

#include <expected>
#include <span>
#include <string>

namespace {

class GlobResult {
public:
    GlobResult() = default;
    GlobResult(const GlobResult&) = delete;
    GlobResult& operator=(const GlobResult&) = delete;
    GlobResult(GlobResult&&) = delete;
    GlobResult& operator=(GlobResult&&) = delete;

    ~GlobResult() { globfree(&value_); }

    glob_t* get() { return &value_; }

    [[nodiscard]] const glob_t& value() const { return value_; }

private:
    glob_t value_{};
};

std::string globErrorMessage(const int error_code) {
    switch (error_code) {
        case GLOB_NOSPACE: return "Not enough memory to expand pattern";
        case GLOB_ABORTED: return "Could not read a directory while expanding pattern";
        case GLOB_NOMATCH: return "No files matched pattern";
        default: return "Could not expand pattern";
    }
}

std::string joinMatches(const glob_t& matches) {
    std::string result;

    for (const char* path : std::span<char*>(matches.gl_pathv, matches.gl_pathc)) {
        if (!result.empty()) {
            result += '\n';
        }

        result += path;
    }

    return result;
}

}  // namespace

namespace microcodex {

std::expected<std::string, std::string> glob(const std::string& pattern) {
    if (pattern.empty()) {
        return std::unexpected("Pattern cannot be empty");
    }

    GlobResult matches;
    // POSIX has no reentrant glob, and concurrent tool calls only read the filesystem.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const int error_code = ::glob(pattern.c_str(), 0, nullptr, matches.get());

    if (error_code != 0) {
        return std::unexpected(globErrorMessage(error_code));
    }

    return joinMatches(matches.value());
}

}  // namespace microcodex
