// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace microcodex::terminal {

    constexpr unsigned char paste_start_event = 4;
    constexpr unsigned char paste_end_event = 5;

    int enableBracketedPaste();
    void disableBracketedPaste();

} // namespace microcodex::terminal
