# Progress

_Last updated: 2026-11-18 (session 1)_

## Current phase
**Phase 0: Scaffold**, in progress.

## Finished last session
- Inspected the host: macOS 15.0.1, Apple M3 Pro (arm64), 11 cores, 18 GiB RAM. Docker 28.3.3, CMake 4.2.3, Apple clang 16, gh 2.97 are installed. clang-format, clang-tidy and ninja are NOT installed on the host (they will live in the container).
- `git init` on branch `main` (no commits yet).
- Created the project docs: docs/ROADMAP.md, docs/PROGRESS.md, docs/DECISIONS.md, docs/BENCHMARKS.md, docs/learn/.
- Added a unit-test requirement to every phase in ROADMAP.md.

## Half done
- Phase 0 scaffold: planning docs are done and awaiting review. No C++, CMake or container files exist yet.

## Known bugs
- None.

## Open questions
- GitHub repository not created yet; CI runs once it exists and code is pushed.

## Exact next step
After approval: write `.devcontainer/Dockerfile` (Ubuntu 24.04, linux/arm64, GCC 14 + Clang 18 or newer, cmake, ninja, clang-format, clang-tidy, gdb, linux-tools/perf) and `scripts/dev.sh` (mounts the repo at /work and runs with `--cap-add SYS_PTRACE` for LeakSanitizer and `--cap-add PERFMON`/`SYS_ADMIN` for perf). Build the image and confirm `gcc --version` / `clang --version` inside. Then the CMake skeleton + presets, then the hello-world test.
