# tinyca(1) — manual

Complete reference for the `tinyca` binary: every command-line option, every
configuration key, and the behaviour each one controls. `SPEC.md` is the design
document and remains authoritative on *why*; this file is the usage reference.

## NAME

tinyca — minimal C++11 coding agent for small local LLMs

## SYNOPSIS

```
tinyca [options] [task]
tinyca --list-sessions
tinyca --session <old> --rename-to <new>
```

With no `task`, no `-p` and `agent.mode = interactive`, TinyCA starts a REPL.
A positional `task` runs that one task and then stays in the REPL; `-p` runs one
task and exits.

## DESCRIPTION

TinyCA drives a tool-use loop against an OpenAI-compatible endpoint exposed by a
local inference server (`llama-server` today, a local vLLM instance potentially
later). Each turn it sends the conversation, parses at most one command out of
the model's reply, runs it through a command filter, executes it in a working
directory, and appends the labelled result to the conversation. Remote or hosted
providers are out of scope by design (SPEC.md §1, §14).

The loop ends when the model replies with no command (task complete), when the
iteration cap is reached, when the same command repeats, on an empty reply twice
in a row, or on a user interrupt. Every request, reply, command and result is
appended to a transcript, which is also the session file.

## OPTIONS

Precedence for anything that exists in both places: **command line > config file
> built-in default**.

| Option | Effect |
|---|---|
| `-c`, `--config <file>` | Config file to read. Default `./config/tinyca.ini`. A missing file is a warning, not an error — built-in defaults apply. |
| `-p`, `--prompt <task>` | Run one task and exit. Implies `agent.mode = oneshot`. Wins over a positional task. |
| `[task]` (positional) | Run one task, then enter the REPL. All remaining arguments are joined with single spaces. Does **not** imply oneshot. |
| `-m`, `--model <id>` | Override `llm.model_id`. |
| `-u`, `--url <base_url>` | Override `llm.base_url`. |
| `--protocol <fenced\|hermes>` | Override `protocol.tool_protocol`. An invalid value is a warning; the configured value stands. |
| `--no-confirm` | Sets `tools.confirm_mode = never`. Does **not** clear `tools.confirm_commands` or `skills.skill_checks`' `confirm_on_create`. |
| `--dry-run` | Parse and report actions, execute nothing. The command filter and executor are skipped entirely, so a dry run never triggers a block or confirmation prompt either. |
| `-v`, `--verbose` | Sets `agent.log_level = debug`. |
| `--resume <name\|file>` | Replay a stored session. A bare name is resolved inside `agent.sessions_dir`; anything else is treated as a path. A target that is neither a stored session nor an existing file is a **fatal** error (exit 1) rather than a silently-created new session. The resumed file also becomes the transcript this run writes to. Given together with `--session`, `--resume` wins and says so. |
| `--resume-prompt <ask\|update\|quit>` | What to do when the resumed session's system prompt differs from the one generated now. Default: `ask` when stdin is a TTY, `quit` otherwise. |
| `--resume-work-dir <ask\|continue\|quit>` | What to do when the resumed session recorded a different `work_dir`. Default: `ask` on a TTY, `quit` otherwise. |
| `--session <name>` | Create or continue a named session; the transcript becomes `<sessions_dir>/<name>.jsonl`. Prints `new session:` or `continuing session:`. |
| `--list-sessions` | List stored sessions (newest first, with date, task count, entry count, size) and exit. If `--rename-to` is also given, the rename happens instead — see below. |
| `--rename-to <new>` | With `--session <old>`: rename that stored session and exit. Takes precedence over `--list-sessions` if both are given; list afterwards to see the result. Without `--session` it is a fatal error, since there is nothing to rename. |
| `--version` | Print the version and exit. |
| `--help` | Print usage and exit. |

Unrecognised options produce a warning and are ignored; they are not fatal.

## EXIT STATUS

| Code | Meaning |
|---|---|
| 0 | Task completed, REPL exited normally, or an informational action (`--help`, `--version`, `--list-sessions`, `--rename-to`) finished. A model failure (iteration cap, repetition abort, empty replies) still exits 0 — the run itself worked; read the final message. |
| 1 | Fatal startup or request-level problem: an invalid configuration combination, `protocol.enable_grammar = true` with a missing `grammar_file`, `mode = oneshot` with no task, an unusable `--session` name, an unknown `--resume` target, a failed `--rename-to`, or a declined `--resume` reconciliation. |

