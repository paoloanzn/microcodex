// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

// termbox2 is a single-header library. Its implementation must be enabled in
// exactly one translation unit, and the header must precede system headers so
// its POSIX feature-test macros take effect.
#define TB_IMPL
#include <termbox2.h>

#include <algorithm>
#include <cstddef>
#include <string_view>

#include "terminal.h"

namespace {

// The bytes termbox has read but not yet parsed into events.
std::string_view pendingInput() { return {global.in.buf, global.in.len}; }

int extractMetaComposerKey(tb_event* event, std::size_t* consumed) {
    const std::string_view input = pendingInput();
    if (input.size() < 2 || input[0] != '\x1b') {
        return TB_ERR;
    }

    const auto key = static_cast<unsigned char>(input[1]);
    if (key != 'b' && key != 'B' && key != 'f' && key != 'F' && key != TB_KEY_BACKSPACE && key != TB_KEY_BACKSPACE2) {
        return TB_ERR;
    }

    event->type = TB_EVENT_KEY;
    event->mod = TB_MOD_ALT;
    if (key == TB_KEY_BACKSPACE || key == TB_KEY_BACKSPACE2) {
        event->key = key;
    } else {
        event->ch = key;
    }
    *consumed = 2;
    return TB_OK;
}

// Termbox has no paste event, so intercept bracketed-paste markers before
// its normal escape-sequence parser handles them as keys. Meta composer
// keys need the same treatment in TB_INPUT_ESC mode: many terminals send
// Option combinations as an Escape-prefixed pair, which termbox otherwise
// splits into a standalone Escape and an unmodified key.
int extractPasteMarker(tb_event* event, std::size_t* consumed, const std::string_view marker,
                       const unsigned char event_type) {
    const std::string_view input = pendingInput();
    const std::size_t compared = std::min(input.size(), marker.size());
    if (input.substr(0, compared) != marker.substr(0, compared)) {
        return TB_ERR;
    }
    if (input.size() < marker.size()) {
        return TB_ERR_NEED_MORE;
    }
    event->type = event_type;
    *consumed = marker.size();
    return TB_OK;
}

int extractCustomInput(tb_event* event, std::size_t* consumed) {
    const int meta_result = extractMetaComposerKey(event, consumed);
    if (meta_result != TB_ERR) {
        return meta_result;
    }

    constexpr std::string_view start = "\x1b[200~";
    constexpr std::string_view end = "\x1b[201~";

    const int start_result = extractPasteMarker(event, consumed, start, microcodex::terminal::paste_start_event);
    if (start_result != TB_ERR) {
        return start_result;
    }
    return extractPasteMarker(event, consumed, end, microcodex::terminal::paste_end_event);
}

}  // namespace

namespace microcodex::terminal {

int enableBracketedPaste() {
    const int callback_result = tb_set_func(TB_FUNC_EXTRACT_PRE, extractCustomInput);
    if (callback_result != TB_OK) {
        return callback_result;
    }
    const int send_result = tb_send("\x1b[?2004h", 8);
    if (send_result != TB_OK) {
        return send_result;
    }
    return bytebuf_flush(&global.out, global.wfd);
}

void disableBracketedPaste() { tb_send("\x1b[?2004l", 8); }

}  // namespace microcodex::terminal
