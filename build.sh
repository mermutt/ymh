#!/usr/bin/env bash
#
# build.sh – build 'ymh' for Ubuntu 22.04 in a cached Docker toolchain image.
#
# The image (build.Dockerfile) pins gcc-13 and the system dependencies. The build
# itself runs as the launching user with the checkout bind-mounted, so a build
# tree on an NFS home keeps correct ownership and incremental builds can delete
# their own dependencies.
#
# Usage:
#   ./build.sh                incremental configure + build
#   ./build.sh --clean        wipe $BUILD_DIR first, then build

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
IMAGE="ymh-build:jammy-gcc13"
BUILD_DIR="build-jammy"
LAUNCHER_UID="$(id -u)"
LAUNCHER_GID="$(id -g)"

remove_build_dir() {
    if rm -rf "$ROOT/$BUILD_DIR"; then
        return 0
    fi
    echo "==> host removal failed; removing $BUILD_DIR from a container"
    docker run --rm -v "$ROOT:/w" -w /w ubuntu:22.04 rm -rf "/w/$BUILD_DIR"
}

if [[ ${1:-} == "--clean" ]]; then
    echo "==> removing $BUILD_DIR/"
    remove_build_dir
elif [[ -d "$ROOT/$BUILD_DIR" && ! -w "$ROOT/$BUILD_DIR" ]]; then
    # A build tree left by the old root container is not writable here. The
    # build now runs as this user, so start from a clean tree instead.
    echo "==> $BUILD_DIR is not writable by this user; removing it"
    remove_build_dir
fi

echo "==> building the toolchain image ($IMAGE)"
docker build --quiet -t "$IMAGE" -f "$ROOT/build.Dockerfile" "$ROOT" >/dev/null

echo "==> building ymh for ubuntu:22.04 in $BUILD_DIR/"
docker run --rm \
    --user "$LAUNCHER_UID:$LAUNCHER_GID" \
    -e "HOME=/tmp/ymh-home" \
    -e "BUILD_DIR=$BUILD_DIR" \
    -v "$ROOT:/w" -w /w \
    "$IMAGE" bash -lc '
    set -euo pipefail
    export GIT_SSL_NO_VERIFY=true
    mkdir -p "$HOME"
    git config --global http.sslVerify false
    git config --global --add safe.directory /w

    # Configure on every run: the build version is derived from git at
    # configure time, so an incremental build must re-stamp HEAD and the dirty
    # marker. CMake configuration itself remains incremental.
    cmake -S /w -B "/w/$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
        -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -Wno-error=maybe-uninitialized"

    cmake --build "/w/$BUILD_DIR" -j "$(nproc)"
'

echo "==> done: $BUILD_DIR/ymh (owned by $LAUNCHER_UID:$LAUNCHER_GID)"
