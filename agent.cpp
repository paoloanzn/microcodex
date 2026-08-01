// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "agent.h"

#include "bash.h"
#include "edit.h"
#include "glob.h"
#include "json.h"
#include "read.h"
#include "skills.h"
#include "system-prompt.h"
#include "write.h"

#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {

    using microcodex::ToolArguments;

    std::expected<microcodex::ToolResult, std::string> executeRead(std::expected<std::string, std::string> (*callable)(const std::string &, std::size_t, std::size_t), const ToolArguments &arguments, const std::stop_token stop_token) {
        auto path = arguments.string("path");
        auto offset = arguments.size("offset");
        auto limit = arguments.size("limit");
        if (!path || !offset || !limit) {
            return std::unexpected(!path ? path.error() : !offset ? offset.error() : limit.error());
        }
        if (stop_token.stop_requested()) {
            return std::unexpected("Tool execution interrupted");
        }
        auto result = callable(*path, *offset, *limit);
        if (!result) return std::unexpected(result.error());
        return microcodex::ToolResult{std::move(*result), {}};
    }

    std::expected<microcodex::ToolResult, std::string> executeWrite(std::expected<int, std::string> (*callable)(const std::string &, std::string_view), const ToolArguments &arguments, const std::stop_token stop_token) {
        auto path = arguments.string("path");
        auto content = arguments.string("content");
        if (!path || !content) {
            return std::unexpected(!path ? path.error() : content.error());
        }
        if (stop_token.stop_requested()) {
            return std::unexpected("Tool execution interrupted");
        }
        auto result = callable(*path, *content);
        if (!result) {
            return std::unexpected(result.error());
        }
        return microcodex::ToolResult{"Created " + *path, {}};
    }

    std::expected<microcodex::ToolResult, std::string> executeEdit(std::expected<microcodex::EditResult, std::string> (*callable)(const std::string &, std::string_view, std::string_view, bool), const ToolArguments &arguments, const std::stop_token stop_token) {
        auto path = arguments.string("path");
        auto old_content = arguments.string("old_content");
        auto new_content = arguments.string("new_content");
        auto replace_all = arguments.boolean("replace_all", false);
        if (!path || !old_content || !new_content || !replace_all) {
            if (!path) return std::unexpected(path.error());
            if (!old_content) return std::unexpected(old_content.error());
            if (!new_content) return std::unexpected(new_content.error());
            return std::unexpected(replace_all.error());
        }
        if (stop_token.stop_requested()) {
            return std::unexpected("Tool execution interrupted");
        }
        auto result = callable(*path, *old_content, *new_content, *replace_all);
        if (!result) {
            return std::unexpected(result.error());
        }
        return microcodex::ToolResult{
            "Edited " + *path,
            std::make_shared<microcodex::EditResult>(std::move(*result)),
        };
    }

    std::expected<microcodex::ToolResult, std::string> executeGlob(std::expected<std::string, std::string> (*callable)(const std::string &), const ToolArguments &arguments, const std::stop_token stop_token) {
        auto pattern = arguments.string("pattern");
        if (!pattern) {
            return std::unexpected(pattern.error());
        }
        if (stop_token.stop_requested()) {
            return std::unexpected("Tool execution interrupted");
        }
        auto result = callable(*pattern);
        if (!result) return std::unexpected(result.error());
        return microcodex::ToolResult{std::move(*result), {}};
    }

    std::expected<microcodex::ToolResult, std::string> executeBash(std::expected<microcodex::BashCommandResult, std::string> (*callable)(const std::string &, std::stop_token), const ToolArguments &arguments, const std::stop_token stop_token) {
        auto command = arguments.string("command");
        if (!command) {
            return std::unexpected(command.error());
        }
        auto result = callable(*command, stop_token);
        if (!result) {
            return std::unexpected(result.error());
        }

        std::string output = "{\"stdout\":";
        microcodex::json::appendString(output, result->stdout);
        output += ",\"stderr\":";
        microcodex::json::appendString(output, result->stderr);
        output += ",\"exit_code\":" + std::to_string(result->error_code) + '}';
        return microcodex::ToolResult{std::move(output), {}};
    }

    using ReadTool  = microcodex::Tool<std::expected<std::string, std::string>, const std::string &, std::size_t, std::size_t>;
    using WriteTool = microcodex::Tool<std::expected<int, std::string>, const std::string &, std::string_view>;
    using EditTool  = microcodex::Tool<std::expected<microcodex::EditResult, std::string>, const std::string &, std::string_view, std::string_view, bool>;
    using GlobTool  = microcodex::Tool<std::expected<std::string, std::string>, const std::string &>;
    using BashTool  = microcodex::Tool<std::expected<microcodex::BashCommandResult, std::string>, const std::string &, std::stop_token>;

} // namespace

namespace microcodex {

    std::vector<std::shared_ptr<const ToolBase>> makeCodingTools() {
        std::vector<std::shared_ptr<const ToolBase>> tools;
        tools.emplace_back(std::make_shared<ReadTool>(
            "read", "Read up to limit bytes from a file starting at offset.", read,
            R"({"type":"object","properties":{"path":{"type":"string"},"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":0}},"required":["path","offset","limit"],"additionalProperties":false})",
            executeRead));
        tools.emplace_back(std::make_shared<WriteTool>(
            "write", "Create a new file with content. Fails when the path already exists.", write,
            R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"],"additionalProperties":false})",
            executeWrite));
        tools.emplace_back(std::make_shared<EditTool>(
            "edit", "Replace exact text in an existing file. replace_all defaults to false when omitted.", edit,
            R"({"type":"object","properties":{"path":{"type":"string"},"old_content":{"type":"string"},"new_content":{"type":"string"},"replace_all":{"type":"boolean","default":false}},"required":["path","old_content","new_content"],"additionalProperties":false})",
            executeEdit));
        tools.emplace_back(std::make_shared<GlobTool>(
            "glob", "Expand a filesystem glob pattern and return matching paths separated by newlines.", glob,
            R"({"type":"object","properties":{"pattern":{"type":"string"}},"required":["pattern"],"additionalProperties":false})",
            executeGlob));
        tools.emplace_back(std::make_shared<BashTool>(
            "bash", "Run a shell command in the user's login-shell environment and return JSON containing stdout, stderr, and exit_code.",
            static_cast<std::expected<BashCommandResult, std::string> (*)(const std::string &, std::stop_token)>(bash),
            R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"],"additionalProperties":false})",
            executeBash));
        return tools;
    }

    CodexApiConfig makeCodingAgentConfig(std::string model) {
        CodexApiConfig config;
        config.model = std::move(model);
        config.instructions = std::string(codingAgentSystemPrompt());
        config.instructions += availableSkillsInstructions();
        config.tools = makeCodingTools();
        return config;
    }

} // namespace microcodex
