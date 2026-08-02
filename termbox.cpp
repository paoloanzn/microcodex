// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

// termbox2 is a single-header library. Its implementation must be enabled in
// exactly one translation unit, and the header must precede system headers so
// its POSIX feature-test macros take effect.
#define TB_IMPL
#include <termbox2.h>

#include "terminal.h"

#include <algorithm>
#include <cstring>

namespace {

    int extractMetaComposerKey(tb_event *event, std::size_t *consumed) {
        if (global.in.len < 2 || global.in.buf[0] != '\x1b') {
            return TB_ERR;
        }

        const unsigned char key = static_cast<unsigned char>(global.in.buf[1]);
        if (key != 'b' && key != 'B' && key != 'f' && key != 'F' &&
            key != TB_KEY_BACKSPACE && key != TB_KEY_BACKSPACE2) {
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
    int extractPasteMarker(tb_event *event, std::size_t *consumed,
                           const char *marker, const std::size_t length,
                           const unsigned char event_type) {
        const std::size_t compared = std::min(global.in.len, length);
        if (std::memcmp(global.in.buf, marker, compared) != 0) {
            return TB_ERR;
        }
        if (global.in.len < length) {
            return TB_ERR_NEED_MORE;
        }
        event->type = event_type;
        *consumed = length;
        return TB_OK;
    }

    int extractCustomInput(tb_event *event, std::size_t *consumed) {
        const int meta_result = extractMetaComposerKey(event, consumed);
        if (meta_result != TB_ERR) {
            return meta_result;
        }

        constexpr char start[] = "\x1b[200~";
        constexpr char end[] = "\x1b[201~";

        const int start_result = extractPasteMarker(
            event, consumed, start, sizeof(start) - 1,
            microcodex::terminal::paste_start_event);
        if (start_result != TB_ERR) {
            return start_result;
        }
        return extractPasteMarker(event, consumed, end, sizeof(end) - 1,
                                  microcodex::terminal::paste_end_event);
    }

} // namespace

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

    void disableBracketedPaste() {
        tb_send("\x1b[?2004l", 8);
    }

} // namespace microcodex::terminal
