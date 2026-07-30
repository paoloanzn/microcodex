// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <string>

namespace microcodex {

    std::expected<int, std::string> edit(const std::string& path,  std::string_view old_content, std::string_view new_content, bool replaceAll);

} // namespace microcodex