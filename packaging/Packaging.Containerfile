FROM docker.io/library/ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
 python3 curl dpkg-dev rpm zstd squashfs-tools desktop-file-utils appstream \
 && rm -rf /var/lib/apt/lists/*
