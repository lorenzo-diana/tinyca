# SPEC — TinyCA

A minimal C++11 coding agent for small local LLMs.

## 1. Scope

TinyCA drives a tool-use loop against an OpenAI-compatible endpoint exposed by
`llama-server`. Target model: **Qwen2.5-Coder-1.5B-Instruct** (Q4_K_M) on a Jetson Nano.

Design premise: at ~1.5B parameters the model is the bottleneck, not the harness. Every
decision favours *format stability and low token count* over feature richness.

**Local-only, by design.** TinyCA targets resource-constrained devices running a local
inference server — `llama-server` today, a local vLLM instance potentially in the
future. There is no remote/hosted-provider support (no Anthropic, no OpenAI API) and
none is planned: transport and codec sit behind interfaces purely to swap *local*
backends, not to add network-hosted ones. This keeps the build single-configuration —
no feature macros, no optional TLS linkage.

## 2. Build configuration

One build, no macros, no feature flags. `make` builds `build/tinyca`; nothing is
compiled out or in conditionally. The build never links OpenSSL — there is no HTTPS
endpoint to reach.

## 3. Layout

```
TinyCA/
  Makefile
  README.md SPEC.md REQUIREMENTS.md LICENSE
  third_party/{json.hpp, httplib.h, VERSIONS.md}
  include/tinyca/
    config.h        ini_parser.h      string_util.h
    transport.h     http_transport.h  message.h  codec.h
    protocol.h      fenced_protocol.h hermes_protocol.h
    skills.h        executor.h        command_filter.h
    agent.h         logger.h          control.h
  src/            # one .cpp per header
  tools/mockllm/  # scripted mock LLM server
  config/{tinyca.ini, system_prompt.txt}
  docs/MANUAL.md                 # project documentation tree
  dev/                           # development record, deliberately disjoint
  skills/fetch_url.sh
  grammars/hermes_tool_call.gbnf
  tests/{fixtures/*.json, run_tests.sh, test_*.cpp}
```

## 4. Configuration

INI format: `key = value`, `#` or `;` comments, `[section]` headers, values trimmed, no
quoting, `\n` unescaped in values. The parser is ~100 lines of `find`/`substr`. Keys are
addressed as `section.key`. Precedence: CLI flag > config file > built-in default.

**The complete key reference — every key, its type, its default and what it does — lives
in `docs/MANUAL.md`, and only there.** This section records the *decisions* behind the
five sections, which is what a design document is for; duplicating the table here is how
the two copies drift apart.

The system prompt lives in a **separate plain-text file** (`agent.system_prompt_file`) —
multi-line prompts do not belong in INI values.

Nothing in the file is fatal: an unknown key, a malformed line, an unusable value and a
duplicate key each produce a warning and leave the built-in default standing. The only
fatal configuration states are the two `enable_grammar` combinations in §6.3. This is a
deliberate choice for a device that may be running unattended — a typo must not stop a
session that would otherwise work.

### 4.1 `[llm]` — talking to the server

One local, OpenAI-compatible endpoint, described by `base_url` + `api_path`. The
timeouts are generous (`request_timeout_s` defaults to 900) because generation on the
target hardware runs at ~4–5 tok/s, and a timeout tuned for a datacentre would abort
every real turn. `context_tokens` is *declared*, not discovered: the server does not
report its `-c`, so the budget check in §9 can only be as right as this value.
`api_key` exists for API-compatibility only — `llama-server` ignores it, and the literal
`none` means "send no header".

### 4.2 `[protocol]` — how actions are expressed

`tool_protocol` selects `fenced` (default) or `hermes`; §6 explains why a markdown fence
beats structured tool calls on a 1.5B model. `enable_grammar` is `hermes`-only and
rewrites `api_path` (§6.3). `strip_think` and `strip_think_from_history` are separate
keys because the useful combination is "parse it out, log it, do not resend it" —
reasoning costs context on every replay. `stop_sequences` overrides the protocol's own
and is literal text: an INI value cannot express a newline, so the `fenced` defaults
(which contain one) are reachable only as protocol defaults.

### 4.3 `[agent]` — the loop and its records

`max_iterations` and `repetition_threshold` bound a model that cannot tell it is stuck
(§9). `work_dir` is the execution jail, enforced by `chdir` in the child rather than by
asking the model to behave. The transcript keys are diagnostics, not features: on the
target board the transcript is the only post-mortem available, which is why there are
two independent caps (§9.2) and why `--resume` reads this file back (§12.2).
`sessions_dir` makes the filesystem the session registry (§12.3).

### 4.4 `[tools]` — what a command may do

The filter keys implement §8.1. `command_filter_mode` defaults to `whitelist` because it
is the only mode that fails closed. The head-token lists are *programs*, not command
lines — see §8.1 for what that does and does not buy. `allow_metacharacters = false`
blocks the classic injection shapes before any token logic runs, at the cost of blocking
heredocs too, which is why the sanctioned sandbox combination for scripted runs is
`command_filter_mode = none` plus `allow_metacharacters = true`. `confirm_commands` is
independent of `confirm_mode` for the same reason `on_blocked_command` is: "ask me about
this program" and "ask me about everything" are different policies.

### 4.5 `[skills]` — what the model is told about