## CONFIGURATION FILE

INI format: `key = value`, `#` or `;` comments, `[section]` headers, values and
keys trimmed, no quoting, no escape sequences. Keys are addressed as
`section.key`. An unknown key, a malformed line or an unusable value produces a
warning and is skipped — the corresponding default stands. A duplicate key keeps
the last value and warns. Nothing in the file is fatal except the two
`enable_grammar` combinations below.

`config/tinyca.ini` is committed, ready to run, and carries the same keys with
inline comments, because a config file nobody can read without a second window
is a bad config file. That is a deliberate, bounded duplication: **this manual
is authoritative**, and if the two ever disagree the comment in the config is
the bug.

### `[llm]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `base_url` | string | `http://127.0.0.1:8080` | Scheme, host and port of the inference server. Use an IP literal if the binary was statically linked (no NSS). |
| `api_path` | string | `/v1/chat/completions` | Rewritten to `/completion` automatically when `protocol.enable_grammar = true`. |
| `model_id` | string | `qwen2.5-coder-1.5b` | Echoed back to the server; arbitrary unless the server was started with `--alias`. |
| `api_key` | string | `none` | Sent as `Authorization: Bearer`. The literal `none` (and an empty value) sends no header at all. |
| `request_timeout_s` | int | `900` | Per request. Generous on purpose: generation runs at ~4–5 tok/s on a Jetson Nano. |
| `connect_timeout_s` | int | `10` | |
| `max_tokens` | int | `512` | A reply cut off at this limit is detected (`finish_reason: length`) and **not** executed. |
| `temperature` | double | `0.2` | |
| `top_p` | double | `0.9` | |
| `context_tokens` | int | `4096` | Must match the server's `-c`. Used for budget warnings and the hard abort. |
| `stream` | bool | `true` | SSE streaming. Presentation only: chunks are accumulated into one complete message before parsing. |

### `[protocol]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `tool_protocol` | enum | `fenced` | `fenced` \| `hermes`. See PROTOCOLS. |
| `enable_grammar` | bool | `false` | GBNF constraint, `hermes` only. With `fenced` it is ignored with a warning. With `strip_think = false` it is **fatal**. When on, `api_path` is forced to `/completion`. |
| `grammar_file` | string | `grammars/hermes_tool_call.gbnf` | Fatal if `enable_grammar = true` and the file is missing. |
| `strip_think` | bool | `true` | Move every `<think>…</think>` block into the message's reasoning field before parsing actions. |
| `strip_think_from_history` | bool | `true` | Also drop reasoning from replayed history (saves context). It is always written to the transcript regardless. |
| `stop_sequences` | string | *(protocol default)* | Comma-separated override, applied after the protocol's own. Entries are trimmed and used verbatim; there is no escape syntax, so a sequence containing a newline cannot be expressed here. |

### `[agent]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `system_prompt_file` | string | `config/system_prompt.txt` | The Tier 1 skills index is appended to its contents at startup. |
| `max_iterations` | int | `8` | Hard cap on turns per task. |
| `repetition_threshold` | int | `2` | Identical consecutive command sets before aborting. Counts occurrences, so `2` aborts on the second identical turn. |
| `work_dir` | string | `./workspace` | Execution directory, created if absent. Enforced with `chdir` in the child, not by asking the model. |
| `sessions_dir` | string | `./logs/sessions` | Where named sessions live, one transcript each. |
| `transcript_log_path` | string | `./logs/transcript.jsonl` | Setting this explicitly opts out of named sessions. `--resume` and `--session` override it. |
| `transcript_max_bytes` | long | `0` | Cap on the transcript **file** size, counting what earlier runs already wrote. `0` = unlimited. |
| `transcript_max_bytes_per_run` | long | `0` | Cap on what **one run** appends. `0` = unlimited. Must be ≤ `transcript_max_bytes`; a larger value is clamped down with a warning. A non-zero cap below 256 bytes cannot hold one entry plus the cutoff marker and is warned about. |
| `log_level` | enum | `info` | `error` \| `warn` \| `info` \| `debug`. Console (stderr) verbosity only; the transcript is unaffected. |
| `mode` | enum | `interactive` | `interactive` \| `oneshot`. |
| `color_output` | enum | `auto` | `auto` \| `always` \| `never`. `auto` colourises only when stdout is a TTY. |

