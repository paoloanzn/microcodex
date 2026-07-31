// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

// termbox2 defines the attribute type used by rendered spans. Keep this include
// before standard headers so its POSIX feature-test macros take effect.
#include <termbox2.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microcodex::ui {

    struct StyledSpan {
        std::string text;
        uintattr_t foreground = TB_DEFAULT;
    };

    struct StyledLine {
        std::vector<StyledSpan> spans;
        uintattr_t background = TB_DEFAULT;
        bool fill_background = false;
    };

    std::pair<std::uint32_t, std::size_t> decodeCodepoint(
        std::string_view text, std::size_t offset);
    int textWidth(std::string_view text);

    // Adjacent spans with the same style are combined. This keeps terminal
    // drawing and wrapping cheap when a parser emits many small text pieces.
    void appendSpan(StyledLine &line, std::string_view text,
                    uintattr_t foreground = TB_DEFAULT);

    StyledLine lineWithPrefix(
        std::initializer_list<StyledSpan> prefix,
        uintattr_t background = TB_DEFAULT,
        bool fill_background = false);

    // Wrap styled text while preserving each span's color and attributes.
    // Prefix templates describe the first and continuation line indentation.
    std::vector<StyledLine> wrapStyledSpans(
        std::span<const StyledSpan> spans, int width,
        const StyledLine &first_template,
        const StyledLine &continuation_template);

    std::vector<StyledLine> wrapStyledText(
        std::string_view text, int width,
        const StyledLine &first_template,
        const StyledLine &continuation_template,
        uintattr_t foreground = TB_DEFAULT);

} // namespace microcodex::ui
