// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "shell-highlight.h"

#include <algorithm>
#include <cctype>
#include <string_view>

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
        constexpr std::string_view keywords[] = {
            "case", "do", "done", "elif", "else", "esac", "fi", "for",
            "function", "if", "in", "select", "then", "time", "until",
            "while",
        };
        return std::ranges::find(keywords, word) != std::end(keywords);
    }

    bool keywordStartsCommand(const std::string_view word) {
        constexpr std::string_view command_keywords[] = {
            "do", "elif", "else", "if", "then", "time", "until", "while",
        };
        return std::ranges::find(command_keywords, word) != std::end(command_keywords);
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
                           [&](const char character) {
                               return valid_rest(static_cast<unsigned char>(character));
                           });
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
        if (std::isalnum(static_cast<unsigned char>(source[position])) != 0 ||
            source[position] == '_') {
            while (position < source.size() &&
                   (std::isalnum(static_cast<unsigned char>(source[position])) != 0 ||
                    source[position] == '_')) {
                ++position;
            }
        } else {
            ++position;
        }
        return position;
    }

    std::size_t operatorEnd(const std::string_view source, const std::size_t start) {
        constexpr std::string_view pairs[] = {
            "&&", "||", ">>", "<<", ";;", "|&", ">&", "<&",
        };
        if (start + 2 <= source.size()) {
            const std::string_view pair = source.substr(start, 2);
            if (std::ranges::find(pairs, pair) != std::end(pairs)) {
                return start + 2;
            }
        }
        return start + 1;
    }

    bool operatorStartsCommand(const std::string_view value) {
        return value == ";" || value == ";;" || value == "|" || value == "||" ||
               value == "|&" || value == "&" || value == "&&" || value == "(";
    }

} // namespace

namespace microcodex::ui {

    std::vector<StyledSpan> highlightShell(const std::string_view source) {
        StyledLine line;
        bool expect_command = true;
        bool at_word_start = true;

        for (std::size_t position = 0; position < source.size();) {
            const char character = source[position];

            if (std::isspace(static_cast<unsigned char>(character)) != 0) {
                if (character == '\n') {
                    expect_command = true;
                }
                const std::size_t start = position++;
                while (position < source.size() &&
                       std::isspace(static_cast<unsigned char>(source[position])) != 0) {
                    if (source[position] == '\n') {
                        expect_command = true;
                    }
                    ++position;
                }
                appendSpan(line, source.substr(start, position - start));
                at_word_start = true;
                continue;
            }

            if (character == '#' && at_word_start) {
                const std::size_t end = source.find('\n', position);
                const std::size_t length = end == std::string_view::npos
                                               ? source.size() - position
                                               : end - position;
                appendSpan(line, source.substr(position, length), comment_style);
                position += length;
                continue;
            }

            if (character == '\'' || character == '"') {
                const std::size_t end = quotedEnd(source, position);
                appendSpan(line, source.substr(position, end - position), string_style);
                if (at_word_start && expect_command) {
                    expect_command = false;
                }
                position = end;
                at_word_start = false;
                continue;
            }

            if (character == '$') {
                const std::size_t end = variableEnd(source, position);
                appendSpan(line, source.substr(position, end - position), variable_style);
                if (at_word_start && expect_command) {
                    expect_command = false;
                }
                position = end;
                at_word_start = false;
                continue;
            }

            if (character == '\\' && position + 1 < source.size()) {
                appendSpan(line, source.substr(position, 2), string_style);
                if (at_word_start && expect_command) {
                    expect_command = false;
                }
                position += 2;
                at_word_start = false;
                continue;
            }

            if (isShellOperator(character) && character != ' ') {
                const std::size_t end = operatorEnd(source, position);
                const std::string_view value = source.substr(position, end - position);
                appendSpan(line, value, operator_style);
                if (operatorStartsCommand(value)) {
                    expect_command = true;
                }
                position = end;
                at_word_start = true;
                continue;
            }

            const std::size_t start = position++;
            while (position < source.size() &&
                   std::isspace(static_cast<unsigned char>(source[position])) == 0 &&
                   !isShellOperator(source[position]) && source[position] != '\'' &&
                   source[position] != '"' && source[position] != '$' &&
                   source[position] != '\\') {
                ++position;
            }
            const std::string_view word = source.substr(start, position - start);
            if (isKeyword(word)) {
                appendSpan(line, word, keyword_style);
                expect_command = keywordStartsCommand(word);
            } else if (expect_command && isAssignment(word)) {
                appendSpan(line, word, variable_style);
            } else if (expect_command) {
                appendSpan(line, word, command_style);
                expect_command = false;
            } else if (word.starts_with('-')) {
                appendSpan(line, word, option_style);
            } else {
                appendSpan(line, word);
            }
            at_word_start = false;
        }

        return std::move(line.spans);
    }

} // namespace microcodex::ui
