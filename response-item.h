// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace microcodex {

    struct CodexToolCall {
        std::string call_id;
        std::string name;
        // The Responses API sends function arguments as JSON encoded in a string.
        std::string arguments;
    };

    struct CodexToolOutput {
        std::string call_id;
        std::string output;
    };

    struct ResponseMessage {
        std::string role;
        std::string text;
    };

    std::string userMessageItem(std::string_view message);
    std::string assistantMessageItem(std::string_view message);
    std::string toolOutputItem(const CodexToolOutput &output);

    std::expected<std::string, std::string> responseItemType(std::string_view item);
    std::expected<std::optional<ResponseMessage>, std::string> responseMessage(std::string_view item);
    std::expected<std::optional<CodexToolCall>, std::string> responseToolCall(std::string_view item);
    std::expected<std::optional<CodexToolOutput>, std::string> responseToolOutput(std::string_view item);

} // namespace microcodex
