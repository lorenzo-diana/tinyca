# Development record — TinyCA

Root of the **development-process** tree. Everything here describes how TinyCA
was built, not what it is: the original brief, the engineering journal, and the
measured runs behind the claims in the project documentation.

This tree is deliberately disjoint from the project documentation. It contains
no links into it, and nothing in it is reachable from the project's `README.md`
— the two have different audiences and different lifetimes, and a reader
looking for how to use TinyCA should never land in a journal entry. Where a
project document is relevant it is named in plain text (`SPEC.md`,
`docs/MANUAL.md`) rather than linked.

- [PROMPT.md](PROMPT.md) — the original build brief: the non-negotiable
  constraints, the build matrix, the phase plan (D0–D8) with its gates, and the
  autonomous-execution workflow the first implementation pass followed.
- [PROGRESS.md](PROGRESS.md) — append-only engineering journal. One entry per
  phase gate and per blocking failure, in order, plus the post-completion
  maintenance sessions. Entries are never edited; a correction is appended.
- [capability_tests/README.md](capability_tests/README.md) — real end-to-end
  runs against the real model on real hardware, including the ones that failed
  and why, with the artifacts each run produced.
- `first_run_transcript.jsonl` — the six JSONL lines from the first successful
  on-device run, kept as the evidence that the loop works against a real small
  model.

## Reading order, if you are picking this up cold

`PROMPT.md` explains what was asked for and under which constraints.
`PROGRESS.md` records what actually happened, including the defects found after
the project was first declared complete — the later entries are the most
useful, because they separate failures that were the model's from failures that
were the harness's. `capability_tests/README.md` is the evidence behind the
limitations the project documentation states.

## What belongs here, and what does not

Here: dated entries, phase gates, review findings, benchmark write-ups, test
diaries, agent notes, anything in the first person.

Not here: how to use TinyCA, what its options mean, how it is designed, what it
cannot do. Those are project documentation, and they live in the project tree
whose root is the repository's `README.md`.
