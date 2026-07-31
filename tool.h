// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "edit.h"

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace microcodex {
    // Tool output sent back to the model stays separate from optional display
    // data. This keeps large diffs out of the model's conversation history.
    // NOTE: Yes, this smells a little. If more tools need custom display data,
    // replace this edit-specific field with a small generic wrapper instead of
    // growing one special field per tool.
    struct ToolResult {
        std::string output;
        std::shared_ptr<const EditResult> edit;
    };

    // Parses the flat scalar argument objects used by the built-in file tools.
    class ToolArguments {
    public:
        explicit ToolArguments(std::string_view json) : json_(json) {}

        std::expected<std::string, std::string> string(std::string_view name) const;
        std::expected<std::size_t, std::string> size(std::string_view name) const;
        std::expected<bool, std::string> boolean(std::string_view name) const;
        std::expected<bool, std::string> boolean(std::string_view name, bool default_value) const;

    private:
        std::string_view json_;
    };

    class ToolBase {
    public:
        virtual ~ToolBase() = default;

        virtual std::string_view name() const = 0;
        virtual std::string toJsonString() const = 0;
        virtual std::expected<ToolResult, std::string> executeJson(std::string_view arguments, std::stop_token stop_token = {}) const = 0;
    };

    namespace detail {
        std::string toolJsonString(std::string_view name, std::string_view description, std::string_view parameters);
    }

    // Example for `std::expected<int, std::string> write(const std::string &, std::string_view)`.
    // ToolArguments provides the shared JSON decoding; the adapter only calls the tool.
    // Tool<std::expected<int, std::string>, const std::string &, std::string_view> writeTool{
    //     "write", "Write content to a new file.", write,
    //     R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"],"additionalProperties":false})",
    //     [](auto callable, const ToolArguments &arguments, std::stop_token stop_token) -> std::expected<ToolResult, std::string> {
    //         auto path = arguments.string("path");
    //         auto content = arguments.string("content");
    //         if (!path) return std::unexpected(path.error());
    //         if (!content) return std::unexpected(content.error());
    //         if (stop_token.stop_requested()) return std::unexpected("Tool execution interrupted");
    //         auto result = callable(*path, *content);
    //         if (!result) return std::unexpected(result.error());
    //         return ToolResult{std::to_string(*result), {}};
    //     }};
    template <typename T, typename ... S>
    class Tool final : public ToolBase {
        public:
            // The adapter maps named JSON fields to the callable, observes
            // cancellation when useful, and serializes the callable's result.
            using JsonExecutionAdapter = std::function<std::expected<ToolResult, std::string>(T (*callable)(S...), const ToolArguments &arguments, std::stop_token stop_token)>;

            explicit Tool(std::string name, std::string description, T (*callable)(S...), std::string parameters = R"({"type":"object","properties":{},"required":[],"additionalProperties":false})", JsonExecutionAdapter adapter = {})
                : name_{std::move(name)}, description_{std::move(description)},
                  callable_(callable), parameters_{std::move(parameters)}, adapter_{std::move(adapter)} {}

            T operator()(S... arguments) const {
                return callable_(arguments...);
            }

            std::string_view name() const override { return name_; }

            // Parameters must be a JSON Schema object accepted by the Responses API.
            std::string toJsonString() const override {
                return detail::toolJsonString(name_, description_, parameters_);
            }

            std::expected<ToolResult, std::string> executeJson(const std::string_view arguments, const std::stop_token stop_token = {}) const override {
                if (!adapter_) {
                    return std::unexpected("Tool '" + name_ + "' has no JSON execution adapter");
                }
                if (stop_token.stop_requested()) {
                    return std::unexpected("Tool execution interrupted");
                }

                auto result = adapter_(callable_, ToolArguments{arguments}, stop_token);
                if (stop_token.stop_requested()) {
                    return std::unexpected("Tool execution interrupted");
                }
                return result;
            }

        private:
            std::string name_;
            std::string description_;
            T (*callable_)(S...);
            std::string parameters_;
            JsonExecutionAdapter adapter_;
    };

} // namespace microcodex
