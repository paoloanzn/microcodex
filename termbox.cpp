// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

// termbox2 is a single-header library. Its implementation must be enabled in
// exactly one translation unit, and the header must precede system headers so
// its POSIX feature-test macros take effect.
#define TB_IMPL
#include <termbox2.h>
