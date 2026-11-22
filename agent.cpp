// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "agent.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>

#include <cstdint>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "api.h"
#include "bash.h"
#include "edit.h"
#include "glob.h"
#include "json.h"
#include "read.h"
#include "skills.h"
#include "system-prompt.h"
#include "tool.h"
#include "write.h"

namespace {

using microcodex::ToolArguments;

std::expected<microcodex::ToolResult, std::string> executeRead(
    std::expected<std::string, std::string> (*callable)(const std::string&, std::size_t, std::size_t),
    const ToolArguments& arguments, const std::stop_token stop_token) {
    auto path = arguments.string("path");
    auto offset = arguments.size("offset");
    auto limit = arguments.size("limit");
    if (!path) return std::unexpected(path.error());
    if (!offset) return std::unexpected(offset.error());
    if (!limit) return std::unexpected(limit.error());
    if (stop_token.stop_requested()) {
        return std::unexpected("Tool execution interrupted");
    }
    auto result = callable(*path, *offset, *limit);
    if (!result) return std::unexpected(result.error());
    return microcodex::ToolResult{.output = std::move(*result), .edit = {}};
}

std::expected<microcodex::ToolResult, std::string> executeWrite(
    std::expected<int, std::string> (*callable)(const std::string&, std::string_view), const ToolArguments& arguments,
    const std::stop_token stop_token) {
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
    return microcodex::ToolResult{.output = "Created " + *path, .edit = {}};
}

std::expected<microcodex::ToolResult, std::string> executeEdit(
    std::expected<microcodex::EditResult, std::string> (*callable)(const std::string&, std::string_view,
                                                                   std::string_view, bool),
    const ToolArguments& arguments, const std::stop_token stop_token) {
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
        .output = "Edited " + *path,
        .edit = std::make_shared<microcodex::EditResult>(std::move(*result)),
    };
}

std::expected<microcodex::ToolResult, std::string> executeGlob(
    std::expected<std::string, std::string> (*callable)(const std::string&), const ToolArguments& arguments,
    const std::stop_token stop_token) {
    auto pattern = arguments.string("pattern");
    if (!pattern) {
        return std::unexpected(pattern.error());
    }
    if (stop_token.stop_requested()) {
        return std::unexpected("Tool execution interrupted");
    }
    auto result = callable(*pattern);
    if (!result) return std::unexpected(result.error());
    return microcodex::ToolResult{.output = std::move(*result), .edit = {}};
}

std::expected<microcodex::ToolResult, std::string> executeBash(
    std::expected<microcodex::BashCommandResult, std::string> (*callable)(const std::string&, std::stop_token),
    const ToolArguments& arguments, const std::stop_token stop_token) {
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
    return microcodex::ToolResult{.output = std::move(output), .edit = {}};
}

// Set in the environment of every sub-agent the agent tool starts.
constexpr std::string_view sub_agent_variable = "MICROCODEX_SUB_AGENT";
// How much of a failed sub-agent's stderr its result keeps, from the end.
constexpr std::size_t agent_error_tail_bytes = 4096;
constexpr std::string_view conversation_line_prefix = "[conversation ";

// The session whose model and effort new sub-agents start on. A process runs
// one CodexApi, connected before its first turn starts any tool thread.
const microcodex::CodexApi* parent_session = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// The running executable, so a sub-agent is the same build as its parent and
// skips the installed launcher.
std::expected<std::string, std::string> executablePath() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return std::unexpected("Could not find the MicroCodex executable");
    }
    path.resize(path.find('\0'));
    return path;
#else
    return "/proc/self/exe";
#endif
}

// The command that starts a sub-agent for an agent tool call, or resumes one.
std::expected<std::vector<std::string>, std::string> agentCommand(const ToolArguments& arguments) {
    auto task = arguments.string("task");
    auto agent_id = arguments.optionalString("agent_id");
    auto model = arguments.optionalString("model");
    auto effort = arguments.optionalString("effort");
    if (!task) return std::unexpected(task.error());
    if (!agent_id) return std::unexpected(agent_id.error());
    if (!model) return std::unexpected(model.error());
    if (!effort) return std::unexpected(effort.error());
    if (task->empty()) return std::unexpected("task must not be empty");
    if (!effort->empty()) {
        auto valid = microcodex::validateReasoningEffort(*effort, "effort");
        if (!valid) return std::unexpected(valid.error());
    }
    // A new sub-agent starts on the parent's model; a follow-up keeps its
    // own unless the call names one.
    if (parent_session != nullptr) {
        if (model->empty() && agent_id->empty()) *model = parent_session->model();
        if (effort->empty()) *effort = parent_session->reasoningEffort();
    }
    auto executable = executablePath();
    if (!executable) return std::unexpected(executable.error());

    std::vector<std::string> command{std::move(*executable)};
    for (auto [flag, value] :
         {std::pair{"--model", &*model}, std::pair{"--effort", &*effort}, std::pair{"resume", &*agent_id}}) {
        if (value->empty()) continue;
        command.emplace_back(flag);
        command.push_back(std::move(*value));
    }
    command.push_back(std::move(*task));
    return command;
}

