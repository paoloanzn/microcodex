// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "ui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <expected>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "api.h"
#include "conversation.h"
#include "edit.h"
#include "event-emitter.h"
#include "markdown.h"
#include "response-item.h"
#include "shell-highlight.h"
#include "slash-command.h"
#include "styled-text.h"
#include "terminal.h"
#include "tool.h"

namespace {

using microcodex::ui::appendSpan;
using microcodex::ui::decodeCodepoint;
using microcodex::ui::highlightShell;
using microcodex::ui::lineWithPrefix;
using microcodex::ui::renderMarkdown;
using microcodex::ui::StyledLine;
using microcodex::ui::StyledSpan;
using microcodex::ui::textWidth;
using microcodex::ui::wrapStyledSpans;
using microcodex::ui::wrapStyledText;

constexpr std::size_t maximum_transcript_bytes = std::size_t{512} * 1024;
constexpr std::size_t maximum_tool_preview_bytes = std::size_t{16} * 1024;
constexpr std::size_t large_paste_character_threshold = 1000;
constexpr std::size_t maximum_collapsed_tool_output_rows = 5;
constexpr int animation_frame_milliseconds = 32;
constexpr int idle_poll_milliseconds = 250;
constexpr int maximum_prompt_rows = 6;
constexpr std::size_t maximum_overlay_rows = 10;

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

enum class EntryKind : std::uint8_t {
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
    std::string active_effort;
    // The open slash-command picker. While it is set it takes the keys.
    std::optional<microcodex::OpenPicker> picker;
    // Selected row of the command menu. The menu's entries are not stored:
    // they are recomputed from `input`, so they always match the composer.
    std::size_t command_selection = 0;
    std::size_t input_cursor = 0;
    std::size_t transcript_bytes = 0;
    std::size_t scroll = 0;
    std::chrono::steady_clock::time_point turn_started_at;
    int working_row = -1;
    bool tool_output_expanded = false;
    bool pasting = false;
    bool dirty = true;
    bool quitting = false;
};

class PendingEvents {
public:
    void push(const microcodex::CodexEvent& event) noexcept {
        try {
            std::string text;
            if (event.type != microcodex::CodexEventType::TurnCompleted) {
                const bool is_tool_event = event.type == microcodex::CodexEventType::ToolStarted ||
                                           event.type == microcodex::CodexEventType::ToolFinished;
                const std::size_t length =
                    is_tool_event ? std::min(event.text.size(), maximum_tool_preview_bytes) : event.text.size();
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

            const std::lock_guard lock(mutex_);
            if (copy.type == microcodex::CodexEventType::TextDelta && !events_.empty() &&
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
        const std::lock_guard lock(mutex_);
        result.swap(events_);
        return result;
    }

    bool takeDroppedFlag() noexcept { return dropped_event_.exchange(false, std::memory_order_relaxed); }

private:
    std::mutex mutex_;
    std::vector<microcodex::CodexEvent> events_;
    std::atomic<bool> dropped_event_ = false;
};

struct TerminalGuard {
    TerminalGuard() = default;
    TerminalGuard(const TerminalGuard&) = delete;
    TerminalGuard& operator=(const TerminalGuard&) = delete;
    TerminalGuard(TerminalGuard&&) = delete;
    TerminalGuard& operator=(TerminalGuard&&) = delete;
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

std::size_t entryBytes(const UiEntry& entry) {
    return entry.turn_id.size() + entry.call_id.size() + entry.title.size() + entry.text.size() + entry.output.size() +
           editBytes(entry.edit);
}

void trimTranscript(UiState& state) {
    while (state.transcript_bytes > maximum_transcript_bytes && state.transcript.size() > 1) {
        state.transcript_bytes -= entryBytes(state.transcript.front());
        state.transcript.pop_front();
    }
}

UiEntry& addEntry(UiState& state, UiEntry entry) {
    state.transcript_bytes += entryBytes(entry);
    state.transcript.push_back(std::move(entry));
    trimTranscript(state);
    return state.transcript.back();
}

void replaceEntryOutput(UiState& state, UiEntry& entry, std::string output) {
    state.transcript_bytes -= entry.output.size();
    entry.output = std::move(output);
    state.transcript_bytes += entry.output.size();
}

void replaceEntryEdit(UiState& state, UiEntry& entry, std::shared_ptr<const microcodex::EditResult> edit) {
    state.transcript_bytes -= editBytes(entry.edit);
    entry.edit = std::move(edit);
    state.transcript_bytes += editBytes(entry.edit);
}

void appendEntryText(UiState& state, UiEntry& entry, const std::string_view text) {
    entry.text.append(text);
    state.transcript_bytes += text.size();
}

UiEntry* findToolEntry(UiState& state, const std::string_view call_id) {
    for (auto& entry : std::ranges::reverse_view(state.transcript)) {
        if (entry.kind == EntryKind::Tool && entry.call_id == call_id) {
            return &entry;
        }
    }
    return nullptr;
}
// Adds one saved response item to the transcript: a message, a tool call, or
// the output of an earlier tool call. Other item types are skipped.
std::expected<void, std::string> addSavedItem(UiState& state, const microcodex::SavedTurn& turn,
                                              const std::string& item) {
    auto message = microcodex::responseMessage(item);
    if (!message) return std::unexpected(message.error());
    if (auto& value = *message; value) {
        const bool user = value->role == "user";
        addEntry(state, {
                            .kind = user ? EntryKind::User : EntryKind::Assistant,
                            .turn_id = turn.id,
                            .call_id = {},
                            .title = user ? "you" : "codex",
                            .text = std::move(value->text),
                            .output = {},
                            .markdown_cache = {},
                            .edit = {},
                        });
        return {};
    }

    auto call = microcodex::responseToolCall(item);
    if (!call) return std::unexpected(call.error());
    if (auto& value = *call; value) {
        addEntry(state, {
                            .kind = EntryKind::Tool,
                            .turn_id = turn.id,
                            .call_id = std::move(value->call_id),
                            .title = std::move(value->name),
                            .text = std::move(value->arguments),
                            .output = {},
                            .markdown_cache = {},
                            .finished = false,
                            .edit = {},
                        });
        return {};
    }

    auto output = microcodex::responseToolOutput(item);
    if (!output) return std::unexpected(output.error());
    if (auto& value = *output; value) {
        UiEntry* entry = findToolEntry(state, value->call_id);
        if (entry != nullptr) {
            replaceEntryOutput(state, *entry, std::move(value->output));
            entry->finished = true;
            entry->succeeded = !entry->output.starts_with("Error:");
        }
    }
    return {};
}

std::expected<void, std::string> loadSavedHistory(UiState& state, microcodex::CodexApi& api) {
    auto initialized = api.initializeConversation();
    if (!initialized) return std::unexpected(initialized.error());

    auto turns = api.readHistoryBefore(api.savedTurnCount(), maximum_transcript_bytes);
    if (!turns) return std::unexpected(turns.error());
    for (const microcodex::SavedTurn& turn : *turns) {
        for (const std::string& item : turn.items) {
            auto added = addSavedItem(state, turn, item);
            if (!added) return added;
        }
    }
    state.scroll = 0;
    state.dirty = true;
    return {};
}

void finishAssistantEntries(UiState& state, const std::string_view turn_id) {
    for (UiEntry& entry : state.transcript) {
        if (entry.kind == EntryKind::Assistant && entry.turn_id == turn_id) {
            entry.finished = true;
        }
    }
}

void addMessage(UiState& state, const EntryKind kind, const std::string_view text) {
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

void addNoticeOnce(UiState& state, const EntryKind kind, const std::string_view text) {
    if (!state.transcript.empty() && state.transcript.back().text == text) {
        return;
    }
    addMessage(state, kind, text);
}

void applyEvent(UiState& state, const microcodex::CodexEvent& event) {
    using microcodex::CodexEventType;

    switch (event.type) {
        case CodexEventType::TurnStarted:
            state.status = "Thinking...";
            state.turn_started_at = std::chrono::steady_clock::now();
            break;

        case CodexEventType::TextDelta:
            if (!state.transcript.empty() && state.transcript.back().kind == EntryKind::Assistant &&
                state.transcript.back().turn_id == event.turn_id && !state.transcript.back().finished) {
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
            UiEntry* entry = findToolEntry(state, event.call_id);
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

void applyPendingEvents(UiState& state, PendingEvents& pending) {
    for (const microcodex::CodexEvent& event : pending.take()) {
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
    return (codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 'A' && codepoint <= 'Z') ||
           (codepoint >= '0' && codepoint <= '9') || codepoint == '_' || codepoint >= 0x80;
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

int visualColumn(const std::string_view text, const std::size_t start, const std::size_t end) {
    int column = 0;
    for (std::size_t offset = start; offset < end;) {
        const auto [codepoint, length] = decodeCodepoint(text, offset);
        if (length == 0) {
            break;
        }
        if (codepoint == '\t') {
            column += 4 - (column % 4);
        } else if (codepoint != '\r' && codepoint != '\n') {
            column += std::max(0, tb_wcwidth(codepoint));
        }
        offset += length;
    }
    return column;
}

std::size_t offsetAtVisualColumn(const std::string_view text, const std::size_t start, const std::size_t end,
                                 const int target_column) {
    int column = 0;
    std::size_t offset = start;
    while (offset < end) {
        const auto [codepoint, length] = decodeCodepoint(text, offset);
        if (length == 0) {
            break;
        }
        int width = 0;
        if (codepoint == '\t') {
            width = 4 - (column % 4);
        } else if (codepoint != '\r' && codepoint != '\n') {
            width = std::max(0, tb_wcwidth(codepoint));
        }
        if (column + width > target_column) {
            break;
        }
        column += width;
        offset += length;
    }
    return offset;
}

enum class VerticalDirection : std::uint8_t {
    Up,
    Down,
};

std::size_t moveCursorVertically(const std::string_view text, const std::size_t cursor,
                                 const VerticalDirection direction) {
    const std::size_t current_start = lineStart(text, cursor);
    const std::size_t current_end = lineEnd(text, cursor);
    const int column = visualColumn(text, current_start, cursor);

    if (direction == VerticalDirection::Up) {
        if (current_start == 0) {
            return cursor;
        }
        const std::size_t previous_end = current_start - 1;
        const std::size_t previous_start = lineStart(text, previous_end);
        return offsetAtVisualColumn(text, previous_start, previous_end, column);
    }
    if (current_end == text.size()) {
        return cursor;
    }
    const std::size_t next_start = current_end + 1;
    const std::size_t next_end = lineEnd(text, next_start);
    return offsetAtVisualColumn(text, next_start, next_end, column);
}

void keepCursorOutsidePaste(UiState& state) {
    for (const auto& [placeholder, paste] : state.pending_pastes) {
        const auto position = state.input.find(placeholder);
        if (position != std::string::npos && state.input_cursor > position &&
            state.input_cursor < position + placeholder.size()) {
            state.input_cursor = position + placeholder.size();
        }
    }
}

void eraseInputRange(UiState& state, std::size_t first, std::size_t last) {
    if (first == last) return;
    // A placeholder is one editable unit, including for word and line erases.
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const auto& [placeholder, paste] : state.pending_pastes) {
            const auto position = state.input.find(placeholder);
            if (position != std::string::npos && first < position + placeholder.size() && last > position &&
                (first > position || last < position + placeholder.size())) {
                first = std::min(first, position);
                last = std::max(last, position + placeholder.size());
                expanded = true;
            }
        }
    }
    std::erase_if(state.pending_pastes, [&](const auto& entry) {
        const auto position = state.input.find(entry.first);
        return position != std::string::npos && first < position + entry.first.size() && last > position;
    });
    state.input.erase(first, last - first);
    state.input_cursor = first;
    state.dirty = true;
}

void insertCodepoint(UiState& state, const std::uint32_t codepoint) {
    std::array<char, 7> utf8{};
    const int length = tb_utf8_unicode_to_char(utf8.data(), codepoint);
    if (length <= 0) {
        return;
    }
    keepCursorOutsidePaste(state);
    state.input.insert(state.input_cursor, utf8.data(), static_cast<std::size_t>(length));
    state.input_cursor += static_cast<std::size_t>(length);
    state.dirty = true;
}

bool isInvisiblePasteCodepoint(const std::uint32_t codepoint) {
    // Keep pasted control and format characters out of termbox's display
    // buffer. They remain in the pending paste and are restored on send.
    if ((codepoint < 0x20 && codepoint != '\r' && codepoint != '\n' && codepoint != '\t') ||
        (codepoint >= 0x7f && codepoint <= 0x9f)) {
        return true;
    }
    return codepoint == 0x00ad || codepoint == 0x061c || (codepoint >= 0x200b && codepoint <= 0x200f) ||
           (codepoint >= 0x202a && codepoint <= 0x202e) || (codepoint >= 0x2060 && codepoint <= 0x206f) ||
           codepoint == 0xfeff;
}

bool pasteNeedsSafeDisplay(const std::string_view paste) {
    for (std::size_t offset = 0; offset < paste.size();) {
        const auto [codepoint, length] = decodeCodepoint(paste, offset);
        if (length == 0) {
            return true;
        }
        if (isInvisiblePasteCodepoint(codepoint)) {
            return true;
        }
        offset += length;
    }
    return false;
}

void insertPaste(UiState& state) {
    for (std::size_t position = state.paste.find('\r'); position != std::string::npos;
         position = state.paste.find('\r', position)) {
        if (position + 1 < state.paste.size() && state.paste[position + 1] == '\n') {
            state.paste.erase(position, 1);
        } else {
            state.paste[position++] = '\n';
        }
    }
    const auto character_count = static_cast<std::size_t>(
        std::ranges::count_if(state.paste, [](const unsigned char byte) { return (byte & 0xc0) != 0x80; }));
    std::string displayed;
    if (character_count > large_paste_character_threshold || pasteNeedsSafeDisplay(state.paste)) {
        const std::string base = "[Pasted Content " + std::to_string(character_count) + " chars]";
        displayed = base;
        for (std::size_t number = 2; state.input.find(displayed) != std::string::npos; ++number) {
            displayed = base + " #" + std::to_string(number);
        }
        state.pending_pastes.emplace_back(displayed, std::move(state.paste));
    } else {
        displayed = std::move(state.paste);
    }
    keepCursorOutsidePaste(state);
    state.input.insert(state.input_cursor, displayed);
    state.input_cursor += displayed.size();
    state.paste.clear();
    state.dirty = true;
}

// Tab stops are every four columns.
constexpr int tab_width = 4;

// Wraps composer text into terminal rows and records the row and column of
// the cursor. Each row starts from a prefix, and wrapping happens only once a
// row holds content beyond that prefix.
class TextWrapper {
public:
    TextWrapper(const std::string_view first_prefix, const int width, const std::string_view continuation_prefix)
        : width_(width),
          continuation_prefix_(continuation_prefix),
          line_(first_prefix),
          column_(textWidth(first_prefix)),
          content_start_(column_) {}

    WrappedText wrap(const std::string_view text, const std::size_t cursor) && {
        for (std::size_t offset = 0; offset < text.size();) {
            if (cursor == offset) {
                recordCursor();
            }

            auto [codepoint, length] = decodeCodepoint(text, offset);
            if (length == 0) {
                break;
            }
            appendCodepoint(codepoint, cursor == offset);
            offset += length;
        }

        if (cursor == text.size()) {
            recordCursor();
        }
        result_.lines.push_back(std::move(line_));
        return std::move(result_);
    }

private:
    void recordCursor() {
        if (!cursor_recorded_) {
            result_.cursor_line = result_.lines.size();
            result_.cursor_column = std::min(column_, std::max(0, width_ - 1));
            cursor_recorded_ = true;
        }
    }

    void startContinuation() {
        result_.lines.push_back(std::move(line_));
        line_.assign(continuation_prefix_);
        column_ = textWidth(continuation_prefix_);
        content_start_ = column_;
    }

    void appendTab() {
        const int spaces = tab_width - (column_ % tab_width);
        for (int index = 0; index < spaces; ++index) {
            if (column_ >= width_ && column_ > content_start_) {
                startContinuation();
            }
            line_ += ' ';
            ++column_;
        }
    }

    void appendCodepoint(std::uint32_t codepoint, const bool cursor_before) {
        if (codepoint == '\r') {
            return;
        }
        if (codepoint == '\n') {
            startContinuation();
            return;
        }
        if (codepoint == '\t') {
            appendTab();
            return;
        }

        int codepoint_width = tb_wcwidth(codepoint);
        if (codepoint_width < 0) {
            codepoint = 0xfffd;
            codepoint_width = 1;
        }
        if (codepoint_width > 0 && column_ + codepoint_width > width_ && column_ > content_start_) {
            startContinuation();
            if (cursor_before) {
                // A cursor before the character belongs on the wrapped
                // line, not beyond the right edge of the previous one.
                cursor_recorded_ = false;
                recordCursor();
            }
        }

        std::array<char, 7> utf8{};
        const int encoded = tb_utf8_unicode_to_char(utf8.data(), codepoint);
        if (encoded > 0) {
            line_.append(utf8.data(), static_cast<std::size_t>(encoded));
        }
        column_ += std::max(0, codepoint_width);
    }

    int width_;
    std::string_view continuation_prefix_;
    WrappedText result_;
    std::string line_;
    int column_ = 0;
    int content_start_ = 0;
    bool cursor_recorded_ = false;
};

WrappedText wrapText(const std::string_view text, const int width, const std::string_view first_prefix,
                     const std::string_view continuation_prefix, const std::size_t cursor = std::string_view::npos) {
    return TextWrapper(first_prefix, width, continuation_prefix).wrap(text, cursor);
}

void appendLines(std::vector<StyledLine>& destination, std::vector<StyledLine> source) {
    for (StyledLine& line : source) {
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

    StyledLine omission = lineWithPrefix({{.text = "    ", .foreground = faint_foreground}});
    appendSpan(omission, "… +" + std::to_string(omitted_rows) + " lines (ctrl + t to view full output)",
               muted_foreground | TB_DIM);
    collapsed.push_back(std::move(omission));

    for (std::size_t index = output_lines.size() - tail_rows; index < output_lines.size(); ++index) {
        collapsed.push_back(std::move(output_lines[index]));
    }
    return collapsed;
}

std::string toolInvocation(const UiEntry& entry) {
    if (entry.text.empty()) {
        return entry.title;
    }
    return entry.title + '(' + entry.text + ')';
}

std::optional<std::string> bashCommand(const UiEntry& entry) {
    if (entry.title != "bash") {
        return std::nullopt;
    }
    auto command = microcodex::ToolArguments(entry.text).string("command");
    if (!command) {
        return std::nullopt;
    }
    return std::move(*command);
}

// One line of an edit diff: ' ' for context, '-' for removed, '+' for added.
struct DiffLine {
    std::string_view text;
    std::size_t number;
    char marker;
};

struct DiffColors {
    uintattr_t foreground = TB_DEFAULT;
    uintattr_t background = TB_DEFAULT;
};

DiffColors diffColors(const char marker) {
    if (marker == '+') return {.foreground = success_foreground, .background = added_background};
    if (marker == '-') return {.foreground = error_foreground, .background = removed_background};
    return {};
}

void appendDiffLine(std::vector<StyledLine>& lines, const std::size_t number_width, const DiffLine& line,
                    const int width) {
    const bool changed = line.marker == '+' || line.marker == '-';
    const DiffColors colors = diffColors(line.marker);
    std::string number = std::to_string(line.number);
    number.insert(0, number_width - number.size(), ' ');
    const StyledLine first = lineWithPrefix(
        {
            {.text = "    ", .foreground = TB_DEFAULT},
            {.text = number + " ", .foreground = muted_foreground | TB_DIM},
            {.text = std::string(1, line.marker), .foreground = colors.foreground},
        },
        colors.background, changed);
    const StyledLine continuation = lineWithPrefix(
        {
            {.text = std::string(4 + number_width + 2, ' '), .foreground = TB_DEFAULT},
        },
        colors.background, changed);
    appendLines(lines, wrapStyledText(line.text, width, first, continuation, colors.foreground));
}

void appendEditLines(std::vector<StyledLine>& lines, const microcodex::EditResult& edit, const int width) {
    std::size_t added = 0;
    std::size_t removed = 0;
    std::size_t maximum_line = 1;
    for (const microcodex::EditHunk& hunk : edit.hunks) {
        added += hunk.added_lines.size();
        removed += hunk.removed_lines.size();
        if (!hunk.removed_lines.empty()) {
            maximum_line = std::max(maximum_line, hunk.old_start + hunk.removed_lines.size() - 1);
        }
        if (!hunk.added_lines.empty()) {
            maximum_line = std::max(maximum_line, hunk.new_start + hunk.added_lines.size() - 1);
        }
        if (hunk.context_after) {
            maximum_line = std::max(maximum_line, hunk.new_start + hunk.added_lines.size());
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
    appendLines(lines, wrapStyledSpans(header.spans, width, StyledLine{},
                                       lineWithPrefix({{.text = "  ", .foreground = TB_DEFAULT}})));

    for (std::size_t hunk_index = 0; hunk_index < edit.hunks.size(); ++hunk_index) {
        const microcodex::EditHunk& hunk = edit.hunks[hunk_index];
        if (hunk_index != 0) {
            StyledLine separator;
            appendSpan(separator, std::string(5 + number_width, ' '));
            appendSpan(separator, "⋮", muted_foreground | TB_DIM);
            lines.push_back(std::move(separator));
        }
        if (hunk.context_before) {
            appendDiffLine(lines, number_width,
                           {.text = *hunk.context_before, .number = hunk.new_start - 1, .marker = ' '}, width);
        }
        for (std::size_t index = 0; index < hunk.removed_lines.size(); ++index) {
            appendDiffLine(lines, number_width,
                           {.text = hunk.removed_lines[index], .number = hunk.old_start + index, .marker = '-'}, width);
        }
        for (std::size_t index = 0; index < hunk.added_lines.size(); ++index) {
            appendDiffLine(lines, number_width,
                           {.text = hunk.added_lines[index], .number = hunk.new_start + index, .marker = '+'}, width);
        }
        if (hunk.context_after) {
            appendDiffLine(
                lines, number_width,
                {.text = *hunk.context_after, .number = hunk.new_start + hunk.added_lines.size(), .marker = ' '},
                width);
        }
    }
}

uintattr_t toolBulletColor(const UiEntry& entry) {
    if (!entry.finished) return muted_foreground;
    return entry.succeeded ? success_foreground : error_foreground;
}

std::string_view toolVerb(const UiEntry& entry, const bool is_command) {
    if (!entry.finished) return "Running ";
    return is_command ? "Ran " : "Called ";
}

void appendToolLines(std::vector<StyledLine>& lines, const UiEntry& entry, const int width,
                     const bool show_full_output) {
    if (entry.finished && entry.succeeded && entry.edit) {
        appendEditLines(lines, *entry.edit, width);
        return;
    }
    const uintattr_t bullet_color = toolBulletColor(entry);
    const std::optional<std::string> command = bashCommand(entry);
    const std::string_view verb = toolVerb(entry, command.has_value());
    const StyledLine header = lineWithPrefix({
        {.text = "• ", .foreground = static_cast<uintattr_t>(bullet_color | TB_BOLD)},
        {.text = std::string(verb), .foreground = TB_DEFAULT | TB_BOLD},
    });
    const StyledLine continuation = lineWithPrefix({{.text = "  │ ", .foreground = faint_foreground}});
    if (command) {
        const std::vector<StyledSpan> highlighted = highlightShell(*command);
        appendLines(lines, wrapStyledSpans(highlighted, width, header, continuation));
    } else {
        appendLines(lines, wrapStyledText(toolInvocation(entry), width, header, continuation));
    }

    if (!entry.finished) {
        return;
    }
    const std::string_view output =
        entry.output.empty() ? std::string_view("(no output)") : std::string_view(entry.output);
    const StyledLine output_first = lineWithPrefix({{.text = "  └ ", .foreground = faint_foreground}});
    const StyledLine output_continuation = lineWithPrefix({{.text = "    ", .foreground = faint_foreground}});
    std::vector<StyledLine> output_lines =
        wrapStyledText(output, width, output_first, output_continuation, muted_foreground);
    if (!show_full_output) {
        output_lines = collapseToolOutput(std::move(output_lines));
    }
    appendLines(lines, std::move(output_lines));
}

void appendAssistantLines(std::vector<StyledLine>& lines, UiEntry& entry, const int width) {
    const int content_width = std::max(1, width - 2);
    MarkdownRenderCache& cache = entry.markdown_cache;
    if (cache.width != content_width || cache.source_size != entry.text.size()) {
        cache.lines = renderMarkdown(entry.text, content_width);
        cache.source_size = entry.text.size();
        cache.width = content_width;
    }

    bool first_content_line = true;
    for (const StyledLine& source : cache.lines) {
        if (source.spans.empty()) {
            lines.push_back(source);
            continue;
        }

        StyledLine line{
            .spans = {},
            .background = source.background,
            .fill_background = source.fill_background,
        };
        appendSpan(line, first_content_line ? "• " : "  ", first_content_line ? muted_foreground | TB_DIM : TB_DEFAULT);
        first_content_line = false;
        for (const StyledSpan& span : source.spans) {
            appendSpan(line, span.text, span.foreground);
        }
        lines.push_back(std::move(line));
    }
}

std::vector<StyledLine> transcriptLines(UiState& state, const int width) {
    std::vector<StyledLine> lines;
    const UiEntry* previous_entry = nullptr;
    for (UiEntry& entry : state.transcript) {
        if (previous_entry != nullptr && previous_entry->kind == EntryKind::Tool && entry.kind == EntryKind::Tool) {
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
                const StyledLine first = lineWithPrefix({{.text = "› ", .foreground = TB_DEFAULT | TB_BOLD | TB_DIM}},
                                                        surface_background, true);
                const StyledLine continuation =
                    lineWithPrefix({{.text = "  ", .foreground = TB_DEFAULT}}, surface_background, true);
                appendLines(lines, wrapStyledText(entry.text, width, first, continuation));
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
                                       lineWithPrefix({{.text = "✗ ", .foreground = error_foreground | TB_BOLD}}),
                                       lineWithPrefix({{.text = "  ", .foreground = TB_DEFAULT}}), error_foreground));
                lines.push_back({});
                break;
            case EntryKind::Notice:
                appendLines(lines, wrapStyledText(
                                       entry.text, width,
                                       lineWithPrefix({{.text = "• ", .foreground = muted_foreground | TB_DIM}}),
                                       lineWithPrefix({{.text = "  ", .foreground = TB_DEFAULT}}), muted_foreground));
                lines.push_back({});
                break;
        }
        previous_entry = &entry;
    }
    if (!lines.empty() && lines.back().spans.empty() && lines.back().background == TB_DEFAULT) {
        lines.pop_back();
    }
    return lines;
}

StyledLine workingLine(const UiState& state) {
    const auto elapsed = std::chrono::steady_clock::now() - state.turn_started_at;
    const auto elapsed_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const bool solid_bullet = ((elapsed_milliseconds / 600) % 2) == 0;
    StyledLine line;
    appendSpan(line, solid_bullet ? "• " : "◦ ", solid_bullet ? TB_DEFAULT : muted_foreground | TB_DIM);

    constexpr std::string_view label = "Working";
    constexpr int padding = 10;
    constexpr int band_half_width = 5;
    const int period = static_cast<int>(label.size()) + (padding * 2);
    const int position = static_cast<int>((elapsed_milliseconds % 2000) * period / 2000);
    for (std::size_t index = 0; index < label.size(); ++index) {
        const int distance = std::abs(static_cast<int>(index) + padding - position);
        const uintattr_t shade = distance > band_half_width ? 242 : static_cast<uintattr_t>(255 - (distance * 2));
        appendSpan(line, label.substr(index, 1), shade | TB_BOLD);
    }

    const auto seconds = std::max<std::int64_t>(0, elapsed_milliseconds / 1000);
    appendSpan(line, " (" + std::to_string(seconds) + "s • esc / ctrl + c to interrupt)", muted_foreground | TB_DIM);
    return line;
}

// What the rows between the composer and the footer show.
enum class Overlay : std::uint8_t {
    None,
    CommandMenu,
    Picker,
};

StyledLine footerLine(const UiState& state, const bool turn_active, const Overlay overlay) {
    StyledLine line;
    if (!turn_active && state.status != "Ready") {
        appendSpan(line, "  " + state.status + "  ·  ", muted_foreground);
    } else {
        appendSpan(line, "  ", muted_foreground);
    }
    appendSpan(line, state.active_model + " " + state.active_effort + "  ·  ", muted_foreground);
    std::string_view keys = turn_active ? "esc / ctrl + c interrupt   ctrl + q quit"
                                        : "enter send   ctrl + j newline   ctrl + r reset   ctrl + d quit";
    if (overlay == Overlay::CommandMenu) {
        keys = "↑ / ↓ select   tab complete   enter run";
    } else if (overlay == Overlay::Picker) {
        keys = "↑ / ↓ select   enter choose   esc close";
    }
    appendSpan(line, keys, muted_foreground | TB_DIM);
    if (state.scroll != 0) {
        appendSpan(line, "   scroll " + std::to_string(state.scroll), muted_foreground | TB_DIM);
    }
    return line;
}

void drawLine(const int row, const StyledLine& line, const int width) {
    if (row < 0 || row >= tb_height()) {
        return;
    }
    if (line.fill_background) {
        for (int column = 0; column < width; ++column) {
            tb_set_cell(column, row, ' ', TB_DEFAULT, line.background);
        }
    }
    int column = 0;
    for (const StyledSpan& span : line.spans) {
        if (column >= width) {
            break;
        }
        tb_print(column, row, span.foreground, line.background, span.text.c_str());
        column += textWidth(span.text);
    }
}

// Index of the first of `rows` visible entries, chosen to keep the selected
// entry on screen.
std::size_t overlayWindowStart(const std::size_t selected, const std::size_t rows) {
    return selected >= rows ? selected - rows + 1 : 0;
}

void appendOverlayRow(std::vector<StyledLine>& lines, const bool selected, const std::string_view label,
                      const std::size_t label_width, const std::string_view description) {
    StyledLine line;
    appendSpan(line, selected ? "› " : "  ", selected ? TB_DEFAULT | TB_BOLD : muted_foreground);
    std::string padded(label);
    padded.resize(std::max(label_width, label.size()) + 3, ' ');
    appendSpan(line, padded, selected ? TB_DEFAULT | TB_BOLD : TB_DEFAULT);
    appendSpan(line, description, muted_foreground | TB_DIM);
    lines.push_back(std::move(line));
}

// The open picker, or the commands matching the composer input. Rows past
// the terminal width are cut by drawLine().
std::vector<StyledLine> overlayLines(const UiState& state, const microcodex::SlashCommandRegistry& commands) {
    std::vector<StyledLine> lines;
    if (state.picker) {
        const microcodex::OpenPicker& picker = *state.picker;
        StyledLine title;
        appendSpan(title, "  " + picker.title, TB_DEFAULT | TB_BOLD);
        lines.push_back(std::move(title));
        std::size_t label_width = 0;
        for (const microcodex::PickerOption& option : picker.options) {
            label_width = std::max(label_width, option.label.size());
        }
        const std::size_t rows = std::min(picker.options.size(), maximum_overlay_rows);
        const std::size_t start = overlayWindowStart(picker.selected, rows);
        for (std::size_t index = start; index < start + rows; ++index) {
            const microcodex::PickerOption& option = picker.options[index];
            appendOverlayRow(lines, index == picker.selected, option.label, label_width, option.description);
        }
        return lines;
    }

    const std::vector<const microcodex::SlashCommand*> matches = commands.completions(state.input);
    if (matches.empty()) return lines;
    std::vector<std::string> labels;
    std::size_t label_width = 0;
    for (const microcodex::SlashCommand* command : matches) {
        std::string label = "/" + std::string(command->name());
        if (!command->argumentHint().empty()) label += " " + std::string(command->argumentHint());
        label_width = std::max(label_width, label.size());
        labels.push_back(std::move(label));
    }
    const std::size_t selected = std::min(state.command_selection, matches.size() - 1);
    const std::size_t rows = std::min(matches.size(), maximum_overlay_rows);
    const std::size_t start = overlayWindowStart(selected, rows);
    for (std::size_t index = start; index < start + rows; ++index) {
        appendOverlayRow(lines, index == selected, labels[index], label_width, matches[index]->description());
    }
    return lines;
}

int renderWorkingFrame(const UiState& state) {
    if (state.working_row < 0 || state.working_row >= tb_height()) {
        return TB_OK;
    }
    StyledLine line = workingLine(state);
    line.fill_background = true;
    drawLine(state.working_row, line, tb_width());
    return tb_present();
}

// Row budget for everything below the transcript. Active turns use:
// transcript, spacer, Working, spacer, composer. The composer contributes its
// own shaded top padding.
struct ComposerLayout {
    int working_rows = 0;
    int composer_spacer_rows = 0;
    int footer_rows = 0;
    int composer_padding = 0;
    int prompt_rows = 0;
    int overlay_rows = 0;
    // First wrapped prompt line shown, chosen to keep the cursor visible.
    std::size_t prompt_start = 0;

    [[nodiscard]] int lowerRows() const {
        return working_rows + composer_spacer_rows + composer_padding + prompt_rows + overlay_rows + footer_rows;
    }
};

ComposerLayout composerLayout(const WrappedText& prompt, const int height, const bool turn_active,
                              const std::size_t overlay_lines) {
    ComposerLayout layout;
    layout.working_rows = turn_active && height >= 7 ? 3 : 0;
    // Without the active-turn area, keep one terminal-background row
    // between the transcript and the composer's shaded surface.
    layout.composer_spacer_rows = layout.working_rows == 0 && height >= 5 ? 1 : 0;
    layout.footer_rows = height >= 2 ? 1 : 0;
    layout.composer_padding = height >= 4 ? 2 : 0;
    const int available_prompt_rows = std::max(
        1, height - layout.working_rows - layout.composer_spacer_rows - layout.footer_rows - layout.composer_padding);
    const int prompt_limit = std::min({maximum_prompt_rows, std::max(1, height / 3), available_prompt_rows});
    layout.prompt_rows = std::min(prompt_limit, static_cast<int>(prompt.lines.size()));

    const auto prompt_rows = static_cast<std::size_t>(layout.prompt_rows);
    if (prompt.lines.size() > prompt_rows) {
        const std::size_t last_start = prompt.lines.size() - prompt_rows;
        layout.prompt_start = std::min(prompt.cursor_line, last_start);
        if (prompt.cursor_line >= layout.prompt_start + prompt_rows) {
            layout.prompt_start = prompt.cursor_line - prompt_rows + 1;
        }
    }
    // The overlay takes what is left, keeping one transcript row.
    const int free_rows = std::max(0, height - layout.lowerRows() - 1);
    layout.overlay_rows = static_cast<int>(std::min(overlay_lines, static_cast<std::size_t>(free_rows)));
    return layout;
}

// Draws the bottom of the transcript into the body rows, honoring and
// clamping the scroll offset.
void drawTranscript(const int body_rows, UiState& state, const int width) {
    const std::vector<StyledLine> transcript = transcriptLines(state, width);
    const auto visible_rows = static_cast<std::size_t>(body_rows);
    const std::size_t maximum_scroll = transcript.size() > visible_rows ? transcript.size() - visible_rows : 0;
    state.scroll = std::min(state.scroll, maximum_scroll);

    const std::size_t transcript_end = transcript.size() - state.scroll;
    const std::size_t transcript_start = transcript_end > visible_rows ? transcript_end - visible_rows : 0;
    int row = body_rows - static_cast<int>(transcript_end - transcript_start);
    for (std::size_t index = transcript_start; index < transcript_end; ++index, ++row) {
        if (row >= 0) {
            drawLine(row, transcript[index], width);
        }
    }
}

void drawPrompt(const UiState& state, const WrappedText& prompt, const ComposerLayout& layout, const int prompt_top,
                const int width) {
    for (int prompt_row = 0; prompt_row < layout.prompt_rows; ++prompt_row) {
        const std::size_t index = layout.prompt_start + static_cast<std::size_t>(prompt_row);
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
        drawLine(prompt_top + prompt_row, line, width);
    }
}

int render(UiState& state, const microcodex::SlashCommandRegistry& commands, const bool turn_active) {
    const int width = tb_width();
    const int height = tb_height();
    if (width <= 0 || height <= 0) {
        return TB_OK;
    }

    int result = tb_clear();
    if (result != TB_OK) {
        return result;
    }

    const WrappedText prompt = wrapText(state.input, width, "› ", "  ", state.input_cursor);
    const std::vector<StyledLine> overlay = overlayLines(state, commands);
    const ComposerLayout layout = composerLayout(prompt, height, turn_active, overlay.size());
    const int body_rows = std::max(0, height - layout.lowerRows());
    drawTranscript(body_rows, state, width);

    if (layout.working_rows != 0) {
        state.working_row = body_rows + 1;
        drawLine(state.working_row, workingLine(state), width);
    } else {
        state.working_row = -1;
    }

    const int composer_top = body_rows + layout.working_rows + layout.composer_spacer_rows;
    for (int composer_row = 0; composer_row < layout.composer_padding + layout.prompt_rows; ++composer_row) {
        drawLine(composer_top + composer_row, {.spans = {}, .background = surface_background, .fill_background = true},
                 width);
    }
    const int prompt_top = composer_top + (layout.composer_padding / 2);
    drawPrompt(state, prompt, layout, prompt_top, width);

    const int overlay_top = composer_top + layout.composer_padding + layout.prompt_rows;
    for (int overlay_row = 0; overlay_row < layout.overlay_rows; ++overlay_row) {
        drawLine(overlay_top + overlay_row, overlay[static_cast<std::size_t>(overlay_row)], width);
    }

    if (layout.footer_rows != 0) {
        Overlay shown = Overlay::None;
        if (state.picker) {
            shown = Overlay::Picker;
        } else if (!overlay.empty()) {
            shown = Overlay::CommandMenu;
        }
        drawLine(height - 1, footerLine(state, turn_active, shown), width);
    }

    const std::size_t visible_cursor_line = prompt.cursor_line - layout.prompt_start;
    tb_set_cursor(
        prompt.cursor_column,
        prompt_top + static_cast<int>(std::min(visible_cursor_line, static_cast<std::size_t>(layout.prompt_rows - 1))));
    result = tb_present();
    if (result == TB_OK) {
        state.dirty = false;
    }
    return result;
}

// The composer text with each paste label replaced by its pasted text.
std::string composerMessage(const UiState& state) {
    std::string message = state.input;
    for (const auto& [placeholder, paste] : state.pending_pastes) {
        const std::size_t position = message.find(placeholder);
        if (position != std::string::npos) {
            message.replace(position, placeholder.size(), paste);
        }
    }
    return message;
}

void clearComposer(UiState& state) {
    state.input.clear();
    state.pending_pastes.clear();
    state.input_cursor = 0;
}

void startTurn(UiState& state, microcodex::CodexApi& api, TurnFuture& turn) {
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

    std::string message = composerMessage(state);
    clearComposer(state);
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
        turn = std::async(std::launch::async,
                          [&api, message = std::move(message)] { return api.sendUserMessage(message); });
        state.turn_started_at = std::chrono::steady_clock::now();
        state.status = "Starting turn...";
    } catch (const std::exception& error) {
        addNoticeOnce(state, EntryKind::Error, std::string("Could not start turn: ") + error.what());
        state.status = "Could not start turn";
    } catch (...) {
        addNoticeOnce(state, EntryKind::Error, "Could not start turn");
        state.status = "Could not start turn";
    }
    state.dirty = true;
}

void collectFinishedTurn(UiState& state, PendingEvents& pending, TurnFuture& turn) {
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
    } catch (const std::exception& error) {
        addNoticeOnce(state, EntryKind::Error, std::string("Turn task failed: ") + error.what());
        state.status = "Turn failed";
    } catch (...) {
        addNoticeOnce(state, EntryKind::Error, "Turn task failed");
        state.status = "Turn failed";
    }

    state.dirty = true;
}

void resetConversation(UiState& state, microcodex::CodexApi& api, const TurnFuture& turn) {
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

template <typename... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

// The only place slash-command effects are carried out.
void applyCommandEffect(UiState& state, microcodex::CodexApi& api, const TurnFuture& turn,
                        microcodex::CommandEffect effect) {
    std::visit(Overloaded{
                   [&state](microcodex::ShowNotice& notice) { addMessage(state, EntryKind::Notice, notice.text); },
                   [&state](microcodex::ShowError& error) { addMessage(state, EntryKind::Error, error.text); },
                   [&](microcodex::ResetConversation& /*reset*/) { resetConversation(state, api, turn); },
                   [&state](microcodex::Quit& /*quit*/) { state.quitting = true; },
                   [&state](microcodex::OpenPicker& picker) {
                       if (picker.options.empty()) return;
                       picker.selected = std::min(picker.selected, picker.options.size() - 1);
                       state.picker = std::move(picker);
                   },
                   [&](microcodex::UpdateApi& update) {
                       // The worker thread owns the agent during a turn.
                       if (turn.valid()) {
                           addMessage(state, EntryKind::Error, "The agent cannot change while a turn is running");
                           return;
                       }
                       auto result = update.apply(api);
                       addMessage(state, result ? EntryKind::Notice : EntryKind::Error,
                                  result ? *result : result.error());
                   },
               },
               effect);
}

microcodex::CommandSnapshot commandSnapshot(const microcodex::CodexApi& api, const TurnFuture& turn) {
    microcodex::CommandSnapshot snapshot{.turn_running = turn.valid(), .agent = std::nullopt};
    if (!snapshot.turn_running) {
        if (auto status = api.status()) snapshot.agent = std::move(*status);
    }
    return snapshot;
}

// Runs `line` when it names a registered command. Returns false otherwise,
// so the caller can send it as a message.
bool runCommand(UiState& state, microcodex::CodexApi& api, TurnFuture& turn,
                const microcodex::SlashCommandRegistry& commands, const std::string& line) {
    std::optional<std::vector<microcodex::CommandEffect>> effects = commands.dispatch(line, commandSnapshot(api, turn));
    if (!effects) return false;
    clearComposer(state);
    state.scroll = 0;
    for (microcodex::CommandEffect& effect : *effects) {
        applyCommandEffect(state, api, turn, std::move(effect));
    }
    // A command may have changed the model or the effort shown in the footer.
    if (auto status = commandSnapshot(api, turn).agent) {
        state.active_model = std::move(status->model);
        state.active_effort = std::move(status->reasoning_effort);
    }
    state.dirty = true;
    return true;
}

void submitInput(UiState& state, microcodex::CodexApi& api, TurnFuture& turn,
                 const microcodex::SlashCommandRegistry& commands) {
    if (state.input.starts_with('/') && runCommand(state, api, turn, commands, composerMessage(state))) {
        return;
    }
    startTurn(state, api, turn);
}

void setComposerText(UiState& state, std::string text) {
    state.input = std::move(text);
    state.input_cursor = state.input.size();
    state.dirty = true;
}

bool isScrollKey(const tb_event& event) {
    return event.key == TB_KEY_PGUP || event.key == TB_KEY_PGDN || event.key == TB_KEY_MOUSE_WHEEL_UP ||
           event.key == TB_KEY_MOUSE_WHEEL_DOWN;
}

// An open picker is modal: it takes every key except quitting, redrawing
// and scrolling.
bool handlePickerKey(UiState& state, microcodex::CodexApi& api, TurnFuture& turn,
                     const microcodex::SlashCommandRegistry& commands, const tb_event& event) {
    if (!state.picker || event.key == TB_KEY_CTRL_Q || event.key == TB_KEY_CTRL_L || isScrollKey(event)) {
        return false;
    }
    microcodex::OpenPicker& picker = *state.picker;
    if (event.key == TB_KEY_ARROW_UP) {
        picker.selected = picker.selected == 0 ? picker.options.size() - 1 : picker.selected - 1;
    } else if (event.key == TB_KEY_ARROW_DOWN) {
        picker.selected = (picker.selected + 1) % picker.options.size();
    } else if (event.key == TB_KEY_ESC || event.key == TB_KEY_CTRL_C) {
        state.picker.reset();
    } else if (event.key == TB_KEY_ENTER) {
        const std::string line = std::move(picker.options[picker.selected].command);
        state.picker.reset();
        if (!runCommand(state, api, turn, commands, line)) {
            addMessage(state, EntryKind::Error, "Unknown command '" + line + "'");
        }
    }
    state.dirty = true;
    return true;
}

// Up and Down select a command, Tab completes its name, and Enter runs it.
bool handleCommandMenuKey(UiState& state, microcodex::CodexApi& api, TurnFuture& turn,
                          const microcodex::SlashCommandRegistry& commands, const tb_event& event) {
    const std::vector<const microcodex::SlashCommand*> matches = commands.completions(state.input);
    if (matches.empty()) return false;
    const std::size_t selected = std::min(state.command_selection, matches.size() - 1);
    const microcodex::SlashCommand& command = *matches[selected];
    if (event.key == TB_KEY_ARROW_UP) {
        state.command_selection = selected == 0 ? matches.size() - 1 : selected - 1;
    } else if (event.key == TB_KEY_ARROW_DOWN) {
        state.command_selection = (selected + 1) % matches.size();
    } else if (event.key == TB_KEY_TAB) {
        const bool takes_arguments = !command.argumentHint().empty();
        setComposerText(state, "/" + std::string(command.name()) + (takes_arguments ? " " : ""));
    } else if (event.key == TB_KEY_ENTER) {
        setComposerText(state, "/" + std::string(command.name()));
        submitInput(state, api, turn, commands);
    } else {
        return false;
    }
    state.dirty = true;
    return true;
}

void eraseBeforeCursor(UiState& state) {
    if (state.input_cursor == 0) {
        return;
    }
    const std::size_t previous = previousUtf8(state.input, state.input_cursor);
    eraseInputRange(state, previous, state.input_cursor);
}

void erasePreviousWord(UiState& state) {
    if (state.input_cursor == 0) {
        return;
    }
    const std::size_t start = previousWord(state.input, state.input_cursor);
    eraseInputRange(state, start, state.input_cursor);
}

void eraseAtCursor(UiState& state) {
    if (state.input_cursor >= state.input.size()) {
        return;
    }
    const std::size_t next = nextUtf8(state.input, state.input_cursor);
    eraseInputRange(state, state.input_cursor, next);
}

void appendPastedKey(UiState& state, const tb_event& event) {
    if (event.key == TB_KEY_ENTER) {
        state.paste += '\r';
    } else if (event.key == TB_KEY_CTRL_J) {
        state.paste += '\n';
    } else if (event.key == TB_KEY_TAB) {
        state.paste += '\t';
    } else if (event.ch != 0) {
        std::array<char, 7> utf8{};
        const int length = tb_utf8_unicode_to_char(utf8.data(), event.ch);
        if (length > 0) {
            state.paste.append(utf8.data(), static_cast<std::size_t>(length));
        }
    }
}

void moveInputCursor(UiState& state, const std::size_t cursor) {
    state.input_cursor = cursor;
    state.dirty = true;
}

bool hasAlt(const tb_event& event) { return (event.mod & TB_MOD_ALT) != 0; }

// Quit, line jumps, and the Escape-prefixed word jumps many terminals send for
// option-left and option-right.
bool handleQuitOrJumpKey(UiState& state, const tb_event& event) {
    if (event.key == TB_KEY_CTRL_Q) {
        state.quitting = true;
        return true;
    }
    if (event.key == TB_KEY_CTRL_A) {
        moveInputCursor(state, lineStart(state.input, state.input_cursor));
        return true;
    }
    if (event.key == TB_KEY_CTRL_E) {
        moveInputCursor(state, lineEnd(state.input, state.input_cursor));
        return true;
    }
    if (hasAlt(event) && (event.ch == 'b' || event.ch == 'B')) {
        // Many terminals encode option-left as escape followed by "b".
        moveInputCursor(state, previousWord(state.input, state.input_cursor));
        return true;
    }
    if (hasAlt(event) && (event.ch == 'f' || event.ch == 'F')) {
        // Likewise, option-right is commonly sent as meta-f.
        moveInputCursor(state, nextWord(state.input, state.input_cursor));
        return true;
    }
    return false;
}

void interruptOrClearInput(UiState& state, microcodex::CodexApi& api, const TurnFuture& turn) {
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
}

// Interrupting, quitting with Ctrl-D, resetting, and erasing to either end
// of the input.
bool handleControlKey(UiState& state, microcodex::CodexApi& api, const TurnFuture& turn, const tb_event& event) {
    if (event.key == TB_KEY_CTRL_C || event.key == TB_KEY_ESC) {
        interruptOrClearInput(state, api, turn);
        return true;
    }
    if (event.key == TB_KEY_CTRL_D) {
        if (!turn.valid() && state.input.empty()) {
            state.quitting = true;
        } else {
            eraseAtCursor(state);
        }
        return true;
    }
    if (event.key == TB_KEY_CTRL_R) {
        resetConversation(state, api, turn);
        return true;
    }
    if (event.key == TB_KEY_CTRL_U) {
        eraseInputRange(state, 0, state.input_cursor);
        return true;
    }
    if (event.key == TB_KEY_CTRL_K) {
        eraseInputRange(state, state.input_cursor, state.input.size());
        return true;
    }
    return false;
}

// Redraw, tool output expansion, newlines, sending, and character erases.
bool handleEditKey(UiState& state, microcodex::CodexApi& api, TurnFuture& turn,
                   const microcodex::SlashCommandRegistry& commands, const tb_event& event) {
    if (event.key == TB_KEY_CTRL_L) {
        tb_invalidate();
        state.dirty = true;
        return true;
    }
    if (event.key == TB_KEY_CTRL_T) {
        state.tool_output_expanded = !state.tool_output_expanded;
        state.scroll = 0;
        state.dirty = true;
        return true;
    }
    if (event.key == TB_KEY_CTRL_J) {
        insertCodepoint(state, '\n');
        return true;
    }
    if (event.key == TB_KEY_ENTER) {
        submitInput(state, api, turn, commands);
        return true;
    }
    if (event.key == TB_KEY_BACKSPACE || event.key == TB_KEY_BACKSPACE2) {
        if (hasAlt(event)) {
            erasePreviousWord(state);
        } else {
            eraseBeforeCursor(state);
        }
        return true;
    }
    if (event.key == TB_KEY_DELETE) {
        eraseAtCursor(state);
        return true;
    }
    return false;
}

bool handleCursorKey(UiState& state, const tb_event& event) {
    const bool word_jump = (event.mod & (TB_MOD_ALT | TB_MOD_CTRL)) != 0;
    if (event.key == TB_KEY_ARROW_LEFT) {
        moveInputCursor(state, word_jump ? previousWord(state.input, state.input_cursor)
                                         : previousUtf8(state.input, state.input_cursor));
        return true;
    }
    if (event.key == TB_KEY_ARROW_RIGHT) {
        moveInputCursor(
            state, word_jump ? nextWord(state.input, state.input_cursor) : nextUtf8(state.input, state.input_cursor));
        return true;
    }
    if (hasAlt(event) && event.key == TB_KEY_ARROW_UP) {
        moveInputCursor(state, lineStart(state.input, state.input_cursor));
        return true;
    }
    if (hasAlt(event) && event.key == TB_KEY_ARROW_DOWN) {
        moveInputCursor(state, lineEnd(state.input, state.input_cursor));
        return true;
    }
    if (event.key == TB_KEY_HOME) {
        moveInputCursor(state, 0);
        return true;
    }
    if (event.key == TB_KEY_END) {
        moveInputCursor(state, state.input.size());
        return true;
    }
    if (event.key == TB_KEY_ARROW_UP) {
        moveInputCursor(state, moveCursorVertically(state.input, state.input_cursor, VerticalDirection::Up));
        return true;
    }
    if (event.key == TB_KEY_ARROW_DOWN) {
        moveInputCursor(state, moveCursorVertically(state.input, state.input_cursor, VerticalDirection::Down));
        return true;
    }
    return false;
}

// Page keys scroll by a screen, the mouse wheel by three rows.
bool handleScrollKey(UiState& state, const tb_event& event) {
    const auto page = static_cast<std::size_t>(std::max(1, tb_height() - 3));
    if (event.key == TB_KEY_PGUP || event.key == TB_KEY_MOUSE_WHEEL_UP) {
        const std::size_t amount = event.key == TB_KEY_MOUSE_WHEEL_UP ? 3 : page;
        state.scroll += amount;
        state.dirty = true;
        return true;
    }
    if (event.key == TB_KEY_PGDN || event.key == TB_KEY_MOUSE_WHEEL_DOWN) {
        const std::size_t amount = event.key == TB_KEY_MOUSE_WHEEL_DOWN ? 3 : page;
        state.scroll = state.scroll > amount ? state.scroll - amount : 0;
        state.dirty = true;
        return true;
    }
    return false;
}

void handleKey(UiState& state, microcodex::CodexApi& api, TurnFuture& turn,
               const microcodex::SlashCommandRegistry& commands, const tb_event& event) {
    const std::size_t input_size = state.input.size();
    const bool handled = handlePickerKey(state, api, turn, commands, event) ||
                         handleCommandMenuKey(state, api, turn, commands, event) || handleQuitOrJumpKey(state, event) ||
                         handleControlKey(state, api, turn, event) ||
                         handleEditKey(state, api, turn, commands, event) || handleCursorKey(state, event) ||
                         handleScrollKey(state, event);
    if (!handled && event.ch != 0 && (tb_iswprint(event.ch) != 0)) {
        insertCodepoint(state, event.ch);
    }
    // Editing the filter starts the command menu from its first entry.
    if (state.input.size() != input_size) state.command_selection = 0;
}

void stopActiveTurn(UiState& state, PendingEvents& pending, microcodex::CodexApi& api, TurnFuture& turn) {
    if (!turn.valid()) {
        return;
    }
    api.interrupt();
    turn.wait();
    collectFinishedTurn(state, pending, turn);
}

// Applies one terminal event. Keys between the bracketed-paste markers are
// collected into the pending paste instead of being handled as keys.
void handleEvent(UiState& state, microcodex::CodexApi& api, TurnFuture& turn,
                 const microcodex::SlashCommandRegistry& commands, const tb_event& event) {
    if (event.type == microcodex::terminal::paste_start_event) {
        state.paste.clear();
        state.pasting = true;
    } else if (event.type == microcodex::terminal::paste_end_event) {
        if (state.pasting) {
            insertPaste(state);
            state.pasting = false;
        }
    } else if (state.pasting && event.type == TB_EVENT_KEY) {
        appendPastedKey(state, event);
    } else if (event.type == TB_EVENT_KEY || event.type == TB_EVENT_MOUSE) {
        handleKey(state, api, turn, commands, event);
    } else if (event.type == TB_EVENT_RESIZE) {
        state.dirty = true;
    }
}

// Enables mouse input, bracketed paste, and 256-color output.
std::expected<void, std::string> configureTerminal() {
    tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE);
    const int paste_mode = microcodex::terminal::enableBracketedPaste();
    if (paste_mode != TB_OK) {
        return std::unexpected("Could not enable terminal paste handling: " + std::string(tb_strerror(paste_mode)));
    }
    const int output_mode = tb_set_output_mode(TB_OUTPUT_256);
    if (output_mode != TB_OK) {
        return std::unexpected("Could not enable 256-color terminal output: " + std::string(tb_strerror(output_mode)));
    }
    return {};
}

}  // namespace

namespace microcodex {

std::expected<void, std::string> runInteractive(CodexApiConfig config, const SlashCommandRegistry& commands) {
    PendingEvents pending;
    CodexEventEmitter emitter([&pending](const CodexEvent& event) { pending.push(event); });
    UiState state;
    state.active_model = config.model;
    state.active_effort = config.reasoning_effort;
    CodexApi api(std::move(config), emitter);
    TurnFuture turn;

    auto history = loadSavedHistory(state, api);
    if (!history) return std::unexpected(history.error());

    const int initialized = tb_init();
    if (initialized != TB_OK) {
        return std::unexpected("Could not initialize terminal UI: " + std::string(tb_strerror(initialized)));
    }
    const TerminalGuard terminal;
    auto configured = configureTerminal();
    if (!configured) return configured;

    std::string terminal_error;
    while (!state.quitting) {
        applyPendingEvents(state, pending);
        collectFinishedTurn(state, pending, turn);

        if (state.dirty) {
            const int rendered = render(state, commands, turn.valid());
            if (rendered != TB_OK) {
                terminal_error = "Could not render terminal UI: " + std::string(tb_strerror(rendered));
                break;
            }
        }

        tb_event event{};
        const int polled = tb_peek_event(&event, turn.valid() ? animation_frame_milliseconds : idle_poll_milliseconds);
        if (polled == TB_OK) {
            handleEvent(state, api, turn, commands, event);
        } else if (polled == TB_ERR_NO_EVENT && turn.valid()) {
            // Only the status row changes between API updates. Redrawing
            // that row avoids re-wrapping and reallocating the transcript
            // 31 times per second just to animate seven characters.
            const int animated = renderWorkingFrame(state);
            if (animated != TB_OK) {
                terminal_error = "Could not render terminal animation: " + std::string(tb_strerror(animated));
                break;
            }
        } else if (polled != TB_ERR_NO_EVENT && (polled != TB_ERR_POLL || tb_last_errno() != EINTR)) {
            terminal_error = "Could not read terminal input: " + std::string(tb_strerror(polled));
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

}  // namespace microcodex