### `[tools]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `bash_timeout_s` | int | `60` | Per command. On expiry the whole process **group** is `SIGKILL`ed, so backgrounded grandchildren die too. |
| `output_head_lines` | int | `40` | |
| `output_tail_lines` | int | `40` | |
| `output_max_bytes` | int | `4096` | Hard cap on the result string: the truncation marker is reserved inside it. Applied after line selection, so it can cut mid-line. |
| `command_filter_mode` | enum | `whitelist` | `whitelist` \| `blacklist` \| `none`. See COMMAND FILTERING. |
| `whitelist` | string | `ls,cat,grep,find,wc,head,tail,pwd,echo,mkdir,cp,mv,touch,make,gcc,g++,python3,git,sed,awk,diff,wget` | Head tokens allowed in `whitelist` mode. |
| `blacklist` | string | `rm,dd,mkfs,shutdown,reboot,sudo,su,chmod,chown,kill,pkill,killall,mount,umount` | Head tokens refused in `blacklist` mode. |
| `on_blocked_command` | enum | `ask` | `ask` \| `deny`. What happens when the filter blocks something. |
| `confirm_mode` | enum | `never` | `always` \| `never`. Confirmation for commands the filter already allowed. |
| `confirm_commands` | string | *(empty)* | Head tokens to confirm before every run even when allowed, in any segment of the command. Independent of `confirm_mode`. The shipped example sets `python3,awk,sed`. |
| `allow_metacharacters` | bool | `false` | When false, a command containing `$(`, a backtick, `>`, `<` or a lone `&` is refused outright, in every filter mode. |

### `[skills]`

| Key | Type | Default | Notes |
|---|---|---|---|
| `skills_dir` | string | `./skills` | Scanned at every startup. Its absolute path is prepended to each command's `PATH` and exported as `TINYCA_SKILLS_DIR`. |
| `tier1_enabled` | bool | `true` | Append the skills index to the system prompt. |
| `tier2_enabled` | bool | `false` | Honour `show_skill: <name>`. Off by default: emitting an exact trigger line is not something a 1.5B model does reliably. |
| `max_tier1_skills` | int | `8` | Cap on *listed* skills; beyond this small-model selection degrades. |
| `tier1_exclude` | string | `add_skill` | Skill names kept out of the index while staying runnable, auto-allowed and Tier-2 loadable. |
| `skill_checks` | string | `filter_on_create,filter_at_startup` | Any subset of `filter_on_create`, `confirm_on_create`, `filter_at_startup`, or `none`. See SKILLS. |
| `add_skill_name` | string | `add_skill` | Which skill `confirm_on_create` applies to. |

## SESSIONS

A session **is** its transcript: `<sessions_dir>/<name>.jsonl`. There is no index
file — the directory is the registry, so name uniqueness is one filesystem check
and sessions can be archived or deleted with `mv` and `rm`.

Names may contain letters, digits, `_`, `-` and `.`; they may not be empty, start
with `.`, or contain anything else (no `/`, no spaces, no `:`), so a session file
is safe to copy anywhere, including FAT/exFAT media.

Which file a run uses, in precedence order:

1. `--resume <name|file>` — that session.
2. `--session <name>` — `<sessions_dir>/<name>.jsonl`, created if new.
3. An explicit `agent.transcript_log_path` — that path, unnamed.
4. Otherwise a new session named `YYYY-MM-DD_hh-mm-ss_<work dir basename>` in
   local time, e.g. `2026-11-01_18-21-01_workspace`. The name is claimed by
   creating the file atomically, so two runs starting in the same second cannot
   share a transcript; a run that dies later in startup leaves that empty
   session behind. The basename is sanitised to
   the legal charset; `/` or `.` as a work dir yields `workdir`. Two sessions
   starting in the same second in the same directory get `-2`, `-3`, … rather
   than sharing a transcript.

