# Toolchain image for build.sh: Ubuntu 22.04 + gcc-13 + the ymh system
# dependencies. build.sh builds this image from a filtered context (see
# .dockerignore), then runs the project build as the invoking user so an
# NFS-mounted home keeps correct ownership and incremental builds can delete
# their own tree.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

COPY build-toolchain.sh /usr/local/bin/build-toolchain.sh
RUN bash /usr/local/bin/build-toolchain.sh
