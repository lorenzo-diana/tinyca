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

# --- resume with a changed system prompt (SPEC.md §12.2) ---
# stdin is not a TTY here, so the default policy is "quit": the binary must
# exit 1 and leave the transcript byte-identical.
RESUME_LOG="build/tests/cli_resume.jsonl"
cat > "$RESUME_LOG" <<'JSONL'
{"ts":1,"kind":"user_task","payload":{"text":"hello"}}
{"ts":2,"kind":"request","payload":{"messages":[{"role":"system","content":"A PROMPT FROM BEFORE"},{"role":"user","content":"hello"}]}}
{"ts":3,"kind":"response","payload":{"content":"hi","role":"assistant"}}
JSONL
cp "$RESUME_LOG" "$RESUME_LOG.expected"

RESUME_OUT=$("$BIN" -c "$STREAM_OFF_INI" --resume "$RESUME_LOG" -p "continue" --no-confirm 2>&1 < /dev/null)
resume_rc=$?
[ "$resume_rc" -eq 1 ] && q1=0 || q1=1
check "a changed system prompt aborts an unattended resume (exit 1)" "$q1"
echo "$RESUME_OUT" | grep -q "resume aborted" && q2=0 || q2=1
check "the aborted resume explains itself" "$q2"
cmp -s "$RESUME_LOG" "$RESUME_LOG.expected" && q3=0 || q3=1
check "an aborted resume leaves the transcript untouched" "$q3"

# With --resume-prompt update it proceeds and records the adoption instead.
"$BIN" -c "$STREAM_OFF_INI" --resume "$RESUME_LOG" --resume-prompt update -u http://127.0.0.1:1 \
    -p "continue" --no-confirm > /dev/null 2>&1 < /dev/null
grep -q "system_prompt_update" "$RESUME_LOG" && u1=0 || u1=1
check "--resume-prompt update records the adopted prompt in the transcript" "$u1"

# --- resume in a different work_dir (SPEC.md §12.2) ---
# The transcript says the session ran in /nonexistent/elsewhere; this run uses
# ./workspace. Unattended, that must quit without touching the file.
WD_LOG="build/tests/cli_resume_wd.jsonl"
cat > "$WD_LOG" <<'JSONL'
{"ts":1,"kind":"session_start","payload":{"work_dir":"/nonexistent/elsewhere"}}
{"ts":2,"kind":"request","payload":{"messages":[{"role":"system","content":"SYS"},{"role":"user","content":"hello"}]}}
{"ts":3,"kind":"response","payload":{"content":"hi","role":"assistant"}}
JSONL
cp "$WD_LOG" "$WD_LOG.expected"

WD_OUT=$("$BIN" -c "$STREAM_OFF_INI" --resume "$WD_LOG" --resume-prompt update -p "continue" --no-confirm 2>&1 < /dev/null)
wd_rc=$?
[ "$wd_rc" -eq 1 ] && w1=0 || w1=1
check "a different work_dir aborts an unattended resume (exit 1)" "$w1"
echo "$WD_OUT" | grep -q "This session last ran with work_dir" && w2=0 || w2=1
check "the message names the session's previous work_dir" "$w2"
cmp -s "$WD_LOG" "$WD_LOG.expected" && w3=0 || w3=1
check "the aborted work_dir resume leaves the transcript untouched" "$w3"

# With --resume-work-dir continue it proceeds and records the move.
"$BIN" -c "$STREAM_OFF_INI" --resume "$WD_LOG" --resume-prompt update --resume-work-dir continue \
    -u http://127.0.0.1:1 -p "continue" --no-confirm > /dev/null 2>&1 < /dev/null
grep -q "work_dir_update" "$WD_LOG" && w4=0 || w4=1
check "--resume-work-dir continue records the move in the transcript" "$w4"
grep -q "session_start" "$WD_LOG" && w5=0 || w5=1
check "an accepted resume records the run's work_dir" "$w5"

# --- named sessions and rename (SPEC.md §12.3) ---
SESS_DIR="build/tests/cli_sessions"
rm -rf "$SESS_DIR"
SESS_INI="build/tests/cli_sessions.ini"
printf '[llm]\nstream = false\nbase_url = http://127.0.0.1:1\n[agent]\nsessions_dir = %s\n' "$SESS_DIR" \
    > "$SESS_INI"

