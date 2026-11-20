// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "slash-command.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "api.h"

namespace {

using microcodex::CommandEffect;
using microcodex::CommandSnapshot;

constexpr std::string_view whitespace = " \t\r\n";

bool isNameCharacter(const char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-';
}

bool isValidName(const std::string_view name) { return !name.empty() && std::ranges::all_of(name, isNameCharacter); }

struct CommandLine {
    std::string_view name;
    std::string_view arguments;
};

// Splits "/name arguments" into its name and its trimmed arguments.
std::optional<CommandLine> parseCommandLine(const std::string_view input) {
    if (!input.starts_with('/')) return std::nullopt;
    const std::string_view rest = input.substr(1);
    const std::size_t name_end = std::min(rest.find_first_of(whitespace), rest.size());
    std::string_view arguments = rest.substr(name_end);
    const std::size_t first = arguments.find_first_not_of(whitespace);
    arguments = first == std::string_view::npos ? std::string_view{} : arguments.substr(first);
    arguments = arguments.substr(0, arguments.find_last_not_of(whitespace) + 1);
    return CommandLine{.name = rest.substr(0, name_end), .arguments = arguments};
}

// 258400 -> "258,400".
std::string groupDigits(const std::size_t value) {
    const std::string digits = std::to_string(value);
    std::string grouped;
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index != 0 && (digits.size() - index) % 3 == 0) grouped += ',';
        grouped += digits[index];
    }
    return grouped;
}

class EffortCommand final : public microcodex::SlashCommand {
public:
    [[nodiscard]] std::string_view name() const override { return "effort"; }
    [[nodiscard]] std::string_view description() const override { return "Choose how hard the model thinks"; }
    [[nodiscard]] std::string_view argumentHint() const override { return "[level]"; }

    [[nodiscard]] std::vector<CommandEffect> run(const std::string_view arguments,
                                                 const CommandSnapshot& snapshot) const override {
        if (arguments.empty()) {
            const std::string_view current = snapshot.agent ? snapshot.agent->reasoning_effort : std::string_view{};
            microcodex::OpenPicker picker{.title = "Reasoning effort", .options = {}, .selected = 0};
            for (const std::string_view level : microcodex::reasoningEffortValues()) {
                if (level == current) picker.selected = picker.options.size();
                picker.options.push_back({
                    .label = std::string(level),
                    .description = level == current ? "current" : "",
                    .command = "/effort " + std::string(level),
                });
            }
            return {std::move(picker)};
        }
        auto valid = microcodex::validateReasoningEffort(arguments, "/effort");
        if (!valid) return {microcodex::ShowError{valid.error()}};
        return {microcodex::UpdateApi{
            [level = std::string(arguments)](microcodex::CodexApi& api) -> std::expected<std::string, std::string> {
                auto changed = api.setReasoningEffort(level);
                if (!changed) return std::unexpected(changed.error());
                return "Reasoning effort set to " + level;
            }}};
    }
};

class ModelCommand final : public microcodex::SlashCommand {
public:
    [[nodiscard]] std::string_view name() const override { return "model"; }
    [[nodiscard]] std::string_view description() const override { return "Choose the model for the next turns"; }
    [[nodiscard]] std::string_view argumentHint() const override { return "[name]"; }

    [[nodiscard]] std::vector<CommandEffect> run(const std::string_view arguments,
                                                 const CommandSnapshot& snapshot) const override {
        if (!snapshot.agent) return {microcodex::ShowError{"The agent status is not available"}};
        const microcodex::CodexApiStatus& agent = *snapshot.agent;
        if (agent.models.empty()) {
            return {
                microcodex::ShowError{"/model: the model list is not available, because the models catalog "
                                      "could not be read at startup"}};
        }
        if (arguments.empty()) {
            microcodex::OpenPicker picker{.title = "Model", .options = {}, .selected = 0};
            for (const std::string& model : agent.models) {
                if (model == agent.model) picker.selected = picker.options.size();
                picker.options.push_back({
                    .label = model,
                    .description = model == agent.model ? "current" : "",
                    .command = "/model " + model,
                });
            }
            return {std::move(picker)};
        }
        if (std::ranges::find(agent.models, arguments) == agent.models.end()) {
            std::string list;
            for (const std::string& model : agent.models) {
                if (!list.empty()) list += ", ";
                list += model;
            }
            return {microcodex::ShowError{"/model: unsupported model '" + std::string(arguments) +
                                          "' (expected one of: " + list + ")"}};
        }
        return {microcodex::UpdateApi{
            [model = std::string(arguments)](microcodex::CodexApi& api) -> std::expected<std::string, std::string> {
                auto changed = api.setModel(model);
                if (!changed) return std::unexpected(changed.error());
                std::string text = "Model set to " + model;
                auto status = api.status();
                if (status && status->compaction_pending) {
                    text += "\nThe context (" + groupDigits(status->context_tokens) +
                            " tokens) is over this model's compaction limit, so it is compacted before the next "
                            "request";
                }
                return text;
            }}};
    }
};

