# PROGRESS — TinyCA

## 2026-10-31T10:10Z — W0 preflight
- `g++` (Debian 12.2.0) and GNU Make 4.3 confirmed — Tier 1 toolchain OK.
- Sibling project findings **present** at `../llama.cpp_ubuntu_18/docs/FINDINGS.md`.
  Recording the four handoff answers here per PROMPT.md W0.3:
  1. **Chat template / tool calls**: `--jinja` applies the real Qwen2.5 ChatML
     template. With `tools[]` on `/v1/chat/completions`, tool calls come back as a
     **structured `tool_calls` array** (`content: null`, `function.name`,
     `function.arguments` JSON string), NOT raw Hermes `<tool_call>` text in
     `content`. TinyCA's Hermes protocol/parser must treat the structured array as
     the primary path, while still handling raw `<tool_call>` text defensively
     (SPEC.md §6.2 already allows for this).
  2. **GBNF grammar reachability**: `grammar` is honoured on `/completion` and
     silently ignored on `/v1/chat/completions`. SPEC.md §6.3 already encodes this:
     `enable_grammar=true` auto-overrides `api_path` to `/completion`.
  3. **Vendored dependency versions**: not applicable — the llama.cpp port vendors
     no httplib/json dependencies of its own (its CMake build needs none). TinyCA
     pins its own `httplib.h`/`json.hpp` independently (REQUIREMENTS.md B).
  4. **Working `llama-server` command line**:
     `llama-server -m <model>.gguf -ngl 99 -c 4096 --host 0.0.0.0 --port 8080 --jinja`
- `jetson.conf` does not exist for this project yet (TinyCA has no `scripts/jetson.sh`
  of its own — deployment in PROMPT.md W3/D7 uses plain `ssh`/`rsync` directly, not a
  wrapper). The Jetson board itself is currently powered off (user shut it down at
  the end of the Step 1 session). Per PROMPT.md W0.4, this is **not a blocker** for
  D0–D6 — only D7 needs hardware.
- `git init` done. `.gitignore`: `build/`, `logs/`, `workspace/`, `config/tinyca.ini`,
  `jetson.conf`, `never_commit/`, `*.gguf`.
- Starting D0.

## 2026-10-31T10:30Z — D0 GATE PASSED
- Full directory skeleton per SPEC.md §3.
- Vendored `third_party/json.hpp` (nlohmann/json v3.11.3) and `third_party/httplib.h`
  (cpp-httplib v0.14.3 — the last tag before the 0.15 bump to a C++17 requirement).
  Both smoke-compiled individually under `-std=c++11` on gcc 12 before committing to
  these versions. SHA-256 + rationale in `third_party/VERSIONS.md`.
- `LICENSE`: BSD-3-Clause.
- `Makefile`: all targets from SPEC.md §13 (`all`, `mock`, `test`,
  `check-portability`, `cross`, `cross-static`, `format`, `check-format`, `clean`,
  `install`), base flags `-std=c++11 -O2 -Wall -Wextra -pthread`.
- Gate verified for real: `make` builds `build/tinyca` from the stub `main()`
  (exit 0); `make check-portability` passes (compiles clean under
  `-pedantic-errors -Wall -Wextra -Wpedantic`, and the grep ban list for
  C++14/17-only constructs finds nothing, as expected on an empty file).
  `make format`/`make check-format` also verified end-to-end against the one real
  source file (caught an intentional formatting violation, then confirmed clean
  after `make format`).
- `cross`/`cross-static` targets are written but **not exercised** — this container
  has no root/sudo, so `g++-aarch64-linux-gnu` (REQUIREMENTS.md A.2, optional Tier 3)
  cannot be installed here. Matches PROMPT.md's own framing of this tier as optional
  and never-deployed; not a blocker. Revisit if a future environment has the
  cross-compiler available, or skip straight to Tier 4 (native Jetson build) at D7.
- `git init` already done at W0; committing this phase now.
- GATE PASSED.

## 2026-10-31T10:50Z — D1 GATE PASSED
- `string_util.h/.cpp`: `trim`, `split`, `to_lower`, `starts_with`.
- `ini_parser.h/.cpp`: `IniParser::parse_file` — `#`/`;` comments, `[section]`
  headers, trimmed `key = value`, missing file reported via `file_found` (not a
  parse failure). Keys outside any section land in the `""` section, so they
  naturally fail the "unknown key" check in `Config::load` without the parser
  needing to know what a valid section is.
- `config.h/.cpp`: `Config` struct with every field from SPEC.md §4 (all five
  sections, including the newer `command_filter_mode`/`blacklist`/
  `on_blocked_command`/`transcript_max_bytes`/`stream`/`color_output` keys) and
  their documented defaults. `load_config` applies CLI (via `getopt_long`) > file
  > default, via a `"section.key"` → setter lookup table so unknown keys are
  detected generically. Implements the §6.3 grammar validation chain in the
  correct order: fenced+grammar → warn & disable grammar *first*, so that doesn't
  spuriously trip the strip_think fatal check; hermes+grammar+strip_think=false →
  fatal; hermes+grammar (otherwise) → auto-override `api_path` to `/completion`.
