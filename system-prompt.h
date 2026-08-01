// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

namespace microcodex {

    // Base coding-agent instructions. Session-specific context such as the
    // available skills catalog is appended when the agent config is created.
    std::string_view codingAgentSystemPrompt();

} // namespace microcodex
