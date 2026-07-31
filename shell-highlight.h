// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "styled-text.h"

#include <string_view>
#include <vector>

namespace microcodex::ui {

    // Highlight shell source without changing its contents. This is a lexer,
    // not a shell parser; unfamiliar or incomplete syntax remains visible as
    // ordinary text.
    std::vector<StyledSpan> highlightShell(std::string_view source);

} // namespace microcodex::ui
