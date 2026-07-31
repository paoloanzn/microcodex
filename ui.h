// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "api.h"

#include <expected>
#include <string>

namespace microcodex {

    // Run one interactive terminal session. The function owns the terminal
    // until the user quits and restores it before returning.
    std::expected<void, std::string> runInteractive(CodexApiConfig config);

} // namespace microcodex
