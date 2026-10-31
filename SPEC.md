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
  PROMPT.md SPEC.md REQUIREMENTS.md README.md LICENSE
  third_party/{json.hpp, httplib.h, VERSIONS.md}
  include/tinyca/
    config.h        ini_parser.h      string_util.h
    transport.h     http_transport.h  message.h  codec.h
    protocol.h      fenced_protocol.h hermes_protocol.h
    skills.h        executor.h        command_filter.h
    agent.h         logger.h          control.h
  src/            # one .cpp per header
  tools/mockllm/  # scripted mock LLM server
  config/{tinyca.ini.example, system_prompt.txt}
  skills/fetch_url.sh
  grammars/hermes_tool_call.gbnf
  tests/{fixtures/*.json, run_tests.sh, test_*.cpp}
```

## 4. Configuration

INI format: `key = value`, `#` or `;` comments, `[section]` headers, values trimmed, no
quoting, `\n` unescaped in values. The parser is ~100 lines of `find`/`substr`. Keys are
addressed as `section.key`. Precedence: CLI flag > config file > built-in default.

The system prompt lives in a **separate plain-text file** (`agent.system_prompt_file`) —
multi-line prompts do not belong in INI values.

### 4.1 `[llm]`

| Key | Default | Notes |
|---|---|---|
| `base_url` | `http://127.0.0.1:8080` | Scheme + host + port. Use an IP if the binary is statically linked. |
| `api_path` | `/v1/chat/completions` | Overridden automatically to `/completion` when `enable_grammar = true` — see §6.3 |
| `model_id` | `qwen2.5-coder-1.5b` | Echoed back to the server; arbitrary unless `--alias` was used |
| `api_key` | `none` | Sent as `Authorization: Bearer`; llama-server ignores it |
| `request_timeout_s` | `900` | Generous: ~5 tok/s generation |
| `connect_timeout_s` | `10` | |
| `max_tokens` | `512` | |
| `temperature` | `0.2` | Low: this is code, not prose |
| `top_p` | `0.9` | |
| `context_tokens` | `4096` | Must match the server's `-c`; used for budget warnings |
| `stream` | `true` | SSE token streaming; `llama-server` supports it on both endpoints — see §9.3 |

### 4.2 `[protocol]`

| Key | Default | Notes |
|---|---|---|
| `tool_protocol` | `fenced` | `fenced` \| `hermes` |
| `enable_grammar` | `false` | GBNF constraint; `hermes` only |
| `grammar_file` | `grammars/hermes_tool_call.gbnf` | |
| `strip_think` | `true` | Remove `<think>...</think>` before action parsing |
| `strip_think_from_history` | `true` | Also drop it from replayed history (saves context) |
| `stop_sequences` | *(protocol default)* | Comma-separated override |

### 4.3 `[agent]`

| Key | Default | Notes |
|---|---|---|
| `system_prompt_file` | `config/system_prompt.txt` | Tier 1 skill index is appended to this |
| `max_iterations` | `8` | Hard cap per task |
| `repetition_threshold` | `2` | Identical consecutive commands before abort |
| `work_dir` | `./workspace` | Execution jail; created if absent |
| `transcript_log_path` | `./logs/transcript.jsonl` | Every request, response, command, result |
| `transcript_max_bytes` | `0` | Cap on the transcript file size; `0` = unlimited. See §9.1 |
| `log_level` | `info` | `error` \| `warn` \| `info` \| `debug` |
| `mode` | `interactive` | `interactive` \| `oneshot` |

### 4.4 `[tools]`

| Key | Default | Notes |
|---|---|---|
| `bash_timeout_s` | `60` | Per command; SIGKILL on expiry |
| `output_head_lines` | `40` | |
| `output_tail_lines` | `40` | |
| `output_max_bytes` | `4096` | Applied after line truncation |
| `command_filter_mode` | `whitelist` | `whitelist` \| `blacklist` \| `none` — see §8.1 |
| `whitelist` | `ls,cat,grep,find,wc,head,tail,pwd,echo,mkdir,cp,mv,touch,make,gcc,g++,python3,git,sed,awk,diff,wget` | Head tokens; used when `command_filter_mode = whitelist` |
| `blacklist` | `rm,dd,mkfs,shutdown,reboot,sudo,su,chmod,chown,kill,pkill,killall,mount,umount` | Head tokens; used when `command_filter_mode = blacklist` |
| `on_blocked_command` | `ask` | `ask` \| `deny` — what happens when the filter blocks a command. See §8.1 |
| `confirm_mode` | `never` | `always` \| `never` — confirmation for commands the filter already *allowed*. Independent of `on_blocked_command`. |
| `allow_metacharacters` | `false` | See §8 |

### 4.5 `[skills]`

| Key | Default | Notes |
|---|---|---|
| `skills_dir` | `./skills` | |
| `tier1_enabled` | `true` | |
| `max_tier1_skills` | `8` | Beyond this, small-model selection degrades |

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
# @usage: skills/fetch_url.sh <url> <destination_folder>
```

`@name`, `@description`, `@usage` are required; a file missing any of them is skipped
with a warning.

**Tier 1 (always resident).** Append to the system prompt:

```
Available skills (run them with the bash tool):
- fetch_url: Download a file from a URL into a destination folder.
  usage: skills/fetch_url.sh <url> <destination_folder>
```

~15–20 tokens per skill. Because this lives in the immutable system prompt, it is
processed once and then served from the KV cache prefix — a *fixed* cost, not per-turn.
Cap at `max_tier1_skills`; log a warning if more are found.

**Tier 2 (on demand).** When the model runs a skill whose behaviour it needs to inspect,
or explicitly requests it, the full script body is appended as a new `user` message
(append-only, so the prefix survives). Trigger: the model emits
`show_skill: <name>` on a line by itself. Unknown name ⇒ append an error message listing
valid names.

**Skills are executed as bash and are auto-allowed**: their path is added to the
whitelist (if `command_filter_mode = whitelist`) or exempted from the blacklist (if
`command_filter_mode = blacklist`) at startup, regardless of `command_filter_mode`.

Design rule to document in the README: prefer skills written as *scripts* over skills
written as *prose instructions*. Script skills reduce the model's job to selection, which
a 1.5B model can do; prose skills require instruction following, which it cannot do
reliably.

## 8. Bash executor and whitelist

**Execution.** `fork` + `execvp` of `/bin/bash -c <command>` with `cwd = work_dir`, stdout
and stderr merged through a pipe. `bash_timeout_s` enforced by `SIGKILL` on the process
group (not just the child, or backgrounded grandchildren survive).

**Truncation.** Keep the first `output_head_lines` and last `output_tail_lines`, inserting
`... [N lines omitted] ...`, then hard-cap at `output_max_bytes`. Without this a single
`pip install` log destroys the context window. Always report the true exit code.

### 8.1 Command filtering — `command_filter_mode`

Three mutually exclusive modes, selected by `command_filter_mode`. In every mode,
metacharacter rejection runs first and is unconditional: reject outright if the
command contains `$(`, backtick, `>`, `>>`, `<`, or `&` and `allow_metacharacters =
false`. This check does not depend on the filter mode — it catches the classic
injection shapes before any token-level logic runs.

For `whitelist` and `blacklist` modes, the same segmentation applies:
1. Split on the separators `;`, `&&`, `||`, `|`, and newline.
2. For **every** segment: trim, strip leading `VAR=value` assignments, take the head
   token, resolve `./skills/x.sh` forms.
3. **`whitelist`**: every segment's head token must be a member of `whitelist`. Any
   segment failing ⇒ reject the whole command and return a `tool` message naming the
   offending token, so the model can correct itself.
4. **`blacklist`**: every segment's head token must *not* be a member of `blacklist`.
   Any segment matching ⇒ reject the whole command and name the offending token, same
   as above. Everything not on the list is allowed.
5. **`none`**: no segmentation, no token check. Metacharacter rejection (above) still
   applies unless `allow_metacharacters = true`, in which case nothing is filtered at
   all — document this combination as unsafe, for `oneshot` sandboxes only.

A check on the first token alone is not acceptable in `whitelist` or `blacklist` mode:
`ls; rm -rf /` must be caught by segment 2, not waved through because `ls` is fine.

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
    if actions.empty(): return msg.content        # task complete
    if repetition_detector.trips(actions): abort with explanation
    if !protocol.supports_parallel() and actions.size() > 1: keep first, warn
    results = executor.run_sequential(actions)     # whitelist + confirm + truncate
    append protocol.format_results(results)
return "iteration limit reached"
```

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

### 9.2 Transcript size cap

`transcript_max_bytes` bounds the transcript file (`0` = unlimited, the default).
Once writing an entry would exceed the cap, the logger stops appending for the rest
of that run and writes one final warning line noting the cutoff — the existing
entries (oldest-first, since the log is append-only) are left intact rather than
rotated or truncated from the front. This is a diagnostic safety valve for
long-running `interactive` sessions on a device with limited storage, not a general
log-rotation feature.

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
# @usage: skills/fetch_url.sh <url> <destination_folder>
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
`PROMPT.md`.

## 12. CLI

```
tinyca [options] [task]
  -c, --config <file>     default ./config/tinyca.ini
  -p, --prompt <task>     one-shot mode; implies --mode oneshot
  -m, --model <id>        override llm.model_id
  -u, --url <base_url>    override llm.base_url
      --protocol <p>      fenced | hermes
      --no-confirm        confirm_mode = never
      --dry-run           parse and print actions, execute nothing
  -v, --verbose           log_level = debug
      --version, --help
```

Interactive mode: a REPL that keeps history across tasks within a session.

| Command | Effect |
|---|---|
| `/reset` | Clear history, start a fresh session (keeps the system prompt) |
| `/skills` | List discovered skills (Tier 1 names + descriptions) |
| `/history` | Print the current message history |
| `/toggle_show_thinking` | Toggle printing `<think>` content as it's produced; off by default — see §9.3/§15 |
| `/help` | List these commands |
| `/quit` | Exit the REPL |

Ctrl+C works at any point, including mid-generation or mid-command (§9.1) — it aborts
the current task, not the session.

### 12.1 TAB completion

No new dependency — no GNU readline/libedit (not in REQUIREMENTS.md's approved list,
and pulling one in just for this is disproportionate). Interactive mode puts stdin
into raw mode via `termios` (already a POSIX facility this project leans on
elsewhere, e.g. `tcsetattr`) and implements a minimal line editor: character echo,
backspace, and TAB-triggered completion against a small fixed set of candidates —
the slash commands (§12) and, after a line starts with `show_skill:`, the discovered
skill names (§7). Completion is prefix-based; TAB on an ambiguous prefix prints the
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

## 16. Known capability ceiling — document in the README

With a 1.5B model, expect: commands ignoring the stated working directory, repeated
retries after an error, and near-total failure at exact-string file edits. Prefer
whole-file rewrites over patches, keep tasks single-step, keep `on_blocked_command =
ask`. The architecture is sound; the model is the limit.
