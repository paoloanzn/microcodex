// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "agent.h"
#include "api.h"
#include "conversation.h"
#include "event-emitter.h"
#include "model-catalog.h"
#include "oauth.h"
#include "response-item.h"
#include "slash-command.h"
#include "ui.h"

namespace {

constexpr std::string_view default_model = "gpt-6.2-sol";
constexpr std::string_view default_reasoning_effort = "medium";

std::string effortValueList() {
    std::string list;
    for (const std::string_view value : microcodex::reasoningEffortValues()) {
        if (!list.empty()) list += ", ";
        list += value;
    }
    return list;
}

void printUsage(const std::string_view executable) {
    std::cout << "Usage:\n"
              << "  " << executable << " login [--device-auth]\n"
              << "  " << executable << " logout\n"
              << "  " << executable << " list\n"
              << "  " << executable << " show ID\n"
              << "  " << executable << " [--model MODEL] [--effort EFFORT] resume ID [PROMPT]\n"
              << "  " << executable << " [--model MODEL] [--effort EFFORT]\n"
              << "  " << executable << " [--model MODEL] [--effort EFFORT] PROMPT\n"
              << "\n"
              << "Effort values: " << effortValueList() << " (default: medium)\n"
              << "  MICROCODEX_EFFORT selects the effort when --effort is absent.\n";
}

struct AgentRequest {
    std::string model;
    bool model_explicit;
    std::optional<std::string> prompt;
    std::optional<std::string> resume_id;
    std::string reasoning_effort;
    bool reasoning_effort_explicit = false;
};

struct AgentOptions {
    std::string model{default_model};
    bool model_explicit = false;
    std::string reasoning_effort{default_reasoning_effort};
    bool reasoning_effort_explicit = false;
};

// Parse the leading --model and --effort options, advancing argument past them.
std::expected<AgentOptions, std::string> parseAgentOptions(const std::span<char* const> arguments,
                                                           std::size_t& argument) {
    AgentOptions options;
    while (argument < arguments.size()) {
        const std::string_view flag(arguments[argument]);
        if (flag == "--model") {
            ++argument;
            if (argument == arguments.size()) {
                return std::unexpected("--model requires a model name");
            }
            const std::string_view value(arguments[argument]);
            // An option-looking token is not a model name; --model is
            // still missing its value.
            if (value.empty() || value.starts_with("--")) {
                return std::unexpected("--model requires a model name");
            }
            options.model = value;
            ++argument;
            options.model_explicit = true;
        } else if (flag == "--effort") {
            ++argument;
            if (argument == arguments.size()) {
                return std::unexpected("--effort requires an effort value (expected one of: " + effortValueList() +
                                       ")");
            }
            auto valid = microcodex::validateReasoningEffort(arguments[argument], "--effort");
            if (!valid) return std::unexpected(valid.error());
            options.reasoning_effort = arguments[argument];
            ++argument;
            options.reasoning_effort_explicit = true;
        } else {
            break;
        }
    }
    return options;
}

std::expected<AgentRequest, std::string> parseAgentRequest(const std::span<char* const> arguments) {
    std::size_t argument = 1;
    auto options = parseAgentOptions(arguments, argument);
    if (!options) return std::unexpected(options.error());

    std::optional<std::string> resume_id;
    if (argument < arguments.size() && std::string_view(arguments[argument]) == "resume") {
        ++argument;
        if (argument == arguments.size() || std::string_view(arguments[argument]).empty()) {
            return std::unexpected("resume requires a conversation ID");
        }
        resume_id = arguments[argument];
        ++argument;
    }

    std::string prompt;
    for (const std::string_view word : arguments.subspan(std::min(argument, arguments.size()))) {
        if (!prompt.empty()) {
            prompt += ' ';
        }
        prompt += word;
    }
    return AgentRequest{
        .model = std::move(options->model),
        .model_explicit = options->model_explicit,
        .prompt = prompt.empty() ? std::nullopt : std::optional<std::string>(std::move(prompt)),
        .resume_id = std::move(resume_id),
        .reasoning_effort = std::move(options->reasoning_effort),
        .reasoning_effort_explicit = options->reasoning_effort_explicit,
    };
}

std::expected<std::filesystem::path, std::string> conversationPath(const std::string_view id) {
    if (id.empty() || id.find('/') != std::string_view::npos || id == "." || id == "..") {
        return std::unexpected("Invalid conversation ID");
    }
    auto directory = microcodex::conversationDirectory();
    if (!directory) return std::unexpected(directory.error());
    std::filesystem::path filename(id);
    if (filename.extension() != ".jsonl") filename += ".jsonl";
    std::filesystem::path path = *directory / filename;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::unexpected("Conversation '" + std::string(id) + "' was not found");
    }
    return path;
}

