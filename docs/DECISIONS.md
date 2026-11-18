# Design Decisions Log

Each entry: date, context, options considered, choice, reason. Newest at the bottom. To reverse a decision, add a new entry that references the old one. Never edit history.

---

## D-001 (2026-11-18): Development environment
**Context:** The host is macOS 15 on an Apple M3 Pro (arm64). The project needs epoll (Phase 6), perf (Phase 4) and Linux-specific socket options, none of which exist on macOS.
**Options:**
1. Build natively on macOS and swap epoll for kqueue. Rejected: trading firms run Linux, and two code paths would double the gateway work.
2. Docker dev container, linux/arm64 (native architecture, runs in Docker Desktop's VM).
3. Docker container, linux/amd64 under emulation. Rejected: emulated x86 makes every benchmark meaningless.
4. A remote Linux box (e.g. UIUC EWS, a cloud VM). Kept as an *additional* benchmark machine later, not the daily environment.

**Choice:** Option 2, an Ubuntu linux/arm64 container with GCC and Clang, entered via `scripts/dev.sh` or VS Code Dev Containers.
**Reason:** Real Linux APIs at native speed with no extra hardware needed.
**Consequences to remember:**
- `rdtsc` is x86-only. On arm64 the equivalent is the generic timer `cntvct_el0` (fixed frequency, lower resolution than TSC). The Phase 4 latency recorder must choose deliberately; that choice will get its own entry.
- Docker Desktop runs Linux in a VM, so benchmarks carry VM noise and every BENCHMARKS.md row must say so.
- perf inside Docker Desktop's VM may not have access to hardware PMU counters. **Not yet verified**; check in Phase 4 before relying on it. Software-event sampling (`-e cpu-clock`) is the fallback.

## D-002 (2026-11-18): Project documentation structure
**Context:** The project spans many work sessions, and anyone (including future me) should be able to resume it cold.
**Choice:** docs/PROGRESS.md (current state and next step), docs/DECISIONS.md (why), docs/ROADMAP.md (what), docs/BENCHMARKS.md (append-only numbers), docs/learn/ (study guides per phase).
**Reason:** Separating state, rationale, plan and measurements keeps each file short and easy to keep accurate.

## D-003 (2026-11-18): Phase 4 pre-split
**Context:** Phase 4 bundles a latency recorder, benchmark workloads, an ITCH parser with replay, and a profiling pass. That is too much for one session.
**Choice:** Planned sub-parts 4a (recorder + benchmarks), 4b (ITCH), 4c (perf hotspot fix). Other phases get split only when a split proves necessary.

## D-004 (2026-11-18): Project name
**Context:** The project needed a name for the repo, the C++ namespace and the binaries.
**Options:** ringmatch (from the ring buffers used between threads), litbook.
**Choice:** litbook. Namespace `litbook`, headers under `include/litbook/`.
**Reason:** A "lit" market is one with a publicly visible order book, as opposed to a dark pool where resting orders are hidden. That is exactly what this project builds: a lit venue that publishes its book over a market data feed.

## D-005 (2026-11-18): Build configurations via CMake presets
**Context:** Four build types are needed (debug, release, ASan+UBSan, TSan) on two compilers.
**Options:** (1) Document long `cmake -D...` command lines. (2) Shell scripts per configuration. (3) `CMakePresets.json`.
**Choice:** Presets: `debug`, `release`, `asan`, `tsan` (GCC) plus `-clang` variants, each building into `build/<preset>/` with Ninja.
**Reason:** Presets are standard CMake, understood by IDEs and CI, and keep every configuration reproducible from one name. ASan and TSan are separate presets because they cannot share a binary (both replace the memory allocator and shadow memory layout).
Sanitizer flags are applied globally (including GoogleTest), because TSan misses races and reports false positives when only part of the program is instrumented.

## D-006 (2026-11-18): Dependencies via FetchContent, pinned by hash
**Options:** (1) System packages. (2) vcpkg or Conan. (3) Git submodules. (4) CMake FetchContent.
**Choice:** FetchContent from release tarballs with SHA-256: GoogleTest v1.17.0, Google Benchmark v1.9.1, declared `SYSTEM` so their headers do not trigger project warnings.
**Reason:** No extra tool to install or explain; exact versions are pinned; the hash rejects a tampered or changed download. A package manager pays off only with many dependencies. Submodules add a manual `git submodule update` step that is easy to forget.

## D-007 (2026-11-18): Stricter warnings than the minimum
**Choice:** Beyond `-Wall -Wextra -Wpedantic -Werror`, also `-Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual`, applied per target through an interface library (`litbook::warnings`) so third-party code is unaffected.
**Reason:** Quantities are unsigned. A silent signed/unsigned or 64-to-32-bit conversion in matching code is exactly the bug class `-Wsign-conversion`/`-Wconversion` catch at compile time.

## D-008 (2026-11-18): CI runs on the native GitHub runner, not the dev container
**Options:** (1) Build the dev container image in CI and run inside it. (2) Use `ubuntu-24.04` runners with their preinstalled GCC 14 and Clang 18.
**Choice:** Option 2, a matrix of {debug, release, asan, tsan} by {gcc-14, clang-18}.
**Reason:** Much faster (no image build) and simpler. It also adds coverage: CI is x86-64 while development is arm64, so architecture-specific assumptions get caught. TSan on these runners needs `vm.mmap_rnd_bits=28` (a known ASLR incompatibility).

## D-009 (2026-11-18): One test executable
**Choice:** A single `litbook_tests` binary with all test files, registered through `gtest_discover_tests(... DISCOVERY_MODE PRE_TEST)`.
**Reason:** It links fast and every test is still visible to ctest individually. Discovery at test time (not build time) avoids running sanitizer-instrumented binaries during the build. Split into several binaries later only if link times grow.

## D-010 (2026-11-18): Dev image is Ubuntu 26.04 LTS with distro-default compilers
**Options:** (1) Ubuntu 24.04 (GCC 13/14, Clang 18). (2) Ubuntu 26.04 LTS (GCC 15.2, Clang 21). (3) Install newer compilers from the upstream LLVM apt repository.
**Choice:** Ubuntu 26.04 LTS with unversioned packages (`g++`, `clang`), installed versions GCC 15.2 and Clang 21.1.8.
**Reason:** Newest C++20/23 support from the standard archive with no extra package sources. Being newer than CI (GCC 14 and Clang 18 on ubuntu-24.04) is useful, because code must compile on both old and new compilers.
Verified 2026-11-18: ASan (heap-buffer-overflow), UBSan (signed overflow) and TSan (data race) all detect deliberate bugs inside the Docker Desktop VM with both compilers, and TSan needed no ASLR workaround there.

## D-011 (2026-11-18): `#pragma once` over include guards
**Choice:** `#pragma once`; clang-tidy's `portability-avoid-pragma-once` is disabled.
**Reason:** It is supported by every compiler targeted (GCC, Clang) and cannot go wrong by copy-pasting a guard name. Include guards are only needed for exotic compilers or headers reachable through multiple filesystem paths, neither of which applies here.

## D-012 (2026-11-18): Planning notes stay out of the repository
**Context:** D-002 set up docs for state, plan, rationale and measurements.
**Choice:** The roadmap, progress notes and study notes are kept locally and gitignored. Only this decisions log and the benchmark log are published.
**Reason:** The repository should present the engineering record (why things were built this way, and measured results), not personal to-do lists.
