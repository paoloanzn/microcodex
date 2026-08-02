// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <string_view>

namespace microcodex {

    // Returns a short reason when a raw shell command matches the denylist.
    // This is a simple lexical guard.
    std::optional<std::string_view> deniedBashCommandReason(std::string_view command);

} // namespace microcodex
