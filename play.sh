#!/usr/bin/env bash
set -euo pipefail
preview_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "$preview_project_root/bin/asset-preview" gui "$@"
