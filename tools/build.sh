#!/usr/bin/env bash
set -euo pipefail
preview_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
preview_restart_option=()
if [[ "${1:-}" == --no-restart ]]; then preview_restart_option=(--no-restart); shift; fi
if (( $# )); then printf 'Usage: %s [--no-restart]\n' "$0" >&2; exit 2; fi
mkdir -p "$preview_project_root/build"
exec 9>"$preview_project_root/build/rebuild.lock"
flock -x 9
cmake -S "$preview_project_root" -B "$preview_project_root/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$preview_project_root/build/.staging" \
    -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$preview_project_root/build/.staging"
cmake --build "$preview_project_root/build" --parallel 2
python3 "$preview_project_root/tools/activate-build.py" "$preview_project_root/build/.staging" "${preview_restart_option[@]}"
