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
- `first_run_transcript.jsonl`: the six JSONL lines from that exact successful
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

**On-device benchmark** (from the real D7 run, `first_run_transcript.jsonl`):
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
writeup, transcripts, and generated artifacts in `capability_tests/`.

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

## 2026-11-01 — post-D8 maintenance session (cleanup, audit, 15 defect fixes)

Not a numbered phase. Entered with D0-D8 complete; left with the same
architecture, a cleaned and re-commented codebase, 13 passing suites (was 12),
and fifteen defects fixed — two of which had been mis-filed as model
limitations.

**Cleanup** (`clean-code`): `config.cpp` 487 -> ~400 lines by replacing 38
`std::function` lambdas plus six `set_*_field` wrappers with typed
member-pointer tables and one `parse_number<T>` trait; key set diffed
old-vs-new (39 keys, identical). Removed dead `Agent::api_path_for`, a
write-only `Action::raw_args`, an unused `httplib::Response`, a parsed-but-
unused `json body` in mockllm, and two `bool` returns that could never be
false. Two inherited comments were false and were corrected.

**Commenting** (`comment-for-audit`): +104 lines, only where reading actually
stumbled; seven `AUDIT NOTE:` hazards tagged. Refused to document one
suspicion that measurement disproved (`Executor::interrupted_` is atomic, no
race) and marked one premise explicitly unverified rather than asserting
llama-server's stop-text behaviour.

**Audit** (`quality-review`): 11 verified findings + 2 found later while
fixing. All fixed this session except where noted:

1. `finish_reason: "length"` was ignored, so a reply cut at `max_tokens` was
   executed as if complete — a truncated `cat > f <<EOF` wrote a partial file
   and reported success. Now detected in both wire shapes and both transports;
   nothing executes, the model is told, the turn counts.
2. Streamed `tool_call` index sized an allocation unchecked: `index: 2000000`
   claimed ~183 MiB from one chunk. Bounded at 64.
3. `grammars/hermes_tool_call.gbnf` did not exist, making `enable_grammar =
   true` fatal in any clone. Written.
4. `protocol.stop_sequences` was parsed, stored and never read while the
   example config documented it as working. Applied; literal-only values
   documented.
5. SPEC §12's positional `[task]` was advertised by `--help` and silently
   dropped, which also left a dead branch in `main.cpp`. Implemented.
6. Tier 2 skills were implemented, unit-tested and unreachable (no production
   caller, no mention in the prompt). Wired both ways — user-initiated in the
   REPL, model-initiated via the trigger — behind `skills.tier2_enabled`
   (default off).
7. `transcript_max_bytes` bounded one run's growth, not the file (194 -> 388 ->
   582 bytes over three runs at cap 200). Now seeded from the file's size, with
   a new `transcript_max_bytes_per_run` for the old meaning and a clamp check
   between them.
8. `output_max_bytes` was not a hard cap (4137 bytes at 4096). Marker now
   reserved inside the cap.
9. Segmentation was not quote-aware: `echo "hello; world"` was rejected on the
   token `world"`. Fixed last, deliberately, as the only change that *expands*
   what runs; landed with an adversarial test set (`ls; rm -rf /`, `echo "a" ;
   rm -rf /`, unbalanced quotes) that still fails closed.
10. The one shipped skill could not run as advertised: commands execute with
    `cwd = work_dir` while Tier 1 advertised a repo-root-relative path (exit
    127), and the model's natural `../skills/...` correction was blocked by the
    filter. `Executor` now puts the absolute skills dir on the child's PATH
    (envp built pre-fork, `execve`) and skills are advertised by bare name.
11. Build hygiene: neither the Makefile nor `run_tests.sh` tracked header
    dependencies. A new `Config` field left stale objects behind and the link
    produced a binary whose translation units disagreed on layout — no link
    error, just `test_agent` taking the `--dry-run` branch with
    `cli_dry_run == false`. Added `-MMD -MP` and a header-mtime check. This had
    been silently poisoning every header edit in this project.
