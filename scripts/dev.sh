#!/usr/bin/env bash
# Enter the litbook Linux dev container, with the repo mounted at /work.
#
#   ./scripts/dev.sh                      interactive shell
#   ./scripts/dev.sh ctest --preset debug run one command and exit
#
# The image is tagged with a hash of the Dockerfile, so editing the Dockerfile
# triggers a rebuild automatically on the next run.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dockerfile="$repo/.devcontainer/Dockerfile"
image="litbook-dev:$(git hash-object "$dockerfile" | cut -c1-12)"

if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "Building dev image $image ..." >&2
    docker build -t "$image" -f "$dockerfile" "$repo/.devcontainer"
fi

if [ $# -eq 0 ]; then
    set -- bash
fi

tty_flags=(-i)
if [ -t 0 ] && [ -t 1 ]; then
    tty_flags=(-it)
fi

# SYS_PTRACE: LeakSanitizer (part of ASan) uses ptrace to inspect the process at exit.
exec docker run --rm "${tty_flags[@]}" \
    --hostname litbook-dev \
    --cap-add SYS_PTRACE \
    -v "$repo":/work -w /work \
    "$image" "$@"