`tier1_enabled` controls the always-resident index, `tier2_enabled` the on-demand source
(§7). `max_tier1_skills` caps the index because small-model selection degrades as the
list grows. `tier1_exclude` and `skill_checks` exist because a skill the *model* wrote is
a different risk from one the operator wrote — §7.1 and §16.

## 5. Core interfaces

```cpp
// transport.h
struct HttpResponse { int status; std::string body; std::string error; };
class ITransport {
public:
    virtual ~ITransport() {}
    virtual HttpResponse post(const std::string& path,
                              const std::string& body,
                              const std::vector<std::pair<std::string,std::string> >& headers) = 0;
};

// message.h
struct ToolCall { std::string id, name, arguments; };   // arguments = raw JSON
struct ToolResult { std::string id, name, content; bool ok; };
struct Message {
    std::string role;                    // system | user | assistant | tool
    std::string content;
    std::string reasoning;               // extracted <think>, never resent when stripped
    std::vector<ToolCall> tool_calls;    // assistant turns
    std::string tool_call_id;            // tool turns
};

// codec.h — wire-format serialisation
class ICodec {
public:
    virtual ~ICodec() {}
    virtual std::string encode_request(const std::vector<Message>&, const RequestOptions&) = 0;
    virtual bool decode_response(const std::string& body, Message& out, std::string& err) = 0;
};
// OpenAIChatCodec is the only codec — the single wire format every local server
// (llama-server today, vLLM potentially later) exposes via an OpenAI-compatible endpoint.

// protocol.h — how actions are expressed in/extracted from model output
struct Action { std::string id, command, tool_name, raw_args; };
class IProtocol {
public:
    virtual ~IProtocol() {}
    virtual void augment_request(RequestOptions&) = 0;              // tools[], grammar, stop
    virtual std::vector<std::string> stop_sequences() const = 0;
    virtual bool parse(const Message& assistant, std::vector<Action>& out, std::string& err) = 0;
    virtual std::vector<Message> format_results(const std::vector<ToolResult>&) = 0;
    virtual bool supports_parallel() const = 0;
};
```

`Agent` depends only on `ITransport`, `ICodec`, `IProtocol`, `SkillRegistry`, `Executor`.

## 6. Protocols

### 6.1 `fenced` (default, single action per turn)

Model emits exactly one:

    ```bash
    <command>
    ```

- `stop_sequences` = `["```\n", "\n```"]` — generation halts before the model can invent
  the command's output.
- Parsing: `find("```bash")`, then `find("```", start)`. If the stop sequence consumed the
  closing fence, treat end-of-text as the terminator.
- `supports_parallel()` = **false**. Exactly one action per turn; if multiple blocks are
  present, execute the first and warn.
- No action found and content non-empty ⇒ task complete, return the text.
- **Results are labelled with the command they came from**: `` Output of `<command>`: `` for
  something that ran, `` Command `<command>` was not run. `` for something the filter or the
  user stopped. This is required, not decorative. Qwen's chat template renders a
  `role: "tool"` message as a user turn wrapped in `<tool_response>`, a shape the model only
  saw in training *after* it emitted a structured `<tool_call>`; following a markdown fence,
  an unlabelled `<tool_response>` does not read as that fence's output and the model
  re-emits the same command, which the repetition detector then aborts. Measured on
  Qwen2.5-Coder-1.5B at `temperature = 0.2`: unlabelled looped 2/2 (as `tool` and as
  `user`), labelled answered correctly 2/2. The role is not what matters; the label is.
- Rationale: markdown fences are enormously more frequent in pretraining than tool-call
  JSON, so a 1.5B model reproduces them far more reliably.

### 6.2 `hermes` (parallel-capable)

Model emits one or more:

    <tool_call>
    {"name": "bash", "arguments": {"command": "ls -la"}}
    </tool_call>

- `stop_sequences` = `["</tool_call>"]`; the terminator is re-appended before parsing.
- `supports_parallel()` = **true**. Multiple blocks are collected and executed
  **sequentially in emission order** — bash has side effects, and four A57 cores make
  concurrency pointless. All results are returned together, correlated by generated id.
- Tool schemas are sent in `tools[]` when the endpoint supports it (see the downstream
  finding from `llama.cpp_ubuntu_18`); otherwise they are rendered into the system prompt.
- Malformed JSON inside a block ⇒ do not abort. Append a `tool` message describing the
  parse error and let the model retry; count it against `max_iterations`.

### 6.3 GBNF

When `enable_grammar = true`, the file at `grammar_file` is sent in the request's
`grammar` field. This makes malformed tool calls impossible by construction — it does
**not** prevent semantically wrong calls.

Per `llama.cpp_ubuntu_18`'s findings, `grammar` is honoured only on the native
`/completion` endpoint and is silently ignored on `/v1/chat/completions`. TinyCA does
not ask the user to get this right: **when `enable_grammar = true`, TinyCA overrides
`api_path` to `/completion` automatically** (logging that it did so, at `info` level),
regardless of whatever `api_path` is configured. `/completion` takes a flattened
`prompt` string rather than a `messages` array, so the request encoder renders the
conversation history into one prompt using the model's own chat-template markers
(the same `<|im_start|>`/`<|im_end|>` tags `--jinja` applies) instead of posting
`messages[]` — this rendering is the one piece of model-specific knowledge the
otherwise template-agnostic `OpenAIChatCodec` needs when grammar is on.

