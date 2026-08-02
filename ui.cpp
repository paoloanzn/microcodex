// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "styled-text.h"
#include "markdown.h"
#include "shell-highlight.h"
#include "terminal.h"
#include "tool.h"
#include "ui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    using microcodex::ui::StyledLine;
    using microcodex::ui::StyledSpan;
    using microcodex::ui::appendSpan;
    using microcodex::ui::decodeCodepoint;
    using microcodex::ui::lineWithPrefix;
    using microcodex::ui::renderMarkdown;
    using microcodex::ui::highlightShell;
    using microcodex::ui::textWidth;
    using microcodex::ui::wrapStyledText;
    using microcodex::ui::wrapStyledSpans;

    constexpr std::size_t maximum_transcript_bytes = 512 * 1024;
    constexpr std::size_t maximum_tool_preview_bytes = 16 * 1024;
    constexpr std::size_t large_paste_character_threshold = 1000;
    constexpr std::size_t maximum_collapsed_tool_output_rows = 5;
    constexpr int animation_frame_milliseconds = 32;
    constexpr int idle_poll_milliseconds = 250;
    constexpr int maximum_prompt_rows = 6;

    // Codex uses a low-contrast surface for user messages and the composer.
    // Indexed colors preserve that look without increasing termbox's per-cell
    // attribute size from 16 to 32 bits just for true color.
    constexpr uintattr_t surface_background = 236;
    constexpr uintattr_t muted_foreground = 245;
    constexpr uintattr_t faint_foreground = 240;
    constexpr uintattr_t success_foreground = 10;
    constexpr uintattr_t error_foreground = 9;
    constexpr uintattr_t added_background = 22;
    constexpr uintattr_t removed_background = 52;

    enum class EntryKind {
        User,
        Assistant,
        Tool,
        Error,
        Notice,
    };

    struct MarkdownRenderCache {
        // Assistant text is append-only, so its byte length is a sufficient
        // revision key. Width changes naturally invalidate resize layout.
        std::vector<StyledLine> lines;
        std::size_t source_size = 0;
        int width = -1;
    };

    struct UiEntry {
        EntryKind kind;
        std::string turn_id;
        std::string call_id;
        std::string title;
        std::string text;
        std::string output;
        MarkdownRenderCache markdown_cache;
        bool finished = true;
        bool succeeded = true;
        std::shared_ptr<const microcodex::EditResult> edit;
    };

    struct UiState {
        std::deque<UiEntry> transcript;
        std::vector<std::pair<std::string, std::string>> pending_pastes;
        std::string input;
        std::string paste;
        std::string status = "Ready";
        std::string active_model;
        std::size_t input_cursor = 0;
        std::size_t transcript_bytes = 0;
        std::size_t scroll = 0;
        std::chrono::steady_clock::time_point turn_started_at{};
        int working_row = -1;
        bool tool_output_expanded = false;
        bool pasting = false;
        bool dirty = true;
        bool quitting = false;
    };

    class PendingEvents {
    public:
        void push(const microcodex::CodexEvent &event) noexcept {
            try {
                std::string text;
                if (event.type != microcodex::CodexEventType::TurnCompleted) {
                    const bool is_tool_event =
                        event.type == microcodex::CodexEventType::ToolStarted ||
                        event.type == microcodex::CodexEventType::ToolFinished;
                    const std::size_t length = is_tool_event
                                                   ? std::min(event.text.size(), maximum_tool_preview_bytes)
                                                   : event.text.size();
                    text.assign(event.text, 0, length);
                    if (length != event.text.size()) {
                        text += "\n[output truncated in UI]";
                    }
                }

                microcodex::CodexEvent copy{
                    .type = event.type,
                    .turn_id = event.turn_id,
                    .call_id = event.call_id,
                    .tool_name = event.tool_name,
                    .text = std::move(text),
                    .edit = event.edit,
                    .succeeded = event.succeeded,
                };

                std::lock_guard lock(mutex_);
                if (copy.type == microcodex::CodexEventType::TextDelta &&
                    !events_.empty() &&
                    events_.back().type == microcodex::CodexEventType::TextDelta &&
                    events_.back().turn_id == copy.turn_id) {
                    // Model streams often contain many tiny deltas. Combining
                    // adjacent ones avoids one allocation and redraw per token.
                    events_.back().text += copy.text;
                    return;
                }
                events_.push_back(std::move(copy));
            } catch (...) {
                // The emitter is a noexcept presentation boundary. Remember a
                // dropped update so the UI can report it without throwing into
                // the API's network callback.
                dropped_event_.store(true, std::memory_order_relaxed);
            }
        }

        std::vector<microcodex::CodexEvent> take() {
            std::vector<microcodex::CodexEvent> result;
            std::lock_guard lock(mutex_);
            result.swap(events_);
            return result;
        }

        bool takeDroppedFlag() noexcept {
            return dropped_event_.exchange(false, std::memory_order_relaxed);
        }

    private:
        std::mutex mutex_;
        std::vector<microcodex::CodexEvent> events_;
        std::atomic<bool> dropped_event_ = false;
    };

    struct TerminalGuard {
        ~TerminalGuard() {
            microcodex::terminal::disableBracketedPaste();
            tb_shutdown();
        }
    };

    struct WrappedText {
        std::vector<std::string> lines;
        std::size_t cursor_line = 0;
        int cursor_column = 0;
    };

    using TurnResult = std::expected<microcodex::CodexApiResponse, std::string>;
    using TurnFuture = std::future<TurnResult>;

    std::size_t editBytes(const std::shared_ptr<const microcodex::EditResult>& edit) {
        if (!edit) return 0;
        std::size_t bytes = edit->path.size();
        for (const microcodex::EditHunk& hunk : edit->hunks) {
            if (hunk.context_before) bytes += hunk.context_before->size();
            if (hunk.context_after) bytes += hunk.context_after->size();
            for (const std::string& line : hunk.removed_lines) bytes += line.size();
            for (const std::string& line : hunk.added_lines) bytes += line.size();
        }
        return bytes;
    }

    std::size_t entryBytes(const UiEntry &entry) {
        return entry.turn_id.size() + entry.call_id.size() + entry.title.size() +
               entry.text.size() + entry.output.size() + editBytes(entry.edit);
    }

    void trimTranscript(UiState &state) {
        while (state.transcript_bytes > maximum_transcript_bytes && state.transcript.size() > 1) {
            state.transcript_bytes -= entryBytes(state.transcript.front());
            state.transcript.pop_front();
        }
    }

    UiEntry &addEntry(UiState &state, UiEntry entry) {
        state.transcript_bytes += entryBytes(entry);
        state.transcript.push_back(std::move(entry));
        trimTranscript(state);
        return state.transcript.back();
    }

    void replaceEntryOutput(UiState &state, UiEntry &entry, std::string output) {
        state.transcript_bytes -= entry.output.size();
        entry.output = std::move(output);
        state.transcript_bytes += entry.output.size();
    }

    void replaceEntryEdit(UiState &state, UiEntry &entry,
                          std::shared_ptr<const microcodex::EditResult> edit) {
        state.transcript_bytes -= editBytes(entry.edit);
        entry.edit = std::move(edit);
        state.transcript_bytes += editBytes(entry.edit);
    }

    void appendEntryText(UiState &state, UiEntry &entry, const std::string_view text) {
        entry.text.append(text);
        state.transcript_bytes += text.size();
    }

    UiEntry *findToolEntry(UiState &state, const std::string_view call_id) {
        for (auto entry = state.transcript.rbegin(); entry != state.transcript.rend(); ++entry) {
            if (entry->kind == EntryKind::Tool && entry->call_id == call_id) {
                return &*entry;
            }
        }
        return nullptr;
    }
    // Maybe this does too much? Could be split into different routines
    std::expected<void, std::string> loadSavedHistory(UiState &state, microcodex::CodexApi &api) {
        auto initialized = api.initializeConversation();
        if (!initialized) return std::unexpected(initialized.error());

        auto turns = api.readHistoryBefore(api.savedTurnCount(), maximum_transcript_bytes);
        if (!turns) return std::unexpected(turns.error());
        for (const microcodex::SavedTurn &turn : *turns) {
            for (const std::string &item : turn.items) {
                auto message = microcodex::responseMessage(item);
                if (!message) return std::unexpected(message.error());
                if (*message) {
                    const bool user = (*message)->role == "user";
                    addEntry(state, {
                        .kind = user ? EntryKind::User : EntryKind::Assistant,
                        .turn_id = turn.id,
                        .call_id = {},
                        .title = user ? "you" : "codex",
                        .text = std::move((*message)->text),
                        .output = {},
                        .markdown_cache = {},
                        .edit = {},
                    });
                    continue;
                }

                auto call = microcodex::responseToolCall(item);
                if (!call) return std::unexpected(call.error());
                if (*call) {
                    addEntry(state, {
                        .kind = EntryKind::Tool,
                        .turn_id = turn.id,
                        .call_id = std::move((*call)->call_id),
                        .title = std::move((*call)->name),
                        .text = std::move((*call)->arguments),
                        .output = {},
                        .markdown_cache = {},
                        .finished = false,
                        .edit = {},
                    });
                    continue;
                }

                auto output = microcodex::responseToolOutput(item);
                if (!output) return std::unexpected(output.error());
                if (*output) {
                    UiEntry *entry = findToolEntry(state, (*output)->call_id);
                    if (entry != nullptr) {
                        replaceEntryOutput(state, *entry, std::move((*output)->output));
                        entry->finished = true;
                        entry->succeeded = !entry->output.starts_with("Error:");
                    }
                }
            }
        }
        state.scroll = 0;
        state.dirty = true;
        return {};
    }

    void finishAssistantEntries(UiState &state, const std::string_view turn_id) {
        for (UiEntry &entry : state.transcript) {
            if (entry.kind == EntryKind::Assistant && entry.turn_id == turn_id) {
                entry.finished = true;
            }
        }
    }

    void addNoticeOnce(UiState &state, const EntryKind kind, const std::string_view text) {
        if (!state.transcript.empty() && state.transcript.back().text == text) {
            return;
        }
        addEntry(state, {
            .kind = kind,
            .turn_id = {},
            .call_id = {},
            .title = kind == EntryKind::Error ? "error" : "notice",
            .text = std::string(text),
            .output = {},
            .markdown_cache = {},
            .edit = {},
        });
    }

    void applyEvent(UiState &state, const microcodex::CodexEvent &event) {
        using microcodex::CodexEventType;

        switch (event.type) {
        case CodexEventType::TurnStarted:
            state.status = "Thinking...";
            state.turn_started_at = std::chrono::steady_clock::now();
            break;

        case CodexEventType::TextDelta:
            if (!state.transcript.empty() &&
                state.transcript.back().kind == EntryKind::Assistant &&
                state.transcript.back().turn_id == event.turn_id &&
                !state.transcript.back().finished) {
                appendEntryText(state, state.transcript.back(), event.text);
            } else {
                addEntry(state, {
                    .kind = EntryKind::Assistant,
                    .turn_id = event.turn_id,
                    .call_id = {},
                    .title = "codex",
                    .text = event.text,
                    .output = {},
                    .markdown_cache = {},
                    .finished = false,
                    .edit = {},
                });
            }
            break;

        case CodexEventType::ToolStarted:
            addEntry(state, {
                .kind = EntryKind::Tool,
                .turn_id = event.turn_id,
                .call_id = event.call_id,
                .title = event.tool_name,
                .text = event.text,
                .output = {},
                .markdown_cache = {},
                .finished = false,
                .edit = {},
            });
            state.status = "Running " + event.tool_name + "...";
            break;

        case CodexEventType::ToolFinished: {
            UiEntry *entry = findToolEntry(state, event.call_id);
            if (entry == nullptr) {
                entry = &addEntry(state, {
                    .kind = EntryKind::Tool,
                    .turn_id = event.turn_id,
                    .call_id = event.call_id,
                    .title = event.tool_name,
                    .text = {},
                    .output = {},
                    .markdown_cache = {},
                    .finished = false,
                    .edit = {},
                });
            }
            // Keep the arguments received in ToolStarted. Codex's transcript
            // shows what was called and the result as two distinct rows.
            replaceEntryOutput(state, *entry, event.text);
            replaceEntryEdit(state, *entry, event.edit);
            entry->finished = true;
            entry->succeeded = event.succeeded;
            state.status = event.succeeded ? "Tool completed" : "Tool failed";
            break;
        }

        case CodexEventType::TurnCompleted:
            // api.cpp turns endpoints that only provide a final message into
            // one TextDelta before emitting TurnCompleted. The queue therefore
            // omits the completion event's duplicate copy of the full answer.
            finishAssistantEntries(state, event.turn_id);
            state.status = "Ready";
            break;

        case CodexEventType::TurnInterrupted:
            finishAssistantEntries(state, event.turn_id);
            addNoticeOnce(state, EntryKind::Notice, event.text);
            state.status = "Turn interrupted";
            break;

        case CodexEventType::Error:
            finishAssistantEntries(state, event.turn_id);
            addNoticeOnce(state, EntryKind::Error, event.text);
            state.status = "Turn failed";
            break;
        }

        trimTranscript(state);
        state.dirty = true;
    }

    void applyPendingEvents(UiState &state, PendingEvents &pending) {
        for (const microcodex::CodexEvent &event : pending.take()) {
            applyEvent(state, event);
        }
        if (pending.takeDroppedFlag()) {
            state.status = "A display update was dropped (out of memory)";
            state.dirty = true;
        }
    }

    std::size_t previousUtf8(const std::string_view text, std::size_t offset) {
        if (offset == 0) {
            return 0;
        }
        --offset;
        while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) {
            --offset;
        }
        return offset;
    }

    std::size_t nextUtf8(const std::string_view text, const std::size_t offset) {
        if (offset >= text.size()) {
            return text.size();
        }
        std::size_t next = offset + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xc0) == 0x80) {
            ++next;
        }
        return next;
    }

    bool isWordCodepoint(const std::uint32_t codepoint) {
        return (codepoint >= 'a' && codepoint <= 'z') ||
               (codepoint >= 'A' && codepoint <= 'Z') ||
               (codepoint >= '0' && codepoint <= '9') ||
               codepoint == '_' || codepoint >= 0x80;
    }

    bool isWordAt(const std::string_view text, const std::size_t offset) {
        return offset < text.size() && isWordCodepoint(decodeCodepoint(text, offset).first);
    }

    std::size_t previousWord(const std::string_view text, std::size_t offset) {
        while (offset > 0) {
            const std::size_t previous = previousUtf8(text, offset);
            if (isWordAt(text, previous)) {
                break;
            }
            offset = previous;
        }
        while (offset > 0) {
            const std::size_t previous = previousUtf8(text, offset);
            if (!isWordAt(text, previous)) {
                break;
            }
            offset = previous;
        }
        return offset;
    }

    std::size_t nextWord(const std::string_view text, std::size_t offset) {
        if (isWordAt(text, offset)) {
            while (offset < text.size() && isWordAt(text, offset)) {
                offset = nextUtf8(text, offset);
            }
            return offset;
        }
        while (offset < text.size() && !isWordAt(text, offset)) {
            offset = nextUtf8(text, offset);
        }
        while (offset < text.size() && isWordAt(text, offset)) {
            offset = nextUtf8(text, offset);
        }
        return offset;
    }

    std::size_t lineStart(const std::string_view text, const std::size_t offset) {
        if (offset == 0) {
            return 0;
        }
        const std::size_t newline = text.rfind('\n', offset - 1);
        return newline == std::string_view::npos ? 0 : newline + 1;
    }

    std::size_t lineEnd(const std::string_view text, const std::size_t offset) {
        const std::size_t newline = text.find('\n', offset);
        return newline == std::string_view::npos ? text.size() : newline;
    }

    void insertCodepoint(UiState &state, const std::uint32_t codepoint) {
        char utf8[7]{};
        const int length = tb_utf8_unicode_to_char(utf8, codepoint);
        if (length <= 0) {
            return;
        }
        state.input.insert(state.input_cursor, utf8, static_cast<std::size_t>(length));
        state.input_cursor += static_cast<std::size_t>(length);
        state.dirty = true;
    }

    void insertPaste(UiState &state) {
        for (std::size_t position = state.paste.find('\r');
             position != std::string::npos;
             position = state.paste.find('\r', position)) {
            if (position + 1 < state.paste.size() && state.paste[position + 1] == '\n') {
                state.paste.erase(position, 1);
            } else {
                state.paste[position++] = '\n';
            }
        }
        const std::size_t character_count = std::count_if(
            state.paste.begin(), state.paste.end(),
            [](const unsigned char byte) { return (byte & 0xc0) != 0x80; });
        std::string displayed;
        if (character_count > large_paste_character_threshold) {
            const std::string base = "[Pasted Content " +
                                     std::to_string(character_count) + " chars]";
            displayed = base;
            for (std::size_t number = 2;
                 state.input.find(displayed) != std::string::npos; ++number) {
                displayed = base + " #" + std::to_string(number);
            }
            state.pending_pastes.emplace_back(displayed, std::move(state.paste));
        } else {
            displayed = std::move(state.paste);
        }
        state.input.insert(state.input_cursor, displayed);
        state.input_cursor += displayed.size();
        state.paste.clear();
        state.dirty = true;
    }

    WrappedText wrapText(const std::string_view text, const int width, const std::string_view first_prefix, const std::string_view continuation_prefix, const std::size_t cursor = std::string_view::npos) {
        WrappedText result;
        std::string line(first_prefix);
        int column = textWidth(first_prefix);
        int content_start = column;
        bool cursor_recorded = false;

        const auto recordCursor = [&] {
            if (!cursor_recorded) {
                result.cursor_line = result.lines.size();
                result.cursor_column = std::min(column, std::max(0, width - 1));
                cursor_recorded = true;
            }
        };

        const auto startContinuation = [&] {
            result.lines.push_back(std::move(line));
            line.assign(continuation_prefix);
            column = textWidth(continuation_prefix);
            content_start = column;
        };

        for (std::size_t offset = 0; offset < text.size();) {
            if (cursor == offset) {
                recordCursor();
            }

            auto [codepoint, length] = decodeCodepoint(text, offset);
            if (length == 0) {
                break;
            }

            if (codepoint == '\r') {
                offset += length;
                continue;
            }
            if (codepoint == '\n') {
                startContinuation();
                offset += length;
                continue;
            }
            if (codepoint == '\t') {
                const int spaces = 4 - (column % 4);
                for (int index = 0; index < spaces; ++index) {
                    if (column >= width && column > content_start) {
                        startContinuation();
                    }
                    line += ' ';
                    ++column;
                }
                offset += length;
                continue;
            }

            int codepoint_width = tb_wcwidth(codepoint);
            if (codepoint_width < 0) {
                codepoint = 0xfffd;
                codepoint_width = 1;
            }
            if (codepoint_width > 0 && column + codepoint_width > width && column > content_start) {
                startContinuation();
                if (cursor == offset) {
                    // A cursor before the character belongs on the wrapped
                    // line, not beyond the right edge of the previous one.
                    cursor_recorded = false;
                    recordCursor();
                }
            }

            char utf8[7]{};
            const int encoded = tb_utf8_unicode_to_char(utf8, codepoint);
            if (encoded > 0) {
                line.append(utf8, static_cast<std::size_t>(encoded));
            }
            column += std::max(0, codepoint_width);
            offset += length;
        }

        if (cursor == text.size()) {
            recordCursor();
        }
        result.lines.push_back(std::move(line));
        return result;
    }

    void appendLines(std::vector<StyledLine> &destination, std::vector<StyledLine> source) {
        for (StyledLine &line : source) {
            destination.push_back(std::move(line));
        }
    }

    std::vector<StyledLine> collapseToolOutput(std::vector<StyledLine> output_lines) {
        if (output_lines.size() <= maximum_collapsed_tool_output_rows) {
            return output_lines;
        }

        const std::size_t visible_rows = maximum_collapsed_tool_output_rows - 1;
        const std::size_t head_rows = visible_rows / 2;
        const std::size_t tail_rows = visible_rows - head_rows;
        const std::size_t omitted_rows = output_lines.size() - head_rows - tail_rows;

        std::vector<StyledLine> collapsed;
        collapsed.reserve(maximum_collapsed_tool_output_rows);
        for (std::size_t index = 0; index < head_rows; ++index) {
            collapsed.push_back(std::move(output_lines[index]));
        }

        StyledLine omission = lineWithPrefix({{"    ", faint_foreground}});
        appendSpan(omission,
                   "… +" + std::to_string(omitted_rows) +
                       " lines (ctrl + t to view full output)",
                   muted_foreground | TB_DIM);
        collapsed.push_back(std::move(omission));

        for (std::size_t index = output_lines.size() - tail_rows;
             index < output_lines.size(); ++index) {
            collapsed.push_back(std::move(output_lines[index]));
        }
        return collapsed;
    }

    std::string toolInvocation(const UiEntry &entry) {
        if (entry.text.empty()) {
            return entry.title;
        }
        return entry.title + '(' + entry.text + ')';
    }

    std::optional<std::string> bashCommand(const UiEntry &entry) {
        if (entry.title != "bash") {
            return std::nullopt;
        }
        auto command = microcodex::ToolArguments(entry.text).string("command");
        if (!command) {
            return std::nullopt;
        }
        return std::move(*command);
    }

    std::string paddedLineNumber(const std::size_t number, const std::size_t width) {
        std::string text = std::to_string(number);
        text.insert(0, width - text.size(), ' ');
        return text;
    }

    void appendDiffLine(std::vector<StyledLine>& lines, const std::string_view text,
                        const std::size_t number, const std::size_t number_width,
                        const char marker, const int width) {
        const bool added = marker == '+';
        const bool removed = marker == '-';
        const uintattr_t foreground = added ? success_foreground
                                            : removed ? error_foreground : TB_DEFAULT;
        const uintattr_t background = added ? added_background
                                            : removed ? removed_background : TB_DEFAULT;
        const StyledLine first = lineWithPrefix({
            {"    ", TB_DEFAULT},
            {paddedLineNumber(number, number_width) + " ", muted_foreground | TB_DIM},
            {std::string(1, marker), foreground},
        }, background, added || removed);
        const StyledLine continuation = lineWithPrefix({
            {std::string(4 + number_width + 2, ' '), TB_DEFAULT},
        }, background, added || removed);
        appendLines(lines, wrapStyledText(text, width, first, continuation, foreground));
    }

    void appendEditLines(std::vector<StyledLine>& lines, const microcodex::EditResult& edit,
                         const int width) {
        std::size_t added = 0;
        std::size_t removed = 0;
        std::size_t maximum_line = 1;
        for (const microcodex::EditHunk& hunk : edit.hunks) {
            added += hunk.added_lines.size();
            removed += hunk.removed_lines.size();
            if (!hunk.removed_lines.empty()) {
                maximum_line = std::max(
                    maximum_line, hunk.old_start + hunk.removed_lines.size() - 1);
            }
            if (!hunk.added_lines.empty()) {
                maximum_line = std::max(
                    maximum_line, hunk.new_start + hunk.added_lines.size() - 1);
            }
            if (hunk.context_after) {
                maximum_line = std::max(
                    maximum_line, hunk.new_start + hunk.added_lines.size());
            }
        }
        const std::size_t number_width = std::to_string(maximum_line).size();

        StyledLine header;
        appendSpan(header, "• ", muted_foreground | TB_DIM);
        appendSpan(header, "Edited ", TB_DEFAULT | TB_BOLD);
        appendSpan(header, edit.path);
        appendSpan(header, " (");
        appendSpan(header, "+" + std::to_string(added), success_foreground);
        appendSpan(header, " ");
        appendSpan(header, "-" + std::to_string(removed), error_foreground);
        appendSpan(header, ")");
        appendLines(lines, wrapStyledSpans(
            header.spans, width, StyledLine{}, lineWithPrefix({{"  ", TB_DEFAULT}})));

        for (std::size_t hunk_index = 0; hunk_index < edit.hunks.size(); ++hunk_index) {
            const microcodex::EditHunk& hunk = edit.hunks[hunk_index];
            if (hunk_index != 0) {
                StyledLine separator;
                appendSpan(separator, std::string(5 + number_width, ' '));
                appendSpan(separator, "⋮", muted_foreground | TB_DIM);
                lines.push_back(std::move(separator));
            }
            if (hunk.context_before) {
                appendDiffLine(lines, *hunk.context_before, hunk.new_start - 1,
                               number_width, ' ', width);
            }
            for (std::size_t index = 0; index < hunk.removed_lines.size(); ++index) {
                appendDiffLine(lines, hunk.removed_lines[index], hunk.old_start + index,
                               number_width, '-', width);
            }
            for (std::size_t index = 0; index < hunk.added_lines.size(); ++index) {
                appendDiffLine(lines, hunk.added_lines[index], hunk.new_start + index,
                               number_width, '+', width);
            }
            if (hunk.context_after) {
                appendDiffLine(lines, *hunk.context_after,
                               hunk.new_start + hunk.added_lines.size(),
                               number_width, ' ', width);
            }
        }
    }

    void appendToolLines(std::vector<StyledLine> &lines, const UiEntry &entry,
                         const int width, const bool show_full_output) {
        if (entry.finished && entry.succeeded && entry.edit) {
            appendEditLines(lines, *entry.edit, width);
            return;
        }
        const uintattr_t bullet_color = !entry.finished
                                            ? muted_foreground
                                            : (entry.succeeded ? success_foreground
                                                               : error_foreground);
        const std::optional<std::string> command = bashCommand(entry);
        const std::string_view verb = entry.finished && command ? "Ran "
                                      : entry.finished           ? "Called "
                                                                 : "Running ";
        const StyledLine header = lineWithPrefix({
            {"• ", static_cast<uintattr_t>(bullet_color | TB_BOLD)},
            {std::string(verb), TB_DEFAULT | TB_BOLD},
        });
        const StyledLine continuation = lineWithPrefix({{"  │ ", faint_foreground}});
        if (command) {
            const std::vector<StyledSpan> highlighted = highlightShell(*command);
            appendLines(lines, wrapStyledSpans(highlighted, width, header, continuation));
        } else {
            appendLines(lines, wrapStyledText(toolInvocation(entry), width, header,
                                              continuation));
        }

        if (!entry.finished) {
            return;
        }
        const std::string_view output = entry.output.empty()
                                            ? std::string_view("(no output)")
                                            : std::string_view(entry.output);
        const StyledLine output_first = lineWithPrefix({{"  └ ", faint_foreground}});
        const StyledLine output_continuation = lineWithPrefix({{"    ", faint_foreground}});
        std::vector<StyledLine> output_lines = wrapStyledText(
            output, width, output_first, output_continuation, muted_foreground);
        if (!show_full_output) {
            output_lines = collapseToolOutput(std::move(output_lines));
        }
        appendLines(lines, std::move(output_lines));
    }

    void appendAssistantLines(std::vector<StyledLine> &lines, UiEntry &entry, const int width) {
        const int content_width = std::max(1, width - 2);
        MarkdownRenderCache &cache = entry.markdown_cache;
        if (cache.width != content_width || cache.source_size != entry.text.size()) {
            cache.lines = renderMarkdown(entry.text, content_width);
            cache.source_size = entry.text.size();
            cache.width = content_width;
        }

        bool first_content_line = true;
        for (const StyledLine &source : cache.lines) {
            if (source.spans.empty()) {
                lines.push_back(source);
                continue;
            }

            StyledLine line{
                .spans = {},
                .background = source.background,
                .fill_background = source.fill_background,
            };
            appendSpan(line, first_content_line ? "• " : "  ",
                       first_content_line ? muted_foreground | TB_DIM : TB_DEFAULT);
            first_content_line = false;
            for (const StyledSpan &span : source.spans) {
                appendSpan(line, span.text, span.foreground);
            }
            lines.push_back(std::move(line));
        }
    }

    std::vector<StyledLine> transcriptLines(UiState &state, const int width) {
        std::vector<StyledLine> lines;
        const UiEntry *previous_entry = nullptr;
        for (UiEntry &entry : state.transcript) {
            if (previous_entry != nullptr && previous_entry->kind == EntryKind::Tool &&
                entry.kind == EntryKind::Tool) {
                // Tool output can be visually dense. Codex leaves a little
                // more air between consecutive calls than between other cells.
                lines.push_back({});
            }
            switch (entry.kind) {
            case EntryKind::User: {
                lines.push_back({
                    .spans = {},
                    .background = surface_background,
                    .fill_background = true,
                });
                const StyledLine first = lineWithPrefix(
                    {{"› ", TB_DEFAULT | TB_BOLD | TB_DIM}}, surface_background, true);
                const StyledLine continuation = lineWithPrefix(
                    {{"  ", TB_DEFAULT}}, surface_background, true);
                appendLines(lines, wrapStyledText(entry.text, width, first,
                                                  continuation));
                lines.push_back({
                    .spans = {},
                    .background = surface_background,
                    .fill_background = true,
                });
                lines.push_back({});
                break;
            }
            case EntryKind::Assistant:
                appendAssistantLines(lines, entry, width);
                lines.push_back({});
                break;
            case EntryKind::Tool:
                appendToolLines(lines, entry, width, state.tool_output_expanded);
                lines.push_back({});
                break;
            case EntryKind::Error:
                appendLines(lines, wrapStyledText(
                    entry.text, width,
                    lineWithPrefix({{"✗ ", error_foreground | TB_BOLD}}),
                    lineWithPrefix({{"  ", TB_DEFAULT}}), error_foreground));
                lines.push_back({});
                break;
            case EntryKind::Notice:
                appendLines(lines, wrapStyledText(
                    entry.text, width,
                    lineWithPrefix({{"• ", muted_foreground | TB_DIM}}),
                    lineWithPrefix({{"  ", TB_DEFAULT}}), muted_foreground));
                lines.push_back({});
                break;
            }
            previous_entry = &entry;
        }
        if (!lines.empty() && lines.back().spans.empty() &&
            lines.back().background == TB_DEFAULT) {
            lines.pop_back();
        }
        return lines;
    }

    StyledLine workingLine(const UiState &state) {
        const auto elapsed = std::chrono::steady_clock::now() - state.turn_started_at;
        const auto elapsed_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        const bool solid_bullet = ((elapsed_milliseconds / 600) % 2) == 0;
        StyledLine line;
        appendSpan(line, solid_bullet ? "• " : "◦ ",
                   solid_bullet ? TB_DEFAULT : muted_foreground | TB_DIM);

        constexpr std::string_view label = "Working";
        constexpr int padding = 10;
        constexpr int band_half_width = 5;
        const int period = static_cast<int>(label.size()) + padding * 2;
        const int position = static_cast<int>(
            (elapsed_milliseconds % 2000) * period / 2000);
        for (std::size_t index = 0; index < label.size(); ++index) {
            const int distance = std::abs(static_cast<int>(index) + padding - position);
            const uintattr_t shade = distance > band_half_width
                                         ? 242
                                         : static_cast<uintattr_t>(255 - distance * 2);
            appendSpan(line, label.substr(index, 1), shade | TB_BOLD);
        }

        const auto seconds = std::max<std::int64_t>(0, elapsed_milliseconds / 1000);
        appendSpan(line, " (" + std::to_string(seconds) +
                             "s • esc / ctrl + c to interrupt)",
                   muted_foreground | TB_DIM);
        return line;
    }

    StyledLine footerLine(const UiState &state, const bool turn_active) {
        StyledLine line;
        if (!turn_active && state.status != "Ready") {
            appendSpan(line, "  " + state.status + "  ·  ", muted_foreground);
        } else {
            appendSpan(line, "  ", muted_foreground);
        }
        appendSpan(line, state.active_model + "  ·  ", muted_foreground);
        appendSpan(line,
                   turn_active
                       ? "esc / ctrl + c interrupt   ctrl + q quit"
                       : "enter send   ctrl + j newline   ctrl + r reset   ctrl + d quit",
                   muted_foreground | TB_DIM);
        if (state.scroll != 0) {
            appendSpan(line, "   scroll " + std::to_string(state.scroll),
                       muted_foreground | TB_DIM);
        }
        return line;
    }

    void drawLine(const StyledLine &line, const int row, const int width) {
        if (row < 0 || row >= tb_height()) {
            return;
        }
        if (line.fill_background) {
            for (int column = 0; column < width; ++column) {
                tb_set_cell(column, row, ' ', TB_DEFAULT, line.background);
            }
        }
        int column = 0;
        for (const StyledSpan &span : line.spans) {
            if (column >= width) {
                break;
            }
            tb_print(column, row, span.foreground, line.background,
                     span.text.c_str());
            column += textWidth(span.text);
        }
    }

    int renderWorkingFrame(const UiState &state) {
        if (state.working_row < 0 || state.working_row >= tb_height()) {
            return TB_OK;
        }
        StyledLine line = workingLine(state);
        line.fill_background = true;
        drawLine(line, state.working_row, tb_width());
        return tb_present();
    }

    int render(UiState &state, const bool turn_active) {
        const int width = tb_width();
        const int height = tb_height();
        if (width <= 0 || height <= 0) {
            return TB_OK;
        }

        int result = tb_clear();
        if (result != TB_OK) {
            return result;
        }

        WrappedText prompt = wrapText(state.input, width, "› ", "  ", state.input_cursor);
        // Active turns use: transcript, spacer, Working, spacer, composer.
        // The composer contributes its own shaded top padding after this.
        const int working_rows = turn_active && height >= 7 ? 3 : 0;
        // Without the active-turn area, keep one terminal-background row
        // between the transcript and the composer's shaded surface.
        const int composer_spacer_rows = working_rows == 0 && height >= 5 ? 1 : 0;
        const int footer_rows = height >= 2 ? 1 : 0;
        const int composer_padding = height >= 4 ? 2 : 0;
        const int available_prompt_rows =
            std::max(1, height - working_rows - composer_spacer_rows -
                            footer_rows - composer_padding);
        const int prompt_limit = std::min({maximum_prompt_rows,
                                           std::max(1, height / 3),
                                           available_prompt_rows});
        const int prompt_rows = std::min(prompt_limit, static_cast<int>(prompt.lines.size()));

        std::size_t prompt_start = 0;
        if (prompt.lines.size() > static_cast<std::size_t>(prompt_rows)) {
            const std::size_t last_start = prompt.lines.size() - static_cast<std::size_t>(prompt_rows);
            prompt_start = std::min(prompt.cursor_line, last_start);
            if (prompt.cursor_line >= prompt_start + static_cast<std::size_t>(prompt_rows)) {
                prompt_start = prompt.cursor_line - static_cast<std::size_t>(prompt_rows) + 1;
            }
        }

        const int lower_rows = working_rows + composer_spacer_rows +
                               composer_padding + prompt_rows + footer_rows;
        const int body_rows = std::max(0, height - lower_rows);
        std::vector<StyledLine> transcript = transcriptLines(state, width);
        const std::size_t maximum_scroll = transcript.size() > static_cast<std::size_t>(body_rows)
                                               ? transcript.size() - static_cast<std::size_t>(body_rows)
                                               : 0;
        state.scroll = std::min(state.scroll, maximum_scroll);

        const std::size_t transcript_end = transcript.size() - state.scroll;
        const std::size_t transcript_start = transcript_end > static_cast<std::size_t>(body_rows)
                                                 ? transcript_end - static_cast<std::size_t>(body_rows)
                                                 : 0;
        int row = body_rows - static_cast<int>(transcript_end - transcript_start);
        for (std::size_t index = transcript_start; index < transcript_end; ++index, ++row) {
            if (row >= 0) {
                drawLine(transcript[index], row, width);
            }
        }

        if (working_rows != 0) {
            state.working_row = body_rows + 1;
            drawLine(workingLine(state), state.working_row, width);
        } else {
            state.working_row = -1;
        }

        const int composer_top = body_rows + working_rows + composer_spacer_rows;
        for (int composer_row = 0;
             composer_row < composer_padding + prompt_rows; ++composer_row) {
            drawLine({.spans = {},
                      .background = surface_background,
                      .fill_background = true},
                     composer_top + composer_row, width);
        }
        const int prompt_top = composer_top + composer_padding / 2;
        for (int prompt_row = 0; prompt_row < prompt_rows; ++prompt_row) {
            const std::size_t index = prompt_start + static_cast<std::size_t>(prompt_row);
            StyledLine line{
                .spans = {},
                .background = surface_background,
                .fill_background = true,
            };
            if (index == 0 && prompt.lines[index].starts_with("›")) {
                appendSpan(line, "›", TB_DEFAULT | TB_BOLD);
                appendSpan(line, std::string_view(prompt.lines[index]).substr(3));
            } else {
                appendSpan(line, prompt.lines[index]);
            }
            if (state.input.empty() && index == 0) {
                appendSpan(line, "Ask MicroCodex to do anything", muted_foreground | TB_DIM);
            }
            drawLine(line, prompt_top + prompt_row, width);
        }

        if (footer_rows != 0) {
            drawLine(footerLine(state, turn_active), height - 1, width);
        }

        const std::size_t visible_cursor_line = prompt.cursor_line - prompt_start;
        tb_set_cursor(prompt.cursor_column,
                      prompt_top + static_cast<int>(std::min(
                                       visible_cursor_line,
                                       static_cast<std::size_t>(prompt_rows - 1))));
        result = tb_present();
        if (result == TB_OK) {
            state.dirty = false;
        }
        return result;
    }

    void startTurn(UiState &state, microcodex::CodexApi &api, TurnFuture &turn) {
        if (turn.valid()) {
            state.status = "A turn is already running";
            state.dirty = true;
            return;
        }
        if (state.input.empty()) {
            state.status = "Enter a message first";
            state.dirty = true;
            return;
        }

        std::string message = std::move(state.input);
        for (auto &[placeholder, paste] : state.pending_pastes) {
            const std::size_t position = message.find(placeholder);
            if (position != std::string::npos) {
                message.replace(position, placeholder.size(), paste);
            }
        }
        state.pending_pastes.clear();
        state.input.clear();
        state.input_cursor = 0;
        state.scroll = 0;
        addEntry(state, {
            .kind = EntryKind::User,
            .turn_id = {},
            .call_id = {},
            .title = "you",
            .text = message,
            .output = {},
            .markdown_cache = {},
            .edit = {},
        });

        try {
            turn = std::async(std::launch::async, [&api, message = std::move(message)] {
                return api.sendUserMessage(message);
            });
            state.turn_started_at = std::chrono::steady_clock::now();
            state.status = "Starting turn...";
        } catch (const std::exception &error) {
            addNoticeOnce(state, EntryKind::Error,
                          std::string("Could not start turn: ") + error.what());
            state.status = "Could not start turn";
        } catch (...) {
            addNoticeOnce(state, EntryKind::Error, "Could not start turn");
            state.status = "Could not start turn";
        }
        state.dirty = true;
    }

    void collectFinishedTurn(UiState &state, PendingEvents &pending, TurnFuture &turn) {
        if (!turn.valid() || turn.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }

        try {
            TurnResult result = turn.get();
            // All normal terminal events are emitted before sendUserMessage()
            // returns. Drain once more before deciding whether an error still
            // needs to be displayed.
            applyPendingEvents(state, pending);
            if (!result) {
                addNoticeOnce(state, EntryKind::Error, result.error());
                if (state.status != "Turn interrupted") {
                    state.status = "Turn failed";
                }
            }
        } catch (const std::exception &error) {
            addNoticeOnce(state, EntryKind::Error,
                          std::string("Turn task failed: ") + error.what());
            state.status = "Turn failed";
        } catch (...) {
            addNoticeOnce(state, EntryKind::Error, "Turn task failed");
            state.status = "Turn failed";
        }

        state.dirty = true;
    }

    void resetConversation(UiState &state, microcodex::CodexApi &api, const TurnFuture &turn) {
        if (turn.valid()) {
            state.status = "Interrupt the active turn before resetting";
            state.dirty = true;
            return;
        }

        auto reset = api.resetConversation();
        if (!reset) {
            state.status = reset.error();
        } else {
            state.transcript.clear();
            state.transcript_bytes = 0;
            state.scroll = 0;
            state.status = "Conversation reset";
        }
        state.dirty = true;
    }

    void eraseBeforeCursor(UiState &state) {
        if (state.input_cursor == 0) {
            return;
        }
        const std::size_t previous = previousUtf8(state.input, state.input_cursor);
        state.input.erase(previous, state.input_cursor - previous);
        state.input_cursor = previous;
        state.dirty = true;
    }

    void eraseAtCursor(UiState &state) {
        if (state.input_cursor >= state.input.size()) {
            return;
        }
        const std::size_t next = nextUtf8(state.input, state.input_cursor);
        state.input.erase(state.input_cursor, next - state.input_cursor);
        state.dirty = true;
    }

    void appendPastedKey(UiState &state, const tb_event &event) {
        if (event.key == TB_KEY_ENTER) {
            state.paste += '\r';
        } else if (event.key == TB_KEY_CTRL_J) {
            state.paste += '\n';
        } else if (event.key == TB_KEY_TAB) {
            state.paste += '\t';
        } else if (event.ch != 0) {
            char utf8[7]{};
            const int length = tb_utf8_unicode_to_char(utf8, event.ch);
            if (length > 0) {
                state.paste.append(utf8, static_cast<std::size_t>(length));
            }
        }
    }

    void handleKey(UiState &state, microcodex::CodexApi &api, TurnFuture &turn, const tb_event &event) {
        if (event.key == TB_KEY_CTRL_Q) {
            state.quitting = true;
            return;
        }
        if (event.key == TB_KEY_CTRL_A) {
            state.input_cursor = lineStart(state.input, state.input_cursor);
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_CTRL_E) {
            state.input_cursor = lineEnd(state.input, state.input_cursor);
            state.dirty = true;
            return;
        }
        if ((event.mod & TB_MOD_ALT) != 0 && (event.ch == 'b' || event.ch == 'B')) {
            // Many terminals encode option-left as escape followed by "b".
            state.input_cursor = previousWord(state.input, state.input_cursor);
            state.dirty = true;
            return;
        }
        if ((event.mod & TB_MOD_ALT) != 0 && (event.ch == 'f' || event.ch == 'F')) {
            // Likewise, option-right is commonly sent as meta-f.
            state.input_cursor = nextWord(state.input, state.input_cursor);
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_CTRL_C || event.key == TB_KEY_ESC) {
            if (turn.valid()) {
                api.interrupt();
                state.status = "Interrupting turn...";
            } else if (!state.input.empty() || !state.pending_pastes.empty()) {
                state.input.clear();
                state.pending_pastes.clear();
                state.input_cursor = 0;
                state.status = "Input cleared";
            }
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_CTRL_D) {
            if (!turn.valid() && state.input.empty()) {
                state.quitting = true;
            } else {
                eraseAtCursor(state);
            }
            return;
        }
        if (event.key == TB_KEY_CTRL_R) {
            resetConversation(state, api, turn);
            return;
        }
        if (event.key == TB_KEY_CTRL_U) {
            state.input.erase(0, state.input_cursor);
            state.input_cursor = 0;
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_CTRL_K) {
            state.input.erase(state.input_cursor);
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_CTRL_L) {
            tb_invalidate();
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_CTRL_T) {
            state.tool_output_expanded = !state.tool_output_expanded;
            state.scroll = 0;
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_CTRL_J) {
            insertCodepoint(state, '\n');
            return;
        }
        if (event.key == TB_KEY_ENTER) {
            startTurn(state, api, turn);
            return;
        }
        if (event.key == TB_KEY_BACKSPACE || event.key == TB_KEY_BACKSPACE2) {
            eraseBeforeCursor(state);
            return;
        }
        if (event.key == TB_KEY_DELETE) {
            eraseAtCursor(state);
            return;
        }
        if (event.key == TB_KEY_ARROW_LEFT) {
            state.input_cursor = (event.mod & (TB_MOD_ALT | TB_MOD_CTRL)) != 0
                                     ? previousWord(state.input, state.input_cursor)
                                     : previousUtf8(state.input, state.input_cursor);
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_ARROW_RIGHT) {
            state.input_cursor = (event.mod & (TB_MOD_ALT | TB_MOD_CTRL)) != 0
                                     ? nextWord(state.input, state.input_cursor)
                                     : nextUtf8(state.input, state.input_cursor);
            state.dirty = true;
            return;
        }
        if ((event.mod & TB_MOD_ALT) != 0 && event.key == TB_KEY_ARROW_UP) {
            state.input_cursor = lineStart(state.input, state.input_cursor);
            state.dirty = true;
            return;
        }
        if ((event.mod & TB_MOD_ALT) != 0 && event.key == TB_KEY_ARROW_DOWN) {
            state.input_cursor = lineEnd(state.input, state.input_cursor);
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_HOME) {
            state.input_cursor = 0;
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_END) {
            state.input_cursor = state.input.size();
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_PGUP || event.key == TB_KEY_ARROW_UP ||
            event.key == TB_KEY_MOUSE_WHEEL_UP) {
            const std::size_t amount = event.key == TB_KEY_MOUSE_WHEEL_UP
                                           ? 3
                                           : static_cast<std::size_t>(std::max(1, tb_height() - 3));
            state.scroll += amount;
            state.dirty = true;
            return;
        }
        if (event.key == TB_KEY_PGDN || event.key == TB_KEY_ARROW_DOWN ||
            event.key == TB_KEY_MOUSE_WHEEL_DOWN) {
            const std::size_t amount = event.key == TB_KEY_MOUSE_WHEEL_DOWN
                                           ? 3
                                           : static_cast<std::size_t>(std::max(1, tb_height() - 3));
            state.scroll = state.scroll > amount ? state.scroll - amount : 0;
            state.dirty = true;
            return;
        }
        if (event.ch != 0 && tb_iswprint(event.ch)) {
            insertCodepoint(state, event.ch);
        }
    }

    void stopActiveTurn(UiState &state, PendingEvents &pending, microcodex::CodexApi &api, TurnFuture &turn) {
        if (!turn.valid()) {
            return;
        }
        api.interrupt();
        turn.wait();
        collectFinishedTurn(state, pending, turn);
    }

} // namespace

