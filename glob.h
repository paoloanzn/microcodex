// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <string>

namespace microcodex {

    // Expands pattern and returns matching paths separated by newlines.
    std::expected<std::string, std::string> glob(const std::string& pattern);

} // namespace microcodex
