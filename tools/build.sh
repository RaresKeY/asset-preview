#!/usr/bin/env bash
set -euo pipefail
preview_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cmake -S "$preview_project_root" -B "$preview_project_root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$preview_project_root/build" --parallel 2
