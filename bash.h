// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <string>

namespace microcodex {

    struct BashCommandResult {
        std::string stdout;
        std::string stderr;
        int error_code;
    };

    std::expected<microcodex::BashCommandResult, std::string> bash(const std::string& cmd);

} // namespace microcodex