namespace microcodex {

    std::expected<void, std::string> runInteractive(CodexApiConfig config) {
        PendingEvents pending;
        CodexEventEmitter emitter([&pending](const CodexEvent &event) {
            pending.push(event);
        });
        UiState state;
        state.active_model = config.model;
        CodexApi api(std::move(config), emitter);
        TurnFuture turn;

        auto history = loadSavedHistory(state, api);
        if (!history) return std::unexpected(history.error());

        const int initialized = tb_init();
        if (initialized != TB_OK) {
            return std::unexpected("Could not initialize terminal UI: " +
                                   std::string(tb_strerror(initialized)));
        }
        TerminalGuard terminal;
        tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE);
        const int paste_mode = terminal::enableBracketedPaste();
        if (paste_mode != TB_OK) {
            return std::unexpected("Could not enable terminal paste handling: " +
                                   std::string(tb_strerror(paste_mode)));
        }
        const int output_mode = tb_set_output_mode(TB_OUTPUT_256);
        if (output_mode != TB_OK) {
            return std::unexpected("Could not enable 256-color terminal output: " +
                                   std::string(tb_strerror(output_mode)));
        }

        std::string terminal_error;
        while (!state.quitting) {
            applyPendingEvents(state, pending);
            collectFinishedTurn(state, pending, turn);

            if (state.dirty) {
                const int rendered = render(state, turn.valid());
                if (rendered != TB_OK) {
                    terminal_error = "Could not render terminal UI: " +
                                     std::string(tb_strerror(rendered));
                    break;
                }
            }

            tb_event event{};
            const int polled = tb_peek_event(
                &event, turn.valid() ? animation_frame_milliseconds : idle_poll_milliseconds);
            if (polled == TB_OK) {
                if (event.type == terminal::paste_start_event) {
                    state.paste.clear();
                    state.pasting = true;
                } else if (event.type == terminal::paste_end_event) {
                    if (state.pasting) {
                        insertPaste(state);
                        state.pasting = false;
                    }
                } else if (state.pasting && event.type == TB_EVENT_KEY) {
                    appendPastedKey(state, event);
                } else if (event.type == TB_EVENT_KEY || event.type == TB_EVENT_MOUSE) {
                    handleKey(state, api, turn, event);
                } else if (event.type == TB_EVENT_RESIZE) {
                    state.dirty = true;
                }
            } else if (polled == TB_ERR_NO_EVENT && turn.valid()) {
                // Only the status row changes between API updates. Redrawing
                // that row avoids re-wrapping and reallocating the transcript
                // 31 times per second just to animate seven characters.
                const int animated = renderWorkingFrame(state);
                if (animated != TB_OK) {
                    terminal_error = "Could not render terminal animation: " +
                                     std::string(tb_strerror(animated));
                    break;
                }
            } else if (polled != TB_ERR_NO_EVENT &&
                       !(polled == TB_ERR_POLL && tb_last_errno() == EINTR)) {
                terminal_error = "Could not read terminal input: " +
                                 std::string(tb_strerror(polled));
                break;
            }
        }

        stopActiveTurn(state, pending, api, turn);
        api.shutdown();
        if (!terminal_error.empty()) {
            return std::unexpected(std::move(terminal_error));
        }
        return {};
    }

} // namespace microcodex
