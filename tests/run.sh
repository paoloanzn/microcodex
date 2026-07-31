#!/bin/sh

set -u

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd) || exit 1
ROOT_DIR=$(CDPATH= cd "$script_dir/.." && pwd) || exit 1
TEST_DIR=$script_dir
APP=$ROOT_DIR/build/app
MOCK_SERVER=$ROOT_DIR/tests/mock-server.rb
RUBY=${RUBY:-ruby}

if ! command -v "$RUBY" >/dev/null 2>&1; then
    printf 'Ruby is required to run the test fixture: %s\n' "$RUBY" >&2
    exit 1
fi

printf '%s\n' "building microcodex"
(cd "$ROOT_DIR" && make all) || exit 1

TEST_WORKDIR=${MICROCODEX_TEST_WORKDIR:-}
own_workdir=0
if [ -z "$TEST_WORKDIR" ]; then
    tmpbase=${TMPDIR:-/tmp}
    TEST_WORKDIR=$(mktemp -d "$tmpbase/microcodex-tests.XXXXXX") || exit 1
    own_workdir=1
else
    mkdir -p "$TEST_WORKDIR" || exit 1
    TEST_WORKDIR=$(mktemp -d "$TEST_WORKDIR/run.XXXXXX") || exit 1
fi

TEST_BIN_DIR=$TEST_WORKDIR/bin
TEST_HOME=$TEST_WORKDIR/codex-home
TEST_PROJECT=$TEST_WORKDIR/project
mkdir -p "$TEST_BIN_DIR" "$TEST_HOME" "$TEST_PROJECT" || exit 1
ln -s "$APP" "$TEST_BIN_DIR/microcodex" || exit 1

tests_run=0
tests_failed=0
case_cwd=$ROOT_DIR
active_mock_pid=

cleanup() {
    status=$?
    if [ -n "$active_mock_pid" ]; then
        kill "$active_mock_pid" 2>/dev/null || true
        wait "$active_mock_pid" 2>/dev/null || true
    fi
    if [ "${MICROCODEX_TEST_KEEP:-0}" = "1" ] || [ "$own_workdir" -ne 1 ]; then
        printf 'kept test workdir: %s\n' "$TEST_WORKDIR"
    else
        rm -rf "$TEST_WORKDIR"
    fi
    exit "$status"
}
trap cleanup 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

print_indented_file() {
    file=$1
    while IFS= read -r line || [ -n "$line" ]; do
        printf '  %s\n' "$line"
    done < "$file"
}

report_difference() {
    label=$1
    expected=$2
    actual=$3
    printf '%s differs:\n' "$label"
    if command -v diff >/dev/null 2>&1; then
        diff -u "$expected" "$actual" || true
    else
        printf 'expected:\n'
        print_indented_file "$expected"
        printf 'actual:\n'
        print_indented_file "$actual"
    fi
}

# Expected stdout is read from stdin and expected stderr from fd 3. Keeping the
# streams separate catches accidental diagnostics on stdout and missing output
# that a merged transcript would hide.
expect_process() {
    desc=$1
    expected_status=$2
    shift 2
    tests_run=$((tests_run + 1))
    case_id=$(printf '%03d' "$tests_run")
    expected_stdout=$TEST_WORKDIR/expected-stdout.$case_id
    expected_stderr=$TEST_WORKDIR/expected-stderr.$case_id
    actual_stdout=$TEST_WORKDIR/actual-stdout.$case_id
    actual_stderr=$TEST_WORKDIR/actual-stderr.$case_id

    cat > "$expected_stdout"
    cat <&3 > "$expected_stderr"

    printf '# %s\n' "$desc"
    (cd "$case_cwd" && "$@") > "$actual_stdout" 2> "$actual_stderr"
    actual_status=$?

    failed=0
    if [ "$actual_status" -ne "$expected_status" ]; then
        printf 'exit status differs: expected %s, actual %s\n' "$expected_status" "$actual_status"
        failed=1
    fi
    if ! cmp -s "$expected_stdout" "$actual_stdout"; then
        report_difference stdout "$expected_stdout" "$actual_stdout"
        failed=1
    fi
    if ! cmp -s "$expected_stderr" "$actual_stderr"; then
        report_difference stderr "$expected_stderr" "$actual_stderr"
        failed=1
    fi

    if [ "$failed" -eq 0 ]; then
        printf 'ok %s - %s\n' "$case_id" "$desc"
    else
        tests_failed=$((tests_failed + 1))
        printf 'not ok %s - %s\n' "$case_id" "$desc"
    fi
}

write_test_credentials() {
    credentials_home=$1
    mkdir -p "$credentials_home" || return 1
    cat > "$credentials_home/auth.json" <<'EOF'
{
  "auth_mode": "chatgpt",
  "tokens": {
    "id_token": "test-id-token",
    "access_token": "test-access-token",
    "refresh_token": "test-refresh-token",
    "account_id": "test-account"
  }
}
EOF
    chmod 600 "$credentials_home/auth.json"
}

# Run the real CLI while a scenario-specific loopback server validates every
# request. Server failures override the CLI status and include the server log.
run_with_mock() {
    scenario=$1
    shift
    mock_number=$((tests_run + 1))
    mock_dir=$TEST_WORKDIR/mock-$mock_number
    port_file=$mock_dir/port
    mkdir -p "$mock_dir" || return 1
    "$RUBY" "$MOCK_SERVER" "$scenario" "$port_file" "$mock_dir" \
        > "$mock_dir/server.stdout" 2> "$mock_dir/server.stderr" &
    active_mock_pid=$!

    attempt=0
    while [ ! -s "$port_file" ]; do
        if ! kill -0 "$active_mock_pid" 2>/dev/null; then
            wait "$active_mock_pid" || true
            active_mock_pid=
            print_indented_file "$mock_dir/server.stderr" >&2
            return 98
        fi
        attempt=$((attempt + 1))
        if [ "$attempt" -ge 200 ]; then
            kill "$active_mock_pid" 2>/dev/null || true
            wait "$active_mock_pid" 2>/dev/null || true
            active_mock_pid=
            printf '%s\n' "mock server did not publish its port" >&2
            return 98
        fi
        sleep 0.01
    done

    endpoint=http://127.0.0.1:$(sed -n '1p' "$port_file")/responses
    MICROCODEX_API_ENDPOINT=$endpoint "$@"
    app_status=$?
    wait "$active_mock_pid"
    server_status=$?
    active_mock_pid=
    if [ "$server_status" -ne 0 ]; then
        print_indented_file "$mock_dir/server.stderr" >&2
        return 98
    fi
    return "$app_status"
}

if [ "$#" -eq 0 ]; then
    set -- "$TEST_DIR"/[0-9][0-9][0-9]-*.sh
fi

for test_file in "$@"; do
    if [ ! -f "$test_file" ]; then
        printf 'missing test file: %s\n' "$test_file" >&2
        exit 1
    fi
    . "$test_file"
done

printf '%s tests, %s failures\n' "$tests_run" "$tests_failed"
[ "$tests_failed" -eq 0 ]
