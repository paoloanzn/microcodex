// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "markdown.h"
#include "shell-highlight.h"

#include "vendor/md4c/src/md4c.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    using microcodex::ui::StyledLine;
    using microcodex::ui::StyledSpan;
    using microcodex::ui::appendSpan;
    using microcodex::ui::lineWithPrefix;
    using microcodex::ui::highlightShell;
    using microcodex::ui::textWidth;
    using microcodex::ui::wrapStyledSpans;
    using microcodex::ui::wrapStyledText;

    constexpr uintattr_t code_color = 14;
    constexpr uintattr_t link_color = 14;
    constexpr uintattr_t quote_color = 10;
    constexpr uintattr_t marker_color = 12;
    constexpr uintattr_t muted_color = 245;
    constexpr uintattr_t code_background = 236;
    constexpr uintattr_t color_mask = 0x00ff;

    uintattr_t mergeStyle(const uintattr_t base, const uintattr_t overlay) {
        const uintattr_t overlay_color = overlay & color_mask;
        const uintattr_t color = overlay_color != 0 ? overlay_color : base & color_mask;
        const uintattr_t attributes = (base | overlay) & ~color_mask;
        return color | attributes;
    }

    void appendLines(std::vector<StyledLine> &destination, std::vector<StyledLine> source) {
        for (StyledLine &line : source) {
            destination.push_back(std::move(line));
        }
    }

    bool isBlank(const StyledLine &line) {
        return line.spans.empty();
    }

    std::string plainText(const std::vector<StyledSpan> &spans) {
        std::string text;
        for (const StyledSpan &span : spans) {
            text += span.text;
        }
        return text;
    }

    std::string repeatText(const std::string_view text, const std::size_t count) {
        std::string result;
        result.reserve(text.size() * count);
        for (std::size_t index = 0; index < count; ++index) {
            result += text;
        }
        return result;
    }

    std::string attributeText(const MD_ATTRIBUTE &attribute) {
        if (attribute.text == nullptr || attribute.size == 0) {
            return {};
        }
        return std::string(attribute.text, attribute.size);
    }

    bool isShellLanguage(const std::string_view language) {
        return language == "bash" || language == "sh" || language == "shell";
    }

    struct ListState {
        bool ordered = false;
        bool tight = true;
        unsigned next_number = 1;
        std::string marker;
        bool marker_pending = false;
    };

    struct TableState {
        using Cell = std::vector<StyledSpan>;
        using Row = std::vector<Cell>;

        std::vector<Row> rows;
        Row current_row;
    };

    // MD4C is callback-based. This object owns the small amount of state
    // needed while one document is being translated into styled terminal rows.
    class MarkdownRenderer {
    public:
        explicit MarkdownRenderer(const int width) : width_(std::max(1, width)) {}

        std::vector<StyledLine> render(const std::string_view source) {
            MD_PARSER parser{};
            parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                           MD_FLAG_NOHTML;
            parser.enter_block = enterBlockCallback;
            parser.leave_block = leaveBlockCallback;
            parser.enter_span = enterSpanCallback;
            parser.leave_span = leaveSpanCallback;
            parser.text = textCallback;

            const int parsed = md_parse(source.data(), static_cast<MD_SIZE>(source.size()),
                                        &parser, this);
            if (parsed != 0 || failed_) {
                return plainFallback(source);
            }
            trimTrailingBlankLines();
            return std::move(lines_);
        }

    private:
        static int enterBlockCallback(const MD_BLOCKTYPE type, void *detail, void *userdata) noexcept {
            return callSafely(userdata, [=](MarkdownRenderer &renderer) {
                renderer.enterBlock(type, detail);
            });
        }

        static int leaveBlockCallback(const MD_BLOCKTYPE type, void *detail, void *userdata) noexcept {
            return callSafely(userdata, [=](MarkdownRenderer &renderer) {
                renderer.leaveBlock(type, detail);
            });
        }

        static int enterSpanCallback(const MD_SPANTYPE type, void *detail, void *userdata) noexcept {
            return callSafely(userdata, [=](MarkdownRenderer &renderer) {
                renderer.enterSpan(type, detail);
            });
        }

        static int leaveSpanCallback(const MD_SPANTYPE type, void *detail, void *userdata) noexcept {
            return callSafely(userdata, [=](MarkdownRenderer &renderer) {
                renderer.leaveSpan(type, detail);
            });
        }

        static int textCallback(const MD_TEXTTYPE type, const MD_CHAR *text, const MD_SIZE size, void *userdata) noexcept {
            return callSafely(userdata, [=](MarkdownRenderer &renderer) {
                renderer.addText(type, std::string_view(text, size));
            });
        }

        template <typename Function>
        static int callSafely(void *userdata, Function function) noexcept {
            auto &renderer = *static_cast<MarkdownRenderer *>(userdata);
            try {
                function(renderer);
                return 0;
            } catch (...) {
                // Exceptions must never cross the C callback boundary.
                renderer.failed_ = true;
                return 1;
            }
        }

        void enterBlock(const MD_BLOCKTYPE type, void *detail) {
            switch (type) {
            case MD_BLOCK_QUOTE:
                ++quote_depth_;
                break;
            case MD_BLOCK_UL: {
                finishPendingText();
                const auto &list = *static_cast<MD_BLOCK_UL_DETAIL *>(detail);
                lists_.push_back({
                    .ordered = false,
                    .tight = list.is_tight != 0,
                    .next_number = 1,
                    .marker = {},
                    .marker_pending = false,
                });
                break;
            }
            case MD_BLOCK_OL: {
                finishPendingText();
                const auto &list = *static_cast<MD_BLOCK_OL_DETAIL *>(detail);
                lists_.push_back({
                    .ordered = true,
                    .tight = list.is_tight != 0,
                    .next_number = list.start,
                    .marker = {},
                    .marker_pending = false,
                });
                break;
            }
            case MD_BLOCK_LI:
                finishPendingText();
                beginListItem(*static_cast<MD_BLOCK_LI_DETAIL *>(detail));
                break;
            case MD_BLOCK_H:
                beginHeading(static_cast<MD_BLOCK_H_DETAIL *>(detail)->level);
                break;
            case MD_BLOCK_P:
                beginParagraph();
                break;
            case MD_BLOCK_CODE: {
                in_code_block_ = true;
                code_.clear();
                const auto &code = *static_cast<MD_BLOCK_CODE_DETAIL *>(detail);
                code_language_ = attributeText(code.lang);
                break;
            }
            case MD_BLOCK_THEAD:
            case MD_BLOCK_TBODY:
                break;
            case MD_BLOCK_TABLE:
                table_.emplace();
                break;
            case MD_BLOCK_TR:
                if (table_) {
                    table_->current_row.clear();
                }
                break;
            case MD_BLOCK_TH:
            case MD_BLOCK_TD:
                current_.spans.clear();
                break;
            case MD_BLOCK_HR:
                appendHorizontalRule();
                break;
            case MD_BLOCK_DOC:
            case MD_BLOCK_HTML:
                break;
            }
        }

        void leaveBlock(const MD_BLOCKTYPE type, void *) {
            switch (type) {
            case MD_BLOCK_QUOTE:
                if (quote_depth_ > 0) {
                    --quote_depth_;
                }
                break;
            case MD_BLOCK_UL:
            case MD_BLOCK_OL:
                if (!lists_.empty()) {
                    lists_.pop_back();
                }
                appendBlankLine();
                break;
            case MD_BLOCK_H:
                finishTextBlock(true);
                block_style_ = TB_DEFAULT;
                break;
            case MD_BLOCK_P:
                finishTextBlock(lists_.empty() || !lists_.back().tight);
                break;
            case MD_BLOCK_CODE:
                renderCodeBlock();
                in_code_block_ = false;
                appendBlankLine();
                break;
            case MD_BLOCK_TH:
            case MD_BLOCK_TD:
                finishTableCell();
                break;
            case MD_BLOCK_TR:
                if (table_) {
                    table_->rows.push_back(std::move(table_->current_row));
                    table_->current_row.clear();
                }
                break;
            case MD_BLOCK_TABLE:
                renderTable();
                table_.reset();
                appendBlankLine();
                break;
            case MD_BLOCK_DOC:
            case MD_BLOCK_HR:
            case MD_BLOCK_HTML:
            case MD_BLOCK_THEAD:
            case MD_BLOCK_TBODY:
                break;
            case MD_BLOCK_LI:
                finishPendingText();
                break;
            }
        }

        void enterSpan(const MD_SPANTYPE type, void *) {
            inline_styles_.push_back(spanStyle(type));
            if (type == MD_SPAN_IMG) {
                ensureTextBlock();
                appendCurrent("[image: ");
            }
        }

        void leaveSpan(const MD_SPANTYPE type, void *) {
            if (type == MD_SPAN_IMG) {
                appendCurrent("]");
            }
            if (!inline_styles_.empty()) {
                inline_styles_.pop_back();
            }
        }

        void addText(const MD_TEXTTYPE type, const std::string_view text) {
            if (in_code_block_) {
                code_.append(text);
                return;
            }
            // Tight list items do not always contain a paragraph block. Start
            // their text lazily so they use the same prefix path as paragraphs.
            ensureTextBlock();
            if (type == MD_TEXT_NULLCHAR) {
                appendCurrent("�");
            } else if (type == MD_TEXT_BR) {
                appendCurrent("\n");
            } else if (type == MD_TEXT_SOFTBR) {
                appendCurrent(" ");
            } else if (type != MD_TEXT_HTML) {
                appendCurrent(text);
            }
        }

        void beginListItem(const MD_BLOCK_LI_DETAIL &item) {
            if (lists_.empty()) {
                return;
            }
            ListState &list = lists_.back();
            if (list.ordered) {
                list.marker = std::to_string(list.next_number++) + ". ";
            } else {
                list.marker = "• ";
            }
            if (item.is_task != 0) {
                const bool checked = item.task_mark == 'x' || item.task_mark == 'X';
                list.marker += checked ? "[x] " : "[ ] ";
            }
            list.marker_pending = true;
        }

        void beginHeading(const unsigned level) {
            beginTextBlock();
            if (level == 1) {
                block_style_ = TB_BOLD | TB_UNDERLINE;
            } else if (level == 2) {
                block_style_ = TB_BOLD;
            } else if (level == 3) {
                block_style_ = TB_BOLD | TB_ITALIC;
            } else {
                block_style_ = TB_ITALIC;
            }
        }

        void beginParagraph() {
            beginTextBlock();
            auto [first, continuation] = paragraphPrefixes();
            first_prefix_ = std::move(first);
            continuation_prefix_ = std::move(continuation);
        }

        void beginTextBlock() {
            current_ = {};
            text_block_active_ = true;
            block_style_ = TB_DEFAULT;
            auto [first, continuation] = quotePrefixes();
            first_prefix_ = std::move(first);
            continuation_prefix_ = std::move(continuation);
        }

        std::pair<StyledLine, StyledLine> quotePrefixes() const {
            StyledLine first;
            StyledLine continuation;
            for (std::size_t depth = 0; depth < quote_depth_; ++depth) {
                appendSpan(first, "│ ", quote_color | TB_DIM);
                appendSpan(continuation, "│ ", quote_color | TB_DIM);
            }
            return {std::move(first), std::move(continuation)};
        }

        std::pair<StyledLine, StyledLine> paragraphPrefixes() {
            auto [first, continuation] = quotePrefixes();
            if (lists_.empty()) {
                return {std::move(first), std::move(continuation)};
            }

            const std::string nesting((lists_.size() - 1) * 2, ' ');
            appendSpan(first, nesting);
            appendSpan(continuation, nesting);

            ListState &list = lists_.back();
            const std::string marker = list.marker_pending ? list.marker : std::string{};
            if (!marker.empty()) {
                appendSpan(first, marker, marker_color);
                list.marker_pending = false;
            }
            const int marker_width = textWidth(list.marker);
            appendSpan(continuation,
                       std::string(static_cast<std::size_t>(marker_width), ' '));
            if (marker.empty()) {
                appendSpan(first,
                           std::string(static_cast<std::size_t>(marker_width), ' '));
            }
            return {std::move(first), std::move(continuation)};
        }

        void finishTextBlock(const bool blank_after) {
            if (!current_.spans.empty()) {
                appendLines(lines_, wrapStyledSpans(current_.spans, width_,
                                                     first_prefix_, continuation_prefix_));
            }
            current_ = {};
            text_block_active_ = false;
            inline_styles_.clear();
            if (blank_after) {
                appendBlankLine();
            }
        }

        void finishPendingText() {
            if (text_block_active_) {
                finishTextBlock(false);
            }
        }

        void ensureTextBlock() {
            if (!table_ && !text_block_active_) {
                beginParagraph();
            }
        }

        void appendCurrent(const std::string_view text) {
            if (table_ && text.find_first_of("\r\n") != std::string_view::npos) {
                std::string flattened(text);
                std::replace(flattened.begin(), flattened.end(), '\r', ' ');
                std::replace(flattened.begin(), flattened.end(), '\n', ' ');
                appendSpan(current_, flattened, currentStyle());
            } else {
                appendSpan(current_, text, currentStyle());
            }
        }

        uintattr_t currentStyle() const {
            uintattr_t style = block_style_;
            for (const uintattr_t inline_style : inline_styles_) {
                style = mergeStyle(style, inline_style);
            }
            return style;
        }

        uintattr_t spanStyle(const MD_SPANTYPE type) const {
            switch (type) {
            case MD_SPAN_EM:
                return TB_ITALIC;
            case MD_SPAN_STRONG:
                return TB_BOLD;
            case MD_SPAN_A:
            case MD_SPAN_WIKILINK:
                return link_color | TB_UNDERLINE;
            case MD_SPAN_CODE:
            case MD_SPAN_LATEXMATH:
            case MD_SPAN_LATEXMATH_DISPLAY:
                return code_color;
            case MD_SPAN_DEL:
                // 16-bit termbox cells do not include strikeout. Dim text is
                // the closest low-cost representation.
                return TB_DIM;
            case MD_SPAN_IMG:
            case MD_SPAN_U:
                return TB_DEFAULT;
            }
            return TB_DEFAULT;
        }

        void renderCodeBlock() {
            if (!code_language_.empty()) {
                StyledLine language;
                language.background = code_background;
                language.fill_background = true;
                appendSpan(language, "  " + code_language_, muted_color | TB_DIM);
                lines_.push_back(std::move(language));
            }

            StyledLine first = lineWithPrefix({{"  ", muted_color}}, code_background, true);
            StyledLine continuation = first;
            if (isShellLanguage(code_language_)) {
                const std::vector<StyledSpan> highlighted = highlightShell(code_);
                appendLines(lines_, wrapStyledSpans(highlighted, width_, first, continuation));
            } else {
                const StyledSpan code_span{code_, code_color};
                appendLines(lines_, wrapStyledSpans(
                    std::span<const StyledSpan>(&code_span, 1), width_, first, continuation));
            }
        }

        void finishTableCell() {
            if (!table_) {
                current_ = {};
                return;
            }
            table_->current_row.push_back(std::move(current_.spans));
            current_ = {};
        }

        void renderTable() {
            if (!table_ || table_->rows.empty()) {
                return;
            }
            const std::vector<std::size_t> widths = tableWidths(table_->rows);
            std::size_t total_width = widths.empty() ? 0 : (widths.size() - 1) * 2;
            for (const std::size_t width : widths) {
                total_width += width;
            }
            if (total_width <= static_cast<std::size_t>(width_)) {
                renderColumnTable(table_->rows, widths);
            } else {
                renderRecordTable(table_->rows);
            }
        }

        std::vector<std::size_t> tableWidths(const std::vector<TableState::Row> &rows) const {
            std::size_t columns = 0;
            for (const TableState::Row &row : rows) {
                columns = std::max(columns, row.size());
            }
            std::vector<std::size_t> widths(columns, 0);
            for (const TableState::Row &row : rows) {
                for (std::size_t column = 0; column < row.size(); ++column) {
                    widths[column] = std::max(
                        widths[column],
                        static_cast<std::size_t>(std::max(0, textWidth(plainText(row[column])))));
                }
            }
            return widths;
        }

        void renderColumnTable(const std::vector<TableState::Row> &rows, const std::vector<std::size_t> &widths) {
            for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
                StyledLine line;
                const TableState::Row &row = rows[row_index];
                for (std::size_t column = 0; column < widths.size(); ++column) {
                    if (column != 0) {
                        appendSpan(line, "  ");
                    }
                    const std::vector<StyledSpan> empty;
                    const std::vector<StyledSpan> &cell =
                        column < row.size() ? row[column] : empty;
                    const uintattr_t header_style = row_index == 0 ? TB_BOLD : TB_DEFAULT;
                    for (const StyledSpan &span : cell) {
                        appendSpan(line, span.text, mergeStyle(span.foreground, header_style));
                    }
                    const int cell_width = textWidth(plainText(cell));
                    const std::size_t padding = widths[column] > static_cast<std::size_t>(cell_width)
                                                    ? widths[column] - static_cast<std::size_t>(cell_width)
                                                    : 0;
                    appendSpan(line, std::string(padding, ' '));
                }
                lines_.push_back(std::move(line));
                if (row_index == 0 && rows.size() > 1) {
                    StyledLine separator;
                    const std::size_t separator_width = std::min(
                        static_cast<std::size_t>(width_),
                        static_cast<std::size_t>(std::max(0, textWidth(plainText(lines_.back().spans)))));
                    appendSpan(separator, repeatText("─", separator_width),
                               muted_color | TB_DIM);
                    lines_.push_back(std::move(separator));
                }
            }
        }

        void renderRecordTable(const std::vector<TableState::Row> &rows) {
            if (rows.size() < 2) {
                if (rows.front().empty()) {
                    return;
                }
                const std::string text = plainText(rows.front().front());
                appendLines(lines_, wrapStyledText(text, width_, {}, {}));
                return;
            }
            const TableState::Row &headers = rows.front();
            for (std::size_t row_index = 1; row_index < rows.size(); ++row_index) {
                const TableState::Row &row = rows[row_index];
                for (std::size_t column = 0; column < row.size(); ++column) {
                    StyledLine content;
                    const std::string header = column < headers.size()
                                                   ? plainText(headers[column])
                                                   : "Column " + std::to_string(column + 1);
                    appendSpan(content, header + ": ", TB_BOLD);
                    for (const StyledSpan &span : row[column]) {
                        appendSpan(content, span.text, span.foreground);
                    }
                    appendLines(lines_, wrapStyledSpans(
                        content.spans, width_, {}, lineWithPrefix({{"  ", TB_DEFAULT}})));
                }
                if (row_index + 1 < rows.size()) {
                    appendBlankLine();
                }
            }
        }

        void appendHorizontalRule() {
            appendBlankLine();
            StyledLine rule;
            appendSpan(rule, repeatText("─", static_cast<std::size_t>(std::max(1, width_))),
                       muted_color | TB_DIM);
            lines_.push_back(std::move(rule));
            appendBlankLine();
        }

        void appendBlankLine() {
            if (!lines_.empty() && !isBlank(lines_.back())) {
                lines_.push_back({});
            }
        }

        void trimTrailingBlankLines() {
            while (!lines_.empty() && isBlank(lines_.back())) {
                lines_.pop_back();
            }
        }

        std::vector<StyledLine> plainFallback(const std::string_view source) const {
            return wrapStyledText(source, width_, {}, {});
        }

        int width_;
        std::vector<StyledLine> lines_;
        StyledLine current_;
        StyledLine first_prefix_;
        StyledLine continuation_prefix_;
        std::vector<uintattr_t> inline_styles_;
        std::vector<ListState> lists_;
        std::optional<TableState> table_;
        std::string code_;
        std::string code_language_;
        uintattr_t block_style_ = TB_DEFAULT;
        std::size_t quote_depth_ = 0;
        bool in_code_block_ = false;
        bool text_block_active_ = false;
        bool failed_ = false;
    };

} // namespace

namespace microcodex::ui {

    std::vector<StyledLine> renderMarkdown(const std::string_view source, const int width) {
        return MarkdownRenderer(width).render(source);
    }

} // namespace microcodex::ui
