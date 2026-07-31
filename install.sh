#!/bin/sh

set -eu

REPOSITORY="paoloanzn/microcodex"
RELEASE="${MICROCODEX_RELEASE:-latest}"
BIN_DIR="${MICROCODEX_INSTALL_DIR:-$HOME/.local/bin}"
BIN_PATH="$BIN_DIR/microcodex"
REAL_BIN_PATH="$BIN_DIR/microcodex-bin"
tmp_dir=""
staged_binary=""
staged_launcher=""
path_action="already"
path_profile=""

step() {
  printf '==> %s\n' "$1"
}

usage() {
  cat <<'EOF'
Usage: install.sh [--release TAG]

Install the latest MicroCodex release for this computer.

Options:
  --release TAG  Install a specific release tag, such as v0.1.0.
  -h, --help     Show this help.

Environment:
  MICROCODEX_RELEASE      Release tag to install (default: latest).
  MICROCODEX_INSTALL_DIR  Destination directory (default: ~/.local/bin).
EOF
}

parse_args() {
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --release)
        if [ "$#" -lt 2 ]; then
          echo "--release requires a tag." >&2
          exit 1
        fi
        RELEASE="$2"
        shift
        ;;
      -h | --help)
        usage
        exit 0
        ;;
      *)
        echo "Unknown argument: $1" >&2
        usage >&2
        exit 1
        ;;
    esac
    shift
  done
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "$1 is required to install MicroCodex." >&2
    exit 1
  fi
}

download_file() {
  url="$1"
  output="$2"

  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --retry 3 "$url" -o "$output"
    return
  fi

  if command -v wget >/dev/null 2>&1; then
    wget -q -O "$output" "$url"
    return
  fi

  echo "curl or wget is required to install MicroCodex." >&2
  exit 1
}

file_sha256() {
  path="$1"

  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print $1}'
    return
  fi

  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print $1}'
    return
  fi

  if command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$path" | sed 's/^.*= //'
    return
  fi

  echo "sha256sum, shasum, or openssl is required to verify MicroCodex." >&2
  exit 1
}

expected_digest() {
  awk 'NR == 1 && length($1) == 64 && $1 !~ /[^0-9a-fA-F]/ { print tolower($1) }' "$1"
}

pick_profile() {
  case "$os:${SHELL:-}" in
    darwin:*/zsh) printf '%s\n' "$HOME/.zprofile" ;;
    darwin:*/bash) printf '%s\n' "$HOME/.bash_profile" ;;
    linux:*/zsh) printf '%s\n' "$HOME/.zshrc" ;;
    linux:*/bash) printf '%s\n' "$HOME/.bashrc" ;;
    *) printf '%s\n' "$HOME/.profile" ;;
  esac
}

add_to_path() {
  case ":$PATH:" in
    *":$BIN_DIR:"*) return ;;
  esac

  profile="$(pick_profile)"
  path_profile="$profile"
  path_line="export PATH=\"$BIN_DIR:\$PATH\""

  if [ -f "$profile" ] && grep -F "$path_line" "$profile" >/dev/null 2>&1; then
    path_action="configured"
    return
  fi

  {
    printf '\n# >>> MicroCodex installer >>>\n'
    printf '%s\n' "$path_line"
    printf '# <<< MicroCodex installer <<<\n'
  } >> "$profile"
  path_action="added"
}

cleanup() {
  if [ -n "$tmp_dir" ] && [ -d "$tmp_dir" ]; then
    rm -rf "$tmp_dir"
  fi
  if [ -n "$staged_binary" ]; then
    rm -f "$staged_binary"
  fi
  if [ -n "$staged_launcher" ]; then
    rm -f "$staged_launcher"
  fi
}

parse_args "$@"

if [ "$RELEASE" != "latest" ] &&
  ! printf '%s\n' "$RELEASE" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$'; then
  echo "Invalid release tag: $RELEASE. Expected latest or a tag such as v1.2.3." >&2
  exit 1
fi

case "$(uname -s)" in
  Darwin) os="darwin" ;;
  Linux) os="linux" ;;
  *)
    echo "install.sh supports macOS and Linux." >&2
    exit 1
    ;;
esac

case "$(uname -m)" in
  x86_64 | amd64) arch="x86_64" ;;
  arm64 | aarch64) arch="aarch64" ;;
  *)
    echo "Unsupported architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

if [ "$os" = "darwin" ] && [ "$arch" = "aarch64" ]; then
  target="aarch64-apple-darwin"
  platform_label="macOS Apple Silicon"
elif [ "$os" = "darwin" ]; then
  target="x86_64-apple-darwin"
  platform_label="macOS Intel"
