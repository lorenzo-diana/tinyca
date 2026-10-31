# REQUIREMENTS — TinyCA

## A. Development host — Debian 12 (bookworm), x86-64

### A.1 Core build and test (Tier 1 + Tier 2)

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    make \
    git \
    ca-certificates \
    curl \
    wget \
    openssh-client \
    rsync
```

`build-essential` supplies g++ 12, which compiles the whole project and the mock server in
`-std=c++11` mode. This is where all development and testing happens.

### A.2 Cross syntax check (Tier 3, optional)

```bash
sudo apt-get install -y \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu
```

**Read this before using it.** Debian 12's cross-gcc targets glibc 2.36; the Jetson has
2.27. Dynamically linked output from this toolchain will fail on the board with
`GLIBC_2.28 not found` or similar. Use it **only** to verify that the source compiles and
codegens for aarch64. The deployable binary is built natively on the Jetson.

`make cross-static` (adds `-static`) does produce a runnable aarch64 binary, but statically
linked glibc disables NSS, so hostname resolution stops working and `llm.base_url` must be
an IP literal. Acceptable as a fallback, not as the default.

Routes deliberately **not** taken, and why:
- qemu + `arm64/ubuntu:18.04` container — nested Docker is unavailable inside the Claude
  Code image.
- `debootstrap` of an arm64 18.04 sysroot — requires `ports.ubuntu.com`, outside the
  network allowlist.
- Neither is needed: TinyCA builds natively on the Jetson in 1–2 minutes.

### A.3 Optional developer tooling

```bash
sudo apt-get install -y clang-format cppcheck valgrind
```

## B. Vendored dependencies — committed, not installed

Fetch once and commit under `third_party/`; record exact versions and commit hashes in
`third_party/VERSIONS.md`.

| File | Source | Constraint |
|---|---|---|
| `third_party/json.hpp` | `nlohmann/json` single-header release | Must compile with gcc 8 in C++11 mode. Recent 3.11.x requires C++11 only — verify. |
| `third_party/httplib.h` | `yhirose/cpp-httplib` | Newer releases require C++17. **Pin a C++11-compatible release** (0.14.x line or earlier) and verify against gcc 8. |

Cross-check both against the versions vendored in the `llama.cpp_ubuntu_18` fork: those
are known to build under gcc 8.5 + CUDA 10.2 on this exact board. Copy those files rather
than adding any build-system coupling between the two projects — TinyCA speaks HTTP to a
server it never links against.

Nothing else may be added: no Boost, no yaml-cpp, no libcurl, no JSON schema validator.

## C. Target — Jetson Nano (Ubuntu 18.04, JetPack 4.6.6)

### C.1 Build

```bash
sudo apt-get install -y build-essential make
```

gcc 8.x must already be present from the `llama.cpp_ubuntu_18` port (see that project's
`REQUIREMENTS.md` §B.4). TinyCA needs no CUDA, no CMake, no Python.

```bash
make            # ~1-2 minutes natively
```

### C.2 Runtime

- `bash` (present)
- `wget` — used by the example skill:

```bash
sudo apt-get install -y wget
```

- A running `llama-server` from the `llama.cpp_ubuntu_18` build, bound with
  `--host 0.0.0.0` if TinyCA runs on a different machine.

### C.3 Resource footprint

TinyCA is expected under ~10 MB RSS. The 4 GB budget is consumed by the model
(~1.0 GB weights) and the KV cache (~28 KB/token, so ~115 MB at 4096 context). Headless
boot is recommended to reclaim ~500–800 MB.

## D. Explicitly not required

CMake, autotools, Node.js, Python, Docker, CUDA, libcurl, Boost, yaml-cpp, any package
manager at runtime.
