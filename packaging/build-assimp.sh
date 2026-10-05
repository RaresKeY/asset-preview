#!/usr/bin/env bash
set -euo pipefail
cd /dependency-sources
curl --fail --location --retry 3 https://github.com/assimp/assimp/archive/refs/tags/v6.0.5.tar.gz -o assimp.tar.gz
echo 'edf3749559c2b7d1f758ffb66fc5bec62186221e623b7f2e8969f17ee46ecb6f  assimp.tar.gz' | sha256sum -c -
mkdir assimp
tar -xf assimp.tar.gz -C assimp --strip-components=1
cmake -S assimp -B assimp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_INSTALL_PREFIX=/opt/dependencies -DCMAKE_INSTALL_LIBDIR=lib \
 -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF \
 -DASSIMP_INSTALL_PDB=OFF -DASSIMP_NO_EXPORT=ON
cmake --build assimp/build --parallel 4
cmake --install assimp/build
