#!/bin/sh

launcher_root=$TEST_WORKDIR/launcher
launcher_releases=$TEST_WORKDIR/launcher-releases
launcher_command=$launcher_root/microcodex
launcher_binary=$launcher_root/microcodex-bin
mkdir -p "$launcher_root" "$launcher_releases" || exit 1
cp "$ROOT_DIR/launcher.sh" "$launcher_command" || exit 1
chmod 755 "$launcher_command" || exit 1

cat > "$launcher_binary" <<'EOF'
#!/bin/sh
printf 'installed binary: %s\n' "$*"
EOF
chmod 755 "$launcher_binary" || exit 1

launcher_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1"
    else
        shasum -a 256 "$1"
    fi
}

case "$(uname -s):$(uname -m)" in
    Darwin:arm64 | Darwin:aarch64)
        launcher_target=aarch64-apple-darwin
        launcher_platform='macOS Apple Silicon'
        ;;
    Darwin:x86_64 | Darwin:amd64)
        launcher_target=x86_64-apple-darwin
        launcher_platform='macOS Intel'
        ;;
    Linux:arm64 | Linux:aarch64)
        launcher_target=aarch64-unknown-linux-gnu
        launcher_platform='Linux arm64'
        ;;
    Linux:x86_64 | Linux:amd64)
        launcher_target=x86_64-unknown-linux-gnu
        launcher_platform='Linux x86_64'
        ;;
    *) printf 'unsupported launcher test platform\n' >&2; exit 1 ;;
esac
launcher_checksum=$launcher_releases/microcodex-$launcher_target.sha256

launcher_sha256 "$launcher_binary" > "$launcher_checksum" || exit 1
expect_process "T6.1: launcher starts an up-to-date installed binary" 0 \
    env MICROCODEX_FORCE_UPDATE_CHECK=1 \
    MICROCODEX_RELEASES_URL="file://$launcher_releases" \
    "$launcher_command" alpha <<'STDOUT' 3<<'STDERR'
installed binary: alpha
STDOUT
STDERR

printf '%064d  microcodex-%s\n' 0 "$launcher_target" > "$launcher_checksum"
expect_process "T6.2: launcher can skip an available update" 0 \
    sh -c 'printf "2\n" | env MICROCODEX_FORCE_UPDATE_CHECK=1 MICROCODEX_RELEASES_URL="file://$2" "$1" beta' \
    launcher-test "$launcher_command" "$launcher_releases" <<'STDOUT' 3<<'STDERR'
installed binary: beta
STDOUT

✨ Update available for MicroCodex.

  1) Update now
  2) Skip

Choice [1]:
STDERR

installer_releases=$TEST_WORKDIR/installer-releases
installer_stage=$TEST_WORKDIR/installer-stage
installer_bin=$TEST_WORKDIR/installed/bin
mkdir -p "$installer_releases" "$installer_stage" "$installer_bin" || exit 1
installer_binary=microcodex-$launcher_target
installer_archive=$installer_binary.tar.gz

cat > "$installer_stage/$installer_binary" <<'EOF'
#!/bin/sh
printf 'released binary: %s\n' "$*"
EOF
chmod 755 "$installer_stage/$installer_binary" || exit 1
launcher_sha256 "$installer_stage/$installer_binary" > "$installer_releases/$installer_binary.sha256" || exit 1
tar -C "$installer_stage" -czf "$installer_releases/$installer_archive" "$installer_binary" || exit 1
launcher_sha256 "$installer_releases/$installer_archive" > "$installer_releases/$installer_archive.sha256" || exit 1
cp "$ROOT_DIR/launcher.sh" "$installer_releases/launcher.sh" || exit 1
launcher_sha256 "$installer_releases/launcher.sh" > "$installer_releases/launcher.sh.sha256" || exit 1

expect_process "T6.3: installer creates the launcher and native binary layout" 0 \
    env HOME="$TEST_WORKDIR/installed-home" \
    PATH="$installer_bin:$PATH" \
    MICROCODEX_INSTALL_DIR="$installer_bin" \
    MICROCODEX_RELEASES_URL="file://$installer_releases" \
    "$ROOT_DIR/install.sh" <<STDOUT 3<<'STDERR'
==> Installing MicroCodex
==> Detected platform: $launcher_platform
==> Resolved release: latest
==> Downloading $installer_archive
==> Installing launcher to $installer_bin/microcodex
==> $installer_bin is already on PATH
MicroCodex installed successfully. Run: microcodex login
STDOUT
STDERR

expect_process "T6.4: installed launcher executes the separate native binary" 0 \
    env PATH="$installer_bin:$PATH" MICROCODEX_NO_UPDATE_CHECK=1 \
    sh -c 'cd / && microcodex delta' <<'STDOUT' 3<<'STDERR'
released binary: delta
STDOUT
STDERR

cat > "$launcher_releases/install.sh" <<'EOF'
#!/bin/sh
cat > "$MICROCODEX_INSTALL_DIR/microcodex-bin" <<'BINARY'
#!/bin/sh
printf 'updated binary: %s\n' "$*"
BINARY
chmod 755 "$MICROCODEX_INSTALL_DIR/microcodex-bin"
EOF
launcher_sha256 "$launcher_releases/install.sh" > "$launcher_releases/install.sh.sha256" || exit 1

expect_process "T6.5: launcher installs an accepted update before starting" 0 \
    sh -c 'printf "\n" | env MICROCODEX_FORCE_UPDATE_CHECK=1 MICROCODEX_RELEASES_URL="file://$2" "$1" gamma' \
    launcher-test "$launcher_command" "$launcher_releases" <<'STDOUT' 3<<'STDERR'
updated binary: gamma
STDOUT

✨ Update available for MicroCodex.

  1) Update now
  2) Skip

Choice [1]:
STDERR
