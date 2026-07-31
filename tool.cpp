// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "tool.h"

#include "json.h"

#include <charconv>

namespace microcodex {

    std::expected<std::string, std::string> ToolArguments::string(const std::string_view name) const {
        auto value = json::scalarMember(json_, name);
        if (!value) return std::unexpected(value.error());
        return json::string(*value);
    }

    std::expected<std::size_t, std::string> ToolArguments::size(const std::string_view name) const {
        auto value = json::scalarMember(json_, name);
        if (!value) return std::unexpected(value.error());
        std::size_t result = 0;
        const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), result);
        if (error != std::errc{} || end != value->data() + value->size()) {
            return std::unexpected("JSON member '" + std::string(name) + "' is not a non-negative integer");
        }
        return result;
    }

    std::expected<bool, std::string> ToolArguments::boolean(const std::string_view name) const {
        auto value = json::scalarMember(json_, name);
        if (!value) return std::unexpected(value.error());
        if (*value == "true") return true;
        if (*value == "false") return false;
        return std::unexpected("JSON member '" + std::string(name) + "' is not a boolean");
    }

    std::expected<bool, std::string> ToolArguments::boolean(const std::string_view name, const bool default_value) const {
        auto value = json::findJsonMember(json_, name);
        if (!value) return std::unexpected(value.error());
        if (!*value) return default_value;
        if (**value == "true") return true;
        if (**value == "false") return false;
        return std::unexpected("JSON member '" + std::string(name) + "' is not a boolean");
    }

} // namespace microcodex

namespace microcodex::detail {

    std::string toolJsonString(const std::string_view name, const std::string_view description, const std::string_view parameters) {
        std::string json = R"({"type":"function","name":)";
        microcodex::json::appendString(json, name);
        json += R"(,"description":)";
        microcodex::json::appendString(json, description);
        json += R"(,"strict":false,"parameters":)";
        json += parameters;
        json += '}';
        return json;
    }

} // namespace microcodex::detail
