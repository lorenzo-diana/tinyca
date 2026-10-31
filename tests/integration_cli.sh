#!/bin/bash
# Integration test for the real build/tinyca binary (D6.5 gate): --help,
# --version, oneshot mode against a real mockllm, and the interactive REPL's
# slash commands over piped (non-tty) stdin.
set -u
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BIN="build/tinyca"
FAIL=0

if [ ! -x "$BIN" ]; then
    echo "FAIL: integration_cli ($BIN not built -- run 'make all')"
    exit 1
fi

check() {
    local desc="$1"
    local condition="$2"
    if [ "$condition" -ne 0 ]; then
        echo "FAIL: integration_cli: $desc"
        FAIL=1
    fi
}

# --- --help / --version ---
HELP_OUT=$("$BIN" --help 2>&1)
echo "$HELP_OUT" | grep -q "Interactive commands:" && h1=0 || h1=1
check "--help prints usage" "$h1"

VERSION_OUT=$("$BIN" --version 2>&1)
echo "$VERSION_OUT" | grep -q "tinyca" && v1=0 || v1=1
check "--version prints a version string" "$v1"

# --- oneshot mode against a real mockllm, streaming disabled (mockllm has no SSE) ---
mkdir -p build/tests
STREAM_OFF_INI="build/tests/cli_stream_off.ini"
printf '[llm]\nstream = false\n' > "$STREAM_OFF_INI"

PORT=18310
LOG="build/tests/cli_oneshot_reqs.jsonl"
build/mockllm --fixture tests/fixtures/well_formed_single_action.json --requests-log "$LOG" \
    --port "$PORT" --max-requests 2 &
MOCK_PID=$!
for _ in $(seq 1 30); do
    curl -sf "http://127.0.0.1:$PORT/health" > /dev/null 2>&1 && break
    sleep 0.1
done

ONESHOT_OUT=$("$BIN" -c "$STREAM_OFF_INI" -u "http://127.0.0.1:$PORT" -p "list the files" --no-confirm 2>&1)
wait "$MOCK_PID" 2>/dev/null
echo "$ONESHOT_OUT" | grep -q "Done." && o1=0 || o1=1
check "oneshot mode completes a real task against mockllm" "$o1"

# --- interactive REPL over piped stdin: /help, /skills, /quit ---
REPL_OUT=$(printf '/help\n/skills\n/quit\n' | "$BIN" -c "$STREAM_OFF_INI" 2>&1)
echo "$REPL_OUT" | grep -q "Interactive commands:" && r1=0 || r1=1
check "REPL /help works over piped stdin" "$r1"
echo "$REPL_OUT" | grep -q "fetch_url:" && r2=0 || r2=1
check "REPL /skills lists the real fetch_url skill" "$r2"

if [ "$FAIL" -eq 0 ]; then
    echo "PASS: integration_cli"
    exit 0
fi
exit 1
