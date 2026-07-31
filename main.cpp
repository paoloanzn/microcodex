// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "agent.h"
#include "oauth.h"
#include "ui.h"

#include <chrono>
#include <cstdlib>
#include <expected>
#include <iostream>
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
                  << "  " << executable << " [--model MODEL]\n"
                  << "  " << executable << " [--model MODEL] PROMPT\n";
    }

    struct AgentRequest {
        std::string model;
        std::optional<std::string> prompt;
    };

    std::expected<AgentRequest, std::string> parseAgentRequest(const int argc, char *argv[]) {
        std::string model(default_model);
        int argument = 1;
        if (argument < argc && std::string_view(argv[argument]) == "--model") {
            if (++argument == argc || std::string_view(argv[argument]).empty()) {
                return std::unexpected("--model requires a model name");
            }
            model = argv[argument++];
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
            .prompt = prompt.empty() ? std::nullopt
                                     : std::optional<std::string>(std::move(prompt)),
        };
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

    auto request = parseAgentRequest(argc, argv);
    if (!request) {
        std::cerr << request.error() << '\n';
        printUsage(executable);
        return 1;
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
    microcodex::applyOAuthCredentials(config, **credentials);
    // Keep transport selection at the executable boundary so black-box tests
    // can exercise the real CLI against a deterministic loopback server. The
    // default remains the production Codex endpoint.
    if (const char *endpoint = std::getenv("MICROCODEX_API_ENDPOINT");
        endpoint != nullptr && endpoint[0] != '\0') {
        config.endpoint = endpoint;
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
