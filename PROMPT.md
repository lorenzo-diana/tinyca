# PROMPT — TinyCA

You are building **TinyCA**, a minimal C++11 coding agent designed to drive a small local
LLM (Qwen2.5-Coder-1.5B-Instruct) served by `llama-server` on an NVIDIA Jetson Nano
running Ubuntu 18.04 / JetPack 4.6.6 / gcc 8.

Read `SPEC.md` in full before writing code. `REQUIREMENTS.md` lists installable packages.

## Non-negotiable constraints

1. **`-std=c++11` only.** Not C++14, not C++17. No `std::make_unique`, no
   `std::filesystem`, no generic lambdas, no `auto` return deduction, no structured
   bindings, no `std::optional`. Use POSIX (`opendir`, `stat`, `fork`/`exec`) where the
   STL lacks C++11 coverage. Chosen over a newer standard specifically for
   compatibility with older Linux distributions beyond just this Jetson image, not
   only for what gcc 8 happens to support.
2. **No `std::regex`.** All parsing uses `std::string::find` / `substr` / `compare`.
   This is a portability and binary-size decision, not a performance one.
3. **Makefile only.** No CMake, no autotools.
4. **Two vendored header-only dependencies**, committed under `third_party/`:
   `nlohmann/json.hpp` and `cpp-httplib/httplib.h`. Pick versions that compile under
   gcc 8 in C++11 mode and record the exact versions in `third_party/VERSIONS.md`.
   Nothing else. No Boost, no yaml-cpp, no libcurl.
5. **The source must be portable, the binary need not be.** Target gcc 8 language and
   library features; assume nothing about glibc beyond 2.27.

## Build matrix — resolve the toolchain problem this way

You are running on Debian 12 (bookworm) x86-64. Its `gcc-12-aarch64-linux-gnu` targets
glibc 2.36 while the Jetson has 2.27, so dynamically linked cross output will not run
there. Do not try to work around this; cross-compilation is not on the critical path,
because a native build on the Jetson takes ~1–2 minutes.

| Tier | Command | Purpose |
|---|---|---|
| 1 | `make` (native x86-64 g++) | All development and the full test suite against the mock server |
| 2 | `make check-portability` | `-std=c++11 -pedantic-errors -Wall -Wextra` plus a grep-based ban list for C++14/17 constructs |
| 3 | `make cross` (`aarch64-linux-gnu-g++`) | Syntax/codegen check for aarch64 only — **never deployed** |
| 4 | `make` on the Jetson over SSH | Authoritative build and deployment artifact |

`make cross-static` (adding `-static`) may optionally produce a runnable aarch64 binary,
but static glibc drops NSS, so `base_url` must then be an IP literal. Document this
trade-off; do not make it the default.

## Development order — tests before hardware

The Jetson may not be reachable and inference is slow, so the agent must be fully
testable on x86 with no GPU and no network:

1. Config parser + unit tests.
2. Mock LLM server (`tools/mockllm`, built on `httplib`'s server side) replaying scripted
   completions from JSON fixtures.
3. Protocol layer (fenced + Hermes) + parse tests against fixtures.
4. Skills discovery and Tier 1 rendering.
5. Bash executor, whitelist validation, output truncation.
6. Agent loop, iteration cap, repetition detection.
7. Only then: end-to-end against a real `llama-server`.

Required fixture scenarios: well-formed single action; malformed/unterminated block;
no-action turn (task complete); two parallel Hermes calls; a `<think>` block preceding an
action; the same failing command emitted three times in a row; tool output of 5 MB;
a command containing `;` and `&&`; a `tool_call` naming a nonexistent tool.

`make test` must pass with no network access and no LLM.

## Correctness rules that are easy to get wrong

- **History is append-only.** `llama-server` matches the longest KV cache prefix, so
  rewriting or re-summarising earlier messages forces full reprocessing — minutes on this
  hardware. Only ever append.
- **The assistant's own output is replayed as `role: "assistant"`.** Never collapse the
  transcript into user messages; the model must see the calls it made.
- **Stop sequences are mandatory.** Without them the model emits a command and then
  hallucinates its output. Generation must halt at the closing delimiter.
- **Whitelist validation splits on separators** and validates every segment's head token.
  A check on the first token only is trivially bypassed by `ls; rm -rf /`.
- **Grammar and thinking conflict.** A GBNF grammar constraining output to a tool call
  forbids a preceding `<think>` block. Detect this combination at startup and refuse to
  run with a clear message.

## Deliverables

Working tree per `SPEC.md` §3, plus:

- `config/tinyca.ini.example`, fully commented, and `config/system_prompt.txt`
- `skills/fetch_url.sh` — the worked example skill (SPEC.md §10)
- `tests/` with fixtures and a runner invoked by `make test`
- `README.md` under 150 lines: build, configure, run, extend
- `LICENSE` — BSD-3-Clause

## Style

- Small translation units, one class per header/source pair.
- No exceptions across interface boundaries; return status codes or result structs.
- Every config key has a documented default; a missing key is never fatal unless the
  agent genuinely cannot run without it.
- Log every LLM request/response and every executed command to the transcript file. When
  something goes wrong on the Jetson, that file is the only diagnostic available.
- Ask before adding any dependency or deviating from `SPEC.md`.

---

# Autonomous execution workflow

## W0. Preflight — run before anything else, every session

1. `cat PROGRESS.md` if it exists. It is your resume point; never restart from zero.
2. `g++ --version && make --version` — confirm the Tier 1 toolchain.
3. Look for the sibling project's findings at
   `../llama.cpp_ubuntu_18/docs/FINDINGS.md`.
   - **Present** ⇒ read it and record the four handoff answers in `PROGRESS.md`.
   - **Absent** ⇒ do not block. Proceed with these documented assumptions, each marked
     `TODO(findings)` in the code and listed in `README.md`:
     - `grammar` is assumed **not** honoured on `/v1/chat/completions` ⇒ `enable_grammar`
       defaults to `false` and validates `api_path` at startup.
     - Hermes `<tool_call>` output is assumed to arrive **in the content string**, not in
       a structured `tool_calls` array ⇒ the Hermes parser must handle both.
     - Pin `json.hpp` and `httplib.h` yourself, C++11-compatible, and record the versions.
4. `./scripts/jetson.sh exec 'echo ok'` if `jetson.conf` exists. Unreachable is **not** a
   blocker for phases D0–D6; only D7 needs hardware.

### Network topology

The container reaches only allowlisted domains (github, pypi, npm, Ubuntu archives).
GitHub is allowlisted, so both vendored headers can be fetched here. Hugging Face is not —
any model download happens on the Jetson. `make test` must pass with **no network at all**.

## W1. Phase plan and gates

Strictly in order; each gate must pass before the next phase. Nothing requires hardware
until D7, so an unreachable Jetson never stalls development.

| Phase | Work | Gate |
|---|---|---|
| D0 | Repo skeleton, Makefile with all targets, vendored headers, LICENSE | `make` and `make check-portability` succeed on an empty `main()` |
| D1 | INI parser, `Config`, defaults, CLI override precedence | Unit tests cover: missing file, missing key, comments, `[section]`, whitespace, unknown key warning, CLI > file > default |
| D2 | `mockllm` + fixture loader | `build/mockllm` serves a fixture and records received requests |
| D3 | `ICodec` / `OpenAIChatCodec`, `Message` model | Round-trip test: encode history ⇒ decode canned response ⇒ correct roles and content |
| D4 | `IProtocol`, `FencedProtocol`, `HermesProtocol` | All nine fixture scenarios from the list above parse to the expected `Action` vector, including the malformed ones |
| D5 | `SkillRegistry`, Tier 1 render, Tier 2 load, `fetch_url.sh` | Tier 1 block matches the SPEC §7 format exactly; a skill missing `@usage` is skipped with a warning; skill path is auto-whitelisted |
| D6 | `Executor`, whitelist, truncation, `Agent` loop, repetition detector | Full loop runs end-to-end against `mockllm`; `ls; rm -rf /` is rejected; 5 MB output is truncated; three identical commands abort |
| D7 | Deploy to Jetson, build natively, run against real `llama-server` | A real task completes on-device; transcript captured |
| D8 | `README.md`, `config/tinyca.ini.example` fully commented | A reader can build, configure and run without reading `SPEC.md` |

`make test` must pass at the end of **every** phase from D1 onward, not only at D6.

## W2. Test-first rule

For each phase, write the test before the implementation. A phase gate is "tests written,
tests failing, implementation added, tests passing" — in that order. This matters here
because the alternative (validating against a real 1.5B model at ~5 tok/s) makes each
iteration minutes long instead of milliseconds.

`tests/run_tests.sh` returns non-zero on any failure and prints a one-line summary per
case. No test framework dependency: a small assert macro in `tests/test_util.h` is enough.

## W3. Deployment and on-device verification (D7)

Only after `make test` passes locally:

```bash
# 1. sync source (never binaries)
rsync -av --exclude build --exclude .git ./ nano@<host>:~/TinyCA/

# 2. native build on the board, ~1-2 min
ssh nano@<host> 'cd ~/TinyCA && make 2>&1 | tail -n 20'

# 3. start the server (detached) if not already running
ssh nano@<host> 'nohup ~/llama.cpp_ubuntu_18/build/bin/llama-server \
  -m ~/models/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf \
  -ngl 99 -c 4096 --host 0.0.0.0 --port 8080 --jinja \
  > ~/logs/llama-server.log 2>&1 & echo started'

# 4. wait for readiness — first load can take minutes
ssh nano@<host> 'until curl -sf http://127.0.0.1:8080/health > /dev/null; do sleep 10; done; echo ready'

# 5. one-shot smoke task
ssh nano@<host> 'cd ~/TinyCA && ./build/tinyca -c config/tinyca.ini \
  -p "List the files in the workspace and report how many there are" --no-confirm 2>&1 | tail -n 40'
```

D7 acceptance: the agent issues at least one bash action, receives its output, and
terminates with a no-action turn inside `max_iterations`. Save the transcript to
`docs/first_run_transcript.jsonl` and commit it — it is the evidence the loop works
against a real small model.

Expect the first on-device run to expose format drift the mock cannot: unterminated
fences, commands wrapped in prose, the model narrating output it never received. Fix by
tightening the system prompt and stop sequences, not by loosening the parser.

## W4. Failure policy

1. Record the full error in `PROGRESS.md` under the failing phase.
2. At most **three distinct hypotheses** per failure; a rerun is not a hypothesis.
3. Still failing ⇒ mark BLOCKED, STOP, and ask a specific question with the evidence.

Never: add a dependency to work around a problem; relax `-std=c++11`; introduce
`std::regex` "just for this one case"; disable a failing test; or weaken the
command-filter splitter (whitelist or blacklist) to make a test pass.

## W5. Progress journal — `PROGRESS.md`

Append-only, committed, one entry per phase gate and per blocking failure:

```markdown
## 2026-09-06T16:40Z — D4 protocols
- FencedProtocol: 9/9 fixtures pass; stop sequence consumed the closing fence as predicted,
  parser falls back to end-of-text
- HermesProtocol: handles both structured tool_calls and raw <tool_call> in content
- GATE PASSED
```

Read it first at the start of every session.

## W6. Git conventions

- Branch `main`; one commit per phase gate.
- Message form: `D4: fenced and hermes protocol parsers + fixtures`.
- `.gitignore`: `build/`, `logs/`, `workspace/`, `config/tinyca.ini` (the live config;
  the `.example` is committed), `jetson.conf`.
- Commit `third_party/` headers and `VERSIONS.md`.
- License header (BSD-3-Clause) at the top of every source file you author; leave vendored
  headers untouched.

## W7. Final report

When D8 completes, print: the phase table with outcomes, the vendored dependency versions,
which `TODO(findings)` assumptions are still unresolved, the on-device benchmark
(turns per task and wall-clock seconds per turn), and the top three observed model failure
modes with the mitigation applied to each.
