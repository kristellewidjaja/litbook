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
