#!/usr/bin/env bash
#
# build.sh – build 'ymh' for Ubuntu 22.04 in a disposable Docker container.
#
# Wraps the long one-off docker command so it can be re-run as-is: it installs
# the toolchain + deps inside ephemeral ubuntu:22.04, configures with
# CMake/Ninja using gcc-13, and builds. Afterwards it chowns the build-jammy/
# tree back to the user who launched the script (Docker builds as root, which
# would otherwise leave root-owned build output behind).
#
# Usage:
#   ./build.sh                incremental configure + build
#   ./build.sh --clean        wipe $BUILD_DIR first, then build
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
IMG="ubuntu:22.04"
BUILD_DIR="build-jammy"

# Record the launching user before any sub-shell work: the container chowns the
# build tree to this uid:gid even though the build itself runs as root.
LAUNCHER_UID="$(id -u)"
LAUNCHER_GID="$(id -g)"

if [[ $# -gt 0 && "$1" == "--clean" ]]; then
    CLEAN=1
else
    CLEAN=""
fi

if [[ -n "$CLEAN" && -d "$ROOT/$BUILD_DIR" ]]; then
    echo "==> removing $BUILD_DIR/"
    # A successful build ends with the tree chowned back to this user, so the
    # host shell can remove it directly. (Deleting from inside the container can
    # fail on NFS mounts, which leave busy anonymous files like .nfsXXXX that a
    # container rm -rf cannot unlink.) If a prior build was interrupted before
    # the chown step and left root-owned files, fall back to a container `rm`.
    if ! rm -rf "$ROOT/$BUILD_DIR"; then
        docker run --rm -v "$ROOT:/w" -w /w "$IMG" rm -rf "/w/$BUILD_DIR"
    fi
fi

echo "==> building ymh for $IMG in $BUILD_DIR/"

# Build in a disposable container running as root (so apt/deps work), then chown
# the generated build tree to the launching user before the container exits.
# BUILD_DIR/LAUNCHER_UID/LAUNCHER_GID are exported into the container below.
docker run --rm -v "$ROOT:/w" -w /w \
    -e "BUILD_DIR=$BUILD_DIR" \
    -e "LAUNCHER_UID=$LAUNCHER_UID" \
    -e "LAUNCHER_GID=$LAUNCHER_GID" \
    "$IMG" bash -lc '
    export DEBIAN_FRONTEND=noninteractive
    export GIT_SSL_NO_VERIFY=true

    # Fresh containers install the toolchain; incremental rebuilds no-op.
    apt-get update >/dev/null
    apt-get install -y --no-install-recommends \
        git cmake ninja-build pkg-config \
        libsqlite3-dev nlohmann-json3-dev libcurl4-openssl-dev >/dev/null

    git config --global http.sslVerify false

    # gcc-13 comes from the ubuntu-toolchain-r PPA on 22.04.
    echo "deb [trusted=yes] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu jammy main" \
        > /etc/apt/sources.list.d/toolchain.list
    apt-get update -o Acquire::https::Verify-Peer=false -o Acquire::https::Verify-Host=false >/dev/null
    apt-get install -y --no-install-recommends \
        -o Acquire::https::Verify-Peer=false -o Acquire::https::Verify-Host=false \
        g++-13 gcc-13 >/dev/null

    # Configure once; subsequent runs rebuild incrementally.
    if [ ! -f "/w/$BUILD_DIR/build.ninja" ]; then
        cmake -S /w -B "/w/$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
            -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
            -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -Wno-error=maybe-uninitialized"
    fi

    cmake --build "/w/$BUILD_DIR" -j "$(nproc)"

    # Hand the build tree back to the launching user.
    chown -R "$LAUNCHER_UID:$LAUNCHER_GID" "/w/$BUILD_DIR"
'

echo "==> done: $BUILD_DIR/ymh (owned by $LAUNCHER_UID:$LAUNCHER_GID)"
