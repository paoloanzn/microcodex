// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "styled-text.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace microcodex::ui {

    std::pair<std::uint32_t, std::size_t> decodeCodepoint(
        const std::string_view text, const std::size_t offset) {
        if (offset >= text.size()) {
            return {0, 0};
        }

        const int requested = tb_utf8_char_length(text[offset]);
        if (requested <= 0 || offset + static_cast<std::size_t>(requested) > text.size()) {
            return {0xfffd, 1};
        }

        char utf8[7]{};
        std::copy_n(text.data() + offset, requested, utf8);
        std::uint32_t codepoint = 0xfffd;
        const int decoded = tb_utf8_char_to_unicode(&codepoint, utf8);
        if (decoded <= 0) {
            return {0xfffd, 1};
        }
        return {codepoint, static_cast<std::size_t>(decoded)};
    }

    int textWidth(const std::string_view text) {
        int width = 0;
        for (std::size_t offset = 0; offset < text.size();) {
            auto [codepoint, length] = decodeCodepoint(text, offset);
            if (length == 0) {
                break;
            }
            width += std::max(0, tb_wcwidth(codepoint));
            offset += length;
        }
        return width;
    }

    void appendSpan(StyledLine &line, const std::string_view text, const uintattr_t foreground) {
        if (text.empty()) {
            return;
        }
        if (!line.spans.empty() && line.spans.back().foreground == foreground) {
            line.spans.back().text.append(text);
        } else {
            line.spans.push_back({std::string(text), foreground});
        }
    }

    StyledLine lineWithPrefix(const std::initializer_list<StyledSpan> prefix, const uintattr_t background, const bool fill_background) {
        StyledLine line{
            .spans = {},
            .background = background,
            .fill_background = fill_background,
        };
        for (const StyledSpan &span : prefix) {
            appendSpan(line, span.text, span.foreground);
        }
        return line;
    }

    std::vector<StyledLine> wrapStyledSpans(const std::span<const StyledSpan> spans, const int requested_width, const StyledLine &first_template, const StyledLine &continuation_template) {
        const int width = std::max(1, requested_width);
        std::vector<StyledLine> result;
        StyledLine line = first_template;
        int column = 0;
        for (const StyledSpan &span : line.spans) {
            column += textWidth(span.text);
        }
        int content_start = column;

        const auto startContinuation = [&] {
            result.push_back(std::move(line));
            line = continuation_template;
            column = 0;
            for (const StyledSpan &span : line.spans) {
                column += textWidth(span.text);
            }
            content_start = column;
        };

        for (const StyledSpan &span : spans) {
            for (std::size_t offset = 0; offset < span.text.size();) {
                auto [codepoint, length] = decodeCodepoint(span.text, offset);
                if (length == 0) {
                    break;
                }
                offset += length;

                if (codepoint == '\r') {
                    continue;
                }
                if (codepoint == '\n') {
                    startContinuation();
                    continue;
                }
                if (codepoint == '\t') {
                    const int spaces = 4 - (column % 4);
                    for (int index = 0; index < spaces; ++index) {
                        if (column >= width && column > content_start) {
                            startContinuation();
                        }
                        appendSpan(line, " ", span.foreground);
                        ++column;
                    }
                    continue;
                }

                int codepoint_width = tb_wcwidth(codepoint);
                if (codepoint_width < 0) {
                    codepoint = 0xfffd;
                    codepoint_width = 1;
                }
                if (codepoint_width > 0 && column + codepoint_width > width &&
                    column > content_start) {
                    startContinuation();
                }

                char utf8[7]{};
                const int encoded = tb_utf8_unicode_to_char(utf8, codepoint);
                if (encoded > 0) {
                    appendSpan(line,
                               std::string_view(utf8, static_cast<std::size_t>(encoded)),
                               span.foreground);
                }
                column += std::max(0, codepoint_width);
            }
        }

        result.push_back(std::move(line));
        return result;
    }

    std::vector<StyledLine> wrapStyledText(const std::string_view text, const int width, const StyledLine &first_template, const StyledLine &continuation_template, const uintattr_t foreground) {
        const StyledSpan span{std::string(text), foreground};
        return wrapStyledSpans(std::span<const StyledSpan>(&span, 1), width,
                               first_template, continuation_template);
    }

} // namespace microcodex::ui