int listSavedConversations() {
    auto directory = microcodex::conversationDirectory();
    if (!directory) {
        std::cerr << directory.error() << '\n';
        return 1;
    }
    auto conversations = microcodex::listConversations(*directory);
    if (!conversations) {
        std::cerr << conversations.error() << '\n';
        return 1;
    }
    for (const microcodex::ConversationSummary& conversation : *conversations) {
        std::cout << conversation.metadata.id << '\t' << conversation.metadata.created_at << '\t'
                  << conversation.metadata.model << '\t' << conversation.metadata.working_directory << '\n';
    }
    return 0;
}

std::expected<void, std::string> printSavedTurn(const microcodex::SavedTurn& turn) {
    for (const std::string& item : turn.items) {
        auto message = microcodex::responseMessage(item);
        if (!message) return std::unexpected(message.error());
        if (const auto& found = *message) {
            std::cout << found->role << ": " << found->text << '\n';
            continue;
        }
        auto call = microcodex::responseToolCall(item);
        if (!call) return std::unexpected(call.error());
        if (const auto& found = *call) {
            std::cout << "tool " << found->name << ": " << found->arguments << '\n';
            continue;
        }
        auto output = microcodex::responseToolOutput(item);
        if (!output) return std::unexpected(output.error());
        if (const auto& found = *output) {
            std::cout << "tool output " << found->call_id << ": " << found->output << '\n';
        }
    }
    return {};
}

int showSavedConversation(const std::string_view id) {
    auto path = conversationPath(id);
    if (!path) {
        std::cerr << path.error() << '\n';
        return 1;
    }
    auto file = microcodex::ConversationFile::openReadOnly(*path);
    if (!file) {
        std::cerr << file.error() << '\n';
        return 1;
    }
    auto resumed = file->resume();
    if (!resumed) {
        std::cerr << resumed.error() << '\n';
        return 1;
    }
    auto turns = file->readTurnsBefore(file->turnCount(), std::numeric_limits<std::size_t>::max());
    if (!turns) {
        std::cerr << turns.error() << '\n';
        return 1;
    }
    for (const microcodex::SavedTurn& turn : *turns) {
        auto printed = printSavedTurn(turn);
        if (!printed) {
            std::cerr << printed.error() << '\n';
            return 1;
        }
    }
    return 0;
}

int completeLogin(const std::expected<microcodex::OAuthCredentials, std::string>& credentials) {
    if (!credentials) {
        std::cerr << "Login failed: " << credentials.error() << '\n';
        return 1;
    }
    auto saved = microcodex::saveOAuthCredentials(*credentials);
    if (!saved) {
        std::cerr << "Login succeeded, but credentials could not be saved: " << saved.error() << '\n';
        return 1;
    }
    std::cout << "Logged in.\n";
    return 0;
}

int runLogin() {
    auto login = microcodex::OAuthLogin::start();
    if (!login) {
        std::cerr << "Login failed: " << login.error() << '\n';
        return 1;
    }

    std::cout << "Open this URL to log in:\n"
              << login->authorizationUrl() << "\n\n"
              << "Waiting for the browser callback on port " << login->callbackPort() << "...\n"
              << "On a remote machine? Run 'microcodex login --device-auth' instead.\n";
    return completeLogin(login->finish(std::chrono::minutes(5)));
}

int deviceLogin() {
    auto login = microcodex::startOAuthDeviceLogin();
    if (!login) {
        std::cerr << "Login failed: " << login.error() << '\n';
        return 1;
    }

    std::cout << "Open this URL in your browser:\n"
              << login->verification_url << "\n\n"
              << "Enter this one-time code (expires in 15 minutes):\n"
              << login->user_code << "\n\n"
              << "Continue only if you started this login in MicroCodex.\n"
              << "Waiting for authorization...\n";
    return completeLogin(microcodex::finishOAuthDeviceLogin(*login));
}

int runLogout() {
    auto logged_out = microcodex::logoutOAuth();
    if (!logged_out) {
        std::cerr << "Logout failed: " << logged_out.error() << '\n';
        return 1;
    }
    std::cout << (*logged_out ? "Logged out.\n" : "Already logged out.\n");
    return 0;
}

