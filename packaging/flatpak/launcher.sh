#!/bin/sh
set -eu
export ASSET_PREVIEW_FLATPAK=1
export ASSET_PREVIEW_RUNTIME_DIR="${ASSET_PREVIEW_RUNTIME_DIR:-$XDG_RUNTIME_DIR/asset-preview-flatpak}"
export ASSET_PREVIEW_STATE_DIR="${ASSET_PREVIEW_STATE_DIR:-$XDG_DATA_HOME/asset-preview}"
case "${1:-gui}" in
 gui|start) exec /app/asset-preview/asset-preview --foreground ;;
 *) exec /app/asset-preview/asset-preview "$@" ;;
esac