- Tests (`tests/test_config.cpp`, `tests/test_util.h` minimal assert macros, no
  framework dependency): missing file, comments/sections/whitespace, missing key
  keeps default, unknown key warns without failing, malformed lines (no `=`,
  empty key) warn and skip without crashing, CLI > file > default precedence,
  `--no-confirm`/`--dry-run`/`-v`/`-p` flag behavior, and all three grammar
  validation branches. 10 checks, all passing.
- `tests/run_tests.sh`: generic — builds every `tests/test_*.cpp` against all of
  `src/*.cpp` except `main.cpp`, so it needs no edits as more source files land
  in later phases.
- Fixed a `Makefile` bug found while testing: the `mock` target failed outright
  with "no input files" since `tools/mockllm/*.cpp` doesn't exist until D2 —
  guarded it to skip gracefully when empty.
- `make test`, `make check-portability`, `make format`/`check-format` all pass.
- GATE PASSED.

## 2026-10-31T11:10Z — D2 GATE PASSED
- `tools/mockllm/main.cpp`: httplib server on `/v1/chat/completions`, `/completion`,
  and `/health`. Loads a JSON fixture (`{"scenario": ..., "responses": [...]}`),
  replays entries in order (clamped to the last entry if more requests arrive than
  fixture entries — robustness, not expected to trigger in well-formed tests).
  Each `responses[]` entry can set `content` (plain text) and/or `tool_calls` (an
  array) — the server builds the same `content: null` + structured `tool_calls`
  shape documented in `../llama.cpp_ubuntu_18/docs/FINDINGS.md`, so fixtures can
  faithfully reproduce what the real server does. `--max-requests N` makes the
  server self-exit after N requests, so test scripts can `wait` on it instead of
  needing to track/kill a PID. Every request's full JSON body is logged to
  `--requests-log`, one line per request.
- Fixed a Makefile bug the first build attempt surfaced: the mock binary's output
  path (`build/mockllm`) collided with the per-object-file subdirectory the old
  rule used (also `build/mockllm/`) — `ld` failed with "Is a directory". Renamed
  the object subdirectory to `build/mockllm_obj/`.
- `tests/fixtures/parallel_hermes.json`: the exact scenario from PROMPT.md's
  required-fixtures list (two parallel Hermes tool calls, then a plain-text
  completion), reused as-is for the upcoming D4 protocol-parser tests.
- `tests/integration_mockllm.sh`: starts the real server as a subprocess, hits it
  with two requests over real HTTP, and checks (a) the structured `tool_calls`
  response shape, (b) the second response's content, (c) the server exits 0 after
  `--max-requests`, (d) exactly 2 lines were logged, and (e) — the important one —
  that request 2's `messages[]` array is a strict superset starting with request
  1's, proving history is sent append-only over the wire, not just claimed to be.
  Wired into `tests/run_tests.sh` as a new `integration_*.sh` category (generic,
  so later phases can add more integration tests without editing the runner).
- `make test`, `make check-portability`, `make format`/`check-format` all pass.
- GATE PASSED.

## 2026-10-31T11:40Z — D3 GATE PASSED
- `message.h`: `ToolCall`/`ToolResult`/`Message` exactly as SPEC.md §5.
- `transport.h`: `ITransport` with `post`, a streaming `post_stream` (SPEC.md §9.3),
  and `cancel()` — added beyond the original SPEC interface sketch specifically for
  the always-interruptible control thread (§9.1): it needs a way to abort an
  in-flight HTTP call from a different thread, which the original two-method
  interface had no hook for.
- `http_transport.cpp` (`HttpTransport`): implements streaming by handing
  `httplib::Request::content_receiver` a raw-byte callback, fed into a hand-rolled
  `SseAccumulator` that splits on newlines, strips the `data: ` prefix, and drops
  the `[DONE]` sentinel before calling the caller's `on_chunk`. `cancel()` calls
  `httplib::Client::stop()` on whichever client is currently active, guarded by a
  mutex against the handoff race (client pointer set right before the blocking
  call, cleared right after).
  - **Verified live, not just by reading the httplib source**: wrote a throwaway
    Python SSE server, confirmed `post_stream` correctly reassembles 3 chunked
    `data:` events; then a second throwaway server that sleeps 10s before
    responding, confirmed `cancel()` called from another thread after 300ms makes
    `post_stream` return in ~300ms rather than waiting out the 60s read timeout —
    the exact behavior §9.1 depends on. Both scratch files deleted after.