Two remaining startup checks:
- `enable_grammar` with `tool_protocol = fenced` ⇒ warn and ignore.
- `enable_grammar` with `strip_think = false` ⇒ **fatal**: a grammar constraining output
  to a tool call forbids a preceding `<think>` block.

## 7. Skills — two tiers

**Discovery.** At startup, scan `skills_dir` (POSIX `opendir` + `stat`) for executable
`*.sh` files. Parse the metadata header:

```bash
#!/bin/bash
# @name: fetch_url
# @description: Download a file from a URL into a destination folder.
# @usage: fetch_url.sh <url> <destination_folder>
```

`@name`, `@description`, `@usage` are required; a file missing any of them is skipped
with a warning.

**Tier 1 (always resident).** Append to the system prompt:

```
Available skills (run them with the bash tool):
- fetch_url: Download a file from a URL into a destination folder.
  usage: fetch_url.sh <url> <destination_folder>
```

Skills are advertised and invoked **by bare filename**. Commands execute with
`cwd = work_dir`, which is not where skills live, so `Executor` prepends the
absolute `skills_dir` to the child's `PATH`; a repo-root-relative form such as
`skills/fetch_url.sh` would fail with exit 127 from inside the work directory.

~15–20 tokens per skill. Because this lives in the immutable system prompt, it is
processed once and then served from the KV cache prefix — a *fixed* cost, not per-turn.
Cap at `max_tier1_skills`; log a warning if more are found.

**Tier 2 (on demand), gated by `tier2_enabled`.** When the model runs a skill whose
behaviour it needs to inspect, or explicitly requests it, the full script body is
appended as a new `user` message (append-only, so the prefix survives). Trigger: the
model emits `show_skill: <name>` on a line by itself. Unknown name ⇒ append an error
message listing valid names. A turn containing the trigger executes **nothing**, even if
it also contained a fenced block — the model asked to read before acting. The same
`show_skill: <name>` line typed by the *user* in the REPL is handled locally, with no
LLM call, appending the source to history for the next task to use.

**Skills are executed as bash and are auto-allowed**: both their path and their bare
filename are added to the whitelist (if `command_filter_mode = whitelist`) or exempted
from the blacklist (if `command_filter_mode = blacklist`) at startup, regardless of
`command_filter_mode`.

Design rule to document in the README: prefer skills written as *scripts* over skills
written as *prose instructions*. Script skills reduce the model's job to selection, which
a 1.5B model can do; prose skills require instruction following, which it cannot do
reliably.

### 7.1 Model-written skills — `add_skill` and its safeguards

`skills/add_skill.sh` lets the agent register a new skill at runtime and call it
by name in a later turn. Its body arrives as **arguments**, not on stdin: with
the default `allow_metacharacters = false` the model cannot type a heredoc or a
redirection, so a stdin-fed design would be unusable exactly where it matters.
Only single-line bodies are supported. The new file is created by copying
`skills/.skill_template` (committed with mode 0755) rather than by `chmod`,
which is absent from the default whitelist — a skill that needed `chmod` would
fail its own screen below. `Executor` exports `TINYCA_SKILLS_DIR` so the script
knows where to write, since commands run with `cwd = work_dir`.

**Why safeguards are needed.** A skill is auto-allowed as a whole (§7), so the
*contents* of a model-written script would otherwise never face the command
filter: the model could write `rm -rf /` into a skill and then run the skill.
`skills.skill_checks` selects which safeguards are enforced, as a
comma-separated subset (default `filter_on_create,filter_at_startup`, `none`
disables all three):

| Token | Effect |
|---|---|
| `filter_on_create` | A skill discovered after a turn's commands ran is screened; if it invokes a forbidden program the file is **deleted** and the model is told why. Deleted, not hidden — an executable file left in `skills_dir` stays invocable by name. |
| `confirm_on_create` | The skill named by `skills.add_skill_name` (default `add_skill`) is confirmed with the user before every run, independent of `confirm_mode`. |
| `filter_at_startup` | Every discovered skill is screened before the system prompt is built. A failing skill is dropped from the registry entirely: not advertised in Tier 1, not auto-allowed, and the user is told on stderr. |

**What the screen is.** A token-level check of each line's command heads
against the active filter (`CommandFilter::check_tokens_only`, which skips the
metacharacter rule because scripts legitimately redirect). Shell keywords,
`VAR=value` lines and non-command tokens such as `case` globs are skipped.
Every segment of a line is inspected, so `echo hi; rm -rf /` is caught, not
just the first command. It is **not** a shell parser: `eval`, `source` and `.`
are deliberately *not* skipped, and a forbidden program reached only through
indirection a static screen cannot see is not caught. The segmenter is also
not quote-aware (§8.1), so a separator inside a quoted string can produce a
false rejection — the skills shipped here avoid quoted separators for that
reason.

**Advertised vs. available.** `skills.tier1_exclude` keeps a skill out of the Tier 1
index without disabling it: it stays discovered, auto-allowed and loadable through
Tier 2, so the operator (or a capable model told about it explicitly) can still run
it. `add_skill` is excluded by default — measured, a 1.5B model offered a
"register a skill" capability treats it as a shortcut for doing the work and
abandons the task. Clear the list when pointing TinyCA at a model that can use it.

