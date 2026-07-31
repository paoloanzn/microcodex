// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "json.h"

#include <cctype>
#include <cstdint>
#include <utility>

namespace microcodex::json {

    using JsonMember = std::expected<std::optional<std::string_view>, std::string>;

    // Advance past JSON whitespace so callers can examine the next token.
    // Example: for "  true", starting at 0 leaves position at the 't'.
    void skipWhitespace(const std::string_view json, std::size_t &position) {
        while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position]))) {
            ++position;
        }
    }

    int hexDigit(const char character) {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    }

    std::expected<std::uint32_t, std::string> parseHexCodePoint(const std::string_view json, std::size_t &position) {
        if (json.size() - position < 4) {
            return std::unexpected("Incomplete Unicode escape in JSON string");
        }

        std::uint32_t code_point = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const int digit = hexDigit(json[position++]);
            if (digit < 0) {
                return std::unexpected("Invalid Unicode escape in JSON string");
            }
            code_point = code_point * 16 + static_cast<std::uint32_t>(digit);
        }

        return code_point;
    }

    // Encode one decoded \uXXXX value as UTF-8. Surrogate pairs are combined by
    // parseJsonString before reaching this routine.
    void appendUtf8(std::string &output, const std::uint32_t code_point) {
        if (code_point <= 0x7f) {
            output += static_cast<char>(code_point);
        } else if (code_point <= 0x7ff) {
            output += static_cast<char>(0xc0 | (code_point >> 6));
            output += static_cast<char>(0x80 | (code_point & 0x3f));
        } else if (code_point <= 0xffff) {
            output += static_cast<char>(0xe0 | (code_point >> 12));
            output += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (code_point & 0x3f));
        } else {
            output += static_cast<char>(0xf0 | (code_point >> 18));
            output += static_cast<char>(0x80 | ((code_point >> 12) & 0x3f));
            output += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (code_point & 0x3f));
        }
    }

    // Parse one quoted JSON string, unescaping characters and Unicode escapes.
    // Example: parsing "\"line\\n\"" returns a string containing line + '\n'.
    // On success position points immediately after the closing quote.
    std::expected<std::string, std::string> parseJsonString(const std::string_view json, std::size_t &position) {
        if (position == json.size() || json[position] != '"') {
            return std::unexpected("Expected a JSON string");
        }

        ++position;
        std::string result;

        while (position < json.size()) {
            const unsigned char character = static_cast<unsigned char>(json[position++]);

            if (character == '"') {
                return result;
            }
            if (character < 0x20) {
                return std::unexpected("Control character in JSON string");
            }
            if (character != '\\') {
                result += static_cast<char>(character);
                continue;
            }
            if (position == json.size()) {
                return std::unexpected("Incomplete escape in JSON string");
            }

            const char escaped = json[position++];
            switch (escaped) {
            case '"':
                result += '"';
                break;
            case '\\':
                result += '\\';
                break;
            case '/':
                result += '/';
                break;
            case 'b':
                result += '\b';
                break;
            case 'f':
                result += '\f';
                break;
            case 'n':
                result += '\n';
                break;
            case 'r':
                result += '\r';
                break;
            case 't':
                result += '\t';
                break;
            case 'u': {
                auto first = parseHexCodePoint(json, position);
                if (!first) {
                    return std::unexpected(first.error());
                }

                std::uint32_t code_point = first.value();
                if (code_point >= 0xd800 && code_point <= 0xdbff) {
                    if (json.size() - position < 6 || json[position] != '\\' ||
                        json[position + 1] != 'u') {
                        return std::unexpected("Missing low surrogate in JSON string");
                    }
                    position += 2;
                    auto second = parseHexCodePoint(json, position);
                    if (!second || second.value() < 0xdc00 || second.value() > 0xdfff) {
                        return std::unexpected("Invalid low surrogate in JSON string");
                    }
                    code_point =
                        0x10000 + ((code_point - 0xd800) << 10) + (second.value() - 0xdc00);
                } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
                    return std::unexpected("Unexpected low surrogate in JSON string");
                }

                appendUtf8(result, code_point);
                break;
            }
            default:
                return std::unexpected("Invalid escape in JSON string");
            }
        }

        return std::unexpected("Unterminated JSON string");
    }

    // Validate and skip exactly one JSON value without building a value tree.
    // This makes it possible to retain lightweight views, e.g. skip {"a":[1,2]}
    // and then slice the original input. The depth limit rejects hostile nesting.
    std::expected<void, std::string>
    skipJsonValue(const std::string_view json, std::size_t &position, const std::size_t depth) {
        if (depth > 128) {
            return std::unexpected("JSON is nested too deeply");
        }

        skipWhitespace(json, position);
        if (position == json.size()) {
            return std::unexpected("Expected a JSON value");
        }

        if (json[position] == '"') {
            auto string = parseJsonString(json, position);
            if (!string) {
                return std::unexpected(string.error());
            }
            return {};
        }

        if (json[position] == '{') {
            ++position;
            skipWhitespace(json, position);
            if (position < json.size() && json[position] == '}') {
                ++position;
                return {};
            }

            while (position < json.size()) {
                auto key = parseJsonString(json, position);
                if (!key) {
                    return std::unexpected(key.error());
                }
                skipWhitespace(json, position);
                if (position == json.size() || json[position++] != ':') {
                    return std::unexpected("Expected ':' in JSON object");
                }
                auto value = skipJsonValue(json, position, depth + 1);
                if (!value) {
                    return value;
                }
                skipWhitespace(json, position);
                if (position == json.size()) {
                    return std::unexpected("Unterminated JSON object");
                }
                const char separator = json[position++];
                if (separator == '}') {
                    return {};
                }
                if (separator != ',') {
                    return std::unexpected("Expected ',' in JSON object");
                }
                skipWhitespace(json, position);
            }
            return std::unexpected("Unterminated JSON object");
        }

        if (json[position] == '[') {
            ++position;
            skipWhitespace(json, position);
            if (position < json.size() && json[position] == ']') {
                ++position;
                return {};
            }

            while (position < json.size()) {
                auto value = skipJsonValue(json, position, depth + 1);
                if (!value) {
                    return value;
                }
                skipWhitespace(json, position);
                if (position == json.size()) {
                    return std::unexpected("Unterminated JSON array");
                }
                const char separator = json[position++];
                if (separator == ']') {
                    return {};
                }
                if (separator != ',') {
                    return std::unexpected("Expected ',' in JSON array");
                }
            }
            return std::unexpected("Unterminated JSON array");
        }

        for (const std::string_view literal : {"true", "false", "null"}) {
            if (json.substr(position, literal.size()) == literal) {
                position += literal.size();
                return {};
            }
        }

        const std::size_t start = position;
        if (json[position] == '-') {
            ++position;
        }
        if (position == json.size()) {
            return std::unexpected("Invalid JSON number");
        }
        if (json[position] == '0') {
            ++position;
        } else if (json[position] >= '1' && json[position] <= '9') {
            while (position < json.size() &&
                   std::isdigit(static_cast<unsigned char>(json[position]))) {
                ++position;
            }
        } else {
            return std::unexpected("Invalid JSON value");
        }
        if (position < json.size() && json[position] == '.') {
            ++position;
            const std::size_t fraction_start = position;
            while (position < json.size() &&
                   std::isdigit(static_cast<unsigned char>(json[position]))) {
                ++position;
            }
            if (position == fraction_start) {
                return std::unexpected("Invalid JSON number");
            }
        }
        if (position < json.size() && (json[position] == 'e' || json[position] == 'E')) {
            ++position;
            if (position < json.size() && (json[position] == '+' || json[position] == '-')) {
                ++position;
            }
            const std::size_t exponent_start = position;
            while (position < json.size() &&
                   std::isdigit(static_cast<unsigned char>(json[position]))) {
                ++position;
            }
            if (position == exponent_start) {
                return std::unexpected("Invalid JSON number");
            }
        }
        if (position == start) {
            return std::unexpected("Invalid JSON value");
        }
        return {};
    }

    // Locate a direct member of an object and return an unparsed view of its value.
    // Example: findJsonMember("{\"ok\":true}", "ok") yields the view "true".
    // A missing key is successful and represented by an empty optional.
    JsonMember findJsonMember(const std::string_view object, const std::string_view wanted_key) {
        std::size_t position = 0;
        skipWhitespace(object, position);
        if (position == object.size() || object[position++] != '{') {
            return std::unexpected("Expected a JSON object");
        }

        skipWhitespace(object, position);
        if (position < object.size() && object[position] == '}') {
            return std::optional<std::string_view>{};
        }

        while (position < object.size()) {
            auto key = parseJsonString(object, position);
            if (!key) {
                return std::unexpected(key.error());
            }
            skipWhitespace(object, position);
            if (position == object.size() || object[position++] != ':') {
                return std::unexpected("Expected ':' in JSON object");
            }
            skipWhitespace(object, position);

            const std::size_t value_start = position;
            auto value = skipJsonValue(object, position);
            if (!value) {
                return std::unexpected(value.error());
            }
            if (key.value() == wanted_key) {
                return std::optional<std::string_view>{
                    object.substr(value_start, position - value_start)};
            }

            skipWhitespace(object, position);
            if (position == object.size()) {
                return std::unexpected("Unterminated JSON object");
            }
            const char separator = object[position++];
            if (separator == '}') {
                return std::optional<std::string_view>{};
            }
            if (separator != ',') {
                return std::unexpected("Expected ',' in JSON object");
            }
            skipWhitespace(object, position);
        }

        return std::unexpected("Unterminated JSON object");
    }

    // Read an optional string-valued member, decoding JSON escapes when present.
    // Example: jsonStringMember("{\"name\":\"Ada\"}", "name") returns "Ada".
    std::expected<std::optional<std::string>, std::string>
    jsonStringMember(const std::string_view object, const std::string_view key) {
        auto member = findJsonMember(object, key);
        if (!member) {
            return std::unexpected(member.error());
        }
        if (!member.value()) {
            return std::optional<std::string>{};
        }

        std::size_t position = 0;
        auto value = parseJsonString(member.value().value(), position);
        if (!value) {
            return std::unexpected("JSON member '" + std::string(key) + "' is not a string");
        }
        skipWhitespace(member.value().value(), position);
        if (position != member.value()->size()) {
            return std::unexpected("Invalid JSON string member '" + std::string(key) + "'");
        }
        return std::optional<std::string>{std::move(value.value())};
    }

    // Like jsonStringMember, but report an error when the key is absent.
    std::expected<std::string, std::string> requiredJsonString(const std::string_view object, const std::string_view key) {
        auto value = jsonStringMember(object, key);
        if (!value) {
            return std::unexpected(value.error());
        }
        if (!value.value()) {
            return std::unexpected("Missing JSON string member '" + std::string(key) + "'");
        }
        return std::move(value.value().value());
    }

    // Split a JSON array into views of its original element text.
    // Example: jsonArrayElements("[1, {\"id\":2}]") returns "1" and "{\"id\":2}".
    // The views remain valid only while the input array text remains alive.
    std::expected<std::vector<std::string_view>, std::string>
    jsonArrayElements(const std::string_view array) {
        std::size_t position = 0;
        skipWhitespace(array, position);
        if (position == array.size() || array[position++] != '[') {
            return std::unexpected("Expected a JSON array");
        }

        std::vector<std::string_view> elements;
        skipWhitespace(array, position);
        if (position < array.size() && array[position] == ']') {
            return elements;
        }

        while (position < array.size()) {
            skipWhitespace(array, position);
            const std::size_t start = position;
            auto value = skipJsonValue(array, position);
            if (!value) {
                return std::unexpected(value.error());
            }
            elements.push_back(array.substr(start, position - start));

            skipWhitespace(array, position);
            if (position == array.size()) {
                return std::unexpected("Unterminated JSON array");
            }
            const char separator = array[position++];
            if (separator == ']') {
                skipWhitespace(array, position);
                if (position != array.size()) {
                    return std::unexpected("Unexpected data after JSON array");
                }
                return elements;
            }
            if (separator != ',') {
                return std::unexpected("Expected ',' in JSON array");
            }
        }

        return std::unexpected("Unterminated JSON array");
    }

    // Append a JSON-quoted string, escaping quotes, backslashes, and control bytes.
    // Example: appending "a\nb" produces "\"a\\nb\"" in the JSON output.
    void appendJsonString(std::string &json, const std::string_view value) {
        constexpr char hex[] = "0123456789abcdef";
        json += '"';
        for (const unsigned char character : value) {
            switch (character) {
            case '"': json += "\\\""; break;
            case '\\': json += "\\\\"; break;
            case '\b': json += "\\b"; break;
            case '\f': json += "\\f"; break;
            case '\n': json += "\\n"; break;
            case '\r': json += "\\r"; break;
            case '\t': json += "\\t"; break;
            default:
                if (character < 0x20) {
                    json += "\\u00";
                    json += hex[character >> 4];
                    json += hex[character & 0x0f];
                } else {
                    json += static_cast<char>(character);
                }
            }
        }
        json += '"';
    }


    // Return a required member as raw JSON text; use string() to decode it if needed.
    std::expected<std::string_view, std::string>
    scalarMember(const std::string_view object, const std::string_view name) {
        auto member = findJsonMember(object, name);
        if (!member) {
            return std::unexpected(member.error());
        }
        if (!*member) {
            return std::unexpected("Missing JSON member '" + std::string(name) + "'");
        }
        return **member;
    }

    // Decode a complete JSON string value, rejecting trailing non-whitespace data.
    // Example: string("\"hello\"") returns "hello".
    std::expected<std::string, std::string> string(const std::string_view value) {
        std::size_t position = 0;
        auto parsed = parseJsonString(value, position);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        skipWhitespace(value, position);
        if (position != value.size()) {
            return std::unexpected("Expected a JSON string");
        }
        return parsed;
    }

    // Compatibility wrapper for appendJsonString.
    void appendString(std::string &output, const std::string_view value) {
        appendJsonString(output, value);
    }

} // namespace microcodex::json
