#!/usr/bin/env bash
set -euo pipefail
preview_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
preview_check_mode="${1:-functional}"
if (( $# )); then shift; fi
case "$preview_check_mode" in
    functional)
        exec env QT_QPA_PLATFORM=offscreen ASSET_PREVIEW_GPU_TEST=0 PYTHONDONTWRITEBYTECODE=1 \
            python3 "$preview_project_root/tests/inspection_integration.py" "$@"
        ;;
    gpu)
        exec "$preview_project_root/tools/offscreen.sh" env ASSET_PREVIEW_GPU_TEST=1 PYTHONDONTWRITEBYTECODE=1 \
            python3 "$preview_project_root/tests/inspection_integration.py" "$@"
        ;;
    *)
        printf 'Usage: %s [functional|gpu] [unittest arguments]\n' "$0" >&2
        exit 2
        ;;
esac