// Removes the "[conversation ID]" line a sub-agent ends its stderr with and
// returns the ID, or nothing when the sub-agent ended before saving one.
std::optional<std::string> takeConversationId(std::string& stderr_text) {
    const std::size_t start = stderr_text.rfind(conversation_line_prefix);
    if (start == std::string::npos || (start > 0 && stderr_text[start - 1] != '\n') || !stderr_text.ends_with("]\n")) {
        return std::nullopt;
    }
    const std::size_t id_start = start + conversation_line_prefix.size();
    std::string id = stderr_text.substr(id_start, stderr_text.size() - 2 - id_start);
    stderr_text.resize(start);
    return id;
}

std::expected<microcodex::ToolResult, std::string> executeAgent(
    std::expected<microcodex::BashCommandResult, std::string> (*callable)(const std::vector<std::string>&, std::string,
                                                                          std::stop_token),
    const ToolArguments& arguments, const std::stop_token stop_token) {
    auto command = agentCommand(arguments);
    if (!command) return std::unexpected(command.error());
    auto result = callable(*command, std::string(sub_agent_variable) + "=1", stop_token);
    if (!result) return std::unexpected(result.error());

    std::string answer = std::move(result->stdout);
    if (answer.ends_with('\n')) answer.pop_back();
    const std::optional<std::string> id = takeConversationId(result->stderr);

    std::string output = R"({"agent_id":)";
    if (id) {
        microcodex::json::appendString(output, *id);
    } else {
        output += "null";
    }
    output += R"(,"answer":)";
    microcodex::json::appendString(output, answer);
    output += R"(,"exit_code":)" + std::to_string(result->error_code);
    if (result->error_code != 0) {
        const std::string_view errors(result->stderr);
        output += R"(,"error":)";
        microcodex::json::appendString(output,
                                       errors.substr(errors.size() - std::min(errors.size(), agent_error_tail_bytes)));
    }
    output += '}';
    return microcodex::ToolResult{.output = std::move(output), .edit = {}};
}

using ReadTool =
    microcodex::Tool<std::expected<std::string, std::string>, const std::string&, std::size_t, std::size_t>;
using WriteTool = microcodex::Tool<std::expected<int, std::string>, const std::string&, std::string_view>;
using EditTool = microcodex::Tool<std::expected<microcodex::EditResult, std::string>, const std::string&,
                                  std::string_view, std::string_view, bool>;
using GlobTool = microcodex::Tool<std::expected<std::string, std::string>, const std::string&>;
using AgentTool = microcodex::Tool<std::expected<microcodex::BashCommandResult, std::string>,
                                   const std::vector<std::string>&, std::string, std::stop_token>;
using BashTool =
    microcodex::Tool<std::expected<microcodex::BashCommandResult, std::string>, const std::string&, std::stop_token>;

}  // namespace

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
        "bash",
        "Run a shell command in the user's login-shell environment and return JSON containing stdout, stderr, and "
        "exit_code.",
        static_cast<std::expected<BashCommandResult, std::string> (*)(const std::string&, std::stop_token)>(bash),
        R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"],"additionalProperties":false})",
        executeBash));
    // A sub-agent gets no agent tool, so sub-agents never nest.
    if (!isSubAgent()) {
        tools.emplace_back(std::make_shared<AgentTool>(
            "agent",
            "Start a sub-agent: a separate MicroCodex session with your tools and working directory. It works on the "
            "task on its own and returns its final answer and an agent_id. Only use it when the user explicitly asks "
            "for sub-agents, delegation or parallel agents. Several agent calls in one response run in parallel, so "
            "never give two of them the same files to edit. Pass agent_id to send a sub-agent a follow-up with its "
            "conversation kept.",
            runProgram,
            R"({"type":"object","properties":{"task":{"type":"string","description":"Everything the sub-agent needs to know: it sees none of this conversation."},"agent_id":{"type":"string","description":"The agent_id of an earlier result, to send that sub-agent a follow-up."},"model":{"type":"string","description":"Defaults to your model; a follow-up keeps the sub-agent's model."},"effort":{"type":"string","description":"Reasoning effort, one of the --effort values. Defaults to yours."}},"required":["task"],"additionalProperties":false})",
            executeAgent,
            // A sub-agent runs a whole turn with its own tool timeouts, so
            // only an interruption stops it.
            std::chrono::seconds(0)));
    }
    return tools;
}

void connectAgentTool(const CodexApi& api) { parent_session = &api; }

bool isSubAgent() {
    const char* value = std::getenv(std::string(sub_agent_variable).c_str());
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

CodexApiConfig makeCodingAgentConfig(std::string model) {
    CodexApiConfig config;
    config.model = std::move(model);
    config.instructions = std::string(codingAgentSystemPrompt());
    config.instructions += availableSkillsInstructions();
    config.user_instructions = loadGlobalInstructions();
    config.tools = makeCodingTools();
    return config;
}

}  // namespace microcodex
