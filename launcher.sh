#!/bin/sh

set -u

REPOSITORY="paoloanzn/microcodex"
RELEASES_BASE_URL="${MICROCODEX_RELEASES_URL:-https://github.com/${REPOSITORY}/releases/latest/download}"
case "$0" in
  */*) launcher_path="$0" ;;
  *) launcher_path="$(command -v "$0")" || exit 1 ;;
esac
script_dir="$(CDPATH= cd "$(dirname "$launcher_path")" && pwd)" || exit 1
REAL_BINARY="$script_dir/microcodex-bin"
tmp_dir=""

cleanup() {
  if [ -n "$tmp_dir" ] && [ -d "$tmp_dir" ]; then
    rm -rf "$tmp_dir"
  fi
}

file_sha256() {
  path="$1"

  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print $1}'
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$path" | sed 's/^.*= //'
  else
    return 1
  fi
}

download_file() {
  url="$1"
  output="$2"

  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --connect-timeout 2 --max-time 10 --retry 2 "$url" -o "$output"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -T 10 -O "$output" "$url"
  else
    return 1
  fi
}

expected_digest() {
  awk 'NR == 1 && length($1) == 64 && $1 !~ /[^0-9a-fA-F]/ { print tolower($1) }' "$1"
}

updates_disabled() {
  case "${MICROCODEX_NO_UPDATE_CHECK:-}" in
    1 | [Tt][Rr][Uu][Ee] | [Yy][Ee][Ss]) return 0 ;;
    *) return 1 ;;
  esac
}

select_target() {
  case "$(uname -s):$(uname -m)" in
    Darwin:arm64 | Darwin:aarch64) printf '%s\n' "aarch64-apple-darwin" ;;
    Darwin:x86_64 | Darwin:amd64) printf '%s\n' "x86_64-apple-darwin" ;;
    Linux:arm64 | Linux:aarch64) printf '%s\n' "aarch64-unknown-linux-gnu" ;;
    Linux:x86_64 | Linux:amd64) printf '%s\n' "x86_64-unknown-linux-gnu" ;;
    *) return 1 ;;
  esac
}

can_prompt() {
  if [ "${MICROCODEX_FORCE_UPDATE_CHECK:-}" = "1" ]; then
    return 0
  fi
  [ -t 0 ] && [ -t 2 ] && [ -r /dev/tty ] && [ -w /dev/tty ]
}

prompt_for_update() {
  if [ "${MICROCODEX_FORCE_UPDATE_CHECK:-}" = "1" ]; then
    {
      printf '\n✨ Update available for MicroCodex.\n\n'
      printf '  1) Update now\n'
      printf '  2) Skip\n\n'
      printf 'Choice [1]:'
    } >&2
    if ! IFS= read -r answer; then
      answer=2
    fi
    printf '\n' >&2
  else
    {
      printf '\n✨ Update available for MicroCodex.\n\n'
      printf '  1) Update now\n'
      printf '  2) Skip\n\n'
      printf 'Choice [1]:'
    } > /dev/tty
    if ! IFS= read -r answer < /dev/tty; then
      answer=2
    fi
    printf '\n' > /dev/tty
  fi

  case "$answer" in
    "" | 1 | y | Y | yes | YES) return 0 ;;
    *) return 1 ;;
  esac
}

run_update_check() {
  updates_disabled && return
  can_prompt || return
  [ -x "$REAL_BINARY" ] || return

  target="$(select_target)" || return
  binary="microcodex-${target}"
  checksum="${binary}.sha256"

  tmp_dir="$(mktemp -d 2>/dev/null)" || return
  trap cleanup EXIT HUP INT TERM
  latest_checksum="$tmp_dir/$checksum"

  download_file "$RELEASES_BASE_URL/$checksum" "$latest_checksum" >/dev/null 2>&1 || return
  latest_digest="$(expected_digest "$latest_checksum")"
  [ -n "$latest_digest" ] || return
  installed_digest="$(file_sha256 "$REAL_BINARY")" || return
  [ "$installed_digest" = "$latest_digest" ] && return

  prompt_for_update || return

  installer="$tmp_dir/install.sh"
  installer_checksum="$tmp_dir/install.sh.sha256"
  if ! download_file "$RELEASES_BASE_URL/install.sh" "$installer" >/dev/null 2>&1 ||
    ! download_file "$RELEASES_BASE_URL/install.sh.sha256" "$installer_checksum" >/dev/null 2>&1; then
    printf 'WARNING: Could not download the MicroCodex update; continuing with the installed version.\n' >&2
    return
  fi

  installer_expected="$(expected_digest "$installer_checksum")"
  installer_actual="$(file_sha256 "$installer")"
  if [ -z "$installer_expected" ] || [ "$installer_actual" != "$installer_expected" ]; then
    printf 'WARNING: The MicroCodex installer checksum did not match; update cancelled.\n' >&2
    return
  fi

  if ! MICROCODEX_INSTALL_DIR="$script_dir" MICROCODEX_NO_UPDATE_CHECK=1 sh "$installer"; then
    printf 'WARNING: MicroCodex could not be updated; continuing with the installed version.\n' >&2
  fi
}

if [ ! -x "$REAL_BINARY" ]; then
  printf 'MicroCodex binary is missing or not executable: %s\n' "$REAL_BINARY" >&2
  exit 1
fi

run_update_check
cleanup
trap - EXIT HUP INT TERM
exec "$REAL_BINARY" "$@"