- `codec.h`/`codec.cpp` (`OpenAIChatCodec`, the only codec — no Anthropic/hosted
  alternative, matching the local-only decision from the SPEC.md discussion):
  - `encode_request`: chat mode (`messages[]`) by default; when
    `options.use_native_completion` (set by `Config` when grammar is on, §6.3),
    switches to a flattened `prompt` string + `n_predict` + `grammar` instead.
    The prompt renderer applies the same ChatML tags `--jinja` uses
    (`<|im_start|>role\ncontent<|im_end|>`) — the one piece of model-specific
    knowledge the SPEC flagged as unavoidable here. An assistant history entry
    whose tool call arrived as a structured array (no text) is re-synthesized as
    Hermes `<tool_call>` text for this flattening, since grammar only ever pairs
    with the hermes protocol.
  - `decode_response`: detects three shapes — structured `tool_calls` array
    (`content: null`, per `../llama.cpp_ubuntu_18/docs/FINDINGS.md`), plain
    `content` string (both the chat and native-completion shapes collapse to the
    same handling), and `{"error": ...}`.
  - `decode_stream_chunk`: merges one SSE delta into a caller-held accumulator —
    content concatenation, and tool-call deltas merged by `index` (name/arguments
    both arrive fragmented across chunks and must be concatenated, not replaced).
- Tests (`tests/test_codec.cpp`): chat-mode encoding shape, native-completion
  encoding (prompt rendering + grammar field), tool-call history resynthesis,
  all three decode_response shapes (structured tool_calls, plain content, native
  completion, error, malformed JSON), streaming accumulation for both content and
  fragmented tool-call deltas, and an in-memory round-trip. 11 checks.
- `tests/test_transport_codec_integration.cpp`: the same round-trip but for real —
  launches `build/mockllm`, sends an actual `HttpTransport::post()` over the
  network, decodes the real response. Named to fit the existing `test_*.cpp` glob
  in `run_tests.sh` rather than adding a second integration-test convention.
- `make test`, `make check-portability`, `make format`/`check-format` all pass.
- GATE PASSED.


## 2026-10-31T12:10Z — D4 GATE PASSED
- `protocol.h`: `Action`, `IProtocol`. Added `kParseErrorToolName` sentinel beyond
  the original SPEC sketch — a fixed `Action` struct has no room for an error
  field, so a malformed tool call becomes a normal `Action` whose `tool_name` is
  this sentinel and `command` carries the human-readable error text; the
  Executor/Agent (D6) checks for it and synthesizes an error `ToolResult` without
  attempting to run anything. Implements SPEC.md §6.2's "do not abort, let the
  model retry" policy without changing the struct shape.
- `FencedProtocol`: single `bash` block per turn. Stop-sequence-consumed closing
  fence falls back to end-of-text (§6.1). Multiple blocks in one turn keep the
  first and set a non-fatal warning in `err` (return value still `true` —
  documented on `IProtocol::parse` that `err` can carry a warning on success,
  not just a failure message).
- `HermesProtocol`: primary path is the structured `tool_calls` array
  (`docs/FINDINGS.md`); defensive fallback parses raw `<tool_call>...</tool_call>`
  text for a model/server that doesn't emit the structured form. Both paths route
  every failure mode (unknown tool name, malformed arguments JSON, unterminated
  block) through the same `kParseErrorToolName` sentinel rather than aborting.
  `augment_request` sends the one "bash" tool schema in `tools[]`, and — when
  constructed with non-empty grammar text — switches to native `/completion` via
  `RequestOptions::use_native_completion`/`grammar`, per §6.3.
- Tests (`tests/test_protocol.cpp`, 16 checks): covers every parser-relevant
  scenario from PROMPT.md's required-fixtures list — well-formed single action,
  unterminated block (both the fenced stop-sequence case and Hermes's literal
  unterminated-tag case), no-action/task-complete, two parallel Hermes calls,
  and a tool call naming a nonexistent tool (both structured and text-form).
  The three remaining required scenarios (same failing command 3x, 5MB tool
  output, a command containing `;`/`&&`) are Executor/Agent-loop concerns, not
  parser concerns — deferred to D6, where they'll actually exercise something.
- Added the missing fixture files for the scenarios that didn't already have one
  from D2 (`well_formed_single_action.json`, `malformed_unterminated_block.json`,
  `no_action_task_complete.json`, `think_block_preceding_action.json`,
  `repeated_failing_command.json`, `metacharacter_command.json`,
  `nonexistent_tool.json`) so D6's end-to-end loop tests can reuse them directly
  instead of re-deriving the scripted responses.
- Rewrote `tests/run_tests.sh`: it was recompiling every library source file from
  scratch for *each* test binary (O(n·m)), which was already visibly slow at 4
  test binaries and would only get worse through D5/D6. Library `.cpp` files now
  compile to cached `.o` once (rebuilt only if newer than the cached object) and
  get relinked per test binary. Cold run unaffected; warm run dropped from ~45s
  to ~12s locally — this matters more each phase from here on.
- `make test`, `make check-portability`, `make format`/`check-format` all pass.
- GATE PASSED.

