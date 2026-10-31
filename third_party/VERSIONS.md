# Vendored dependencies

Fetched once, committed verbatim, never modified in place. Both verified to compile
with `g++ -std=c++11` (gcc 12 on the x86 dev host; re-verify on the Jetson's gcc 8 at
D7).

| File | Source | Version / tag | SHA-256 |
|---|---|---|---|
| `json.hpp` | [nlohmann/json](https://github.com/nlohmann/json) | `v3.11.3` | `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6` |
| `httplib.h` | [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) | `v0.14.3` | `2b857880236a38a64383e72e19d5af5a1b9a8f856884435ad2f4940cbcc4229a` |

`httplib.h` is pinned to the 0.14.x line deliberately: releases from 0.15 onward
require C++17, which this project does not use (PROMPT.md's non-negotiable
constraint #1). `0.14.3` is the last tag before that bump.

Nothing else is vendored. No Boost, no yaml-cpp, no libcurl.
