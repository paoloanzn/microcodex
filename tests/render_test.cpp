// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "../markdown.h"
#include "../shell-highlight.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using microcodex::ui::StyledLine;
    using microcodex::ui::StyledSpan;
    using microcodex::ui::renderMarkdown;
    using microcodex::ui::highlightShell;
    using microcodex::ui::textWidth;

    int failures = 0;

    void require(const bool condition, const std::string_view message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }

    std::string lineText(const StyledLine &line) {
        std::string text;
        for (const StyledSpan &span : line.spans) {
            text += span.text;
        }
        return text;
    }

    std::string renderedText(const std::vector<StyledLine> &lines) {
        std::string text;
        for (std::size_t index = 0; index < lines.size(); ++index) {
            if (index != 0) {
                text += '\n';
            }
            text += lineText(lines[index]);
        }
        return text;
    }

    bool hasStyledText(const std::vector<StyledLine> &lines,
                       const std::string_view text,
                       const uintattr_t attributes) {
        for (const StyledLine &line : lines) {
            for (const StyledSpan &span : line.spans) {
                if (span.text.find(text) != std::string::npos &&
                    (span.foreground & attributes) == attributes) {
                    return true;
                }
            }
        }
        return false;
    }

    bool hasForegroundText(const std::vector<StyledSpan> &spans,
                           const std::string_view text,
                           const uintattr_t foreground) {
        for (const StyledSpan &span : spans) {
            if (span.text.find(text) != std::string::npos &&
                (span.foreground & 0x00ff) == foreground) {
                return true;
            }
        }
        return false;
    }

    std::string spanText(const std::vector<StyledSpan> &spans) {
        std::string text;
        for (const StyledSpan &span : spans) {
            text += span.text;
        }
        return text;
    }

    void testInlineMarkdown() {
        const auto lines = renderMarkdown(
            "# Heading\n\nPlain **bold** and `code` with *emphasis*.", 80);
        const std::string text = renderedText(lines);

        require(text.find('#') == std::string::npos, "heading marker is not rendered");
        require(text.find("**") == std::string::npos, "strong markers are not rendered");
        require(hasStyledText(lines, "Heading", TB_BOLD | TB_UNDERLINE),
                "level-one heading is bold and underlined");
        require(hasStyledText(lines, "bold", TB_BOLD), "strong text is bold");
        require(hasStyledText(lines, "emphasis", TB_ITALIC), "emphasis is italic");
        require(hasStyledText(lines, "code", 14), "inline code uses the code color");
    }

    void testBlocksAndLists() {
        const auto lines = renderMarkdown(
            "> quoted text\n\n- first item\n- second item\n\n1. ordered\n2. next", 40);
        const std::string text = renderedText(lines);

        require(text.find("│ quoted text") != std::string::npos,
                "block quote uses a visible gutter");
        require(text.find("• first item") != std::string::npos,
                "unordered list uses a bullet");
        require(text.find("1. ordered") != std::string::npos,
                "ordered list retains its number");
        require(text.find("2. next") != std::string::npos,
                "ordered list increments its number");
    }

    void testCodeBlock() {
        const auto lines = renderMarkdown("```cpp\nint value = 3;\n```", 30);
        require(renderedText(lines).find("int value = 3;") != std::string::npos,
                "fenced code content is rendered");
        require(!lines.empty() && lines.front().fill_background &&
                    lines.front().background == 236,
                "fenced code uses the code-block surface");
        require(hasStyledText(lines, "cpp", TB_DIM),
                "fenced code language is shown unobtrusively");
    }

    void testTables() {
        const std::string markdown =
            "| Name | Value |\n| --- | --- |\n| one | a long value |\n";
        const std::string wide = renderedText(renderMarkdown(markdown, 60));
        const std::string narrow = renderedText(renderMarkdown(markdown, 12));

        require(wide.find("Name") != std::string::npos &&
                    wide.find("a long value") != std::string::npos,
                "wide table keeps its cells");
        require(narrow.find("Name: one") != std::string::npos,
                "narrow table falls back to header/value records");
        require(narrow.find("Value: a lo") != std::string::npos,
                "narrow record values wrap instead of disappearing");
    }

    void testWidthLimit() {
        const auto lines = renderMarkdown(
            "A deliberately long paragraph that must wrap on a narrow terminal.", 16);
        require(lines.size() > 1, "long Markdown wraps onto multiple lines");
        for (const StyledLine &line : lines) {
            require(textWidth(lineText(line)) <= 16,
                    "rendered Markdown line respects the requested width");
        }
    }

    void testShellHighlighting() {
        const std::string source =
            "FOO=bar echo \"$FOO\" --flag | grep value # explanation";
        const std::vector<StyledSpan> spans = highlightShell(source);

        require(spanText(spans) == source,
                "shell highlighting preserves the command exactly");
        require(hasForegroundText(spans, "FOO=bar", 14),
                "shell assignment is styled as a variable");
        require(hasStyledText({StyledLine{.spans = spans}}, "echo", TB_BOLD),
                "shell executable is bold");
        require(hasForegroundText(spans, "\"$FOO\"", 10),
                "quoted shell string uses the string color");
        require(hasForegroundText(spans, "--flag", 12),
                "shell option uses the option color");
        require(hasForegroundText(spans, "|", 13),
                "shell operator uses the operator color");
        require(hasStyledText({StyledLine{.spans = spans}}, "grep", TB_BOLD),
                "command after a pipe is recognized");
        require(hasStyledText({StyledLine{.spans = spans}}, "# explanation", TB_DIM),
                "shell comment is dimmed");

        const std::string incomplete = "echo 'unfinished";
        require(spanText(highlightShell(incomplete)) == incomplete,
                "incomplete shell quote remains visible");
    }

    void testShellCodeFence() {
        const auto lines = renderMarkdown("```bash\necho --version\n```", 40);
        require(hasStyledText(lines, "echo", TB_BOLD),
                "bash code fence uses shell highlighting");
    }

} // namespace

int main() {
    testInlineMarkdown();
    testBlocksAndLists();
    testCodeBlock();
    testTables();
    testWidthLimit();
    testShellHighlighting();
    testShellCodeFence();

    if (failures != 0) {
        std::cerr << failures << " render test(s) failed\n";
        return 1;
    }
    return 0;
}
