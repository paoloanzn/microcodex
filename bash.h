// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <stop_token>
#include <string>

namespace microcodex {

    struct BashCommandResult {
        std::string stdout;
        std::string stderr;
        int error_code;
    };

    std::expected<BashCommandResult, std::string> bash(const std::string &cmd);
    std::expected<BashCommandResult, std::string> bash(const std::string &cmd, std::stop_token stop_token);

} // namespace microcodex