**After a skill is created**, the registry is re-scanned, the survivor's path
and bare name are auto-allowed immediately, and a `user` message announces it
to the model. The system prompt is **not** rebuilt mid-session: history is
append-only and rewriting the prefix would invalidate llama-server's KV cache.
The Tier 1 index picks the new skill up at the next startup, since the prompt
is generated from a live scan of `skills_dir` every time TinyCA starts.

## 8. Bash executor and whitelist

**Execution.** `fork` + `execvp` of `/bin/bash -c <command>` with `cwd = work_dir`, stdout
and stderr merged through a pipe. `bash_timeout_s` enforced by `SIGKILL` on the process
group (not just the child, or backgrounded grandchildren survive).

**Truncation.** Keep the first `output_head_lines` and last `output_tail_lines`, inserting
`... [N lines omitted] ...`, then hard-cap at `output_max_bytes`. The cap is hard in the
strict sense: the `... [truncated at output_max_bytes] ...` marker is reserved inside it,
so the returned string never exceeds `output_max_bytes`. Without this a single
`pip install` log destroys the context window. Always report the true exit code.

### 8.1 Command filtering — `command_filter_mode`

Three mutually exclusive modes, selected by `command_filter_mode`. In every mode,
metacharacter rejection runs first and is unconditional: reject outright if the
command contains `$(`, backtick, `>`, `>>`, `<`, or `&` and `allow_metacharacters =
false`. This check does not depend on the filter mode — it catches the classic
injection shapes before any token-level logic runs.

For `whitelist` and `blacklist` modes, the same segmentation applies:
1. Split on the separators `;`, `&&`, `||`, `|`, and newline — **outside quotes
   only**. Quote handling follows bash: `'...'` is literal through to the next
   `'`; inside `"..."` a backslash escapes the next character; outside quotes a
   backslash escapes the next character. An unbalanced quote swallows the rest
   of the line as one segment, which is what bash does with it too. Without
   this, `echo "hello; world"` was rejected on the token `world"` — it failed
   closed, so it was a usability defect rather than a hole, but it also made
   §7.1's screen of skill bodies unable to tolerate quoted punctuation.
2. For **every** segment: trim, strip leading `VAR=value` assignments, take the head
   token, **unquote it the way bash word-expands a command name** (`"rm"`, `'rm'`,
   `\rm`, `r""m` and `$'rm'` all name `rm`), and resolve `./skills/x.sh` forms.
   Without the unquoting step a blacklist was bypassed by one pair of quotes, and a
   whitelist rejected a model that quoted its own command.
3. **`whitelist`**: every segment's head token must be a member of `whitelist`. Any
   segment failing ⇒ reject the whole command and return a `tool` message naming the
   offending token, so the model can correct itself.
4. **`blacklist`**: every segment's head token must *not* be a member of `blacklist`,
   **matched by name or by basename** — the blacklist names programs, and `/bin/rm`
   is the program `rm`. The whitelist deliberately does *not* match basenames:
   listing `ls` must not bless `/opt/anything/ls`, so a path-qualified program fails
   closed there.
   Any segment matching ⇒ reject the whole command and name the offending token, same
   as above. Everything not on the list is allowed.
5. **`none`**: no segmentation, no token check. Metacharacter rejection (above) still
   applies unless `allow_metacharacters = true`, in which case nothing is filtered at
   all — document this combination as unsafe, for `oneshot` sandboxes only.

A check on the first token alone is not acceptable in `whitelist` or `blacklist` mode:
`ls; rm -rf /` must be caught by segment 2, not waved through because `ls` is fine.

**What this filter is and is not.** It screens the *programs* a command invokes.
It is not a sandbox, and whitelisting an interpreter grants everything that
interpreter can do: `python3 -c "import os; os.system('id')"` passes a default
whitelist, because its head token is `python3`. (Before segmentation became
quote-aware that particular line was rejected — by accident, on the token
`os.system('id')"`, not by design; `python3 -c "import os,subprocess"` was
always allowed.) The same holds for `awk`, `sed -i`, `make`, `gcc` and
`git`. Treat `command_filter_mode = whitelist` as a guard against accidents and
obviously destructive commands, and `work_dir` plus `bash_timeout_s` as the
actual containment. An operator who needs more can list those
programs in `tools.confirm_commands`, which prompts before every run of a
listed head token (in any segment of the command), or drop them from the
whitelist entirely.

`confirm_commands` is independent of `confirm_mode`, like `on_blocked_command`:
`--no-confirm` does not clear it. In an unattended run there is nobody to
approve the prompt, so a listed program is refused and reported to the model as
user-blocked — fail-closed, and the reason to clear the list for scripted use.

**On a blocked command** — `on_blocked_command` decides what happens next, independent
of `confirm_mode` (which only governs commands the filter already let through):

- **`deny`**: reject outright, no interaction. The `tool` message returned to the
  model states the command was blocked by the command filter for security reasons and
  names the offending token, so it can try a different approach.
