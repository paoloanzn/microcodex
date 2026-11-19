// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "bash-safety.h"

#include <array>
#include <optional>
#include <regex>
#include <string_view>

namespace {

struct DeniedCommandPattern {
    std::regex pattern;
    std::string_view reason;
};

const std::array<DeniedCommandPattern, 5> deniedCommandPatterns{{
    {
        .pattern =
            std::regex{
                R"((^|[[:space:];|&()])([^[:space:];|&()]+/)?rm[[:space:]]+([^;|&\n]*[[:space:]])?(--force|-[[:alpha:]]*f[[:alpha:]]*)($|[[:space:];|&]))",
                std::regex::extended | std::regex::icase,
            },
        .reason = "forced file removal is blocked",
    },
    {
        .pattern =
            std::regex{
                R"((^|[[:space:];|&()])git[[:space:]]+reset[[:space:]]+([^;|&\n]*[[:space:]])?--hard($|[[:space:];|&]))",
                std::regex::extended | std::regex::icase,
            },
        .reason = "git reset --hard is blocked",
    },
    {
        .pattern =
            std::regex{
                R"((^|[[:space:];|&()])git[[:space:]]+clean[[:space:]]+([^;|&\n]*[[:space:]])?(-[[:alpha:]]*f[[:alpha:]]*|--force)($|[[:space:];|&]))",
                std::regex::extended | std::regex::icase,
            },
        .reason = "forced git clean is blocked",
    },
    {
        .pattern =
            std::regex{
                R"((^|[[:space:];|&()])git[[:space:]]+checkout[[:space:]]+--($|[[:space:];|&]))",
                std::regex::extended | std::regex::icase,
            },
        .reason = "git checkout -- is blocked",
    },
    {
        .pattern =
            std::regex{
                R"((^|[[:space:];|&()])(mkfs(\.[[:alnum:]_-]+)?|fdisk|parted|shutdown|reboot|halt|poweroff)($|[[:space:];|&]))",
                std::regex::extended | std::regex::icase,
            },
        .reason = "system or disk destructive command is blocked",
    },
}};

}  // namespace

namespace microcodex {

std::optional<std::string_view> deniedBashCommandReason(const std::string_view command) {
    for (const DeniedCommandPattern& entry : deniedCommandPatterns) {
        if (std::regex_search(command.begin(), command.end(), entry.pattern)) {
            return entry.reason;
        }
    }
    return std::nullopt;
}

}  // namespace microcodex
