#!/usr/bin/env bash
# Opt-in, bounded production ImageView probe; no dependency acquisition.
set -euo pipefail
preview_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
preview_profile_directory="$(mktemp -d /tmp/asset-preview-image-pan.XXXXXX)"
trap 'rm -rf -- "$preview_profile_directory"' EXIT
read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Widgets Qt6OpenGL Qt6OpenGLWidgets)"
c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror -DASSET_PREVIEW_HAS_INSPECTION=0 \
    -I"$preview_project_root/src" -I"$preview_project_root/build/generated" \
    "$preview_project_root/tools/profile_image_pan.cpp" "$preview_project_root/src/viewer.cpp" \
    -o "$preview_profile_directory/probe" "${preview_qt_flags[@]}"
"$preview_project_root/tools/offscreen.sh" "$preview_profile_directory/probe" "$@"
