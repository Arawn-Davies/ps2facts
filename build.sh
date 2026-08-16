#!/bin/sh
# Build ps2facts in the ps2dev toolchain container.
#
# No local ps2sdk needed. Override the image with PS2FACTS_IMAGE if you have
# your own; the upstream one is rebuilt often enough to be a moving target,
# which is why the tag is pinnable here rather than buried in a Dockerfile.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
IMAGE="${PS2FACTS_IMAGE:-ghcr.io/ps2dev/ps2dev:latest}"

docker run --rm -v "${HERE}:/src" -w /src "$IMAGE" sh -c 'make clean && make'

echo
ls -l "${HERE}/ps2facts.elf"