12. Command output never reached the terminal (transcript only), and SPEC §15's
    bold-yellow action colour had no call site. Added `set_on_action` /
    `set_on_tool_output`.
13. **Tool output was fed back unlabelled** — the session's most significant
    finding, and not a model limitation. Qwen's template renders a `role:
    "tool"` message as a user turn wrapped in `<tool_response>`, a shape the
    model only ever saw after emitting a structured `<tool_call>`; after a
    markdown fence an unlabelled result does not read as that fence's output
    and the likeliest continuation is to re-emit the command. Measured at
    `temperature = 0.2`: unlabelled looped 2/2 as `tool` and 2/2 as `user`;
    labelled answered correctly 2/2. Results now carry `Output of <cmd>:`.
    "List the files and report how many there are" went from a 4-turn
    repetition abort (20.4 s) to one command and a correct answer (4.9 s).
14. **Regression found by re-running, not by inspection**: with `add_skill`
    advertised in Tier 1, the 1.5B model abandoned a compile-and-run task,
    invented a GitHub URL to download a skill from, and registered a junk
    `skills/sort.sh` into the live skills directory. New `skills.tier1_exclude`
    (default `add_skill`) keeps a skill runnable but unadvertised.
15. An empty generation (`content: ""`, `finish_reason: "stop"`) was reported as
    a finished task with a blank answer — flagged at D7 and again post-D8,
    never fixed. Now nudged once, then aborted honestly.

**New features requested this session**: `add_skill` so the agent can create
its own skills, with three operator-selectable safeguards
(`skills.skill_checks`: screen a new skill's body, confirm before creation,
screen every skill at startup) — a skill is auto-allowed as a whole, so its
*contents* would otherwise bypass the whitelist entirely. Also
`--resume-prompt ask|update|quit`: a resumed session whose stored system prompt
differs from the regenerated one now reconciles explicitly instead of silently
swapping the agent's instructions, and a resumed session continues writing to
its own transcript.

While building the startup screen I found and fixed a bug in my own validator:
the first `VAR=value` line produced an empty head token and ended the scan, so
nothing was ever rejected. Verified both directions afterwards — shipped skills
pass under the default whitelist, `echo cleanup; rm -rf /tmp/x` buried on line
2 of a skill is caught.

**Capability re-run after fix 13** (`capability_tests/README.md`): the
previously-successful sort task still succeeds (73 s, no regression); the
one-shot sort prompt still fails the same way (skips writing the file, repeats
the failing `gcc`); Fibonacci still produces the same deterministic bug but did
not fabricate a success this time. So labelling removed the failure class where
the model had the answer and did not recognise it, and left planning and
error-adaptation untouched. SPEC §16 now separates measured model limits from
harness bugs, with the evidence for each.

**Gates**: `make`, `make test` (13 suites), `make check-portability`,
`make check-format` all clean on x86-64 and natively on the Jetson under
gcc 8.4.0. Hardware-verified on-device: labelled results, the action/output
hooks, skills by bare name, `add_skill` creating a working skill and announcing
it mid-session, and the Tier 1 index picking it up after restart.

## 2026-11-01 (cont.) — confirm_commands, resume work-dir check, named sessions

Follow-on work in the same session, after the 15 fixes above.

- `tools.confirm_commands`: head tokens confirmed before every run even though
  the filter allows them. Built-in default empty (no config file = unchanged
  behaviour); `tinyca.ini.example` ships `python3,awk,sed`, because whitelisting
  an interpreter grants everything that interpreter can do. Independent of
  `confirm_mode` like `on_blocked_command`, so `--no-confirm` does not clear it
  — documented, including that an unattended run then refuses those programs.
- **Resume checks the work directory.** This needed the work dir to be recorded
  at all, which it was not: every run now appends a `session_start` entry with
  its absolute `work_dir`, and `--resume` compares. On a difference it prints
  both paths and `--resume-work-dir ask|continue|quit` decides (ask on a TTY,
  quit otherwise). Accepting records a `work_dir_update` entry naming the
  previous directory.
  Two ordering bugs found by the tests, not by inspection: `session_start` was
  written at startup, which touched the transcript of a resume the user then
  declined; and the system-prompt adoption was committed before the work-dir
  question was asked. Reconciliation is now transactional — every difference is
  decided first, then committed — so declining at any point leaves the file
  byte-identical.
- **Named sessions** (SPEC.md §12.3), new `src/session.cpp`: a session *is* its
  transcript at `<sessions_dir>/<name>.jsonl`. No index file — the directory is
  the registry, so uniqueness is one `stat()` that cannot drift, and sessions
  can be archived with `mv`/`rm`. `--session <name>` creates or continues,
  `--list-sessions` lists newest-first with task/entry/byte counts, `--resume`
  accepts a bare name (falling back to treating the argument as a path, so
  existing invocations keep working). Names are restricted to what is safe as a
  filename, refused before anything is written.
  Every session has a name, including unnamed ones:
  `YYYY-MM-DD_hh-mm-ss_<work dir basename>` in local time, sanitised, with
  `-2`, `-3`… for the same-second-same-directory case. The time uses dashes and
  `:` is not a legal name character, so a session file can be copied onto a
  FAT/exFAT/Windows volume as-is. An explicit `agent.transcript_log_path`
  still opts out entirely.
- **Rename**: `/rename <name>` in the REPL (moves the file with `rename(2)`,
  points the logger at it via the new `Logger::set_path`, and records a
  `session_rename` entry inside the file) and `--session <old> --rename-to
  <new>` for scripts. Refused, with nothing changed, on an unusable name, a
  taken name, the current name, or a missing source. `/sessions` marks the
  current session, and TAB completes session names after `/rename `.

New suite `test_session` (14 suites total). All gates clean on x86-64 and
natively on the Jetson under gcc 8.4.0.

## 2026-11-01 (cont.) — manual, second cleanup/comment/review pass

- `docs/MANUAL.md` (452 lines): man-page-style reference for every CLI option,
  every config key with type and default, sessions, skills and their
  safeguards, protocols, command filtering, the REPL, the transcript entry
  kinds, the environment passed to child commands, files, examples and exit
  codes. Generated against the code (key tables extracted from `config.cpp`,
  flags from `parse_cli`) rather than from memory, and cross-linked from
  README and SPEC §3.
- **Cleanup, KISS-focused**: `main()` 518 -> 260 lines by extracting four jobs
  it was doing inline — `build_system_prompt`, `resolve_session_file`,
  `resume_into`, `register_new_skills` — plus one session listing shared by
  `--list-sessions` and `/sessions` instead of two printers. Removed three
  further duplications: one `split_trimmed` helper for the five sites that
  open-coded split+trim+drop-empty, `SkillRegistry::invocation_tokens` for the
  three sites that derived a skill's two invocable spellings, and a hand-rolled
  bubble sort replaced by `std::sort`. Dead-symbol sweep came back silent.
- **Comments**: added the three notes a reviewer actually stumbles on — why
  streamed `tool_call` fields are appended rather than assigned (the easy
  mistake is reading `+=` as a bug), the `unique_session_name` 999-attempt
  fallback, and the per-turn cost of the skills re-scan. Mean density 19%.
- **Review**: six correctness findings, one of them a real security defect in
  `blacklist` mode (quoting the program name bypasses it — verified with bash
  actually deleting the files), plus a test-harness defect that produces false
  failures. One suspicion measured and withdrawn. Details in the session
  summary; nothing applied, per the review/fix separation.

All gates clean on x86-64 and natively on the Jetson: 14 suites, zero
warnings, `check-portability`, `check-format`.

## 2026-11-01 (cont.) — all review findings fixed, docs made reachable

- **Finding 1 (security, blacklist mode)**: `extract_head_token` now unquotes
  the head token the way bash word-expands a command name, so `"rm"`, `'rm'`,
  `\rm`, `r""m` and `$'rm'` all resolve to `rm`. The blacklist additionally
  matches the **basename**, closing `/bin/rm` — found while fixing the quoting
  case, same defect class (the filter was comparing spellings, not programs).
  The whitelist deliberately does not match basenames, so `/opt/evil/ls` still
  fails closed. Written test-first: 14 assertions failed before the fix, and
  the original probe that watched bash delete files now reports BLOCK on every
  spelling.
- **Test-harness leak**: `launch_mockllm` became a scoped `MockServer` guard
  that kills its child in the destructor and polls the port instead of sleeping
  300 ms. Both halves mattered: the leak poisoned the *next* run (16 strays had
  accumulated on the Jetson, 115 on the dev host), and the fixed sleep produced
  "Could not establish connection" failures under load that looked like agent
  defects. `run_tests.sh` also reaps strays defensively, for the case where a
  test binary dies before its destructor runs. Verified: 0 strays after a full
  run, from a clean slate.
- **Finding 2**: `--resume <unknown>` is now fatal (exit 1) instead of starting
  a fresh session logging into a file named after the typo, invisible to
  `--list-sessions`.
- **Finding 3**: `--session` together with `--resume` warns that it is ignored.
- **Finding 4**: the session listing's task count tolerates JSON spacing
  instead of substring-matching Logger's exact bytes, which silently reported
  "0 task(s)" — a wrong number that reads like a real one.
- **Finding 7**: `unique_session_name` now CLAIMS the name with
  `O_CREAT|O_EXCL` rather than checking existence, closing the race where two
  runs starting in the same second could interleave into one transcript. Noted
  honestly: claiming creates the file, so a run that dies later in startup
  leaves an empty session behind.
- **Documentation reachability**: README has a Documentation section linking
  every one of the eight `.md` files with a sentence on what each is for, and
  `tests/integration_docs.sh` (new, 15th suite) fails if any `.md` in the repo
  is unreachable from README.md or if a link points at a missing file. Both
  failure modes verified by introducing them deliberately.
- Also updated the `clean-code` skill with a final "Phase 9 — KISS, as the LAST
  action" covering function scope, duplicate implementations, hand-rolled
  standard algorithms and naming, with the explicit bar that simpler is not
  shorter and that abstracting to satisfy the phase makes it worse.

## 2026-11-01 (cont.) — documentation split into two disjoint trees

Applied a new `clean-documentation` skill (written in the same session) to this
repository. Four failure modes, in its order:

- **Coverage** (derived from the code, not from the docs): all 46 config keys
  and all 16 CLI long options were already documented in `docs/MANUAL.md`. One
  real defect found: `--help` and SPEC §12 both advertised `implies --mode
  oneshot`, and there is no `--mode` flag — help text that lied. Fixed in the
  code and the spec.
- **Duplication**: every config key was *defined* twice (SPEC §4 tables and
  `docs/MANUAL.md` tables), the CLI option list twice, the capability ceiling
  three times (README, SPEC §16, MANUAL), and the result-labelling rationale
  three times. Each now has one home — MANUAL for what an option/key is and
  does, SPEC for why it is that way, SPEC §16 for the limits — with one-line
  references elsewhere. SPEC §4 and §12 became *decision* sections, which is
  what a design document is for.
- **Mixed material**: PROMPT.md (build brief + agent workflow), PROGRESS.md
  (this journal) and the capability-test write-ups are development process, not
  project documentation. They moved to `dev/` with their own index at
  `dev/README.md`. SPEC §16 kept the measured limits and lost the narrative of
  how they came to be measured; that story lives here.
- **Reachability**: two disjoint trees, project rooted at `README.md` and
  process at `dev/README.md`, with no link crossing in either direction.

`tests/integration_docs.sh` was rewritten to enforce all four: orphans,
dangling links, cross-tree links in either direction, and code-surface coverage
of the manual. Breaking each rule on purpose exposed a bug in the checker
itself — a single shared "seen" map meant the second tree walk skipped
documents the first had already found, so a `dev/` document linking into the
project tree passed. Independent walks per tree; all four violations now fail
as they should.
