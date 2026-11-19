// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "api.h"

namespace microcodex {

// Slash commands never change the UI or the agent themselves. A command
// returns effects, plain descriptions of what should happen, and the
// interactive UI is the only place that carries them out. New commands
// combine the effects below; only a new kind of effect needs UI changes.

struct ShowNotice {
    std::string text;
};

struct ShowError {
    std::string text;
};

struct ResetConversation {};

struct Quit {};

// One choice in a picker. Choosing it runs `command` exactly as if the user
// had typed it, so a picker needs no callbacks and validation stays in the
// command.
struct PickerOption {
    std::string label;
    std::string description;
    std::string command;
};

struct OpenPicker {
    std::string title;
    std::vector<PickerOption> options;
    std::size_t selected = 0;
};

// Changes agent settings on the UI thread. The registry only runs commands
// that are unavailable during a turn when no turn is running, so `apply` is
// never called concurrently with the worker. Success text is shown as a
// notice and failure text as an error.
struct UpdateApi {
    std::function<std::expected<std::string, std::string>(CodexApi&)> apply;
};

using CommandEffect = std::variant<ShowNotice, ShowError, ResetConversation, Quit, OpenPicker, UpdateApi>;

// The facts a command may read. The agent state belongs to the worker
// thread during a turn, so `agent` is empty while one is running.
struct CommandSnapshot {
    bool turn_running = false;
    std::optional<CodexApiStatus> agent;
};

class SlashCommand {
public:
    SlashCommand() = default;
    virtual ~SlashCommand() = default;
    SlashCommand(const SlashCommand&) = delete;
    SlashCommand& operator=(const SlashCommand&) = delete;
    SlashCommand(SlashCommand&&) = delete;
    SlashCommand& operator=(SlashCommand&&) = delete;

    // Lowercase letters, digits and '-', typed after the '/'.
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual std::string_view description() const = 0;
    // Shown after the name in the menu, such as "[level]". A command with no
    // hint takes no arguments.
    [[nodiscard]] virtual std::string_view argumentHint() const { return {}; }
    [[nodiscard]] virtual bool availableDuringTurn() const { return false; }
    // `arguments` is the text after the name, without surrounding whitespace.
    [[nodiscard]] virtual std::vector<CommandEffect> run(std::string_view arguments,
                                                         const CommandSnapshot& snapshot) const = 0;
};

class SlashCommandRegistry {
public:
    // Rejects an invalid or duplicate name. Commands are listed in the order
    // they are added.
    std::expected<void, std::string> add(std::shared_ptr<const SlashCommand> command);

    // Runs `input` when it names a registered command, after checking that
    // the command may run now and accepts arguments. Returns nothing when
    // `input` is not a command, so the caller can send it as a message.
    [[nodiscard]] std::optional<std::vector<CommandEffect>> dispatch(std::string_view input,
                                                                     const CommandSnapshot& snapshot) const;

    // The commands matching a composer input such as "/ef". Empty unless the
    // input is a '/' followed by the start of a command name.
    [[nodiscard]] std::vector<const SlashCommand*> completions(std::string_view input) const;

private:
    std::vector<std::shared_ptr<const SlashCommand>> commands_;
};

// The commands every interactive session starts with.
std::expected<SlashCommandRegistry, std::string> makeBuiltinSlashCommands();

}  // namespace microcodex