- **`ask`** (default): prompt the user interactively — command, offending token, and
  an allow/block choice, the same shape as Claude Code's own command-approval prompt.
  - Allowed ⇒ run it this one time (the filter is not permanently relaxed).
  - Blocked ⇒ the `tool` message tells the model the *user* blocked this specific
    execution, distinct from an automatic filter denial — a model that keeps hitting
    the filter itself should try another command, but one blocked by the user may be
    retried if the user is just being asked for confirmation on a legitimate action.

**Confirmation of allowed commands.** `confirm_mode = always` additionally prompts
before every command the filter already allowed (belt-and-suspenders, for maximum
caution); `never` (default) skips that extra prompt — the filter and
`on_blocked_command` are the safety net. `confirm_mode = never` plus
`command_filter_mode = none` is the fully unattended combination and must be
documented as unsafe, for `oneshot` sandboxes only.

## 9. Agent loop

```
load config -> load system prompt -> discover skills -> render Tier 1
history = [system]
append user task
for i in 0..max_iterations:
    request  = codec.encode(history, options augmented by protocol)
    response = transport.post(...)
    msg      = codec.decode(response)
    if strip_think: extract <think> into msg.reasoning
    append msg to history            # ALWAYS as role=assistant, ALWAYS append-only
    actions = protocol.parse(msg)
    if actions.empty():
        if msg.content is blank: nudge once, else abort   # empty turn != done
        return msg.content                               # task complete
    if repetition_detector.trips(actions): abort with explanation
    if !protocol.supports_parallel() and actions.size() > 1: keep first, warn
    results = executor.run_sequential(actions)     # whitelist + confirm + truncate
    append protocol.format_results(results)
return "iteration limit reached"
```

**Empty turns.** A turn with no action *and* no text is a failed turn, not a
finished task: the model occasionally returns `content: ""` with
`finish_reason: "stop"`, and returning that as the final answer reports failure
as success. The first empty turn gets one nudge appended as a result message and
the loop continues (counting against `max_iterations`); a second consecutive
empty turn aborts with `"the model returned an empty reply twice in a row"`.
Whitespace-only content counts as empty. The counter resets per task.

**Repetition detection.** Hash the normalised command text; if the same hash appears in
`repetition_threshold` consecutive turns, abort. Small models loop on failing commands
indefinitely and would otherwise burn the whole iteration budget.

**Thinking.** Extract `<think>...</think>` into `Message::reasoning` before parsing
actions. When `strip_think_from_history = true`, it is not resent — matching Qwen's own
convention and saving context — but it is always written to the transcript.

**Context budget.** Estimate tokens as `bytes/4`. At >80% of `context_tokens`, warn. Do
**not** auto-summarise: rewriting history invalidates the KV prefix and costs minutes.
Aborting with a clear message is the correct behaviour.

### 9.1 Always-interruptible: the control thread

Both HTTP generation (up to `request_timeout_s`, i.e. minutes on this hardware) and
bash execution (up to `bash_timeout_s`) are long blocking calls on the worker thread
running the agent loop. A stop request must cut through *either* one immediately —
checking a flag only between loop iterations is not good enough, since one iteration
is exactly where the time is spent.

Two threads, one shared `std::atomic<bool> stop_requested`:

- **Worker thread**: runs the agent loop (§9) unchanged.
- **Control thread**: started at process launch, before anything else. All threads
  mask `SIGINT`/`SIGTERM` (`pthread_sigmask`); the control thread alone calls
  `sigwait()` on them, so it is permanently parked waiting for exactly one thing —
  Ctrl+C, or an external `kill` — and nothing else competes with it for the signal.

On wake, the control thread:
1. Sets `stop_requested = true`.
2. If a bash command is currently running, `SIGKILL`s its process group immediately
   (the same process-group kill `bash_timeout_s` already uses on expiry — this just
   triggers it early, on demand rather than only on timeout).
3. If an HTTP request is in flight, closes the underlying socket from `ITransport`
   out from under the blocking read, forcing it to return with an error rather than
   waiting out `request_timeout_s`.

The worker thread treats a transport or executor error caused by `stop_requested`
exactly like reaching `max_iterations`: it stops cleanly, writes an "interrupted by
user" entry to the transcript, and returns. In `oneshot` mode this ends the process;
in `interactive` mode it aborts the in-flight task and returns to the REPL prompt —
the session itself is not killed, only the stuck turn.

### 9.2 Transcript size caps

Two independent caps bound the transcript (`0` = unlimited on either):

- `transcript_max_bytes` bounds **the file**. The counter is seeded with the
  size the transcript already had when the process started, so repeated runs
  against the same file cannot grow it past the cap.
- `transcript_max_bytes_per_run` bounds **what one run appends**, regardless of
  how large the file already is. It is the knob for "no single session may add
  more than X", useful for a long `interactive` session on a device with
  limited storage.

Whichever cap would be exceeded first stops appending for the rest of that run
and writes one final `transcript_cutoff` line naming the cap that tripped. The
marker is reserved *inside* the cap, so neither cap is ever exceeded by the act
of reporting that it was reached — a consequence of which is that a cap too
small to hold one entry plus the marker (~131 bytes) leaves the transcript
empty; `load_config` warns for any non-zero cap below 256 bytes.

A per-run cap larger than the file cap is incoherent (the file cap would always
win), so `load_config` clamps it down to the file cap and warns.

