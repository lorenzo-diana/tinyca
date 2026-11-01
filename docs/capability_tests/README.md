# Capability tests

Real end-to-end runs of `tinyca` against the real `llama-server` +
Qwen2.5-Coder-1.5B-Instruct on the Jetson Nano (not the mock server used by
`make test`). Sandbox config: `config/tinyca_capability.ini` on the board
(`command_filter_mode = none`, `allow_metacharacters = true` — SPEC.md §8.1's
own sanctioned oneshot-sandbox exception, not a change to any shipped
default). Goal: honestly measure what the architecture + a real 1.5B model
can and can't finish unattended, not to cherry-pick a success.

## Test 1 — write, compile, run a sort algorithm (`test1_sort/`)

**Task**: write `sort.c` (bubble sort, fixed test array), compile with gcc,
run the binary, report the sorted output.

**Result: genuine success**, on the 4th prompt attempt.
- Attempts 1-2 (one-shot instruction): model skipped writing the file
  entirely and jumped straight to `gcc -o sort_test sort.c` / `./sort_test`,
  which fails (no such file), then repeated the identical failing command —
  caught correctly by the repetition detector.
- Attempt 3 (isolated "do ONLY step 1"): model wrote `sort.c` correctly via
  heredoc, but then emitted a nonsense bare-filename "command" on the next
  turn and looped — aborted by the repetition detector again.
- Attempt 4 (explicit numbered steps, heredoc example embedded in the
  prompt, "stop after you see the result"): model wrote `sort.c`, compiled
  it, ran it, and reported the correct sorted output
  (`-2 0 1 3 4 5 8 9`), on its own combining all three steps into one bash
  block. Re-verified independently by re-running the compiled binary over
  SSH — output matches.

Artifacts: `sort.c` (model-written), `sort_aarch64_binary` (compiled on
device), `transcript.jsonl` (all 4 attempts, 33 lines).

## Test 2 — split a text file into per-paragraph files + summarize
(`test2_split_summarize/`)

**Task**: given a 3-paragraph `input.txt` (Jetson Nano / bubble sort /
phone hotspot — three unrelated topics on purpose, so a generic summary
can't accidentally look correct), split into `para_1.txt`..`para_3.txt`
and write a real summary of each into `summary_1.txt`..`summary_3.txt`.

**Result: genuine, documented failure** — not retried to a forced success.
- Full-pipeline attempt: model skipped the split step entirely and wrote
  `summary_1.txt`..`summary_3.txt` directly, but their content was generic
  placeholder text ("This is the topic of paragraph 1.") with no relation
  to the actual paragraph content — i.e. it produced the right *filenames*
  without doing the *reasoning*.
- Narrower isolation (read `input.txt`, write one real summary for
  paragraph 1 alone): model correctly ran `cat input.txt`, then on the next
  turn tried to `cat summary_1.txt` (reading a file that was supposed to be
  written, not read) — errored, repeated identically, aborted by the
  repetition detector.

This matches the capability ceiling already documented in SPEC.md §16 /
README.md: a 1.5B model reliably does mechanical file operations but not
content-level reasoning. Artifacts: `input.txt`, `transcript.jsonl` (15
lines, full-pipeline attempt).

## Test 3 — write and run a Fibonacci script (`test3_fibonacci/`)

Added test (not requested verbatim, picked to contrast with test 2: a
purely mechanical/computational task — no content reasoning required — in
a different language, Python vs. C).

**Task**: write `fib.py` printing the first 15 Fibonacci numbers, run it,
confirm the output.

**Result: genuine, reproducible failure — model fabricates success instead
of verifying it.**
- First attempt (prose instruction with a parenthetical heredoc example):
  the model returned a **completely empty generation**
  (`content:"" tool_calls:[]`) — no action, no text, no error. Confirmed
  this was a real model behavior, not an infra problem: `llama-server`
  was healthy (`/health` → `ok`), no server log errors, process alive
  throughout. TinyCA correctly treated the empty reply as a (vacuous)
  final answer and exited cleanly — this exposed a real model failure
  mode distinct from tests 1/2 (total non-response to a sufficiently
  long/complex prompt) rather than a bug in the agent.
- Second attempt (shorter "step 1 only" prompt): model skipped straight to
  running code via `python <<EOF ... EOF` on stdin — never created
  `fib.py` at all — then repeated the identical command and was aborted by
  the repetition detector.
- Third attempt (isolated "only write the file"): model wrote `fib.py` via
  heredoc correctly, but the function it wrote has a bug — `fibonacci(n)`
  returns only the final scalar `a` instead of a list of the sequence, so
  `print(' '.join(map(str, fibonacci(15))))` raises
  `TypeError: 'int' object is not iterable`. In the same reply, **the model
  then fabricated a plausible-looking success line**
  ("The first 15 Fibonacci numbers are: 0 1 1 2 3 5 8 13 21 34 55 89 144
  233 377") inside a second fenced block, without ever having run the
  script — followed by garbage ("The" interpreted as a shell command),
  which repeated and aborted.
- Fourth attempt (full step-by-step prompt mirroring test 1's winning
  structure — the one that produced a genuine success for the sort task):
  model wrote the **same buggy `fibonacci()`** (deterministic at
  temperature 0.2) and this time combined write+run in one block. The
  actual run would have raised the same `TypeError`; the model's next turn
  just re-issued `python3 fib.py` identically twice in a row and was
  aborted by the repetition detector — i.e. it never engaged with the
  error output at all.

Independently re-running the model's `fib.py` directly on the board
confirms the bug: `TypeError: 'int' object is not iterable`.

This is the most important finding of the three tests: the agent's safety
net (the repetition detector) worked correctly in every case, but the
model itself does not reliably distinguish "I ran this and it worked" from
"I wrote something that looks like it should work." The attempt where it
fabricated a success message rather than erroring or looping is a sharper,
more concerning failure mode than tests 1/2's errors — a model that
confidently reports a result it never produced is a correctness risk no
command filter or repetition threshold is designed to catch.

Artifacts: `fib.py` (model-written, buggy), `transcript.jsonl` (the final
attempt).

## Summary

| Test | Task type | Result |
|---|---|---|
| 1 — sort | mechanical (write/compile/run C) | **Success** (attempt 4/4) |
| 2 — split+summarize | content reasoning | **Failure** (placeholder text, read/write confusion) |
| 3 — fibonacci | mechanical (write/run Python) | **Failure** (buggy code + fabricated success report) |

The architecture (fenced protocol, repetition detector, command filter) held
up correctly across all 12 total attempts — no hang, no runaway loop, no
unsafe command execution. The 1.5B model is the limiting factor, consistent
with SPEC.md §16, and the specific failure modes found here (skipping steps,
read/write confusion, and — new — fabricating an unverified success) are
useful, concrete evidence beyond the single first-run transcript already in
`docs/first_run_transcript.jsonl`.
