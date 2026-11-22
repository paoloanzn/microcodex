// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <stop_token>
#include <string>
#include <vector>

namespace microcodex {

struct BashCommandResult {
    std::string stdout;
    std::string stderr;
    int error_code;
};

std::expected<BashCommandResult, std::string> bash(const std::string& cmd);
std::expected<BashCommandResult, std::string> bash(const std::string& cmd, std::stop_token stop_token);

// Runs a program directly, without a shell or the denylist, in its own
// process group, with /dev/null as standard input, the default SIGINT and
// SIGTERM handling, and variable (NAME=VALUE) added to its environment. A
// stop sends SIGINT to the whole group, then SIGKILL three seconds later:
// long enough for a program that first stops its own commands, such as a
// MicroCodex sub-agent.
std::expected<BashCommandResult, std::string> runProgram(const std::vector<std::string>& arguments,
                                                         std::string variable, std::stop_token stop_token);

}  // namespace microcodex