Existing entries are always left intact — oldest-first, append-only — rather
than rotated or trimmed from the front. These are diagnostic safety valves,
not a general log-rotation feature.

### 9.3 Streaming

`llama-server` supports standard OpenAI-style SSE streaming (`"stream": true`) on
both `/v1/chat/completions` and `/completion`. With `stream = true` (the default),
`ITransport` uses a streaming POST that invokes a callback per SSE `data:` chunk
instead of blocking for the full body; the CLI prints each token's text as it
arrives (§15), so the interface is never frozen waiting on the slowest part of the
pipeline — this hardware generates at ~4-5 tok/s, so a 200-token reply would
otherwise sit blank for the better part of a minute.

This is purely a presentation concern: chunks are still accumulated into one
complete `Message` before `protocol.parse()` ever runs — action parsing, stop
sequences, and `<think>` extraction all work on the fully assembled turn exactly as
in the non-streaming case. Streaming changes *when the user sees text*, not what the
rest of the loop operates on. `stream = false` falls back to the blocking request
unchanged from §9.1's description — useful mainly for `oneshot`/scripted invocations
piping output elsewhere, where incremental flushes add nothing.

## 10. Example skill — `skills/fetch_url.sh`

```bash
#!/bin/bash
# @name: fetch_url
# @description: Download a file from a URL into a destination folder using wget.
# @usage: fetch_url.sh <url> <destination_folder>
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <url> <destination_folder>" >&2
    exit 2
fi

url="$1"
dest="$2"

mkdir -p "$dest"
wget --no-verbose --timeout=30 --tries=2 -P "$dest" "$url"
echo "saved to: $dest"
```

Executable bit set, committed as-is. It is the reference for the metadata header format.

## 11. Mock LLM server (`tools/mockllm`)

An `httplib` server exposing `/v1/chat/completions`, replaying responses from a JSON
fixture in order, so the whole loop is testable on x86 with no GPU and no network.

```json
{
  "scenario": "parallel_hermes",
  "responses": [
    {"content": "<tool_call>\n{\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}\n</tool_call><tool_call>\n{\"name\":\"bash\",\"arguments\":{\"command\":\"pwd\"}}\n</tool_call>"},
    {"content": "Both commands ran successfully."}
  ]
}
```

It also records every received request to a file so tests can assert that history is
append-only and that stop sequences were sent. Required scenarios are listed in
the development record under `dev/`.

## 12. CLI

**The option and REPL-command reference lives in `docs/MANUAL.md`, and only there.**
This section records the interface *decisions*; `--help` is generated from the same code
and is the third place a reader can look, so a fourth copy of the flag list here would be
a fourth thing to keep in step.

Decisions worth stating, rather than the list:

- `-p/--prompt` implies `agent.mode = oneshot`. There is deliberately **no** `--mode`
  flag: the mode follows from what you asked for, and a flag that can contradict `-p`
  would need a precedence rule nobody would remember.
- A positional `[task]` is equivalent to `-p` **except** that it does not imply
  oneshot: it runs one task and then drops into the REPL. `-p` wins if both are given.
- Every session has a name, including the ones nobody names (§12.3), because a session
  you cannot name is one you cannot come back to.
- A resumed session reconciles what changed since it was recorded instead of adopting it
  silently (§12.2).

Interactive mode is a REPL that keeps history across tasks within a session. Its command
set, and what each command does, is in `docs/MANUAL.md`; the one design point is that
`show_skill:` typed by the *user* is answered locally with no LLM call, because the
source is on disk and asking the model to fetch it would be slower and less reliable.

Ctrl+C works at any point, including mid-generation or mid-command (§9.1) — it aborts
the current task, not the session.

### 12.2 Resuming a session, and the system prompt

`--resume <transcript.jsonl>` replays a recorded session's `user_task`,
`response` and `tool_result` entries as the starting history. `request` entries
are skipped — they are a derived re-encoding of history — and no stored
`system` message is ever replayed as history.

The system prompt is regenerated at every startup from `system_prompt_file`
plus a live scan of `skills_dir` (§7), so it can legitimately differ from the
one a stored session ran with: a skill added, removed, renamed or rejected by
§7.1's screen changes it. TinyCA extracts the stored prompt from the first
logged `request` (its `messages[0]`) and compares.

Reconciliation is **transactional**: every difference is decided before
anything is written, so a resume declined at any point leaves the transcript
byte-identical. Two things can differ — the system prompt and the work
directory — and both are asked about before either is recorded.

If the prompts differ, `--resume-prompt` decides:

| Value | Behaviour |
|---|---|
| `ask` (default on a TTY) | Print how they differ and ask. Yes ⇒ `update`, no ⇒ `quit`. |
| `update` | Adopt the current prompt and continue. The whole replayed session goes to the model on the next request, which reprocesses the full prompt — llama-server's KV prefix is invalidated, costing minutes on this hardware. The adoption is recorded as a `system_prompt_update` entry; the transcript stays append-only and is never rewritten. |
| `quit` (default when stdin is not a TTY) | Exit 1 immediately, leaving the transcript untouched. An unattended run must not silently adopt different instructions. |

