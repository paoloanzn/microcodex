// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "agent.h"
#include "conversation.h"
#include "model-catalog.h"
#include "oauth.h"
#include "response-item.h"
#include "ui.h"

#include <chrono>
#include <charconv>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

    constexpr std::string_view default_model = "gpt-5.6-sol";

    void printUsage(const std::string_view executable) {
        std::cout << "Usage:\n"
                  << "  " << executable << " login\n"
                  << "  " << executable << " logout\n"
                  << "  " << executable << " list\n"
                  << "  " << executable << " show ID\n"
                  << "  " << executable << " [--model MODEL] resume ID [PROMPT]\n"
                  << "  " << executable << " [--model MODEL]\n"
                  << "  " << executable << " [--model MODEL] PROMPT\n";
    }

    struct AgentRequest {
        std::string model;
        bool model_explicit;
        std::optional<std::string> prompt;
        std::optional<std::string> resume_id;
    };

    std::expected<AgentRequest, std::string> parseAgentRequest(const int argc, char *argv[]) {
        std::string model(default_model);
        bool model_explicit = false;
        int argument = 1;
        if (argument < argc && std::string_view(argv[argument]) == "--model") {
            if (++argument == argc || std::string_view(argv[argument]).empty()) {
                return std::unexpected("--model requires a model name");
            }
            model = argv[argument++];
            model_explicit = true;
        }
        std::optional<std::string> resume_id;
        if (argument < argc && std::string_view(argv[argument]) == "resume") {
            ++argument;
            if (argument == argc || std::string_view(argv[argument]).empty()) {
                return std::unexpected("resume requires a conversation ID");
            }
            resume_id = argv[argument++];
        }

        std::string prompt;
        for (; argument < argc; ++argument) {
            if (!prompt.empty()) {
                prompt += ' ';
            }
            prompt += argv[argument];
        }
        return AgentRequest{
            .model = std::move(model),
            .model_explicit = model_explicit,
            .prompt = prompt.empty() ? std::nullopt
                                     : std::optional<std::string>(std::move(prompt)),
            .resume_id = std::move(resume_id),
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
        const std::filesystem::path path = *directory / filename;
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
        for (const microcodex::ConversationSummary &conversation : *conversations) {
            std::cout << conversation.metadata.id << '\t'
                      << conversation.metadata.created_at << '\t'
                      << conversation.metadata.model << '\t'
                      << conversation.metadata.working_directory << '\n';
        }
        return 0;
    }

    std::expected<void, std::string> printSavedTurn(const microcodex::SavedTurn &turn) {
        for (const std::string &item : turn.items) {
            auto message = microcodex::responseMessage(item);
            if (!message) return std::unexpected(message.error());
            if (*message) {
                std::cout << (*message)->role << ": " << (*message)->text << '\n';
                continue;
            }
            auto call = microcodex::responseToolCall(item);
            if (!call) return std::unexpected(call.error());
            if (*call) {
                std::cout << "tool " << (*call)->name << ": " << (*call)->arguments << '\n';
                continue;
            }
            auto output = microcodex::responseToolOutput(item);
            if (!output) return std::unexpected(output.error());
            if (*output) {
                std::cout << "tool output " << (*output)->call_id << ": "
                          << (*output)->output << '\n';
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
        auto turns = file->readTurnsBefore(file->turnCount(),
                                           std::numeric_limits<std::size_t>::max());
        if (!turns) {
            std::cerr << turns.error() << '\n';
            return 1;
        }
        for (const microcodex::SavedTurn &turn : *turns) {
            auto printed = printSavedTurn(turn);
            if (!printed) {
                std::cerr << printed.error() << '\n';
                return 1;
            }
        }
        return 0;
    }

    int login() {
        auto login = microcodex::OAuthLogin::start();
        if (!login) {
            std::cerr << "Login failed: " << login.error() << '\n';
            return 1;
        }

        std::cout << "Open this URL to log in:\n" << login->authorizationUrl() << "\n\n"
                  << "Waiting for the browser callback on port " << login->callbackPort() << "...\n";
        auto credentials = login->finish(std::chrono::minutes(5));
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

    int logout() {
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
        microcodex::CodexEventEmitter events([&received_text](const microcodex::CodexEvent &event) {
            switch (event.type) {
            case microcodex::CodexEventType::TextDelta:
                std::cout << event.text << std::flush;
                received_text = true;
                break;
            case microcodex::CodexEventType::ToolStarted:
                std::cerr << "\n[tool " << event.tool_name << "] " << event.text << '\n';
                break;
            case microcodex::CodexEventType::ToolFinished:
                std::cerr << "[tool " << event.tool_name << (event.succeeded ? " completed]" : " failed]")
                          << ' ' << event.text << '\n';
                break;
            default:
                break;
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

    std::expected<void, std::string> applySizeEnvironment(const char *name, std::size_t &destination) {
        const char *text = std::getenv(name);
        if (text == nullptr || text[0] == '\0') return {};
        std::size_t value = 0;
        const char *end = text;
        while (*end != '\0') ++end;
        const auto [parsed_end, error] = std::from_chars(text, end, value);
        if (error != std::errc{} || parsed_end != end) {
            return std::unexpected(std::string(name) + " must be an unsigned integer");
        }
        destination = value;
        return {};
    }

    std::expected<void, std::string> applyConversationEnvironment(microcodex::CodexApiConfig &config) {
        auto compact_at = applySizeEnvironment("MICROCODEX_COMPACT_AT_TOKENS",
                                               config.compaction.compact_at_tokens);
        if (!compact_at) return compact_at;
        return applySizeEnvironment("MICROCODEX_RETAINED_CONTEXT_TOKENS",
                                    config.compaction.retained_context_tokens);
    }

    std::expected<void, std::string> applyModelContextLimits(microcodex::CodexApiConfig &config) {
        auto models = microcodex::fetchModelContextLimits(config.endpoint, config.access_token, config.account_id);
        if (!models) return std::unexpected(models.error());
        const microcodex::ModelContextLimits *model = microcodex::findModelContextLimits(*models, config.model);
        if (model == nullptr) return std::unexpected("Models API did not return metadata for '" + config.model + "'");
        config.compaction = microcodex::compactionConfigForModel(*model, config.compaction.retained_context_tokens, config.compaction.maximum_summary_bytes);
        return {};
    }

} // namespace

int main(const int argc, char *argv[]) {
    const std::string_view executable = argc > 0 ? argv[0] : "microcodex";
    if (argc > 1 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        printUsage(executable);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "login") {
        return login();
    }
    if (argc > 1 && std::string_view(argv[1]) == "logout") {
        return logout();
    }
    if (argc > 1 && std::string_view(argv[1]) == "list") {
        return listSavedConversations();
    }
    if (argc > 1 && std::string_view(argv[1]) == "show") {
        if (argc != 3) {
            std::cerr << "show requires one conversation ID\n";
            return 1;
        }
        return showSavedConversation(argv[2]);
    }

    auto request = parseAgentRequest(argc, argv);
    if (!request) {
        std::cerr << request.error() << '\n';
        printUsage(executable);
        return 1;
    }

    std::optional<std::filesystem::path> resume_path;
    if (request->resume_id) {
        auto path = conversationPath(*request->resume_id);
        if (!path) {
            std::cerr << path.error() << '\n';
            return 1;
        }
        auto metadata = microcodex::readConversationMetadata(*path);
        if (!metadata) {
            std::cerr << metadata.error() << '\n';
            return 1;
        }
        if (!request->model_explicit) request->model = metadata->model;

        std::error_code error;
        std::filesystem::current_path(metadata->working_directory, error);
        if (error) {
            std::cerr << "Could not restore conversation working directory: "
                      << error.message() << '\n';
            return 1;
        }
        resume_path = std::move(*path);
    }
    auto credentials = microcodex::loadOAuthCredentials();
    if (!credentials) {
        std::cerr << "Could not load saved credentials: " << credentials.error() << '\n';
        return 1;
    }
    if (!*credentials) {
        std::cerr << "Not logged in. Run '" << executable << " login' first.\n";
        return 1;
    }

    auto config = microcodex::makeCodingAgentConfig(std::move(request->model));
    config.resume_conversation = std::move(resume_path);
    microcodex::applyOAuthCredentials(config, **credentials);
    // Keep transport selection at the executable boundary so black-box tests
    // can exercise the real CLI against a deterministic loopback server. The
    // default remains the production Codex endpoint.
    if (const char *endpoint = std::getenv("MICROCODEX_API_ENDPOINT");
        endpoint != nullptr && endpoint[0] != '\0') {
        config.endpoint = endpoint;
    }
    auto model_context = applyModelContextLimits(config);
    if (!model_context) {
        std::cerr << "Warning: " << model_context.error() << ". Using built-in context limits.\n";
    }
    auto configured = applyConversationEnvironment(config);
    if (!configured) {
        std::cerr << configured.error() << '\n';
        return 1;
    }
    if (request->prompt) {
        return runPrompt(std::move(config), *request->prompt);
    }

    auto result = microcodex::runInteractive(std::move(config));
    if (!result) {
        std::cerr << result.error() << '\n';
        return 1;
    }
    return 0;
}
