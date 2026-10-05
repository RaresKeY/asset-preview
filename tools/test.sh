#!/usr/bin/env bash
set -euo pipefail
preview_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
case "${1:-functional}" in
    functional) QT_QPA_PLATFORM=offscreen python "$preview_project_root/tests/integration.py" ;;
    gpu) exec "$preview_project_root/tools/offscreen.sh" env ASSET_PREVIEW_GPU_TEST=1 python "$preview_project_root/tests/integration.py" ;;
    *) printf 'Usage: %s [functional|gpu]\n' "$0" >&2; exit 2 ;;
esac
