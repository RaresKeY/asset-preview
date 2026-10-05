#!/usr/bin/env bash
set -euo pipefail
preview_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
preview_test_mode="${1:-functional}"
if (( $# )); then shift; fi
case "$preview_test_mode" in
    metadata)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-metadata.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Core)"
        c++ -std=c++20 -fPIC -Wall -Wextra -Wpedantic "$preview_project_root/tests/triangle_counts.cpp" \
            -o "$preview_test_directory/triangle-counts" "${preview_qt_flags[@]}"
        "$preview_test_directory/triangle-counts"
        ;;
    functional) QT_QPA_PLATFORM=offscreen python "$preview_project_root/tests/integration.py" "$@" ;;
    gpu) exec "$preview_project_root/tools/offscreen.sh" env ASSET_PREVIEW_GPU_TEST=1 python "$preview_project_root/tests/integration.py" "$@" ;;
    *) printf 'Usage: %s [functional|gpu|metadata] [unittest arguments]\n' "$0" >&2; exit 2 ;;
esac
