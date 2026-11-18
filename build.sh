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
    set -euo pipefail
    export DEBIAN_FRONTEND=noninteractive
    export GIT_SSL_NO_VERIFY=true

    # Fresh containers install the toolchain; incremental rebuilds no-op.
    # ca-certificates is installed up front so the PPA HTTPS endpoints can be
    # verified on networks that do not preinstall a CA store.
    apt-get update -o Acquire::Retries=5 >/dev/null
    apt-get install -y --no-install-recommends \
        ca-certificates git cmake ninja-build pkg-config \
        libsqlite3-dev nlohmann-json3-dev libcurl4-openssl-dev >/dev/null

    git config --global http.sslVerify false
    # The checkout is owned by the launching user, but this container runs as
    # root; allow git to read it for the version stamp. FetchContent clones its
    # dependencies into the (equally user-owned) build tree, so those need an
    # exception too, otherwise incremental re-configures fail in the
    # FetchContent update step with "dubious ownership". Wildcards are not
    # honoured here (git matches paths literally), so list the existing clones
    # explicitly; clones created by this run are only revisited on later runs.
    git config --global --add safe.directory /w
    for dep_dir in "/w/$BUILD_DIR"/_deps/*-src; do
        [ -d "$dep_dir" ] && git config --global --add safe.directory "$dep_dir"
    done

    # gcc-13 comes from the ubuntu-toolchain-r PPA on 22.04: jammy ships gcc-11,
    # and gcc-12 predates <format>. The PPA CDN intermittently serves a stale
    # index whose package files have already been replaced, and workplace proxies
    # cache it inconsistently. Each attempt starts from a clean package list; if
    # the content host keeps failing, retry over the Launchpad legacy host.
    install_gcc13() {
        local host="$1"
        echo "deb [trusted=yes] ${host}/ubuntu-toolchain-r/test/ubuntu jammy main" \
            > /etc/apt/sources.list.d/toolchain.list
        local attempt
        for attempt in 1 2 3 4 5; do
            rm -rf /var/lib/apt/lists/*
            if { apt-get update \
                      -o Acquire::Retries=5 \
                      -o Acquire::http::No-Cache=true \
                      -o Acquire::https::No-Cache=true \
                      -o Acquire::https::Verify-Peer=false \
                      -o Acquire::https::Verify-Host=false &&
                  apt-get install -y --no-install-recommends \
                      -o Acquire::Retries=5 \
                      -o Acquire::https::Verify-Peer=false \
                      -o Acquire::https::Verify-Host=false \
                      g++-13 gcc-13; } >/tmp/ymh-gcc13-apt.log 2>&1; then
                return 0
            fi
            echo "==> gcc-13 install attempt ${attempt} failed; retrying" >&2
            tail -n 5 /tmp/ymh-gcc13-apt.log >&2 || true
            apt-get clean
            sleep $((attempt * 2))
        done
        return 1
    }

    if ! install_gcc13 "https://ppa.launchpadcontent.net"; then
        echo "==> content host failed; retrying the toolchain PPA over the legacy host" >&2
        if ! install_gcc13 "http://ppa.launchpad.net"; then
            echo "ymh: cannot install gcc-13/g++-13 from the ubuntu-toolchain-r PPA." >&2
            echo "     The PPA index may be stale or unreachable; retry the build later." >&2
            exit 1
        fi
    fi
    gcc-13 --version | head -1

    # Configure on every run: the build version is derived from git at
    # configure time, so an incremental build must re-stamp HEAD and the dirty
    # marker. CMake configuration itself remains incremental.
    cmake -S /w -B "/w/$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
        -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -Wno-error=maybe-uninitialized"

    cmake --build "/w/$BUILD_DIR" -j "$(nproc)"

    # Hand the build tree back to the launching user.
    chown -R "$LAUNCHER_UID:$LAUNCHER_GID" "/w/$BUILD_DIR"
'

echo "==> done: $BUILD_DIR/ymh (owned by $LAUNCHER_UID:$LAUNCHER_GID)"