**The work directory.** Every run records its absolute `work_dir` as a
`session_start` entry, so a resume can tell whether it is being resumed in the
same place. Resuming elsewhere is sometimes intended, but silently is bad: every
relative path in the replayed history (`sort.c`, `./sort_test`) now refers to
something else. When they differ TinyCA prints both directories and
`--resume-work-dir` decides:

| Value | Behaviour |
|---|---|
| `ask` (default on a TTY) | Print both paths and ask. Yes ⇒ `continue`, no ⇒ `quit`. |
| `continue` | Keep running in the current directory and record the move as a `work_dir_update` entry naming the previous one. |
| `quit` (default when stdin is not a TTY) | Exit 1, transcript untouched, with the session's previous `work_dir` named so it can be restarted there or with `agent.work_dir` set to it. |

A transcript with no `session_start` entry (written before this existed) is
reported as such and resumed in the current directory.

A resumed session continues **in the transcript it was resumed from**:
`--resume <file>` also sets `agent.transcript_log_path` to that file, so one
file stays the complete record of the session, and the `system_prompt_update`
entry lands where the rest of that session's history is.

A transcript whose requests carry no system message (an older file) is reported
as such and resumed with the current prompt.

### 12.3 Named sessions

A session **is** its transcript: the file `<sessions_dir>/<name>.jsonl`. There is
deliberately no index file — the directory is the registry, so "is this name
taken" is one `stat()` that cannot drift out of sync with reality, and a session
can be archived, copied or deleted with `mv`, `cp` and `rm`.

Names double as filenames, so the charset is restricted the same way skill names
are: letters, digits, `_`, `-`, `.`, non-empty, no leading `.`, nothing that
could traverse a path or need quoting. An unusable name is refused before
anything is written. There is deliberately no `:`, which is why the default
name writes the time as `hh-mm-ss`: a session file stays copyable onto a
FAT/exFAT or Windows volume — an SD card, say — without renaming it first.

**Every session has a name, including the ones nobody names.** Which file a run
uses, in precedence order:

| Condition | Session file |
|---|---|
| `--resume <name>` | that stored session (a bare name is resolved in `sessions_dir`; anything else is treated as a path, so existing `--resume <file>` invocations keep working) |
| `--session <name>` | `<sessions_dir>/<name>.jsonl`, created if new — the run says `new session:` or `continuing session:` |
| `agent.transcript_log_path` set explicitly in the config or on the CLI | that path, unnamed (back-compatible) |
| nothing of the above | a new session named `YYYY-MM-DD_hh-mm-ss_<work dir basename>`, e.g. `2026-11-01_17-36-04_workspace` |

The default name is to the second, in local time (the name is for a human
reading a listing), so two sessions differ by construction unless they start in
the same second in the same directory — that case gets the first free `-2`,
`-3`, … suffix rather than appending to the earlier session's transcript. The
basename is sanitised to the valid charset, and a work directory with no usable
basename (`/`, `.`) yields `workdir`.

**Maintenance commands.** `--list-sessions` and `--session <old> --rename-to
<new>` act on the session store and exit before the agent is set up at all.
`--rename-to` wins if both are given: performing the rename and then printing a
listing that predates it would be the one ordering that lies.

**Renaming.** `/rename <new-name>` renames the session being written right now:
the file is moved with `rename(2)` (atomic within the directory), the logger is
pointed at the new path, and the move is recorded *inside the file* as a
`session_rename` entry — append-only, like `system_prompt_update` and
`work_dir_update`, so the transcript keeps explaining its own history. The
scripted form is `--session <old> --rename-to <new>`, which renames and exits.

A rename is refused, with nothing changed, when the new name is unusable, when
it is already taken (names are unique across stored sessions), when it equals
the current name, or when the source session does not exist. A run whose
transcript path was set explicitly has no session name, so `/rename` reports
that rather than guessing one.

`--list-sessions` prints each stored session newest first with its date, task
count, entry count and size, then exits.

### 12.1 TAB completion

