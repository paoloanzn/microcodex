// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <string>

namespace microcodex {

    std::expected<std::string, std::string> read(const std::string& path, size_t offset, size_t limit);

} // namespace microcodex
