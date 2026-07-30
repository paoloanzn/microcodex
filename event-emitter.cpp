// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "event-emitter.h"

#include <utility>

namespace microcodex {

    CodexEventEmitter::CodexEventEmitter(CodexEventHandler handler) : handler_(std::move(handler)) {}

    void CodexEventEmitter::setHandler(CodexEventHandler handler) {
        std::lock_guard lock(mutex_);
        handler_ = std::move(handler);
    }

    void CodexEventEmitter::clearHandler() {
        std::lock_guard lock(mutex_);
        handler_ = {};
    }

    void CodexEventEmitter::emit(const CodexEvent &event) const noexcept {
        try {
            CodexEventHandler handler;
            {
                std::lock_guard lock(mutex_);
                handler = handler_;
            }
            if (!handler) {
                return;
            }

            // Invoke outside the mutex. A handler may replace itself or ask
            // CodexApi to interrupt the active turn without deadlocking here.
            handler(event);
        } catch (...) {
            // Presentation code must not be able to corrupt conversation state
            // or strand an in-flight turn by throwing through the emitter.
        }
    }

} // namespace microcodex
