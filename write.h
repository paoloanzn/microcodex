// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <expected>

namespace microcodex {

    std::expected<int, std::string> write(const std::string& path, std::string_view content);

} // namespace microcodex