`--list-sessions` and `--rename-to` both act on the session store and exit
before any of the agent's own setup runs — no model, work directory or command
filter is involved. They are checked in that order of precedence: a run given
both performs the rename and exits, so the listing would show a state the
rename has already changed. `/rename <name>` renames the live session (the file
is moved, the logger follows it, and a `session_rename` entry records the move);
`--session <old> --rename-to <new>` does it from a script. A rename is refused,
with nothing changed, when the new name is unusable, already taken, identical to
the current one, or the source does not exist.

### Resuming

`--resume` replays `user_task`, `response` and `tool_result` entries as history.
`request` entries are skipped (they are a re-encoding of history), and no stored
`system` message is ever replayed. Reconciliation is **transactional**: every
difference is decided before anything is written, so declining at any point
leaves the transcript byte-identical.

Two things are reconciled:

- **System prompt** — regenerated every startup from `system_prompt_file` plus a
  live scan of `skills_dir`, so it changes when skills change. On a difference,
  `--resume-prompt` decides. `update` adopts the new prompt, records a
  `system_prompt_update` entry and continues; the next request re-sends the whole
  session, which invalidates the server's KV prefix and reprocesses the full
  prompt (minutes on slow hardware).
- **Work directory** — every run records its absolute `work_dir` in a
  `session_start` entry. Resuming elsewhere means the replayed history's relative
  paths now refer to different files, so `--resume-work-dir` decides. `continue`
  keeps the current directory and records a `work_dir_update` entry naming the
  previous one.

A transcript missing either record (an older file) is reported and resumed with
the current values.

## SKILLS

A skill is an **executable `*.sh` script** in `skills_dir` with this header:

```bash
#!/bin/bash
# @name: fetch_url
# @description: Download a file from a URL into a destination folder using wget.
# @usage: fetch_url.sh <url> <destination_folder>
```

All three fields are required within the first 20 lines; a file missing any of
them is skipped with a warning, as is a non-executable or non-`.sh` file.

**Tier 1** (always resident): the index above is appended to the system prompt,
~15–20 tokens per skill, so the model's job is reduced to *selecting* a script.
Skills are invoked **by bare filename** — commands run with `cwd = work_dir`,
which is not where skills live, so their directory is prepended to the child's
`PATH`. Both the path and the bare name are auto-allowed by the command filter
regardless of filter mode.

**Tier 2** (`tier2_enabled`, off by default): the full script body is appended to
the conversation as a `user` message on demand. The model triggers it by emitting
`show_skill: <name>` on a line by itself — such a turn executes nothing — and you
can type the same line in the REPL, which is answered locally with no LLM call.

### Skills the model writes

`add_skill.sh` registers a new skill at runtime:

```
add_skill.sh <name> <description> <usage> <command line...>
```

The body arrives as arguments, not on stdin, because with
`allow_metacharacters = false` the model cannot type a heredoc or a redirection.
Single-line bodies only. After a successful creation the registry is re-scanned,
the new skill is auto-allowed immediately, and a `user` message announces it; the
system prompt is **not** rebuilt mid-session (history is append-only and the KV
prefix must survive), so the Tier 1 index picks it up at the next start.

A skill is auto-allowed as a whole, so its *contents* would otherwise bypass the
command filter entirely. `skill_checks` selects the safeguards:

| Token | Effect |
|---|---|
| `filter_on_create` | A newly created skill is screened; if its body invokes a program the filter forbids, the file is **deleted** and the model is told why. |
| `confirm_on_create` | `add_skill_name` must be confirmed by the user before every run. |
| `filter_at_startup` | Every discovered skill is screened before the system prompt is built; a failing skill is dropped entirely — not advertised, not auto-allowed — and reported on stderr. |

The screen is a token-level check of each line's command heads in every segment,
so `echo hi; rm -rf /` on line 2 of a skill is caught. It is not a shell parser:
`eval`, `source` and `.` are deliberately not exempt, and a whitelisted
interpreter can still run anything.

## PROTOCOLS

**`fenced`** (default, one action per turn). The model emits exactly one:

````
```bash
<command>
```
````

