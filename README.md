# TinyCA

A minimal C++11 coding agent for small local LLMs. Drives a tool-use loop against
an OpenAI-compatible endpoint — `llama-server` running `Qwen2.5-Coder-1.5B-Instruct`
on a Jetson Nano (running Ubuntu 18.04) is the target this was built and tuned for.
Local-only by design: no Anthropic/OpenAI/hosted-provider support exists or is
planned (see SPEC.md §1).
Why to have such a coding agent? Just to have something to experiment with and
able to run on older boards.

## Build

```bash
make            # native build -> build/tinyca  (~seconds on x86, ~1 min on a Jetson)
make test       # offline test suite -- no network, no LLM required
```

Needs nothing beyond `build-essential`/`make` (REQUIREMENTS.md A.1). Two
header-only dependencies are vendored under `third_party/` (`json.hpp`,
`httplib.h`) — nothing else, no package manager at runtime. `make
check-portability` and `make cross` exist for cross-toolchain verification; see
`PROMPT.md`'s build-matrix section for why native Tier 4 (building directly on the
target board) is the authoritative path rather than cross-compilation.

## Configure

```bash
cp config/tinyca.ini.example config/tinyca.ini
```

Every key is documented inline in `tinyca.ini.example` (SPEC.md §4 is the
authoritative reference). A missing or partial config file is never fatal —
TinyCA logs a warning per missing/unknown key and falls back to built-in
defaults. Precedence: CLI flag > config file > default.

The system prompt lives in `config/system_prompt.txt`, not the INI file —
multi-line prompts don't belong in INI values. The Tier 1 skills index
(`skills/*.sh`, SPEC.md §7) is appended to it automatically at startup.

Point `llm.base_url` at a running `llama-server`:

```bash
llama-server -m qwen2.5-coder-1.5b-instruct-q4_k_m.gguf \
  -ngl 99 -c 4096 --host 0.0.0.0 --port 8080 --jinja
```

## Run

```bash
./build/tinyca -p "list the files here" --no-confirm   # one-shot
./build/tinyca                                          # interactive REPL
```

CLI flags (`-c/--config`, `-p/--prompt`, `-m/--model`, `-u/--url`, `--protocol`,
`--no-confirm`, `--dry-run`, `-v/--verbose`, `--version`, `--help`) are listed by
`--help` and specified in SPEC.md §12. Interactive mode supports `/reset`,
`/skills`, `/history`, `/toggle_show_thinking`, `/help`, `/quit`, and TAB
completion for all of the above plus `show_skill:<name>`.

## Extend

**Add a skill**: drop an executable `.sh` file into `skills/` with a
`@name`/`@description`/`@usage` header (see `skills/fetch_url.sh` for the exact
format). Prefer skills written as scripts over skills written as prose
instructions — a 1.5B model can reliably *select* a script by its one-line
description; it cannot reliably *follow* multi-step prose instructions
(SPEC.md §7).

**Switch protocols**: `protocol.tool_protocol = fenced` (default, one
` ```bash ` block per turn) or `hermes` (parallel-capable, structured tool
calls). See SPEC.md §6 for the exact wire formats and why fenced is the safer
default for a model this small.

**Point at a different local server**: nothing here is llama.cpp-specific
beyond the `--jinja`/chat-template assumption — any OpenAI-compatible local
server (a local vLLM instance, for instance) works by changing `llm.base_url`.
Remote/hosted providers are explicitly out of scope (SPEC.md §14), not a future
flag.

## Known capability ceiling

With a 1.5B model, expect: commands ignoring the stated working directory,
repeated retries after an error, and near-total failure at exact-string file
edits. Prefer whole-file rewrites over patches, keep tasks single-step, keep
`on_blocked_command = ask`. The architecture is sound; the model is the limit
(SPEC.md §16). See `PROGRESS.md`'s D7 entry and `docs/first_run_transcript.jsonl`
for the concrete format-drift issues this surfaced on real hardware and how each
was fixed.

## Project layout

See `SPEC.md` §3 for the full tree. `PROMPT.md` is the original build brief and
phase-by-phase workflow this was built against; `PROGRESS.md` is the append-only
journal of every phase gate, failure, and fix, in order.

## AI generated project

This project has been written mainly by AI.

