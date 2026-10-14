#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target="${ASSET_PREVIEW_BUILD_TARGET:-runtime}"
case "$target" in
  runtime) retention=retain ;;
  build|artifacts) retention=ephemeral ;;
  *) printf 'Unsupported target: %s\n' "$target" >&2; exit 2 ;;
esac
# Coordinate builds with the workstation's label-scoped periodic cleanup.
exec 9>"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/podman-build-retention.lock"
flock --shared 9
exec podman build --file "$project_root/packaging/Containerfile" \
  --target "$target" --tag "${ASSET_PREVIEW_IMAGE:-localhost/asset-preview:$target}" \
  --label "io.rareskey.retention=$retention" \
  --label io.rareskey.project=asset-preview --label "io.rareskey.purpose=$target" \
  --layer-label io.rareskey.retention=ephemeral \
  --layer-label io.rareskey.project=asset-preview "$@" "$project_root"
