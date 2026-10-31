#!/bin/bash
# Integration test for tools/mockllm (D2 gate): serves a fixture, records
# received requests, and the log proves history was sent append-only.
set -u
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BIN="build/mockllm"
LOG="build/tests/mockllm_requests.jsonl"
PORT=18099
FAIL=0

if [ ! -x "$BIN" ]; then
    echo "FAIL: integration_mockllm ($BIN not built -- run 'make mock')"
    exit 1
fi

mkdir -p build/tests
"$BIN" --fixture tests/fixtures/parallel_hermes.json --requests-log "$LOG" \
    --port "$PORT" --max-requests 2 &
PID=$!

for _ in $(seq 1 30); do
    curl -sf "http://127.0.0.1:$PORT/health" > /dev/null 2>&1 && break
    sleep 0.1
done

R1=$(curl -sf -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
     -d '{"messages":[{"role":"user","content":"do stuff"}]}')
R2=$(curl -sf -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
     -d '{"messages":[{"role":"user","content":"do stuff"},{"role":"assistant","tool_calls":[]},{"role":"tool","content":"ok"}]}')

wait "$PID"
EXIT_CODE=$?

echo "$R1" | grep -q '"tool_calls"' || { echo "FAIL: first response missing tool_calls"; FAIL=1; }
echo "$R1" | grep -q '"call_1"' || { echo "FAIL: first response missing call_1 id"; FAIL=1; }
echo "$R2" | grep -q "Both commands ran successfully" || { echo "FAIL: second response wrong content"; FAIL=1; }
[ "$EXIT_CODE" -eq 0 ] || { echo "FAIL: server did not exit 0 after --max-requests"; FAIL=1; }

[ -f "$LOG" ] || { echo "FAIL: requests log not written"; FAIL=1; }
LINES=$(wc -l < "$LOG")
[ "$LINES" -eq 2 ] || { echo "FAIL: expected 2 logged requests, got $LINES"; FAIL=1; }
# Second logged request's messages[] must be a strict extension of the first's
# (append-only history) -- 1 entry in request 1, 3 entries in request 2.
grep -c '"role":"user"' "$LOG" | head -1 > /dev/null
python3 - "$LOG" <<'PYEOF'
import json, sys
lines = [json.loads(l) for l in open(sys.argv[1])]
msgs1 = lines[0]["body"]["messages"]
msgs2 = lines[1]["body"]["messages"]
assert len(msgs1) == 1, "request 1 should have 1 message, got %d" % len(msgs1)
assert len(msgs2) == 3, "request 2 should have 3 messages, got %d" % len(msgs2)
assert msgs2[:1] == msgs1, "request 2's history does not start with request 1's (not append-only)"
print("append-only check: OK")
PYEOF
[ $? -eq 0 ] || FAIL=1

if [ "$FAIL" -eq 0 ]; then
    echo "PASS: integration_mockllm"
    exit 0
fi
exit 1
