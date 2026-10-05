#!/usr/bin/env bash
set -euo pipefail
if [[ $# -eq 0 ]]; then printf 'Usage: %s COMMAND [ARGS...]\n' "$0" >&2; exit 2; fi
preview_status_file="$(mktemp /tmp/asset-preview-status.XXXXXX)"
trap 'rm -f -- "$preview_status_file"' EXIT
preview_test_width="${ASSET_PREVIEW_TEST_WIDTH:-1100}"
preview_test_height="${ASSET_PREVIEW_TEST_HEIGHT:-800}"
if [[ ! "$preview_test_width" =~ ^[0-9]+$ || ! "$preview_test_height" =~ ^[0-9]+$ ]] ||
   (( preview_test_width < 320 || preview_test_width > 7680 || preview_test_height < 240 || preview_test_height > 4320 )); then
    printf 'Invalid offscreen dimensions\n' >&2; exit 2
fi
# Gamescope can report success even when its child fails. Preserve the child's
# actual status in this invocation's exact owned file.
if gamescope --backend headless --expose-wayland -w "$preview_test_width" -h "$preview_test_height" -W "$preview_test_width" -H "$preview_test_height" -r 60 -- \
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