Stop sequences are `` ```\n `` and `` \n``` ``, so generation halts before the
model can invent the command's output; a missing closing fence is therefore not
an error. Multiple blocks: the first runs and a warning is logged.

Results come back as `role: "tool"` messages labelled with their command —
`` Output of `<cmd>`: `` when it ran, `` Command `<cmd>` was not run. ``
otherwise. The label is not decoration; SPEC.md §6.1 has the measurement that
explains why it is mandatory.

**`hermes`** (parallel-capable). The model emits one or more:

```
<tool_call>
{"name": "bash", "arguments": {"command": "ls -la"}}
</tool_call>
```

Structured `tool_calls` arrays are the primary path; raw `<tool_call>` text in
the content is handled as a fallback. Calls execute sequentially in emission
order, correlated by id. Malformed JSON does not abort the task: a `tool` message
describing the parse error is appended and counts against `max_iterations`. At
most 64 parallel calls are accepted from one streamed turn.

## COMMAND FILTERING

Metacharacter rejection runs first and in every mode: unless
`allow_metacharacters = true`, a command containing `$(`, a backtick, `>`, `<` or
a lone `&` is refused. (`&&` is a separator, not a metacharacter.)

Then, in `whitelist` and `blacklist` mode, the command is split on `;`, `&&`,
`||`, `|` and newline — **outside quotes only** — leading `VAR=value`
assignments are stripped from each segment, and each segment's head token is
checked. Every segment must pass, so `ls; rm -rf /` is refused on `rm`. Quote
handling follows bash, including backslash escapes; an unbalanced quote swallows
the rest of the line as one segment, as bash does.

The head token is **unquoted before it is compared**, because bash
word-expands a command name before looking it up: `"rm"`, `'rm'`, `\rm`,
`r""m` and `$'rm'` all name `rm`, and all of them are refused by a blacklist
containing `rm`. The blacklist additionally matches the **basename**, so
`/bin/rm` is caught. The whitelist does not match basenames — listing `ls` must
not bless `/opt/anything/ls` — so a path-qualified program fails closed there.

`none` mode skips segmentation entirely. `none` plus `allow_metacharacters = true`
filters nothing at all and is documented as unsafe — `oneshot` sandboxes only.

On a block, `on_blocked_command` decides: `deny` refuses outright and tells the
model it was a filter decision; `ask` prompts you with the command and the
offending token, and a refusal is reported to the model as the *user's* decision,
which is a different signal from an automatic denial.

**This is not a sandbox.** It screens which *programs* run. Whitelisting an
interpreter grants everything that interpreter can do: `python3 -c "…"` passes a
default whitelist, as do `awk`, `sed -i`, `make`, `gcc` and `git`. `work_dir` and
`bash_timeout_s` are the actual containment; `confirm_commands` puts specific
programs behind a prompt.

## INTERACTIVE MODE

| Command | Effect |
|---|---|
| `/reset` | Clear history, keep the system prompt |
| `/skills` | List discovered skills (all of them, including ones excluded from the index) |
| `/sessions` | List stored sessions; the current one is marked `*` |
| `/rename <name>` | Rename the live session |
| `/history` | Print the current message history |
| `/toggle_show_thinking` | Show or hide `<think>` content as it streams (off by default) |
| `/help` | Print usage |
| `/quit` | Exit |
| `show_skill: <name>` | Print a skill's source and add it to the conversation (needs `tier2_enabled`) |

Line editing is a minimal `termios` raw-mode editor: character echo, backspace,
and TAB completion against the slash commands, skill names after `show_skill:`,
and session names after `/rename `. An ambiguous prefix prints the candidates
once. When stdin is not a TTY it falls back to plain line-buffered input.

`Ctrl+C` aborts the current task — including mid-generation and mid-command — and
returns to the prompt; it does not kill the session. Every thread blocks
`SIGINT`/`SIGTERM` and one dedicated control thread waits on them with
`sigwait`, so an interrupt cuts through a blocking HTTP read (the socket is
closed under it) and a running command (the process group is killed) instead of
waiting out `request_timeout_s` or `bash_timeout_s`.

Four things are visually distinguished when colour is on: your input (bold cyan),
`<think>` reasoning (dim), the model's reply (green), and the command about to
run (bold yellow, prefixed `▶`). Command output is printed unstyled.

