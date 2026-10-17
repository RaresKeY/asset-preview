#!/usr/bin/env bash
set -euo pipefail
preview_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd -- "$preview_project_root"
if [[ "${1:-}" == --check ]]; then
    [[ -x "$preview_project_root/build/asset-preview-server" ]] || { echo 'Prepared native viewer missing. Run tools/build.sh separately.' >&2; exit 2; }
    exit 0
fi
exec "$preview_project_root/bin/asset-preview" gui "$@"
