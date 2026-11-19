// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "styled-text.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace microcodex::ui {

std::pair<std::uint32_t, std::size_t> decodeCodepoint(const std::string_view text, const std::size_t offset) {
    if (offset >= text.size()) {
        return {0, 0};
    }

    const int requested = tb_utf8_char_length(text[offset]);
    if (requested <= 0 || offset + static_cast<std::size_t>(requested) > text.size()) {
        return {0xfffd, 1};
    }

    std::array<char, 7> utf8{};
    text.copy(utf8.data(), static_cast<std::size_t>(requested), offset);
    std::uint32_t codepoint = 0xfffd;
    const int decoded = tb_utf8_char_to_unicode(&codepoint, utf8.data());
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

void appendSpan(StyledLine& line, const std::string_view text, const uintattr_t foreground) {
    if (text.empty()) {
        return;
    }
    if (!line.spans.empty() && line.spans.back().foreground == foreground) {
        line.spans.back().text.append(text);
    } else {
        line.spans.push_back({std::string(text), foreground});
    }
}

StyledLine lineWithPrefix(const std::initializer_list<StyledSpan> prefix, const uintattr_t background,
                          const bool fill_background) {
    StyledLine line{
        .spans = {},
        .background = background,
        .fill_background = fill_background,
    };
    for (const StyledSpan& span : prefix) {
        appendSpan(line, span.text, span.foreground);
    }
    return line;
}

namespace {

// Tab stops are every four columns.
constexpr int tab_width = 4;

// Builds wrapped lines one codepoint at a time. Each new line starts from a
// prefix template, and wrapping happens only once the line holds content
// beyond that prefix.
class SpanWrapper {
public:
    SpanWrapper(StyledLine first_line, const int width, const StyledLine& continuation_template)
        : width_(width), continuation_template_(continuation_template), line_(std::move(first_line)) {
        startLine();
    }

    void appendSpan(const StyledSpan& span) {
        for (std::size_t offset = 0; offset < span.text.size();) {
            auto [codepoint, length] = decodeCodepoint(span.text, offset);
            if (length == 0) {
                break;
            }
            offset += length;
            appendCodepoint(codepoint, span.foreground);
        }
    }

    std::vector<StyledLine> finish() && {
        result_.push_back(std::move(line_));
        return std::move(result_);
    }

private:
    void startLine() {
        column_ = 0;
        for (const StyledSpan& span : line_.spans) {
            column_ += textWidth(span.text);
        }
        content_start_ = column_;
    }

    void startContinuation() {
        result_.push_back(std::move(line_));
        line_ = continuation_template_;
        startLine();
    }

    void appendTab(const uintattr_t foreground) {
        const int spaces = tab_width - (column_ % tab_width);
        for (int index = 0; index < spaces; ++index) {
            if (column_ >= width_ && column_ > content_start_) {
                startContinuation();
            }
            ui::appendSpan(line_, " ", foreground);
            ++column_;
        }
    }

    void appendCodepoint(std::uint32_t codepoint, const uintattr_t foreground) {
        if (codepoint == '\r') {
            return;
        }
        if (codepoint == '\n') {
            startContinuation();
            return;
        }
        if (codepoint == '\t') {
            appendTab(foreground);
            return;
        }

        int codepoint_width = tb_wcwidth(codepoint);
        if (codepoint_width < 0) {
            codepoint = 0xfffd;
            codepoint_width = 1;
        }
        if (codepoint_width > 0 && column_ + codepoint_width > width_ && column_ > content_start_) {
            startContinuation();
        }

        std::array<char, 7> utf8{};
        const int encoded = tb_utf8_unicode_to_char(utf8.data(), codepoint);
        if (encoded > 0) {
            ui::appendSpan(line_, std::string_view(utf8.data(), static_cast<std::size_t>(encoded)), foreground);
        }
        column_ += std::max(0, codepoint_width);
    }

    int width_;
    const StyledLine& continuation_template_;
    std::vector<StyledLine> result_;
    StyledLine line_;
    int column_ = 0;
    int content_start_ = 0;
};

}  // namespace

std::vector<StyledLine> wrapStyledSpans(const std::span<const StyledSpan> spans, const int requested_width,
                                        const StyledLine& first_template, const StyledLine& continuation_template) {
    SpanWrapper wrapper(first_template, std::max(1, requested_width), continuation_template);
    for (const StyledSpan& span : spans) {
        wrapper.appendSpan(span);
    }
    return std::move(wrapper).finish();
}

std::vector<StyledLine> wrapStyledText(const std::string_view text, const int width, const StyledLine& first_template,
                                       const StyledLine& continuation_template, const uintattr_t foreground) {
    const StyledSpan span{.text = std::string(text), .foreground = foreground};
    return wrapStyledSpans(std::span<const StyledSpan>(&span, 1), width, first_template, continuation_template);
}

}  // namespace microcodex::ui
