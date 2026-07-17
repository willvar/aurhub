# aurhub Agent Guidelines

## Build

```bash
cmake -S . -B build -G Ninja && cmake --build build
```

Requirements: C++23, CMake 4.0+, Ninja, zlib, CLI11, simdjson.

## Test

```bash
ctest --test-dir build
```

## Lint

```bash
clang-tidy --config-file=.clang-tidy SRC_FILE -- -std=c++23 -Isrc
```

Check all source files:

```bash
for f in src/*.cpp tests/*.cpp; do
    clang-tidy --config-file=.clang-tidy "$f" -- -std=c++23 -Isrc
done
```

### Suppressed Checks

Only these are intentionally suppressed (system-level code, unavoidable):

| Check | Reason |
|---|---|
| `cppcoreguidelines-pro-type-reinterpret-cast` | mmap/zlib C API requires pointer cast |
| `cppcoreguidelines-pro-type-const-cast` | zlib C API requires non-const pointer |
| `cppcoreguidelines-no-malloc` | `getline` uses malloc internally |
| `clang-analyzer-unix.Malloc` | false positive on `std::optional` cross-thread paths |
| `clang-analyzer-unix.Stream` | false positive on forked process pipe |
| `portability-avoid-pragma-once` | project uses `#pragma once` everywhere |

Single-function NOLINT annotations in source:

| Location | Check | Reason |
|---|---|---|
| `search.cpp:scan_range` | `bugprone-easily-swappable-parameters` | `count`/`max_results` are distinct by name |
| `search.cpp:scan_blocks` | same | `first_block`/`end_block` and `kept`/`dirty_blocks` |
| `search_bench.cpp:benchmark` | same | `warmup`/`iterations` |
| `server.cpp:token_list_contains` | same | `values`/`wanted` |
| `server.cpp:route_request` | same | `method`/`target` |
| `server.cpp:register_epoll` | same | `fd`/`events` |
| `generation.cpp:write_padding` | same | `fd`/`bytes` |
| `snapshot.cpp:write_padding` | same | `fd`/`bytes` |
| `srcinfo.cpp:append_arch` | same | `arch`/`value` |
| `indexer.cpp:merge loop` | `bugprone-branch-clone` | standard merge-sort pattern |
| `server.cpp:SnapshotReloader::run` | `misc-const-correctness` | `unique_lock` must be mutable |
| All `main()` functions | `bugprone-exception-escape` | `app.exit()` handles own errors |

### Do NOT suppress without equivalent reason

Before adding any new suppression, verify it is truly necessary (system interop / false positive). Style preferences and refactorable patterns must be fixed, not suppressed.

## Libraries

### CLI Arguments

Use **CLI11** (header-only, `/usr/include/CLI/CLI.hpp`). Every executable uses CLI11 for option parsing. Do not write hand-rolled `parse_options` functions.

Pattern:

```cpp
#include <CLI/CLI.hpp>

int main(int argc, char** argv) {
    Options options;
    CLI::App app{"description"};
    app.add_option("--flag", options.member, "help")->required();
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }
    // ...
}
```

### JSON Generation

Use **simdjson** `string_builder` (`/usr/include/simdjson.h`). Do not hand-build JSON strings.

Pattern:

```cpp
#include <simdjson.h>

simdjson::builder::string_builder sb(capacity);
sb.start_object();
sb.append_key_value("key", value);
sb.append_comma();
sb.start_array();
for (...) { sb.escape_and_append_with_quotes(item); }
sb.end_array();
sb.end_object();
return std::string(sb);
```

## Code Style

- C++23, `snake_case` for functions/variables/members, `PascalCase` for structs/classes, `kPascalCase` for constants
- `#pragma once` in all headers
- `snake_case_` suffix for private member variables
- Anonymous namespaces for file-local code (not `static`)

## Commit

Single-line: `TAG: message`

Tags are short lowercase labels describing the change category. No
body unless the change is unusually complex and the a reader six months
from now would need context that does not fit in the summary.

## README Sync

When changing build requirements (C++ standard, CMake version, dependencies) or architecture,
update `README.md` to match.
