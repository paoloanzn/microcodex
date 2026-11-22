#!/bin/sh
# Checks the product's core promise: MicroCodex stays small on disk and in
# memory. It measures the stripped executable and the peak memory of a session
# against the mock server, and fails when a budget below is exceeded. With
# --base it also builds another commit and fails when a number grows more than
# the allowed percentage over it.
#
# Usage: sh tests/footprint.sh [--base REV] [--report FILE]
#
# `make footprint` builds build/microcodex and build/peak-rss and runs this
# script; `make footprint FOOTPRINT_BASE=origin/main` adds the comparison.

# --- Budgets: the single source of truth ------------------------------------
# Changing a budget is a product decision: change it here, in the pull request
# that needs it, and explain why there. CI measures the x86_64 Linux release
# build (Debian 12); other platforms are checked against the same numbers.
max_binary_bytes=1048576 # 1 MiB: stripped build/microcodex
max_peak_rss_kib=16384   # 16 MiB: peak RSS of one session against the mock server
max_growth_percent=5     # with --base: largest growth over that commit, per number
# ------------------------------------------------------------------------------

# Mock-server scenarios that make up a session, as "scenario|prompt". Each runs
# `runs` times and its largest peak counts.
scenarios='default-model|Say hello with the default model
tool-finish-order|Run a slow and a fast command'
runs=3

usage() {
    printf 'usage: sh tests/footprint.sh [--base REV] [--report FILE]\n' >&2
    exit 2
}

base=
report_file=
while [ "$#" -gt 0 ]; do
    case $1 in
        --base)
            [ "$#" -ge 2 ] || usage
            base=$2
            shift 2
            ;;
        --report)
            [ "$#" -ge 2 ] || usage
            report_file=$2
            shift 2
            ;;
        *) usage ;;
    esac
done

repo=$(cd "$(dirname "$0")/.." && pwd) || exit 1
binary=$repo/build/microcodex
peak_rss=$repo/build/peak-rss
for required in "$binary" "$peak_rss"; do
    if [ ! -x "$required" ]; then
        printf '%s is missing; run "make footprint"\n' "$required" >&2
        exit 1
    fi
done

# Settings from the caller's environment would change what is measured.
unset MICROCODEX_EFFORT MICROCODEX_COMPACT_AT_TOKENS MICROCODEX_RETAINED_CONTEXT_TOKENS \
    MICROCODEX_TOOL_EXECUTION_TIMEOUT_SECONDS MICROCODEX_API_ENDPOINT

work=$(mktemp -d "${TMPDIR:-/tmp}/microcodex-footprint.XXXXXX") || exit 1
base_tree=
# shellcheck disable=SC2317 # Called by the EXIT trap.
cleanup() {
    if [ -n "$base_tree" ]; then
        git -C "$repo" worktree remove --force "$base_tree" >/dev/null 2>&1
    fi
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# Prints the largest peak RSS, in KiB, of `runs` runs of BINARY in SCENARIO,
# served by MOCK_SERVER. Each run gets fresh credentials and a fresh project.
measure_peak_rss() {
    measured_binary=$1
    mock_server=$2
    scenario=$3
    prompt=$4
    largest=0
    run=1
    while [ "$run" -le "$runs" ]; do
        dir=$(mktemp -d "$work/run.XXXXXX") || return 1
        mkdir -p "$dir/codex" "$dir/project" || return 1
        printf '%s\n' '{"auth_mode":"chatgpt","tokens":{"id_token":"test-id-token","access_token":"test-access-token","refresh_token":"test-refresh-token","account_id":"test-account"}}' \
            > "$dir/codex/auth.json" || return 1
        chmod 600 "$dir/codex/auth.json"

        ruby "$mock_server" "$scenario" "$dir/port" "$dir" </dev/null >"$dir/mock.out" 2>"$dir/mock.err" &
        mock_pid=$!
        attempt=0
        while [ ! -s "$dir/port" ]; do
            attempt=$((attempt + 1))
            if [ "$attempt" -ge 500 ] || ! kill -0 "$mock_pid" 2>/dev/null; then
                kill "$mock_pid" 2>/dev/null
                printf 'mock server for %s did not start:\n' "$scenario" >&2
                cat "$dir/mock.err" >&2
                return 1
            fi
            sleep 0.01
        done

        (cd "$dir/project" &&
            HOME=$dir CODEX_HOME=$dir/codex SHELL=/bin/sh \
                MICROCODEX_API_ENDPOINT=http://127.0.0.1:$(sed -n '1p' "$dir/port")/responses \
                "$peak_rss" "$dir/peak" "$measured_binary" "$prompt") \
            </dev/null >"$dir/app.out" 2>"$dir/app.err"
        app_status=$?
        wait "$mock_pid"
        mock_status=$?
        if [ "$app_status" -ne 0 ] || [ "$mock_status" -ne 0 ]; then
            printf '%s failed in scenario %s (exit %s, mock server exit %s):\n' \
                "$measured_binary" "$scenario" "$app_status" "$mock_status" >&2
            cat "$dir/app.err" "$dir/mock.err" >&2
            return 1
        fi
        peak=$(cat "$dir/peak") || return 1
        [ "$peak" -gt "$largest" ] && largest=$peak
        run=$((run + 1))
    done
    printf '%s\n' "$largest"
}

# Builds REV in the worktree at base_tree and prints its executable's path.
# base_tree is set by the caller, because this runs in a subshell and cleanup()
# must see it.
build_base() {
    git -C "$repo" worktree add --quiet --detach "$base_tree" "$1" >&2 || return 1
    git -C "$base_tree" submodule --quiet update --init --recursive >&2 || return 1
    if ! make -C "$base_tree" build/microcodex >"$work/base-build.log" 2>&1; then
        cat "$work/base-build.log" >&2
        return 1
    fi
    printf '%s\n' "$base_tree/build/microcodex"
}

# The formats check() shows numbers in, called through its FORMAT argument.
# shellcheck disable=SC2317
kib() { awk -v value="$1" 'BEGIN { printf "%.1f KiB", value / 1024 }'; }
# shellcheck disable=SC2317
mib() { awk -v value="$1" 'BEGIN { printf "%.2f MiB", value / 1024 }'; }

failed=0
rows=
# Adds one report row and records a failure when CURRENT is over BUDGET or,
# when BASE is a number, more than max_growth_percent above it.
check() {
    label=$1
    format=$2
    budget=$3
    base_value=$4
    current=$5
    result="✅"
    if [ "$current" -gt "$budget" ]; then
        result="❌ over budget"
        failed=1
    fi
    base_shown=–
    change=–
    case $base_value in
        '' | *[!0-9]*) [ -n "$base_value" ] && base_shown=$base_value ;;
        *)
            base_shown=$($format "$base_value")
            change=$(awk -v now="$current" -v was="$base_value" \
                'BEGIN { change = (now - was) * 100 / was
                        if (change > -0.05 && change < 0.05) change = 0
                        printf "%+.1f%%", change }')
            if [ $((current * 100)) -gt $((base_value * (100 + max_growth_percent))) ]; then
                result="❌ grew more than ${max_growth_percent}%"
                failed=1
            fi
            ;;
    esac
    rows="$rows| $label | $($format "$budget") | $base_shown | $($format "$current") | $change | $result |