elif [ "$arch" = "aarch64" ]; then
  target="aarch64-unknown-linux-gnu"
  platform_label="Linux arm64"
else
  target="x86_64-unknown-linux-gnu"
  platform_label="Linux x86_64"
fi

archive="microcodex-${target}.tar.gz"
archive_checksum="${archive}.sha256"
binary="microcodex-${target}"
binary_checksum="${binary}.sha256"
launcher="launcher.sh"
launcher_checksum="${launcher}.sha256"

if [ -n "${MICROCODEX_RELEASES_URL:-}" ]; then
  base_url="$MICROCODEX_RELEASES_URL"
  release_label="$RELEASE"
elif [ "$RELEASE" = "latest" ]; then
  base_url="https://github.com/${REPOSITORY}/releases/latest/download"
  release_label="latest"
else
  base_url="https://github.com/${REPOSITORY}/releases/download/${RELEASE}"
  release_label="$RELEASE"
fi

require_command mktemp
require_command tar
require_command install

tmp_dir="$(mktemp -d)"
trap cleanup EXIT HUP INT TERM
archive_path="$tmp_dir/$archive"
archive_checksum_path="$tmp_dir/$archive_checksum"
binary_checksum_path="$tmp_dir/$binary_checksum"
launcher_path="$tmp_dir/$launcher"
launcher_checksum_path="$tmp_dir/$launcher_checksum"

step "Installing MicroCodex"
step "Detected platform: $platform_label"
step "Resolved release: $release_label"
step "Downloading $archive"
download_file "$base_url/$archive" "$archive_path"
download_file "$base_url/$archive_checksum" "$archive_checksum_path"
download_file "$base_url/$binary_checksum" "$binary_checksum_path"
download_file "$base_url/$launcher" "$launcher_path"
download_file "$base_url/$launcher_checksum" "$launcher_checksum_path"

expected_archive_digest="$(expected_digest "$archive_checksum_path")"
if [ -z "$expected_archive_digest" ]; then
  echo "The downloaded checksum file is invalid." >&2
  exit 1
fi

actual_archive_digest="$(file_sha256 "$archive_path")"
if [ "$actual_archive_digest" != "$expected_archive_digest" ]; then
  echo "Downloaded MicroCodex archive checksum did not match." >&2
  echo "expected: $expected_archive_digest" >&2
  echo "actual:   $actual_archive_digest" >&2
  exit 1
fi

expected_launcher_digest="$(expected_digest "$launcher_checksum_path")"
actual_launcher_digest="$(file_sha256 "$launcher_path")"
if [ -z "$expected_launcher_digest" ] || [ "$actual_launcher_digest" != "$expected_launcher_digest" ]; then
  echo "Downloaded MicroCodex launcher checksum did not match." >&2
  exit 1
fi

archive_entries="$(tar -tzf "$archive_path")"
if [ "$archive_entries" != "$binary" ]; then
  echo "The MicroCodex archive has an unexpected layout." >&2
  exit 1
fi

tar -xzf "$archive_path" -C "$tmp_dir"
if [ ! -f "$tmp_dir/$binary" ]; then
  echo "The MicroCodex binary was not found in the archive." >&2
  exit 1
fi

expected_binary_digest="$(expected_digest "$binary_checksum_path")"
actual_binary_digest="$(file_sha256 "$tmp_dir/$binary")"
if [ -z "$expected_binary_digest" ] || [ "$actual_binary_digest" != "$expected_binary_digest" ]; then
  echo "Extracted MicroCodex binary checksum did not match." >&2
  exit 1
fi

step "Installing launcher to $BIN_PATH"
mkdir -p "$BIN_DIR"
staged_binary="$BIN_DIR/.microcodex-bin.$$"
staged_launcher="$BIN_DIR/.microcodex-launcher.$$"
install -m 755 "$tmp_dir/$binary" "$staged_binary"
install -m 755 "$launcher_path" "$staged_launcher"
mv -f "$staged_binary" "$REAL_BIN_PATH"
staged_binary=""
mv -f "$staged_launcher" "$BIN_PATH"
staged_launcher=""

add_to_path
case "$path_action" in
  added)
    step "Added $BIN_DIR to PATH in $path_profile"
    step "Open a new terminal, or run: export PATH=\"$BIN_DIR:\$PATH\""
    ;;
  configured)
    step "$BIN_DIR is already configured in $path_profile"
    ;;
  *)
    step "$BIN_DIR is already on PATH"
    ;;
esac

printf 'MicroCodex installed successfully. Run: microcodex login\n'
