// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <mutex>
#include <string>

namespace microcodex {

    enum class CodexEventType {
        TurnStarted,
        TextDelta,
        ToolStarted,
        ToolFinished,
        TurnCompleted,
        TurnInterrupted,
        Error,
    };

    // Events are intentionally plain data so a UI can copy them into
    // its own event queue without depending on CodexApi internals.
    //
    // text contains an assistant delta, tool arguments, tool output, final
    // response, interruption reason, or error depending on type. call_id and
    // tool_name are populated only for tool events. succeeded is meaningful
    // only for ToolFinished.
    struct CodexEvent {
        CodexEventType type;
        std::string turn_id;
        std::string call_id;
        std::string tool_name;
        std::string text;
        bool succeeded = true;
    };

    using CodexEventHandler = std::function<void(const CodexEvent &)>;

    // This is the one-way boundary from CodexApi to its consumer. Commands
    // travel in the opposite direction through explicit CodexApi methods.
    class CodexEventEmitter {
    public:
        CodexEventEmitter() = default;
        explicit CodexEventEmitter(CodexEventHandler handler);

        CodexEventEmitter(const CodexEventEmitter &) = delete;
        CodexEventEmitter &operator=(const CodexEventEmitter &) = delete;

        void setHandler(CodexEventHandler handler);
        void clearHandler();
        void emit(const CodexEvent &event) const noexcept;

    private:
        mutable std::mutex mutex_;
        CodexEventHandler handler_;
    };

} // namespace microcodex
