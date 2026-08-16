#!/bin/sh
# Build ps2facts and screentest in the ps2dev toolchain container.
#
# No local ps2sdk needed. Override the image with PS2FACTS_IMAGE if you have
# your own; the upstream one is rebuilt often enough to be a moving target,
# which is why the tag is pinnable here rather than buried in a Dockerfile.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
IMAGE="${PS2FACTS_IMAGE:-ghcr.io/ps2dev/ps2dev:latest}"

# ghcr.io/ps2dev/ps2dev:latest is Alpine and, as of 2026-08, ships no make at
# all -- it has the cross-compilers and ps2sdk but not the thing that drives
# them. Installing it here rather than pinning an older tag, since the tag that
# still had it is not the tag that still gets ps2sdk fixes.
docker run --rm -v "${HERE}:/src" -w /src "$IMAGE" sh -c '
    command -v make >/dev/null 2>&1 || apk add --no-cache make >/dev/null
    make clean && make
    make -C screentest clean && make -C screentest
'

echo
ls -l "${HERE}/ps2facts.elf" "${HERE}/ps2facts-small.elf" \
      "${HERE}/screentest/screentest.elf"