# An unnamed run names itself YYYY-MM-DD_<work dir basename>.
UNNAMED_OUT=$("$BIN" -c "$SESS_INI" -p "x" --no-confirm 2>&1 < /dev/null)
echo "$UNNAMED_OUT" | grep -qE "new session: [0-9]{4}-[0-9]{2}-[0-9]{2}_" && s1=0 || s1=1
check "an unnamed session gets the YYYY-MM-DD_workdir default name" "$s1"

# A second unnamed run the same day must not append to the first one's file.
"$BIN" -c "$SESS_INI" -p "y" --no-confirm > /dev/null 2>&1 < /dev/null
[ "$(ls "$SESS_DIR" | wc -l)" -eq 2 ] && s2=0 || s2=1
check "a second unnamed session the same day gets its own file" "$s2"

# Named, then continued by name.
"$BIN" -c "$SESS_INI" --session cli-named -p "x" --no-confirm > /dev/null 2>&1 < /dev/null
CONT_OUT=$("$BIN" -c "$SESS_INI" --session cli-named -p "y" --no-confirm 2>&1 < /dev/null)
echo "$CONT_OUT" | grep -q "continuing session: cli-named" && s3=0 || s3=1
check "--session continues an existing session" "$s3"

LIST_OUT=$("$BIN" -c "$SESS_INI" --list-sessions 2>&1)
echo "$LIST_OUT" | grep -q "cli-named" && s4=0 || s4=1
check "--list-sessions lists a stored session" "$s4"

# Rename, including the two refusals.
"$BIN" -c "$SESS_INI" --session cli-named --rename-to cli-renamed > /dev/null 2>&1
[ -f "$SESS_DIR/cli-renamed.jsonl" ] && [ ! -f "$SESS_DIR/cli-named.jsonl" ] && s5=0 || s5=1
check "--rename-to moves the session file" "$s5"

"$BIN" -c "$SESS_INI" --session cli-renamed --rename-to "../escape" > /dev/null 2>&1
[ "$?" -eq 1 ] && s6=0 || s6=1
check "--rename-to refuses a name that is not usable as a filename" "$s6"

"$BIN" -c "$SESS_INI" --session cli-renamed -p "z" --no-confirm > /dev/null 2>&1 < /dev/null
"$BIN" -c "$SESS_INI" --session cli-renamed --rename-to cli-renamed > /dev/null 2>&1
[ "$?" -eq 1 ] && s7=0 || s7=1
check "--rename-to refuses renaming a session to its own name" "$s7"

# Resuming by bare name resolves inside sessions_dir.
RESUME_NAME_OUT=$("$BIN" -c "$SESS_INI" --resume cli-renamed --resume-prompt update --resume-work-dir continue \
    -p "w" --no-confirm 2>&1 < /dev/null)
echo "$RESUME_NAME_OUT" | grep -q "resumed .* message" && s8=0 || s8=1
check "--resume accepts a session name, not just a path" "$s8"

# --- a resume target that does not exist must fail, not invent a session ---
# Finding 2: this used to start a fresh session logging into a file named after
# the typo, in the current directory, invisible to --list-sessions.
TYPO_OUT=$("$BIN" -c "$SESS_INI" --resume definitely-not-a-session -p "x" --no-confirm 2>&1 < /dev/null)
typo_rc=$?
[ "$typo_rc" -eq 1 ] && t1=0 || t1=1
check "--resume with an unknown name exits 1" "$t1"
echo "$TYPO_OUT" | grep -q "no stored session or transcript file" && t2=0 || t2=1
check "--resume with an unknown name says so" "$t2"
[ ! -e "definitely-not-a-session" ] && t3=0 || t3=1
check "--resume with an unknown name creates no stray file" "$t3"

# Finding 3: --session alongside --resume is ignored, and now says so.
"$BIN" -c "$SESS_INI" --session cli-renamed -p "v" --no-confirm > /dev/null 2>&1 < /dev/null
BOTH_OUT=$("$BIN" -c "$SESS_INI" --session some-other-name --resume cli-renamed --resume-prompt update \
    --resume-work-dir continue -p "w" --no-confirm 2>&1 < /dev/null)
echo "$BOTH_OUT" | grep -q "warning: --session is ignored when --resume is given" && t4=0 || t4=1
check "--session with --resume warns that it is ignored" "$t4"

if [ "$FAIL" -eq 0 ]; then
    echo "PASS: integration_cli"
    exit 0
fi
exit 1

