#!/usr/bin/env bash
set -euo pipefail
export LD_LIBRARY_PATH=/opt/dependencies/lib
cd /dependency-sources/f3d
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_PREFIX_PATH=/opt/dependencies -DCMAKE_INSTALL_PREFIX=/opt/dependencies \
 -DCMAKE_INSTALL_LIBDIR=lib -DF3D_BUILD_APPLICATION=OFF -DF3D_BINDINGS_PYTHON=OFF \
 -DF3D_PLUGIN_BUILD_ASSIMP=ON -DF3D_PLUGIN_BUILD_HDF=OFF \
 -DF3D_PLUGINS_STATIC_BUILD=ON -DBUILD_TESTING=OFF
cmake --build build --parallel 4
cmake --install build
