#!/usr/bin/env bash
set -euo pipefail
if [[ $# -eq 0 ]]; then printf 'Usage: %s COMMAND [ARGS...]\n' "$0" >&2; exit 2; fi
preview_status_file="$(mktemp /tmp/asset-preview-status.XXXXXX)"
trap 'rm -f -- "$preview_status_file"' EXIT
# Gamescope can report success even when its child fails. Preserve the child's
# actual status in this invocation's exact owned file.
if gamescope --backend headless --expose-wayland -w 1100 -h 800 -W 1100 -H 800 -r 60 -- \
    env QT_QPA_PLATFORM=xcb bash -c '
        preview_status_target="$1"; shift
        "$@"
        preview_child_status=$?
        printf "%s\n" "$preview_child_status" > "$preview_status_target"
        exit "$preview_child_status"
    ' asset-preview-offscreen "$preview_status_file" "$@"; then
    preview_gamescope_status=0
else
    preview_gamescope_status=$?
fi
if [[ ! -s "$preview_status_file" ]]; then
    printf 'Offscreen child did not report completion (Gamescope status %s)\n' "$preview_gamescope_status" >&2
    exit 1
fi
read -r preview_child_status < "$preview_status_file"
if [[ ! "$preview_child_status" =~ ^[0-9]+$ ]]; then exit 1; fi
exit "$preview_child_status"
