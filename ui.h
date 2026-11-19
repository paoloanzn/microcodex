// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <string>

#include "api.h"
#include "slash-command.h"

namespace microcodex {

// Run one interactive terminal session. The function owns the terminal
// until the user quits and restores it before returning. `commands` are the
// slash commands the composer offers.
std::expected<void, std::string> runInteractive(CodexApiConfig config, const SlashCommandRegistry& commands);

}  // namespace microcodex
