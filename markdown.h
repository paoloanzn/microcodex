// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "styled-text.h"

#include <string_view>
#include <vector>

namespace microcodex::ui {

    // Render Markdown into terminal-width physical lines. The source remains
    // authoritative; callers may invoke this again after a terminal resize.
    std::vector<StyledLine> renderMarkdown(std::string_view source, int width);

} // namespace microcodex::ui
