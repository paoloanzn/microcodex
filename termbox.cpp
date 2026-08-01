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

    // termbox has no paste event, so intercept bracketed-paste markers before
    // its normal escape-sequence parser handles them as keys.
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

    int extractBracketedPaste(tb_event *event, std::size_t *consumed) {
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
        const int callback_result = tb_set_func(TB_FUNC_EXTRACT_PRE, extractBracketedPaste);
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
