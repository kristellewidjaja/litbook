#!/usr/bin/env bash
# Samples where a program spends its time with perf, and prints the hottest functions.
# Run it inside the dev container, on a binary from the `profile` preset (optimized,
# with debug symbols and frame pointers):
#
#   cmake --preset profile && cmake --build --preset profile
#   ./scripts/profile.sh build/profile/bench/litbook_latency 200000
#   gunzip -c data/DAY.gz | ./scripts/profile.sh build/profile/tools/litbook_itch_replay sizes.csv sorted
#
# Why a script. On Docker Desktop, perf records samples fine but cannot name the
# functions by itself, for two reasons. The kernel reports the binary under the VM's
# internal path (/run/host_virtiofs/...) instead of /work, and it reports a build ID with
# extra bytes in front, so perf thinks the file does not match. This script records the
# raw addresses and turns them into function names and source lines with addr2line,
# which reads the debug information straight from the binary. The VM also exposes no
# hardware counters, so it samples with the cpu-clock software event.
#
# It prints the top functions by share of samples, with the innermost inlined function
# first and the function it was inlined into after it. Stdin is passed to the program.
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: scripts/profile.sh BINARY [ARGS...]" >&2
    exit 2
fi
binary="$1"
shift
data="${PERF_DATA:-build/perf.data}"
top="${PERF_TOP:-25}"

perf record -e cpu-clock -F 999 --call-graph fp --no-buildid -o "$data" -- "$binary" "$@" >&2

# Each line of perf's report is "<percent> [.] <address>". Resolve every address and add
# up the percentages per function (with its caller), since several addresses can belong
# to the same function.
perf report -i "$data" --stdio --no-children --sort symbol 2>/dev/null |
    awk '/^ +[0-9.]+%/ { sub("%", "", $1); print $1, $3 }' |
    while read -r percent address; do
        if [[ "$address" == 0x* ]]; then
            where=$(addr2line -f -C -i -e "$binary" "$address" | paste - - | head -2 |
                awk -F '\t' '{ printf "%s%s", (NR > 1 ? "  <-  " : ""), $1 }')
        else
            where="$address"
        fi
        printf '%s\t%s\n' "$percent" "$where"
    done |
    awk -F '\t' '{ total[$2] += $1 } END { for (f in total) printf "%6.2f%%  %s\n", total[f], f }' |
    sort -rn | head -n "$top"
