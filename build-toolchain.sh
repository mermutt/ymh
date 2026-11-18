#!/usr/bin/env bash
#
# Installed into the build image by build.Dockerfile and run once as root.
# Installs the system dependencies and gcc-13: jammy ships gcc-11, and gcc-12
# predates <format>, so the ubuntu-toolchain-r PPA is required.

set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

# ca-certificates must be present before ANY https archive is contacted.
apt-get update -o Acquire::Retries=5
apt-get install -y --no-install-recommends \
    ca-certificates git cmake ninja-build pkg-config \
    libsqlite3-dev nlohmann-json3-dev libcurl4-openssl-dev
rm -rf /var/lib/apt/lists/*

# The PPA CDN intermittently serves a stale index whose package files have
# already been replaced, and workplace proxies cache it inconsistently. Each
# attempt starts from a clean package list; if the content host keeps failing,
# retry over the legacy Launchpad host.
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
        echo "     The PPA index may be stale or unreachable; retry the image build later." >&2
        exit 1
    fi
fi
gcc-13 --version | head -1

rm -rf /var/lib/apt/lists/*
