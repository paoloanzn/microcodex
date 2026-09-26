#!/bin/sh

empty_home=$TEST_WORKDIR/empty-home
logout_home=$TEST_WORKDIR/logout-home
mkdir -p "$empty_home"

expect_process "T1.1: help describes every CLI mode" 0 \
    env PATH="$TEST_BIN_DIR:$PATH" microcodex --help <<'STDOUT' 3<<'STDERR'
Usage:
  microcodex login [--device-auth]
  microcodex logout
  microcodex list
  microcodex show ID
  microcodex [--model MODEL] [--effort EFFORT] resume ID [PROMPT]
  microcodex [--model MODEL] [--effort EFFORT]
  microcodex [--model MODEL] [--effort EFFORT] PROMPT
STDOUT
STDERR

expect_process "T1.2: --model requires a value" 1 \
    env PATH="$TEST_BIN_DIR:$PATH" microcodex --model <<'STDOUT' 3<<'STDERR'
Usage:
  microcodex login [--device-auth]
  microcodex logout
  microcodex list
  microcodex show ID
  microcodex [--model MODEL] [--effort EFFORT] resume ID [PROMPT]
  microcodex [--model MODEL] [--effort EFFORT]
  microcodex [--model MODEL] [--effort EFFORT] PROMPT
STDOUT
--model requires a model name
STDERR

expect_process "T1.3: a prompt requires login" 1 \
    env CODEX_HOME="$empty_home" PATH="$TEST_BIN_DIR:$PATH" microcodex hello <<'STDOUT' 3<<'STDERR'
STDOUT
Not logged in. Run 'microcodex login' first.
STDERR

expect_process "T1.4: --effort requires a value" 1 \
    env PATH="$TEST_BIN_DIR:$PATH" microcodex --effort <<'STDOUT' 3<<'STDERR'
Usage:
  microcodex login [--device-auth]
  microcodex logout
  microcodex list
  microcodex show ID
  microcodex [--model MODEL] [--effort EFFORT] resume ID [PROMPT]
  microcodex [--model MODEL] [--effort EFFORT]
  microcodex [--model MODEL] [--effort EFFORT] PROMPT
STDOUT
--effort requires a value
STDERR

expect_process "T1.5: --effort rejects invalid values" 1 \
    env PATH="$TEST_BIN_DIR:$PATH" microcodex --effort superhigh hello <<'STDOUT' 3<<'STDERR'
Usage:
  microcodex login [--device-auth]
  microcodex logout
  microcodex list
  microcodex show ID
  microcodex [--model MODEL] [--effort EFFORT] resume ID [PROMPT]
  microcodex [--model MODEL] [--effort EFFORT]
  microcodex [--model MODEL] [--effort EFFORT] PROMPT
STDOUT
--effort must be one of: minimal, low, medium, high
STDERR

expect_process "T1.6: --effort accepts a valid value" 1 \
    env CODEX_HOME="$empty_home" PATH="$TEST_BIN_DIR:$PATH" microcodex --effort high hello <<'STDOUT' 3<<'STDERR'
STDOUT
Not logged in. Run 'microcodex login' first.
STDERR

write_test_credentials "$logout_home" || exit 1
expect_process "T1.7: logout removes saved credentials" 0 \
    env CODEX_HOME="$logout_home" PATH="$TEST_BIN_DIR:$PATH" microcodex logout <<'STDOUT' 3<<'STDERR'
Logged out.
STDOUT
STDERR

expect_process "T1.8: repeated logout is idempotent" 0 \
    env CODEX_HOME="$logout_home" PATH="$TEST_BIN_DIR:$PATH" microcodex logout <<'STDOUT' 3<<'STDERR'
Already logged out.
STDOUT
STDERR