## TRANSCRIPT

One JSON object per line: `{"ts":<unix_ms>,"kind":"<kind>","payload":{…}}`.
Append-only; never rewritten or rotated.

| Kind | Payload |
|---|---|
| `session_start` | `work_dir` this run used |
| `user_task` | `text` of the task |
| `request` | The exact request body sent to the server, including `messages[0]`, the system prompt |
| `response` | `role`, `content`, `reasoning`, `tool_calls`, `tool_call_id` |
| `tool_result` | `id`, `name`, `ok`, `content`, `command`, `executed` |
| `skills_changed` | `note` announced to the model after a skill appeared |
| `tier2_skill` | `name` requested and whether it was `found` |
| `system_prompt_update` | The prompt a resumed session adopted |
| `work_dir_update` | `work_dir` adopted and the `previous` one |
| `session_rename` | `previous` and `current` names |
| `transcript_cutoff` | Which byte cap stopped further entries |

## ENVIRONMENT

Variables TinyCA sets **for the commands it runs** (not read from your shell):

| Variable | Value |
|---|---|
| `PATH` | The absolute `skills_dir` prepended to the inherited `PATH`, so skills resolve by bare filename. |
| `TINYCA_SKILLS_DIR` | Absolute `skills_dir`, so a skill (notably `add_skill.sh`) can find the skills directory while running with `cwd = work_dir`. |

The child environment is built before `fork`, so nothing allocates between
`fork` and `exec`.

## FILES

| Path | Purpose |
|---|---|
| `config/tinyca.ini` | The config TinyCA loads by default, committed and ready to run. Point `-c` elsewhere to use another one. |
| `config/system_prompt.txt` | System prompt; the skills index is appended to it at startup. |
| `skills/*.sh` | Skills. `skills/.skill_template` is the 0755 template `add_skill.sh` copies. |
| `grammars/hermes_tool_call.gbnf` | GBNF used when `enable_grammar = true`. |
| `logs/sessions/*.jsonl` | Named sessions. |
| `workspace/` | Default `work_dir`. |

## EXAMPLES

Start the server this was tuned for:

```bash
llama-server -m qwen2.5-coder-1.5b-instruct-q4_k_m.gguf \
  -ngl 99 -c 4096 --host 0.0.0.0 --port 8080 --jinja
```

One-shot task, no prompts:

```bash
tinyca -p "list the files in the workspace and report how many there are" --no-confirm
```

One task, then stay interactive:

```bash
tinyca "count the lines in main.cpp"
```

Named session, continued the next day:

```bash
tinyca --session sort-bug
tinyca --resume sort-bug
```

Rename a session, then list them:

```bash
tinyca --session 2026-11-01_18-21-01_workspace --rename-to sort-bug
tinyca --list-sessions
```

Parse without executing, to see what the model would do:

```bash
tinyca --dry-run -p "clean up the build directory"
```

Unattended run that must not stop for prompts (clear `confirm_commands` in the
config first if it lists a program the task needs):

```bash
tinyca -p "run the test suite" --no-confirm --resume-prompt quit
```

## LIMITATIONS

TinyCA is built for a model that will disappoint you in specific, measured ways.
The table of what was observed — skipped prerequisite steps, no adaptation to
error messages, failure at content-level reasoning, occasional empty replies,
derailment by meta-capabilities — together with the example and the mitigation
for each, is **SPEC.md §16**. It is not repeated here, because a limitation
documented twice is a limitation that will be updated once.

The operational consequences, which is what this manual is for: keep tasks
single-step, prefer whole-file rewrites over exact-string edits, leave
`skills.tier2_enabled = false` and `skills.tier1_exclude = add_skill` alone
unless the model is a capable one, and read the transcript rather than trusting
a reported success.

## SEE ALSO

[SPEC.md](../SPEC.md) — design decisions and §16's capability ceiling.
[README.md](../README.md) — quick start and the documentation map.
[REQUIREMENTS.md](../REQUIREMENTS.md) — packages and toolchains.

The development record (build brief, engineering journal, on-device run
write-ups) is a separate tree under `dev/`, not linked from here on purpose.
