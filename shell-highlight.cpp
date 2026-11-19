// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "shell-highlight.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

#include "styled-text.h"

namespace {

constexpr uintattr_t command_style = 14 | TB_BOLD;
constexpr uintattr_t keyword_style = 13 | TB_BOLD;
constexpr uintattr_t option_style = 12;
constexpr uintattr_t string_style = 10;
constexpr uintattr_t variable_style = 14;
constexpr uintattr_t operator_style = 13;
constexpr uintattr_t comment_style = 245 | TB_DIM;

bool isShellOperator(const char character) {
    return std::string_view("|&;()<> ").find(character) != std::string_view::npos;
}

bool isKeyword(const std::string_view word) {
    constexpr std::array<std::string_view, 16> keywords = {
        "case",     "do", "done", "elif",   "else", "esac", "fi",    "for",
        "function", "if", "in",   "select", "then", "time", "until", "while",
    };
    return std::ranges::find(keywords, word) != keywords.end();
}

bool keywordStartsCommand(const std::string_view word) {
    constexpr std::array<std::string_view, 8> command_keywords = {
        "do", "elif", "else", "if", "then", "time", "until", "while",
    };
    return std::ranges::find(command_keywords, word) != command_keywords.end();
}

bool isAssignment(const std::string_view word) {
    const std::size_t equals = word.find('=');
    if (equals == std::string_view::npos || equals == 0) {
        return false;
    }
    const auto valid_first = [](const unsigned char character) {
        return std::isalpha(character) != 0 || character == '_';
    };
    const auto valid_rest = [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '_';
    };
    if (!valid_first(static_cast<unsigned char>(word.front()))) {
        return false;
    }
    return std::all_of(word.begin() + 1, word.begin() + static_cast<std::ptrdiff_t>(equals),
                       [&](const char character) { return valid_rest(static_cast<unsigned char>(character)); });
}

std::size_t quotedEnd(const std::string_view source, const std::size_t start) {
    const char quote = source[start];
    std::size_t position = start + 1;
    while (position < source.size()) {
        if (quote == '"' && source[position] == '\\' && position + 1 < source.size()) {
            position += 2;
        } else if (source[position++] == quote) {
            break;
        }
    }
    return position;
}

std::size_t variableEnd(const std::string_view source, const std::size_t start) {
    std::size_t position = start + 1;
    if (position >= source.size()) {
        return position;
    }
    if (source[position] == '{') {
        const std::size_t closing = source.find('}', position + 1);
        return closing == std::string_view::npos ? source.size() : closing + 1;
    }
    if (source[position] == '(') {
        // Command substitutions can contain full shell programs. Keeping
        // their balanced contents together is clearer than pretending to
        // parse nested shell grammar here.
        int depth = 1;
        position += 1;
        while (position < source.size() && depth != 0) {
            if (source[position] == '(') {
                ++depth;
            } else if (source[position] == ')') {
                --depth;
            }
            ++position;
        }
        return position;
    }
    if (std::isalnum(static_cast<unsigned char>(source[position])) != 0 || source[position] == '_') {
        while (position < source.size() &&
               (std::isalnum(static_cast<unsigned char>(source[position])) != 0 || source[position] == '_')) {
            ++position;
        }
    } else {
        ++position;
    }
    return position;
}

std::size_t operatorEnd(const std::string_view source, const std::size_t start) {
    constexpr std::array<std::string_view, 8> pairs = {
        "&&", "||", ">>", "<<", ";;", "|&", ">&", "<&",
    };
    if (start + 2 <= source.size()) {
        const std::string_view pair = source.substr(start, 2);
        if (std::ranges::find(pairs, pair) != pairs.end()) {
            return start + 2;
        }
    }
    return start + 1;
}

bool operatorStartsCommand(const std::string_view value) {
    return value == ";" || value == ";;" || value == "|" || value == "||" || value == "|&" || value == "&" ||
           value == "&&" || value == "(";
}

bool isSpace(const char character) { return std::isspace(static_cast<unsigned char>(character)) != 0; }

bool endsWord(const char character) {
    return isSpace(character) || isShellOperator(character) || character == '\'' || character == '"' ||
           character == '$' || character == '\\';
}

// Tokenizes one shell source string into styled spans. Words are colored by
// role: the first word of each command, keywords, options and assignments.
class ShellHighlighter {
public:
    explicit ShellHighlighter(const std::string_view source) : source_(source) {}

    std::vector<microcodex::ui::StyledSpan> run() && {
        while (position_ < source_.size()) {
            const char character = source_[position_];
            if (isSpace(character)) {
                appendWhitespace();
            } else if (character == '#' && at_word_start_) {
                appendComment();
            } else if (character == '\'' || character == '"') {
                appendWordPart(quotedEnd(source_, position_), string_style);
            } else if (character == '$') {
                appendWordPart(variableEnd(source_, position_), variable_style);
            } else if (character == '\\' && position_ + 1 < source_.size()) {
                appendWordPart(position_ + 2, string_style);
            } else if (isShellOperator(character) && character != ' ') {
                appendOperator();
            } else {
                appendWord();
            }
        }
        return std::move(line_.spans);
    }

private:
    void append(const std::size_t end, const uintattr_t style = TB_DEFAULT) {
        microcodex::ui::appendSpan(line_, source_.substr(position_, end - position_), style);
        position_ = end;
    }

    void appendWhitespace() {
        std::size_t end = position_;
        while (end < source_.size() && isSpace(source_[end])) {
            if (source_[end] == '\n') {
                expect_command_ = true;
            }
            ++end;
        }
        append(end);
        at_word_start_ = true;
    }

    void appendComment() {
        const std::size_t end = source_.find('\n', position_);
        append(end == std::string_view::npos ? source_.size() : end, comment_style);
    }

    // Quotes, variables and escapes are part of a word; one at the start of a
    // command stands in for the command name.
    void appendWordPart(const std::size_t end, const uintattr_t style) {
        append(end, style);
        if (at_word_start_ && expect_command_) {
            expect_command_ = false;
        }
        at_word_start_ = false;
    }

    void appendOperator() {
        const std::size_t end = operatorEnd(source_, position_);
        const std::string_view value = source_.substr(position_, end - position_);
        append(end, operator_style);
        if (operatorStartsCommand(value)) {
            expect_command_ = true;
        }
        at_word_start_ = true;
    }

    void appendWord() {
        std::size_t end = position_ + 1;
        while (end < source_.size() && !endsWord(source_[end])) {
            ++end;
        }
        const std::string_view word = source_.substr(position_, end - position_);
        if (isKeyword(word)) {
            append(end, keyword_style);
            expect_command_ = keywordStartsCommand(word);
        } else if (expect_command_ && isAssignment(word)) {
            append(end, variable_style);
        } else if (expect_command_) {
            append(end, command_style);
            expect_command_ = false;
        } else if (word.starts_with('-')) {
            append(end, option_style);
        } else {
            append(end);
        }
        at_word_start_ = false;
    }

    std::string_view source_;
    std::size_t position_ = 0;
    microcodex::ui::StyledLine line_;
    bool expect_command_ = true;
    bool at_word_start_ = true;
};

}  // namespace

namespace microcodex::ui {

std::vector<StyledSpan> highlightShell(const std::string_view source) { return ShellHighlighter(source).run(); }

}  // namespace microcodex::ui
