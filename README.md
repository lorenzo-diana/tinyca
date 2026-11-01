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
check-portability` and `make cross` exist for cross-toolchain verification;
[REQUIREMENTS.md](REQUIREMENTS.md) §A.2 explains why a native build on the
target board is the authoritative artifact rather than a cross-compiled one.

## Configure

`config/tinyca.ini` ships ready to run: every value in it is the shipped
default, and the only line most setups need to touch is `llm.base_url`. Each
key is documented inline; [docs/MANUAL.md](docs/MANUAL.md) is the authoritative
reference and [SPEC.md](SPEC.md) §4 explains why the defaults are what they
are. A missing or partial config file is never fatal —
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
./build/tinyca --session sort-bug                       # named session
./build/tinyca --list-sessions                          # what is stored
./build/tinyca --resume sort-bug                        # continue it later
./build/tinyca --session sort-bug --rename-to fixed     # rename it
```

Every session is a transcript under `agent.sessions_dir`; one that nobody names
gets `YYYY-MM-DD_hh-mm-ss_<work dir>` (SPEC.md §12.3).

CLI flags (`-c/--config`, `-p/--prompt`, `-m/--model`, `-u/--url`, `--protocol`,
`--no-confirm`, `--dry-run`, `-v/--verbose`, `--resume`, `--resume-prompt`,
`--version`, `--help`) are listed by `--help` and specified in SPEC.md §12. A
bare positional task runs it once and then stays interactive. `--resume
<transcript.jsonl>` replays a previous session. If the system prompt has changed
since (a skill was added, say) or the session last ran in a different
`work_dir`, TinyCA names the difference and asks before continuing rather than
silently changing the agent's instructions or resolving the history's relative
paths somewhere else; declining leaves the transcript untouched
(`--resume-prompt`, `--resume-work-dir`, SPEC.md §12.2). Interactive mode supports `/reset`,
`/skills`, `/sessions`, `/rename <name>`, `/history`, `/toggle_show_thinking`,
`/help`, `/quit`, and TAB completion for all of the above plus
`show_skill:<name>` and session names after `/rename`.

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

A 1.5B model skips prerequisite steps, does not adapt to error messages, fails
at content-level reasoning while handling mechanical file operations fine, and
occasionally returns nothing at all. The measured table — what was observed,
with the example and what TinyCA does about each — is [SPEC.md §16](SPEC.md),
and it is the only place those limits are written down.

Two things worth knowing before you blame the model: tool output used to be fed
back without saying which command produced it, and a reply cut off at
`max_tokens` used to be executed as if complete. Both were bugs here, both are
fixed, and both looked exactly like model stupidity. Check the harness first.

Settings that work at this size: whole-file rewrites over exact-string edits,
single-step tasks, `on_blocked_command = ask`, and the shipped defaults for
`skills.tier2_enabled` and `skills.tier1_exclude`.

## Documentation

Every document about TinyCA, and what it is for:

- [docs/MANUAL.md](docs/MANUAL.md) — the usage reference: every CLI option,
  every config key with its type and default, sessions, skills, protocols,
  command filtering, the transcript format, exit codes. Start here to *use*
  TinyCA, and it is the single definition of each option and key.
- [SPEC.md](SPEC.md) — the design document: what was decided and why, the
  component interfaces, and §16's measured capability ceiling.
- [REQUIREMENTS.md](REQUIREMENTS.md) — packages and toolchains for the
  development host and the Jetson, and the cross-compilation trade-offs.
- [third_party/VERSIONS.md](third_party/VERSIONS.md) — the exact versions of
  the two vendored headers, and why those versions.

Where to look for a given fact: **what** an option does, `docs/MANUAL.md`;
**why** it is that way, `SPEC.md`. Each fact is defined in exactly one of them
and referenced from the other, so when they disagree the reference is the bug.

The development record — the original build brief, the engineering journal, and
the measured on-device runs — is deliberately **not** part of this tree. It
lives under `dev/`, starting at `dev/README.md`, and is kept separate because it
describes how TinyCA was built rather than what it is.
`tests/integration_docs.sh` enforces both halves of that: no document in either
tree may be orphaned, and no link may cross between them.

## Project layout

See [SPEC.md](SPEC.md) §3 for the full tree.

## AI generated project

This project has been written mainly by AI.