"
}

base_binary=
if [ -n "$base" ]; then
    base_commit=$(git -C "$repo" rev-parse --verify --quiet "$base^{commit}") || {
        printf 'unknown base revision: %s\n' "$base" >&2
        exit 1
    }
    printf 'Building base %s...\n' "$base" >&2
    base_tree=$work/base
    base_binary=$(build_base "$base_commit") || exit 1
fi

check "Executable (stripped)" kib "$max_binary_bytes" \
    "$([ -n "$base_binary" ] && wc -c <"$base_binary" | tr -d ' ')" \
    "$(wc -c <"$binary" | tr -d ' ')"

while IFS='|' read -r scenario prompt; do
    printf 'Measuring %s...\n' "$scenario" >&2
    current=$(measure_peak_rss "$binary" "$repo/tests/mock-server.rb" "$scenario" "$prompt") || exit 1
    base_value=
    if [ -n "$base_binary" ]; then
        # The base runs against its own mock server, which matches its requests.
        # A scenario it does not have yet is reported, not failed.
        if [ -f "$base_tree/tests/mock-server.rb" ] &&
            base_value=$(measure_peak_rss "$base_binary" "$base_tree/tests/mock-server.rb" \
                "$scenario" "$prompt" 2>/dev/null); then
            :
        else
            base_value="n/a"
        fi
    fi
    check "Peak RSS: $scenario" mib "$max_peak_rss_kib" "$base_value" "$current"
done <<EOF
$scenarios
EOF

head_commit=$(git -C "$repo" rev-parse --short HEAD 2>/dev/null || printf 'working tree')
if [ -n "$base" ]; then
    compared="\`$head_commit\` compared with base \`$(git -C "$repo" rev-parse --short "$base_commit")\`"
else
    compared="\`$head_commit\`"
fi
if [ "$failed" -eq 0 ]; then
    verdict="Within budget."
else
    verdict="**Footprint check failed.** Shrink the change, or raise the budget and explain why in the pull request."
fi

report=$(cat <<EOF
## Footprint

$compared on $(uname -sm). Peak RSS is the largest of $runs runs against the mock server.

| Measure | Budget | Base | This build | Change | Result |
|---|---|---|---|---|---|
$rows
$verdict Budgets and the ${max_growth_percent}% growth limit are set at the top of \`tests/footprint.sh\`.
EOF
)
printf '%s\n' "$report"
if [ -n "$report_file" ]; then
    printf '%s\n' "$report" >"$report_file" || exit 1
fi
exit "$failed"
