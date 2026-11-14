# Building and running TinyCA — ARM (aarch64) and x86-64

TinyCA is a plain `make` project with no configure step, no package manager at
runtime and two vendored headers. Building it is one command on either
architecture; this file is about the parts that differ — which target to use,
where the binary lands, how to be sure it is for the machine you intend, and
what has to be running before TinyCA is useful.

This file is the procedure. The facts it leans on are defined elsewhere and
referenced here, never copied:

- Packages and toolchains per machine: [REQUIREMENTS.md](../REQUIREMENTS.md)
  (§A.1 x86-64 host, §A.2 the aarch64 cross toolchain and its caveats, §C.1–C.2
  the Jetson).
- What each make target means: [SPEC.md](../SPEC.md) §13.
- Every CLI option and config key: [docs/MANUAL.md](MANUAL.md).

## Which build do you want

| Goal | Command | Output |
|---|---|---|
| Run on this machine, whatever it is | `make` | `build/tinyca` |
| An explicitly Intel/AMD 64-bit binary | `make x86_64` | `build/x86_64/tinyca` |
| …statically linked | `make x86_64-static` | `build/x86_64-static/tinyca` |
| Check the source still compiles for aarch64 | `make cross` | `build/cross/tinyca` — a check, not a deployable; fails outright without the toolchain |
| A runnable aarch64 binary from an x86-64 host | `make cross-static` | `build/cross-static/tinyca` — read the caveat below |
| Install for use as `tinyca` | `make install` | `$(DESTDIR)/usr/local/bin/tinyca` |

`make` is the authoritative build on both architectures: it compiles with the
host's own g++ and links against the host's own glibc, which is why a binary
built on the target machine always runs there.

## x86-64 (Intel/AMD, 64-bit)

Prerequisites: `build-essential` and `make` (REQUIREMENTS.md §A.1).

```bash
make                 # -> build/tinyca              (seconds)
make test            # the offline suite: no network, no LLM needed
make x86_64          # -> build/x86_64/tinyca       (explicitly Intel/AMD 64-bit)
make x86_64-static   # -> build/x86_64-static/tinyca
```

On an x86-64 host, `make x86_64` compiles exactly what `make` does; what it adds
is a *name*. `build/tinyca` is "whatever this host produced", which is ambiguous
the moment two machines share a working tree over `rsync`, while
`build/x86_64/tinyca` says what it is. The target drives the compiler through
the `x86_64-linux-gnu-` prefix rather than passing `-m64`, so running it on a
non-x86 host cannot silently produce something else, and it verifies the result
from the ELF header instead of assuming:

```
$ make x86_64
build/x86_64/tinyca: Advanced Micro Devices X86-64
```

A host whose native g++ is the right compiler but which has no prefixed alias
for it needs the prefix cleared:

```bash
make x86_64 X86_CROSS=
```

The same check is available on its own, for a binary of unknown provenance:

```bash
make check-arch BIN=build/x86_64/tinyca WANT=X86-64
```

Static linking removes the glibc version dependency, at the cost documented in
REQUIREMENTS.md §A.2: statically linked glibc disables NSS, so hostname
resolution stops working and `llm.base_url` must then be an IP literal.

## ARM (aarch64 — the Jetson Nano)

Prerequisites: `build-essential` and `make`; gcc 8.x is already present on a
board set up for the `llama.cpp_ubuntu_18` port, and TinyCA needs no CUDA, no
CMake and no Python (REQUIREMENTS.md §C.1).

**Build on the board.** This is the deployable artifact:

```bash
make                 # -> build/tinyca              (1-2 minutes on a Jetson Nano)
make test            # same offline suite, and worth running here: it is the
                     # only place the aarch64 compiler's opinion is heard
```

Copy the source rather than a binary, and build there. Over SSH from a
development host:

```bash
rsync -az --delete --exclude .git --exclude build ./ <user>@<board>:~/TinyCA/
ssh <user>@<board> 'cd ~/TinyCA && make -s && tests/run_tests.sh'
```

Excluding `build/` matters: the two architectures' object files share names, and
a stale `.o` from the other machine is a link error at best.

**Cross-compiling for aarch64 is a check, not a shortcut.** With the aarch64
toolchain installed (REQUIREMENTS.md §A.2), `make cross` verifies that the
source compiles and code-generates for the architecture. Its *dynamically
linked* output will not run on the Jetson: Debian 12's cross-gcc targets glibc
2.36 and the board has 2.27, which fails at load with `GLIBC_2.28 not found`.
`make cross-static` does produce a runnable binary, with the NSS consequence
above — a fallback, not the normal route. REQUIREMENTS.md §A.2 has the full
reasoning, including the routes deliberately not taken.

Building *for* x86-64 *on* the board works the same way as anywhere else
(`make x86_64`) but needs an x86-64 g++ cross-toolchain present, which a stock
Ubuntu 18.04 arm64 install does not have; building on the x86 machine is the
easier answer.

## Running it

Two things run: an OpenAI-compatible inference server, and TinyCA pointed at it.
Nothing about the agent is architecture-specific — the differences are all in
how the server is launched.

**1. Start the server.** The command this project was tuned for is in
docs/MANUAL.md's EXAMPLES section. Two flags are worth matching to the machine:

- `-ngl 99` offloads layers to the GPU. On the Jetson, with the CUDA-enabled
  `llama.cpp_ubuntu_18` build, that is the point of the board; on a CPU-only
  x86 box it does nothing and can be dropped.
- `-t <n>` sets the CPU thread count; match it to the machine's cores.

Add `--host 0.0.0.0` whenever TinyCA runs on a different machine from the
server, and `--jinja` always: TinyCA assumes the server applies the model's own
chat template, which is the one llama.cpp-specific assumption it makes.

**2. Point TinyCA at it and run.** `llm.base_url` in `config/tinyca.ini` is the
only line most setups change; `-u/--url` overrides it for one run.

```bash
./build/tinyca                     # interactive, server on this machine
./build/tinyca -u http://<board>:8080           # server on the board, agent here
./build/x86_64/tinyca -c config/tinyca.ini     # an explicitly-named binary
```

A split setup is the common one with a Jetson: the board serves the model, and
the agent runs wherever you are. It works in either direction — what matters is
that the server was started with `--host 0.0.0.0` and that `llm.base_url` names
a reachable address. With a statically linked build, that address must be an IP
literal rather than a hostname (NSS, above).

`docs/MANUAL.md` covers everything past this point: the options, the config
keys, sessions, skills, and the interactive commands.

## Checking a build before trusting it

Without a server:

```bash
make test                                 # the offline suite: no network, no LLM
./build/tinyca --version
make check-arch BIN=<binary> WANT=X86-64  # or WANT=AArch64
```

`make test` needs no inference server — it builds a mock one. Running it on
every machine you build on is the cheap way to find out that a compiler
disagrees, which is the whole reason the aarch64 build is done natively.

With a server running, for a first end-to-end check that executes nothing:

```bash
./build/tinyca --dry-run -p "list the files"
```

`--dry-run` skips the command filter and the executor, so no command runs and
no approval is asked — but the model still has to answer, so this one needs
`llm.base_url` to be reachable. A binary whose server is unreachable reports
`transport error: Could not establish connection` and stops, which is itself a
useful signal: the build works and the address does not.
