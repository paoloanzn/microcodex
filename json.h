// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microcodex::json {
    using JsonMember = std::expected<std::optional<std::string_view>, std::string>;

    void skipWhitespace(std::string_view json, std::size_t &position);
    std::expected<std::string, std::string> parseJsonString(std::string_view json, std::size_t &position);
    std::expected<void, std::string> skipJsonValue(std::string_view json, std::size_t &position, std::size_t depth = 0);
    JsonMember findJsonMember(std::string_view object, std::string_view key);
    std::expected<std::optional<std::string>, std::string> jsonStringMember(std::string_view object, std::string_view key);
    std::expected<std::string, std::string> requiredJsonString(std::string_view object, std::string_view key);
    std::expected<std::vector<std::string_view>, std::string> jsonArrayElements(std::string_view array);
    void appendJsonString(std::string &json, std::string_view value);

    std::expected<std::string_view, std::string>
    scalarMember(std::string_view object, std::string_view name);
    std::expected<std::string, std::string> string(std::string_view value);
    void appendString(std::string &json, std::string_view value);
} // namespace microcodex::json

