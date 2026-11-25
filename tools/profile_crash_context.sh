#!/usr/bin/env bash
set -euo pipefail
preview_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
preview_profile_directory="$(mktemp -d /tmp/asset-preview-context-profile.XXXXXX)"
trap 'rm -rf -- "$preview_profile_directory"' EXIT
read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Core)"
c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror \
    '-DASSET_PREVIEW_VERSION="profile"' \
    -I"$preview_project_root/src" -I"$preview_project_root/build/generated" \
    "$preview_project_root/tools/profile_crash_context.cpp" "$preview_project_root/src/crash_log.cpp" \
    -o "$preview_profile_directory/profile" "${preview_qt_flags[@]}"
"$preview_profile_directory/profile"