class NewCommand final : public microcodex::SlashCommand {
public:
    [[nodiscard]] std::string_view name() const override { return "new"; }
    [[nodiscard]] std::string_view description() const override { return "Start a new conversation"; }

    [[nodiscard]] std::vector<CommandEffect> run(std::string_view /*arguments*/,
                                                 const CommandSnapshot& /*snapshot*/) const override {
        return {microcodex::ResetConversation{}};
    }
};

class StatusCommand final : public microcodex::SlashCommand {
public:
    [[nodiscard]] std::string_view name() const override { return "status"; }
    [[nodiscard]] std::string_view description() const override { return "Show the model, effort and context usage"; }

    [[nodiscard]] std::vector<CommandEffect> run(std::string_view /*arguments*/,
                                                 const CommandSnapshot& snapshot) const override {
        if (!snapshot.agent) return {microcodex::ShowError{"The agent status is not available"}};
        const microcodex::CodexApiStatus& agent = *snapshot.agent;
        std::string text = "Model: " + agent.model + "\nReasoning effort: " + agent.reasoning_effort +
                           "\nSaved turns: " + std::to_string(agent.saved_turns) +
                           "\nContext: " + groupDigits(agent.context_tokens) + " of " +
                           groupDigits(agent.context_limit_tokens) + " tokens, ";
        text += agent.compact_at_tokens == 0 ? "automatic compaction off"
                                             : "compacts at " + groupDigits(agent.compact_at_tokens);
        return {microcodex::ShowNotice{std::move(text)}};
    }
};

class QuitCommand final : public microcodex::SlashCommand {
public:
    [[nodiscard]] std::string_view name() const override { return "quit"; }
    [[nodiscard]] std::string_view description() const override { return "Exit MicroCodex"; }
    [[nodiscard]] bool availableDuringTurn() const override { return true; }

    [[nodiscard]] std::vector<CommandEffect> run(std::string_view /*arguments*/,
                                                 const CommandSnapshot& /*snapshot*/) const override {
        return {microcodex::Quit{}};
    }
};

}  // namespace

namespace microcodex {

std::expected<void, std::string> SlashCommandRegistry::add(std::shared_ptr<const SlashCommand> command) {
    if (!command) return std::unexpected("Slash command cannot be null");
    const std::string_view name = command->name();
    if (!isValidName(name)) {
        return std::unexpected("Invalid slash command name '" + std::string(name) + "'");
    }
    const bool duplicate =
        std::ranges::any_of(commands_, [name](const auto& existing) { return existing->name() == name; });
    if (duplicate) return std::unexpected("Duplicate slash command '/" + std::string(name) + "'");
    commands_.push_back(std::move(command));
    return {};
}

std::optional<std::vector<CommandEffect>> SlashCommandRegistry::dispatch(const std::string_view input,
                                                                         const CommandSnapshot& snapshot) const {
    const std::optional<CommandLine> line = parseCommandLine(input);
    if (!line) return std::nullopt;
    const auto found =
        std::ranges::find_if(commands_, [line](const auto& command) { return command->name() == line->name; });
    if (found == commands_.end()) return std::nullopt;

    const SlashCommand& command = **found;
    const std::string quoted = "'/" + std::string(command.name()) + "'";
    if (snapshot.turn_running && !command.availableDuringTurn()) {
        return std::vector<CommandEffect>{ShowError{quoted + " is not available while a turn is running"}};
    }
    if (command.argumentHint().empty() && !line->arguments.empty()) {
        return std::vector<CommandEffect>{ShowError{quoted + " takes no arguments"}};
    }
    return command.run(line->arguments, snapshot);
}

std::vector<const SlashCommand*> SlashCommandRegistry::completions(const std::string_view input) const {
    std::vector<const SlashCommand*> matches;
    if (!input.starts_with('/')) return matches;
    const std::string_view prefix = input.substr(1);
    if (!std::ranges::all_of(prefix, isNameCharacter)) return matches;
    for (const auto& command : commands_) {
        if (command->name().starts_with(prefix)) matches.push_back(command.get());
    }
    return matches;
}

std::expected<SlashCommandRegistry, std::string> makeBuiltinSlashCommands() {
    SlashCommandRegistry registry;
    for (std::shared_ptr<const SlashCommand> command : {
             std::shared_ptr<const SlashCommand>(std::make_shared<ModelCommand>()),
             std::shared_ptr<const SlashCommand>(std::make_shared<EffortCommand>()),
             std::shared_ptr<const SlashCommand>(std::make_shared<NewCommand>()),
             std::shared_ptr<const SlashCommand>(std::make_shared<StatusCommand>()),
             std::shared_ptr<const SlashCommand>(std::make_shared<QuitCommand>()),
         }) {
        auto added = registry.add(std::move(command));
        if (!added) return std::unexpected(added.error());
    }
    return registry;
}

}  // namespace microcodex
