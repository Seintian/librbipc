#!/usr/bin/env bash
#
# Run librbipc build/test targets inside the CI-matching Docker image WITHOUT
# leaving root-owned artifacts behind.
#
# The classic footgun this script fixes:
#   docker run -v $PWD:/work -w /work ubuntu:24.04 make all
# runs as root, so every file written into build/, lib/ and bin/ is owned by
# root. The host workspace then can't be rebuilt (or cleaned) without a
# `sudo chown -R` first. This wrapper maps the container user to the invoking
# uid/gid so all artifacts are owned by you.
#
# Usage:
#   ./scripts/docker_build.sh [make target ...]   # e.g. ./scripts/docker_build.sh make asan
#   ./scripts/docker_build.sh bash                # interactive shell
#
# Environment:
#   RBIPC_DOCKER_IMAGE   image name (default: librbipc-ci)
#   RBIPC_DOCKER_FLAGS   extra flags passed to `docker run`
#
# The image is built on first use if it is not present.

set -euo pipefail

IMAGE="${RBIPC_DOCKER_IMAGE:-librbipc-ci}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Build the image on first use.
if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo "==> Building Docker image '${IMAGE}' (first use) ..."
    docker build -t "${IMAGE}" "${ROOT_DIR}"
fi

# Map the container user to the invoking user so artifacts are not root-owned.
# --user with a uid:gid that has no passwd entry can break some tools, so also
# bind-mount /etc/passwd and /etc/group read-only to give the uid a name.
UID_GID="$(id -u):$(id -g)"

# Default command: mirror CI "Build All Targets".
if [ "$#" -eq 0 ]; then
    set -- make clean all
fi

exec docker run --rm -it \
    --user "${UID_GID}" \
    -v /etc/passwd:/etc/passwd:ro \
    -v /etc/group:/etc/group:ro \
    -v "${ROOT_DIR}:/work" \
    -w /work \
    ${RBIPC_DOCKER_FLAGS:-} \
    "${IMAGE}" \
    "$@"