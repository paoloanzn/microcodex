// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "response-item.h"

#include "json.h"

#include <utility>
#include <vector>

namespace microcodex {

    namespace {

        std::expected<std::string, std::string> messageText(const std::string_view item) {
            auto content = json::findJsonMember(item, "content");
            if (!content) {
                return std::unexpected(content.error());
            }
            if (!*content) {
                return std::unexpected("Response message has no content");
            }
            auto parts = json::jsonArrayElements(**content);
            if (!parts) {
                return std::unexpected(parts.error());
            }

            std::string text;
            for (const std::string_view part : *parts) {
                auto type = json::requiredJsonString(part, "type");
                if (!type) {
                    return std::unexpected(type.error());
                }
                if (*type != "input_text" && *type != "output_text") {
                    continue;
                }
                auto part_text = json::requiredJsonString(part, "text");
                if (!part_text) {
                    return std::unexpected(part_text.error());
                }
                text += *part_text;
            }
            return text;
        }

    } // namespace

    std::string userMessageItem(const std::string_view message) {
        std::string item =
            "{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":";
        json::appendJsonString(item, message);
        item += "}]}";
        return item;
    }

    std::string toolOutputItem(const CodexToolOutput &output) {
        std::string item = "{\"type\":\"function_call_output\",\"call_id\":";
        json::appendJsonString(item, output.call_id);
        item += ",\"output\":";
        json::appendJsonString(item, output.output);
        item += '}';
        return item;
    }

    std::expected<std::string, std::string> responseItemType(const std::string_view item) {
        return json::requiredJsonString(item, "type");
    }

    std::expected<std::optional<ResponseMessage>, std::string> responseMessage(const std::string_view item) {
        auto type = responseItemType(item);
        if (!type) {
            return std::unexpected(type.error());
        }
        if (*type != "message") {
            return std::optional<ResponseMessage>{};
        }

        auto role = json::requiredJsonString(item, "role");
        auto text = messageText(item);
        if (!role || !text) {
            return std::unexpected(!role ? role.error() : text.error());
        }
        return std::optional<ResponseMessage>(ResponseMessage{
            .role = std::move(*role),
            .text = std::move(*text),
        });
    }

    std::expected<std::optional<CodexToolCall>, std::string> responseToolCall(const std::string_view item) {
        auto type = responseItemType(item);
        if (!type) {
            return std::unexpected(type.error());
        }
        if (*type != "function_call") {
            return std::optional<CodexToolCall>{};
        }

        auto call_id = json::requiredJsonString(item, "call_id");
        auto name = json::requiredJsonString(item, "name");
        auto arguments = json::requiredJsonString(item, "arguments");
        if (!call_id || !name || !arguments) {
            return std::unexpected("Invalid function_call response item");
        }
        return std::optional<CodexToolCall>(CodexToolCall{
            .call_id = std::move(*call_id),
            .name = std::move(*name),
            .arguments = std::move(*arguments),
        });
    }

    std::expected<std::optional<CodexToolOutput>, std::string> responseToolOutput(const std::string_view item) {
        auto type = responseItemType(item);
        if (!type) {
            return std::unexpected(type.error());
        }
        if (*type != "function_call_output") {
            return std::optional<CodexToolOutput>{};
        }

        auto call_id = json::requiredJsonString(item, "call_id");
        auto output = json::requiredJsonString(item, "output");
        if (!call_id || !output) {
            return std::unexpected("Invalid function_call_output response item");
        }
        return std::optional<CodexToolOutput>(CodexToolOutput{
            .call_id = std::move(*call_id),
            .output = std::move(*output),
        });
    }

} // namespace microcodex