## 2026-10-31T12:30Z — D5 GATE PASSED
- `skills.h`/`skills.cpp` (`SkillRegistry`): discovery via POSIX `opendir`/`stat`
  (no new dependency), scanning for executable `*.sh` files and parsing the
  `@name`/`@description`/`@usage` header (first 20 lines, marker text matched
  anywhere on the line so comment-prefix style doesn't matter). A file missing
  any required field, or lacking the executable bit, is skipped with a warning
  — never fatal, matching PROMPT.md's "a missing key is never fatal" style rule
  extended to skills. Discovery order is sorted filename order, so Tier 1
  rendering is deterministic.
- `render_tier1` produces SPEC.md §7's exact block format; verified byte-for-byte
  against the real `skills/fetch_url.sh` header, not just a hand-written example.
- `load_tier2` returns the full script body for Tier 2 (on-demand) loading;
  `find_show_skill_trigger` matches the model's `show_skill: <name>` line exactly
  as specified — anywhere in the text, but only when it's the whole (trimmed)
  line, not a substring of other content.
- `paths()` exposes every discovered skill's path so D6's command filter can
  auto-allow them regardless of `command_filter_mode` (SPEC.md §7's last
  paragraph) — the actual whitelist/blacklist wiring happens in D6, this just
  provides the list.
- `skills/fetch_url.sh`: committed verbatim from SPEC.md §10, executable bit set.
- Test fixtures (`tests/fixtures/skills_test/`): a well-formed pair of skills, one
  missing `@usage`, one well-formed but not executable, and one non-`.sh` file —
  covering every discovery failure mode in one directory, committed rather than
  generated at test time so the exec-bit distinction survives git normally.
- Tests (`tests/test_skills.cpp`, 9 cases): discovery against the real `skills/`
  dir, exact Tier 1 formatting, every skip reason (missing `@usage`, not
  executable, wrong extension), a missing `skills_dir` not being fatal, the
  `max_tier1_skills` cap, Tier 2 load for known/unknown names, `paths()`, and the
  `show_skill:` trigger (present and absent). Two of my own test assertions were
  wrong on the first run, not the implementation: the expected Tier-1 string used
  SPEC §7's illustrative description text instead of §10's actual (slightly
  longer) one, and the Tier-1-cap check counted literal `-` characters, which
  also matched the hyphen inside "well-formed" in a fixture's own description.
  Fixed both in the test, not the code.
- `make test`, `make check-portability`, `make format`/`check-format` all pass.
- GATE PASSED.

## 2026-10-31T13:15Z — D6 GATE PASSED
The largest phase — Executor, CommandFilter, Logger, ControlThread, and the Agent
loop itself, all wired together and proven end-to-end against a real `mockllm`
subprocess, not just unit-tested in isolation.

- `command_filter.h`/`.cpp` (`CommandFilter`, SPEC.md §8.1): metacharacter
  rejection runs first, unconditionally, in every mode. One subtlety the literal
  SPEC wording doesn't resolve on its own: it bans a bare `&` but also lists
  `&&` as a valid segment separator — a naive "contains '&'" check would reject
  legitimate `&&` usage. Resolved by stripping every `&&` pair before checking
  for a remaining lone `&`. `whitelist`/`blacklist`/`none` modes implemented per
  spec; skill paths (once D5 hands them over) are auto-allowed regardless of
  mode, normalized the same way a head token is (`./x` and `x` both match).
  **Verified the classic bypass is actually caught**: `ls; rm -rf /` correctly
  rejected on the `rm` segment, not waved through because `ls` alone is fine.
- `executor.h`/`.cpp` (`Executor`, SPEC.md §8): `fork`+`execl("/bin/bash","-c",...)`,
  merged stdout/stderr via a pipe read with `poll()` against a deadline,
  `truncate_output` (head/tail lines + hard byte cap) as a free function so it's
  directly testable. `interrupt()` is the control thread's hook, distinct from
  the timeout path but sharing the same process-group `SIGKILL`.
  **Verified for real**, not assumed from reading the code: a command that
  backgrounds a long `sleep` and then itself sleeps past the timeout — confirmed
  via `pgrep` afterward that the backgrounded grandchild is actually dead, not
  just the direct child (a `kill(child_pid)` instead of `kill(-pgid)` would have
  left it running). Also verified the 5MB-output fixture scenario doesn't hang
  or balloon memory, and that `interrupt()` stops a 60s sleep in ~300ms.
- `logger.h`/`.cpp` (`Logger`): JSONL transcript with `transcript_max_bytes`
  cap (0 = unlimited) — once exceeded, one cutoff line is written and all
  further entries are dropped, verified by actually counting lines in the
  output file, not just reading the cutoff logic. Separate leveled console
  logger (`error`/`warn`/`info`/`debug`) for `log_level`.
- `control.h`/`.cpp` (`ControlThread`, SPEC.md §9.1 — the always-interruptible
  design from the original spec discussion): `start()` blocks `SIGINT`/`SIGTERM`
  in the *calling* thread before spawning its own, so every subsequently-created
  thread inherits the same mask (POSIX semantics) and only the control thread's
  own `sigwait()` loop ever receives them. On receipt: sets `stop_requested()`,
  calls `executor->interrupt()` and `transport->cancel()`. `stop()` wakes its own
  `sigwait()` via `pthread_kill(..., SIGTERM)` for a clean join.
  **Verified with a real signal, not a mocked one**: sent an actual `SIGINT` via
  `kill(getpid(), SIGINT)` from a second thread while the main thread was blocked
  in `executor.run("sleep 60")`, confirmed it returned in ~300ms with
  `killed_by_control = true` — proof the signal-to-thread-to-interrupt chain
  actually works, not just that each piece compiles.
- `agent.h`/`.cpp` (`Agent`): the loop from SPEC.md §9, plus:
  - Context budget check (bytes/4 estimate) — warns past 80%, aborts outright
    past 100%, never auto-summarizes (rewriting history would invalidate the
    KV-cache prefix, costing minutes on this hardware).
  - Repetition detection: hashes (here, just string-joins) each turn's full set
    of action commands and compares to the previous turn's; `repetition_threshold`
    consecutive identical turns aborts. Works for Hermes's multi-action turns too
    (compares the whole set, not just one command).
  - `run_actions`: for each `Action`, checks the `kParseErrorToolName` sentinel
    first (D4's malformed-tool-call escape hatch) before ever touching the
    filter; then `CommandFilter::check`; then `on_blocked_command`
    (`deny`→immediate rejection message, `ask`→`confirm_callback_`, defaulting to
    **deny** if no callback is injected — the safe choice for headless/test use);
    then, for an already-allowed command, `confirm_mode=always` gets the same
    callback treatment. Only after all of that does `Executor::run` actually
    fire.
  - Streaming (`run_streaming_request`): a small `ThinkStreamSplitter` taps the
    raw delta stream to route `<think>`/non-`<think>` text to separate UI
    callbacks live, buffering across chunk boundaries so a tag split mid-stream
    (e.g. `<thi` + `nk>`) isn't missed — independent of `Message::content`'s own
    accumulation, which always gets the complete raw text regardless. The
    *history*-facing `<think>` extraction (`extract_think`) only ever runs on
    the fully-assembled message, once, after the stream (or the blocking call)
    completes — matching SPEC.md §9 exactly ("before parsing actions"), not
    something attempted incrementally.
  - `api_path_for` relies on `Config::load_config` having already resolved
    `llm_api_path` to `/completion` when grammar is on (D1) as the single
    source of truth for which endpoint to hit; `RequestOptions::use_native_completion`
    (set independently by `IProtocol::augment_request`, derived from the same
    `enable_grammar` flag) only controls the codec's request-body shape. Two
    code paths, same underlying config flag, kept consistent by construction
    rather than by cross-checking at runtime.
- Tests: `test_command_filter.cpp` (13 cases), `test_executor.cpp` (9, including
  the process-group and 5MB scenarios), `test_logger.cpp` (4), `test_control.cpp`
  (3, including the real-signal test), and — the actual gate —
  `test_agent.cpp`'s 5 end-to-end cases against a real `mockllm` subprocess:
  - Full happy-path loop (fenced): command runs, result feeds back, model says
    "Done.", history is exactly the 4 append-only entries SPEC.md §9 implies.
  - `ls; rm -rf /` (via the `metacharacter_command` fixture from D4): rejected,
    the tool-result message fed back to history names the offending token.
  - 5MB output: exercised directly through `Executor` (fixtures can't
    parameterize arbitrary byte counts), confirms the same truncation proven in
    the Executor-level test also applies on the path history actually sees.
  - Three identical failing commands (`repeated_failing_command` fixture):
    aborts on the 2nd consecutive repeat (the default `repetition_threshold=2`),
    with the fixture's 3rd scripted response never even needed — confirms the
    detector doesn't wait for a 3rd before tripping.
  - Hermes two-parallel-calls, full round trip: both commands run, both results
    correlate back by `tool_call_id`, model's closing text comes through as the
    final result.
- `make test`, `make check-portability`, `make format`/`check-format` all pass.
- GATE PASSED.

## 2026-10-31T13:45Z — D6.5 GATE PASSED
Not an official PROMPT.md phase letter, but a real gate: the binary actually runs
end-to-end, CLI included.

- `main.cpp`: argument/config loading (delegates to D1's `load_config`), builds the
  system prompt (file content + Tier 1 skills block, D5), reads the grammar file
  when `enable_grammar=true` (fatal if missing — the user explicitly asked for it),
  constructs `FencedProtocol` or `HermesProtocol` by `tool_protocol`, wires every
  component into one `Agent`, and runs either oneshot (`-p`) or the interactive REPL.
- **Fixed a real gap found while smoke-testing, not by inspection**: `--dry-run` was
  parsed into `Config` back in D1 but nothing ever read `cli_dry_run` — D6's
  `run_actions` would have executed commands anyway. Added the check in
  `Agent::run_actions`, placed *before* the command filter so a dry run never
  trips `on_blocked_command`/`confirm_mode` prompts either — SPEC.md §12 says
  "parse and print actions, execute nothing," not "execute nothing unless the
  filter would have blocked it anyway."
- `LineEditor` (§12.1): raw `termios` mode (no readline/libedit — not an approved
  dependency), TAB-completes `/`-prefixed slash commands and, once a line starts
  with `show_skill:`, real discovered skill names. Falls back to plain
  `std::getline` when stdin isn't a TTY (also how `isatty(STDOUT_FILENO)` drives
  `color_output=auto`, §15).
- Terminal color (§15): `Colors` struct, four styles (user input bold cyan,
  `<think>` dim grey gated by `/toggle_show_thinking`, assistant reply green,
  action bold yellow — the action color isn't wired to a print site yet, since
  D6's `Agent` doesn't currently emit an explicit "about to run X" event the CLI
  can hook; deferred, noted below).
- Streaming UI hooks (`on_assistant_text_chunk`/`on_think_chunk` from D6) are wired
  to print live, colored, as chunks arrive.
- `/reset`, `/skills`, `/history`, `/toggle_show_thinking`, `/help`, `/quit` all
  implemented and manually verified; `confirm_prompt` is a plain blocking
  stdin y/n read (only ever invoked between tasks' network/exec calls, never
  racing the raw-mode `LineEditor`, which restores cooked mode before returning).
- **Verified for real against the actual compiled binary**, not just via unit
  tests calling `Agent` directly:
  - `./build/tinyca --help` / `--version`.
  - Oneshot mode (`-p ... --no-confirm`) against a real `mockllm` subprocess:
    ran the scripted `ls -la`, printed "Done." — the full stack, CLI included.
  - Interactive REPL over piped (non-TTY) stdin: `/help`, `/skills` (lists the
    real `fetch_url` skill), a real task, `/quit` — all work.
  - Discovered along the way: `mockllm` doesn't speak SSE (D2 never needed to),
    so `llm.stream=true` (the default) against it fails with "no content
    received from stream" — not a bug, just a real server requirement `mockllm`
    was never built to satisfy. `test_agent.cpp` already set `stream=false` for
    exactly this reason; added the same override to the new
    `tests/integration_cli.sh`.
- `tests/integration_cli.sh`: drives the real `build/tinyca` binary as a
  subprocess (not a unit test against `Agent` directly) — `--help`, `--version`,
  a full oneshot task against `mockllm`, and the REPL slash commands over piped
  stdin.
- Added `test_dry_run_never_executes_even_a_blocked_command` to
  `tests/test_agent.cpp`: feeds the `metacharacter_command` fixture (`ls; rm -rf
  /`) with `on_blocked_command=deny` *and* `cli_dry_run=true`, confirms the
  history shows "[dry-run] would execute: ... rm -rf /" with no "blocked" text
  anywhere — proof the filter was never even consulted, not just that execution
  was suppressed after the fact.
- Known gap, not blocking: the bold-yellow "requested action" color (§15) has no
  live call site yet, since `Agent` doesn't expose an explicit pre-execution hook
  the way it does for streamed text. Cosmetic only — functionally the action
  still runs and its result is shown; revisit if this matters before D8's README
  is written, otherwise document as a known simplification there.
- `make test`, `make check-portability`, `make format`/`check-format` all pass.
- GATE PASSED.

## 2026-10-31T14:05Z — D7 GATE PASSED
Board was reachable (powered back on since the earlier shutdown this session), fan
confirmed running (PWM=200, nonzero) and uptime continuous — no thermal risk, no
`start.sh` needed.

- `rsync -av --exclude build --exclude .git ...` source (never binaries) to
  `~/TinyCA` on the board.
- **Native build with the real target compiler (gcc 8.4.0), not just gcc 12 locally**:
  clean, zero warnings. Then `make test` **natively on the Jetson** — this is the
  authoritative portability check PROMPT.md's Tier 2 (`check-portability` on gcc 12)
  can only approximate.
  - One real test failure surfaced here, invisible on the x86 dev host: a
    pre-existing unrelated `sleep 300` process already running on this machine made
    `test_executor`'s `pgrep -f '[s]leep 30'` check false-positive, since unanchored
    regex matching finds "sleep 30" as a substring of "sleep 300" too. Not an
    Executor bug — fixed by anchoring the pattern (`'[s]leep 30$'`). Verified clean on
    both x86 and the Jetson afterward.
- Started the real `llama-server -ngl 99 -c 4096 --host 0.0.0.0 --port 8080 --jinja`
  from Step 1's build, waited for `/health`.
- **First real on-device run exposed exactly the format drift PROMPT.md predicted**:
  with no system prompt (`config/system_prompt.txt` didn't exist yet — that's
  nominally a D8 deliverable, written now because D7 genuinely needs it to produce
  real actions, not narration), the model just explained how to run `ls` in prose
  instead of emitting a fenced block. Fixed by writing a real `system_prompt.txt`
  (SPEC.md §7's Tier 1 skills block gets appended to this) that states the fenced
  format as a hard requirement and explicitly forbids narrating. Per PROMPT.md's own
  instruction: fixed by tightening the prompt, not by loosening `FencedProtocol`.
- **Two more real bugs found by actually running the binary**, not by inspection:
  1. With `llm.stream=true` (the default) against the real server, the CLI printed
     the final assistant text **twice** — once live via the streaming callback, once
     again from the accumulated return value. `Agent` had no way to tell the CLI
     "this was already shown live." Added `Agent::last_result_was_streamed()`,
     set only on normal (non-abort) completion when streaming was on; `main.cpp`'s
     `print_final_result` now skips the redundant print when it's true, always
     printed for abort/control messages (which are never streamed).
  2. Multiple agent-loop iterations' streamed text ran together with no separator —
     visible once the repetition detector's abort message got jammed directly onto
     the end of the second identical fenced block with no newline. Fixed in `Agent`
     itself (not just the CLI): emits a single `"\n"` through
     `on_assistant_text_chunk_` at the start of every iteration after the first, when
     streaming — the right layer, since any future UI consuming these hooks would
     have hit the same problem.
  - Both fixes rebuilt, retested (all green on x86), reformatted, redeployed, and
    reverified on the board before the final successful run below.
- **Final successful run** (`-p "List the files in the workspace and report how many
  there are" --no-confirm`): model emitted one real fenced action
  (` ```bash\nls -l | wc -l` `), Executor ran it for real, result fed back
  (`"1\n\n[exit code: 0]"`), model then replied in plain text with no further
  action — exactly PROMPT.md's D7 acceptance: at least one real bash action, its
  real output received, termination on a no-action turn inside `max_iterations`.
- `docs/first_run_transcript.jsonl`: the six JSONL lines from that exact successful
  run (user_task → request → response-with-action → tool_result → request →
  final response-with-no-action), extracted from the board's full transcript file
  (which also still has the earlier narration-only and empty-response attempts in
  it, left in place on the board itself as the honest unedited record — only the
  clean final run is what's committed here, per PROMPT.md's instruction to commit
  *the* transcript as evidence, not a debugging log).
- `llama-server` stopped afterward (idle load matters while unattended); smoke-test
  scratch files cleaned up from the board.
- GATE PASSED.

## 2026-10-31T14:25Z — D8 GATE PASSED — PROJECT COMPLETE
- `README.md` (91 lines, under the 150-line cap): build, configure, run, extend,
  known capability ceiling, pointers to SPEC.md/PROGRESS.md for the rest.
- `config/tinyca.ini.example`: every key from SPEC.md §4, all five sections,
  fully commented (not just a bare key=value list) — each comment explains the
  *why*, not just restates the key name. Verified it actually parses clean
  against the real `load_config` with zero "unknown key"/"invalid value"
  warnings, not just eyeballed for typos.
- `make test`, `make check-portability`, `make format`/`check-format` all still
  pass (no code changes this phase, docs/config only).
- GATE PASSED.

## PROJECT COMPLETE — final report (PROMPT.md W7)

| Phase | Outcome |
|---|---|
| D0 | Repo skeleton, Makefile (all SPEC §13 targets), vendored `json.hpp` 3.11.3 + `httplib.h` 0.14.3, BSD-3-Clause LICENSE |
| D1 | INI parser + `Config` (every SPEC §4 key), CLI>file>default precedence, §6.3 grammar validation chain |
| D2 | `mockllm` fixture server — structured `tool_calls` + plain content, `/completion`, request logging, append-only history proven over real HTTP |
| D3 | `Message`, `ITransport`/`HttpTransport` (SSE streaming + `cancel()`, both verified against real throwaway servers), `OpenAIChatCodec` |
| D4 | `IProtocol`/`FencedProtocol`/`HermesProtocol`, `kParseErrorToolName` sentinel for non-aborting malformed-call recovery |
| D5 | `SkillRegistry` (two-tier), `skills/fetch_url.sh` |
| D6 | `CommandFilter` (whitelist/blacklist/none, classic `ls; rm -rf /` bypass verified caught), `Executor` (process-group kill verified against a real backgrounded grandchild), `Logger`, `ControlThread` (verified with a real `SIGINT`, not a mock), `Agent` loop — full end-to-end gate against real `mockllm` |
| D6.5 | CLI entry point, REPL, TAB completion, terminal colour — found and fixed a real gap (`--dry-run` parsed but never wired to anything) |
| D7 | Deployed to the Jetson, native gcc-8.4 build + full test suite both clean, real `llama-server` run — found and fixed three more real bugs only visible on real hardware (see below) |
| D8 | README, fully-commented config example, this report |

**Vendored dependency versions**: `json.hpp` v3.11.3 (nlohmann/json),
`httplib.h` v0.14.3 (cpp-httplib — the last tag before the 0.15 bump to a C++17
requirement). Both smoke-compiled individually under `-std=c++11` before
committing to them; both also just compiled clean natively under gcc 8.4.0 on
the Jetson at D7, which is the real confirmation that mattered.

**`TODO(findings)` assumptions still unresolved**: none. PROMPT.md's W0.3
fallback assumptions (grammar unreachable on `/v1/chat/completions`, Hermes
tool calls assumed raw-text) were never needed — `../llama.cpp_ubuntu_18`'s
`docs/FINDINGS.md` was present from the start of this session, with concrete
evidence for both questions, and both were built into `Config`/`HermesProtocol`
directly rather than deferred.

**On-device benchmark** (from the real D7 run, `docs/first_run_transcript.jsonl`):
2 turns for the task ("list files" → one real `ls -l | wc -l` action → final
answer). Per-turn wall-clock: 3.14s (turn 1, action-producing) and 3.51s (turn
2, final answer) — 6.66s total task wall-clock, both short replies. Underlying
hardware baseline from `../llama.cpp_ubuntu_18`'s Step 1 `llama-bench`: ~62
tok/s prompt processing, ~4.25 tok/s generation — consistent with these
per-turn numbers given how short both replies were. The generous
`request_timeout_s=900` default is sized for a worst case far longer than
anything observed here.

**Top three observed model failure modes and mitigations** (all from the real
D7 run, not hypothetical):
1. **Narration instead of action** — with no system prompt loaded, the model
   explained in prose how one would run `ls` rather than emitting a fenced
   block at all. *Mitigation*: wrote `config/system_prompt.txt` stating the
   fenced format as the model's entire required reply and explicitly
   forbidding narration — per PROMPT.md's own instruction, fixed by tightening
   the prompt, not by loosening `FencedProtocol`'s parser.
2. **Identical command repeated turn over turn** — observed mid-session (2
   consecutive identical empty/narration-style turns) before the prompt fix
   landed. *Mitigation*: already handled architecturally by D6's repetition
   detector (`repetition_threshold=2`), confirmed tripping correctly on real
   model output, not just in the mockllm-driven unit tests; also indirectly
   resolved by fix #1 above, since a model that stops narrating stops looping
   on the same non-committal reply.
3. **A fully empty response on one turn** (`content: ""`, no action, no text)
   — observed once, transiently, during the debugging sequence between fixes.
   *Not fully root-caused*: `FencedProtocol` handles it safely (empty content
   → empty actions → task "completes" with a blank final answer, no crash,
   no hang), but an empty *successful* turn that isn't a real answer is a
   silent near-miss worth watching for in extended use — no mitigation beyond
   the existing graceful handling is implemented yet, since it didn't recur
   after the prompt fix and three turns is too small a sample to diagnose
   confidently. Flagging honestly rather than claiming a fix that wasn't
   actually verified.

Architecture note for future work: §15's bold-yellow "requested action" colour
has no live call site (D6.5's known, documented gap) — `Agent` doesn't expose a
pre-execution hook the way it does for streamed text. Cosmetic only.

## 2026-10-31T16:07Z — post-D8 capability tests (not a numbered phase)

Three real end-to-end tasks run against the real `llama-server` +
Qwen2.5-Coder-1.5B on the Jetson, in a dedicated sandbox config
(`command_filter_mode = none`, `allow_metacharacters = true` — SPEC.md §8.1's
own sanctioned oneshot-sandbox exception, not a shipped-default change). Full
writeup, transcripts, and generated artifacts in `docs/capability_tests/`.

1. **Write/compile/run a sort algorithm** — genuine success on the 4th prompt
   attempt (model wrote `sort.c`, compiled, ran it, correct output,
   independently re-verified). Attempts 1-3 each failed differently (skipped
   the write step and ran a nonexistent binary; wrote the file then emitted a
   nonsense bare-filename "command") — each caught cleanly by the D6
   repetition detector, no hang, no unsafe command.
2. **Split a text file into per-paragraph files + summarize each** — genuine,
   not-forced failure. Model produced the right filenames but generic
   placeholder summary text unrelated to the actual paragraph content; a
   narrower isolation attempt also failed (tried to `cat` a file it was
   supposed to write). Confirms the SPEC.md §16 capability ceiling: mechanical
   file ops work, content-level reasoning doesn't.
3. **Write/run a Fibonacci script** (added test, not in the original brief) —
   genuine, reproducible failure, and the most significant finding of the
   three. First attempt produced a **fully empty generation**
   (`content:"" tool_calls:[]`) on a longer prompt — confirmed not an infra
   issue (`llama-server` healthy throughout). This reproduces and resolves the
   D7 "transient empty response" note above: it recurs on real hardware, is
   handled safely by the existing empty→blank-final-answer path, and is a
   genuine model behavior rather than a one-off. Later attempts wrote a
   `fibonacci()` with a real bug (returns a scalar, not the sequence) and then
   — on a prompt structurally identical to the one that worked for the sort
   test — **fabricated a plausible success message claiming the correct
   output without ever running the script**. The buggy code was deterministic
   across separate runs at `temperature=0.2`. Independently re-running the
   model's `fib.py` confirms the bug (`TypeError: 'int' object is not
   iterable`).

**New finding beyond D7**: a model that confidently reports a result it never
produced is a sharper risk than one that errors or loops — no command filter
or repetition threshold is designed to catch a fabricated success message,
only a repeated or malformed command. Flagging honestly as an open capability
gap, not something this phase attempts to fix; the architecture's existing
safety nets (repetition detector, command filter, graceful empty-response
handling) held correctly across all 12 attempts total.