int runPrompt(microcodex::CodexApiConfig config, const std::string_view prompt) {
    bool received_text = false;
    microcodex::CodexEventEmitter events([&received_text](const microcodex::CodexEvent& event) {
        switch (event.type) {
            case microcodex::CodexEventType::TextDelta:
                std::cout << event.text << std::flush;
                received_text = true;
                break;
            case microcodex::CodexEventType::ToolStarted:
                std::cerr << "\n[tool " << event.tool_name << "] " << event.text << '\n';
                break;
            case microcodex::CodexEventType::ToolFinished:
                std::cerr << "[tool " << event.tool_name << (event.succeeded ? " completed]" : " failed]") << ' '
                          << event.text << '\n';
                break;
            default: break;
        }
    });

    microcodex::CodexApi agent(std::move(config), events);
    auto response = agent.sendUserMessage(prompt);
    if (!response) {
        std::cerr << "Agent failed: " << response.error() << '\n';
        return 1;
    }
    if (!received_text && !response->text.empty()) {
        std::cout << response->text;
    }
    if (received_text || !response->text.empty()) {
        std::cout << '\n';
    }
    return 0;
}

std::expected<void, std::string> applySizeEnvironment(const char* name, std::size_t& destination) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return {};
    const std::string_view digits(text);
    const char* const end = std::to_address(digits.end());
    std::size_t value = 0;
    const auto [parsed_end, error] = std::from_chars(std::to_address(digits.begin()), end, value);
    if (error != std::errc{} || parsed_end != end) {
        return std::unexpected(std::string(name) + " must be an unsigned integer");
    }
    destination = value;
    return {};
}

std::expected<void, std::string> applyToolTimeoutEnvironment(microcodex::CodexApiConfig& config) {
    auto parsed =
        applySizeEnvironment("MICROCODEX_TOOL_EXECUTION_TIMEOUT_SECONDS", config.tool_execution_timeout_seconds);
    if (!parsed) return parsed;
    // Zero disables the timeout. Anything above a year is almost certainly
    // a mistake, and it would overflow std::chrono::seconds' signed
    // representation, silently disabling the timeout instead of erroring.
    constexpr std::size_t one_year_seconds = std::size_t{365} * 24 * 60 * 60;
    if (config.tool_execution_timeout_seconds > one_year_seconds) {
        return std::unexpected(
            "MICROCODEX_TOOL_EXECUTION_TIMEOUT_SECONDS must not exceed 31536000 (one year); use 0 to disable the "
            "timeout");
    }
    return {};
}

std::expected<void, std::string> applyConversationEnvironment(microcodex::CodexApiConfig& config) {
    // Kept as an override too, so /model does not replace it with the new
    // model's catalog threshold.
    if (const char* text = std::getenv("MICROCODEX_COMPACT_AT_TOKENS"); text != nullptr && *text != '\0') {
        std::size_t compact_at_tokens = 0;
        auto compact_at = applySizeEnvironment("MICROCODEX_COMPACT_AT_TOKENS", compact_at_tokens);
        if (!compact_at) return compact_at;
        config.compaction.compact_at_tokens = compact_at_tokens;
        config.compact_at_tokens_override = compact_at_tokens;
    }
    auto retained =
        applySizeEnvironment("MICROCODEX_RETAINED_CONTEXT_TOKENS", config.compaction.retained_context_tokens);
    if (!retained) return retained;
    return applyToolTimeoutEnvironment(config);
}

std::expected<void, std::string> applyEffortEnvironment(microcodex::CodexApiConfig& config,
                                                        const bool effort_explicit) {
    // An explicit --effort flag wins over the environment.
    if (effort_explicit) return {};
    const char* value = std::getenv("MICROCODEX_EFFORT");
    if (value == nullptr) return {};
    auto valid = microcodex::validateReasoningEffort(value, "MICROCODEX_EFFORT");
    if (!valid) return std::unexpected(valid.error());
    config.reasoning_effort = value;
    return {};
}

std::expected<void, std::string> applyModelContextLimits(microcodex::CodexApiConfig& config) {
    auto models = microcodex::fetchModelContextLimits(config.endpoint, config.access_token, config.account_id);
    if (!models) return std::unexpected(models.error());
    // Kept even when it does not list the starting model, so /model can
    // still switch to a model it does list.
    config.models = std::move(*models);
    const microcodex::ModelContextLimits* model = microcodex::findModelContextLimits(config.models, config.model);
    if (model == nullptr) return std::unexpected("Models API did not return metadata for '" + config.model + "'");
    config.compaction = microcodex::compactionConfigForModel(*model, config.compaction.retained_context_tokens,
                                                             config.compaction.maximum_summary_bytes);
    return {};
}

// Run the subcommands that do not start an agent session. Returns nullopt when
// the arguments select an agent session instead.
std::optional<int> runCommand(const std::span<char* const> arguments, const std::string_view executable) {
    if (arguments.size() <= 1) return std::nullopt;
    const std::string_view command(arguments[1]);
    if (command == "--help" || command == "-h") {
        printUsage(executable);
        return 0;
    }
    if (command == "login") {
        if (arguments.size() == 2) return runLogin();
        if (arguments.size() == 3 && std::string_view(arguments[2]) == "--device-auth") return deviceLogin();
        printUsage(executable);
        return 1;
    }
    if (command == "logout") {
        return runLogout();
    }
    if (command == "list") {
        return listSavedConversations();
    }
    if (command == "show") {
        if (arguments.size() != 3) {
            std::cerr << "show requires one conversation ID\n";
            return 1;
        }
        return showSavedConversation(arguments[2]);
    }
    return std::nullopt;
}