No new dependency — no GNU readline/libedit (not in REQUIREMENTS.md's approved list,
and pulling one in just for this is disproportionate). Interactive mode puts stdin
into raw mode via `termios` (already a POSIX facility this project leans on
elsewhere, e.g. `tcsetattr`) and implements a minimal line editor: character echo,
backspace, and TAB-triggered completion against a small fixed set of candidates —
the slash commands (§12); after a line starts with `show_skill:`, the discovered
skill names (§7); and after `/rename `, the stored session names (§12.3) — the
names already taken are exactly what is useful to see there, since a taken name
is the one error that command can make. Completion is prefix-based; TAB on an ambiguous prefix prints the
matching candidates once (same look as a shell's) rather than cycling through them —
simpler to implement correctly and sufficient for a handful of commands/skills, not a
general-purpose readline replacement. Falls back to plain line-buffered input (no
completion, no raw mode) when stdin isn't a TTY, same trigger as `color_output = auto`
resolving off (§15).

## 13. Makefile targets

| Target | Meaning |
|---|---|
| `all` | Native build ⇒ `build/tinyca` |
| `mock` | `build/mockllm` |
| `test` | Builds and runs `tests/run_tests.sh` — must pass offline |
| `check-portability` | `-std=c++11 -pedantic-errors -Wall -Wextra -Wpedantic` plus the C++14/17 grep ban list |
| `cross` | `CROSS=aarch64-linux-gnu-` syntax/codegen check only |
| `cross-static` | Adds `-static`; documented NSS caveat |
| `format` | Runs `clang-format -i` (style: `.clang-format`, repo root) over `include/` and `src/` |
| `check-format` | Same, `--dry-run --Werror` — fails if anything isn't already formatted; wired into `make test` |
| `clean` / `install` | |

Base flags: `-std=c++11 -O2 -Wall -Wextra -pthread`. No OpenSSL linkage — there is no
TLS endpoint and no build configuration that needs one.

## 14. Out of scope for v1

Sub-agents, plan mode, MCP, RAG or embeddings, git integration beyond bash, patch/diff
editing tools, TUI rendering, signed thinking blocks, provider retry/backoff. **Remote
or hosted LLM providers (Anthropic, OpenAI, or any other network API) are not a future
flag to add later — they are explicitly out of scope for this project.** The only
backend evolution anticipated is swapping which *local* server TinyCA talks to
(`llama-server` today, vLLM potentially later), both already OpenAI-compatible over
HTTP, needing no new codec.

## 15. Terminal output and colour

Four things need to be visually distinguishable at a glance in the terminal: what
the *user* typed, the model's `<think>` reasoning, the model's actual (non-thinking)
reply, and an action the model is about to take (a bash command, a skill
invocation) before it runs. Tool/command *output* (stdout/stderr from execution) is
printed unstyled — it is already visually distinct by context (it follows an
action line) and dumping ANSI codes through arbitrary command output risks breaking
it.

New `[agent]` key: `color_output` = `auto` (default) \| `always` \| `never`. `auto`
colourizes only when stdout is a TTY (checked once via `isatty(STDOUT_FILENO)`) —
piping to a file or another process gets plain text, no escape codes to strip later.

| Element | Style | When shown |
|---|---|---|
| User input | bold cyan | Interactive mode, as typed |
| `<think>` reasoning | dim/grey | Only when `/toggle_show_thinking` is on (default off) |
| Assistant reply (non-thinking content) | default/green | Every turn with no action, or trailing prose around one |
| Requested action (command about to run) | bold yellow | Right before `Executor` runs it, prefixed `▶` |

Exact ANSI SGR codes (`\x1b[1;36m` etc.) are an implementation detail of `logger.h`'s
terminal-output half, not pinned here — the four-way distinction above is the
contract; the codes just need to be ones a typical terminal (including the Jetson's
own, and whatever the user SSHes in from) renders correctly, and must be skipped
entirely when `color_output` resolves to off.

## 16. Known capability ceiling

Everything below is measured against Qwen2.5-Coder-1.5B-Instruct (Q4_K_M) on the
Jetson at `temperature = 0.2`, not inferred. **This section is the only place
these limits are defined**; the README and `docs/MANUAL.md` point here.

**Before concluding "the model is bad".** Two failure classes that looked exactly
like model stupidity were defects in this harness: tool output fed back without
saying which command produced it (§6.1), and a reply cut off at `max_tokens`
executed as if complete (§9). Both are fixed. The lesson is not historical — any
new "the model loops" report should be checked against the harness first. The
runs that produced these conclusions are kept outside the project
documentation, in the development record (`dev/capability_tests/`).

**Genuinely the model.** These reproduce with the harness behaving correctly:

| Observed | Example | What TinyCA does about it |
|---|---|---|
| Skips a prerequisite step | Asked to write, compile and run `sort.c` in one prompt, it starts with `gcc -o sort_test sort.c` on a file that does not exist | Nothing can fix this; the repetition detector bounds the damage |
| Does not adapt to an error | Shown a real `TypeError: 'int' object is not iterable`, it re-emits the identical script. The bug (`return a` instead of the sequence) is deterministic across runs | Repetition detector aborts after `repetition_threshold` |
| Needs the mechanism spelled out | The same sort task fails one-shot but succeeds (73 s) when given numbered steps and a heredoc example | Keep tasks single-step; put the example in the prompt |
| Content-level reasoning | Asked for per-paragraph summaries it produced correct *filenames* with placeholder text unrelated to the paragraphs | None. Mechanical file operations work; comprehension does not |
| Fully empty generation | `content: ""`, no actions, `finish_reason: "stop"` on a longer prompt | §9's empty-turn guard: the turn is retried once rather than reported as a finished task with a blank answer |
| Fabricated success | Once, historically: claimed a script's correct output without ever running it. Not reproduced since results carry their command | Nothing catches this by construction — neither the filter nor the repetition detector is designed to. Read the transcript |
| Meta-capabilities derail it | With `add_skill` advertised in Tier 1, it abandoned the sort task to fetch a skill from a hallucinated GitHub URL and register a skill named `sort` | `skills.tier1_exclude` keeps such skills runnable but unadvertised; `add_skill` is excluded by default (§7.1) |
| Ignores the stated working directory | Long-standing, from the first on-device runs | `work_dir` is enforced by `chdir` in the child, not by asking |

Practical settings for a model this size: prefer whole-file rewrites over
exact-string edits, keep tasks single-step, keep `on_blocked_command = ask`, leave
`skills.tier2_enabled = false` and `skills.tier1_exclude = add_skill` alone.

The architecture is sound and the model is the limit — but verify which of the two
you are looking at before concluding that.