// For a resumed conversation, restore its newest model (unless --model was
// given) and its working directory, and return the path of its file.
std::expected<std::optional<std::filesystem::path>, std::string> prepareResume(AgentRequest& request) {
    const std::optional<std::string>& resume_id = request.resume_id;
    if (!resume_id) return std::optional<std::filesystem::path>{};

    auto path = conversationPath(*resume_id);
    if (!path) return std::unexpected(path.error());
    auto metadata = microcodex::readConversationMetadata(*path);
    if (!metadata) return std::unexpected(metadata.error());
    if (!request.model_explicit) {
        auto model = microcodex::readConversationModel(*path);
        if (!model) return std::unexpected(model.error());
        request.model = std::move(*model);
    }

    std::error_code error;
    std::filesystem::current_path(metadata->working_directory, error);
    if (error) {
        return std::unexpected("Could not restore conversation working directory: " + error.message());
    }
    return std::optional<std::filesystem::path>{std::move(*path)};
}

// Build the agent configuration from the request, the saved credentials and
// the environment. A missing model catalog only prints a warning.
std::expected<microcodex::CodexApiConfig, std::string> configureAgent(AgentRequest& request,
                                                                      std::optional<std::filesystem::path> resume_path,
                                                                      const microcodex::OAuthCredentials& credentials) {
    auto config = microcodex::makeCodingAgentConfig(std::move(request.model));
    config.reasoning_effort = std::move(request.reasoning_effort);
    config.resume_conversation = std::move(resume_path);
    microcodex::applyOAuthCredentials(config, credentials);
    // Validate the effort before any network attempt so a bad value fails
    // with its own clear error instead of following a models-API warning.
    auto effort_config = applyEffortEnvironment(config, request.reasoning_effort_explicit);
    if (!effort_config) return std::unexpected(effort_config.error());
    // Keep transport selection at the executable boundary so black-box tests
    // can exercise the real CLI against a deterministic loopback server. The
    // default remains the production Codex endpoint.
    if (const char* endpoint = std::getenv("MICROCODEX_API_ENDPOINT"); endpoint != nullptr && *endpoint != '\0') {
        config.endpoint = endpoint;
    }
    auto model_context = applyModelContextLimits(config);
    if (!model_context) {
        std::cerr << "Warning: " << model_context.error() << ". Using built-in context limits.\n";
    }
    auto configured = applyConversationEnvironment(config);
    if (!configured) return std::unexpected(configured.error());
    return config;
}

int runAgent(const std::span<char* const> arguments, const std::string_view executable) {
    auto request = parseAgentRequest(arguments);
    if (!request) {
        std::cerr << request.error() << '\n';
        printUsage(executable);
        return 1;
    }

    auto resume_path = prepareResume(*request);
    if (!resume_path) {
        std::cerr << resume_path.error() << '\n';
        return 1;
    }
    auto credentials = microcodex::loadOAuthCredentials();
    if (!credentials) {
        std::cerr << "Could not load saved credentials: " << credentials.error() << '\n';
        return 1;
    }
    const std::optional<microcodex::OAuthCredentials>& saved_credentials = *credentials;
    if (!saved_credentials) {
        std::cerr << "Not logged in. Run '" << executable << " login' first.\n";
        return 1;
    }

    auto config = configureAgent(*request, std::move(*resume_path), *saved_credentials);
    if (!config) {
        std::cerr << config.error() << '\n';
        return 1;
    }
    if (const std::optional<std::string>& prompt = request->prompt) {
        return runPrompt(std::move(*config), *prompt);
    }

    auto commands = microcodex::makeBuiltinSlashCommands();
    if (!commands) {
        std::cerr << commands.error() << '\n';
        return 1;
    }
    auto result = microcodex::runInteractive(std::move(*config), *commands);
    if (!result) {
        std::cerr << result.error() << '\n';
        return 1;
    }
    return 0;
}

}  // namespace

int main(const int argc, char* argv[]) {
    const std::span<char* const> arguments(argv, static_cast<std::size_t>(argc));
    const std::string_view executable = arguments.empty() ? std::string_view("microcodex") : arguments.front();
    if (const std::optional<int> status = runCommand(arguments, executable)) {
        return *status;
    }
    return runAgent(arguments, executable);
}
