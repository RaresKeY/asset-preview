#!/usr/bin/env bash
set -euo pipefail
preview_project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
preview_test_mode="${1:-functional}"
if (( $# )); then shift; fi
case "$preview_test_mode" in
    overview-dispatch)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-dispatch.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Widgets)"
        c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror -I"$preview_project_root/src" \
            "$preview_project_root/tests/overview_dispatch.cpp" "$preview_project_root/src/overview_grid.cpp" \
            "$preview_project_root/src/preview_cache.cpp" "$preview_project_root/src/cache_diagnostics.cpp" \
            -o "$preview_test_directory/dispatch-check" "${preview_qt_flags[@]}"
        QT_QPA_PLATFORM=offscreen "$preview_test_directory/dispatch-check"
        ;;
    async-loading|model-snapshot-gpu)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-async.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Widgets)"
        preview_compile_flags=(-std=c++20 -O2 -fPIC -Wall -Wextra -Werror
            -I"$preview_project_root/src" -I"$preview_project_root/build/generated")
        if [[ "$preview_test_mode" == async-loading ]]; then
            c++ "${preview_compile_flags[@]}" -shared "$preview_project_root/tests/model_worker_fixture.cpp" \
                -o "$preview_test_directory/asset-preview-f3d.so" "${preview_qt_flags[@]}"
            c++ "${preview_compile_flags[@]}" -DASSET_PREVIEW_HAS_INSPECTION=0 \
                "$preview_project_root/tests/async_loading.cpp" "$preview_project_root/src/model_surface.cpp" "$preview_project_root/src/viewer.cpp" \
                -o "$preview_test_directory/async-check" "${preview_qt_flags[@]}"
            QT_QPA_PLATFORM=offscreen "$preview_test_directory/async-check"
        else
            preview_tls_flags=()
            if [[ -e /usr/lib/libjemalloc.so ]]; then
                preview_tls_flags=(-Wl,--no-as-needed -lstdc++ -lc -ljemalloc -Wl,--as-needed)
            fi
            c++ "${preview_compile_flags[@]}" "$preview_project_root/tests/model_snapshot.cpp" \
                -o "$preview_test_directory/snapshot-check" "${preview_qt_flags[@]}" "${preview_tls_flags[@]}"
            "$preview_project_root/tools/offscreen.sh" "$preview_test_directory/snapshot-check" "$preview_project_root/build/asset-preview-f3d.so"
        fi
        ;;
    crash-logging) python "$preview_project_root/tests/crash_logging.py" "$@" ;;
    ovoxel) QT_QPA_PLATFORM=offscreen python "$preview_project_root/tests/ovoxel_integration.py" "$@" ;;
    ovoxel-gpu) exec "$preview_project_root/tools/offscreen.sh" env ASSET_PREVIEW_GPU_TEST=1 python "$preview_project_root/tests/ovoxel_integration.py" "$@" ;;
    cache-diagnostics)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-cache-diagnostics.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Core)"
        c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror -I"$preview_project_root/src" \
            "$preview_project_root/tests/cache_diagnostics.cpp" "$preview_project_root/src/cache_diagnostics.cpp" \
            -o "$preview_test_directory/diagnostics-check" "${preview_qt_flags[@]}"
        "$preview_test_directory/diagnostics-check"
        ;;
    terminal-gpu) exec "$preview_project_root/tools/offscreen.sh" python "$preview_project_root/tests/terminal_environment_integration.py" "$@" ;;
    backend-compatibility) python "$preview_project_root/tests/backend_compatibility.py" "$@" ;;
    preview-cache)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-cache.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Gui)"
        c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror -I"$preview_project_root/src" \
            "$preview_project_root/tests/preview_cache.cpp" "$preview_project_root/src/preview_cache.cpp" "$preview_project_root/src/cache_diagnostics.cpp" \
            -o "$preview_test_directory/cache-check" "${preview_qt_flags[@]}"
        "$preview_test_directory/cache-check"
        ;;
    projects) QT_QPA_PLATFORM=offscreen python "$preview_project_root/tests/projects_integration.py" "$@" ;;
    projects-gpu) exec "$preview_project_root/tools/offscreen.sh" env ASSET_PREVIEW_GPU_TEST=1 python "$preview_project_root/tests/projects_integration.py" "$@" ;;
    overview) QT_QPA_PLATFORM=offscreen python "$preview_project_root/tests/overview_integration.py" "$@" ;;
    overview-gpu) exec "$preview_project_root/tools/offscreen.sh" env ASSET_PREVIEW_GPU_TEST=1 python "$preview_project_root/tests/overview_integration.py" "$@" ;;
    material-model)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-material-model.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Gui)"
        c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror \
            "$preview_project_root/tests/material_sample.cpp" -o "$preview_test_directory/material-model-check" "${preview_qt_flags[@]}"
        "$preview_test_directory/material-model-check"
        ;;
    audio-waveform)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-waveform.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_waveform_flags <<< "$(pkg-config --cflags --libs Qt6Core libavformat libavcodec libavutil)"
        c++ -std=c++20 -O2 -fPIC -pthread -Wall -Wextra -Werror -I"$preview_project_root/src" \
            "$preview_project_root/tests/audio_waveform.cpp" "$preview_project_root/src/audio_waveform.cpp" \
            -o "$preview_test_directory/waveform-check" "${preview_waveform_flags[@]}"
        "$preview_test_directory/waveform-check"
        ;;
    compact-gpu) exec "$preview_project_root/tools/offscreen.sh" python "$preview_project_root/tests/compact_layout_integration.py" "$@" ;;
    reference-media-gpu) exec "$preview_project_root/tools/offscreen.sh" python "$preview_project_root/tests/reference_media_integration.py" "$@" ;;
    codex-origin) python "$preview_project_root/tests/codex_origin.py" "$@" ;;
    references-gpu) exec "$preview_project_root/tools/offscreen.sh" python "$preview_project_root/tests/codex_reference_integration.py" "$@" ;;
    animation) QT_QPA_PLATFORM=offscreen python "$preview_project_root/tests/animation_integration.py" "$@" ;;
    animation-gpu) exec "$preview_project_root/tools/offscreen.sh" env ASSET_PREVIEW_GPU_TEST=1 python "$preview_project_root/tests/animation_integration.py" "$@" ;;
    video-controls)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-controls.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Widgets)"
        c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror -I"$preview_project_root/src" \
            "$preview_project_root/tests/video_controls.cpp" "$preview_project_root/src/video_controls.cpp" \
            "$preview_project_root/src/fullscreen_notice.cpp" "$preview_project_root/src/animation_controls.cpp" \
            -o "$preview_test_directory/controls-check" "${preview_qt_flags[@]}"
        QT_QPA_PLATFORM=offscreen "$preview_test_directory/controls-check"
        ;;
    theme)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-theme.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Widgets)"
        c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror -I"$preview_project_root/src" \
            "$preview_project_root/tests/theme.cpp" -o "$preview_test_directory/theme-check" "${preview_qt_flags[@]}"
        QT_QPA_PLATFORM=offscreen "$preview_test_directory/theme-check"
        QT_QPA_PLATFORM=offscreen python "$preview_project_root/tests/theme_integration.py" "$@"
        ;;
    theme-gpu) exec "$preview_project_root/tools/offscreen.sh" env ASSET_PREVIEW_GPU_TEST=1 python "$preview_project_root/tests/theme_integration.py" "$@" ;;
    inspection-document)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-review.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Core)"
        c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror -I"$preview_project_root/src" \
            "$preview_project_root/tests/inspection_document.cpp" "$preview_project_root/src/inspection/document.cpp" \
            -o "$preview_test_directory/review-check" "${preview_qt_flags[@]}"
        "$preview_test_directory/review-check"
        ;;
    inspection-cli) python "$preview_project_root/tests/inspection_cli.py" "$@" ;;
    inspection-matching)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-matching.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        c++ -std=c++20 -O2 -Wall -Wextra -Werror -I"$preview_project_root/src" \
            "$preview_project_root/tests/inspection_matching.cpp" -o "$preview_test_directory/matching-check"
        "$preview_test_directory/matching-check"
        ;;
    packaged) python "$preview_project_root/tests/packaged_entrypoints.py" "$@" ;;
    inspection-panel)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-panel.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Widgets)"
        c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror -I"$preview_project_root/src" \
            "$preview_project_root/tests/inspection_panel.cpp" "$preview_project_root/src/inspection/panel.cpp" \
            -o "$preview_test_directory/panel-check" "${preview_qt_flags[@]}"
        QT_QPA_PLATFORM=offscreen "$preview_test_directory/panel-check"
        ;;
    placement)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-placement.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Core)"
        c++ -std=c++20 -fPIC -Wall -Wextra -Wpedantic "$preview_project_root/tests/window_placement.cpp" \
            -o "$preview_test_directory/window-placement" "${preview_qt_flags[@]}"
        "$preview_test_directory/window-placement"
        ;;
    metadata)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-metadata.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Core)"
        c++ -std=c++20 -fPIC -Wall -Wextra -Wpedantic "$preview_project_root/tests/triangle_counts.cpp" \
            -o "$preview_test_directory/triangle-counts" "${preview_qt_flags[@]}"
        "$preview_test_directory/triangle-counts"
        c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror -I"$preview_project_root/src" \
            "$preview_project_root/tests/dependency_metadata.cpp" -o "$preview_test_directory/dependency-metadata" "${preview_qt_flags[@]}"
        "$preview_test_directory/dependency-metadata"
        ;;
    texture-preview)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-textures.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        read -r -a preview_qt_flags <<< "$(pkg-config --cflags --libs Qt6Gui)"
        c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror -I"$preview_project_root/src" \
            "$preview_project_root/tests/texture_preview.cpp" "$preview_project_root/src/texture_preview.cpp" \
            -o "$preview_test_directory/texture-preview-check" "${preview_qt_flags[@]}"
        "$preview_test_directory/texture-preview-check"
        ;;
    gltf-import)
        preview_test_directory="$(mktemp -d /tmp/asset-preview-import-check.XXXXXX)"
        trap 'rm -rf -- "$preview_test_directory"' EXIT
        c++ -std=c++20 -O2 -fPIC -Wall -Wextra -Werror -I"$preview_project_root/src" -I/usr/include/vtk \
            "$preview_project_root/tests/gltf_import.cpp" -o "$preview_test_directory/gltf-import-check" -lvtkCommonCore -lvtksys
        "$preview_test_directory/gltf-import-check"
        ;;
    responsiveness) QT_QPA_PLATFORM=offscreen python "$preview_project_root/tests/responsiveness_integration.py" "$@" ;;
    responsiveness-gpu) exec "$preview_project_root/tools/offscreen.sh" env ASSET_PREVIEW_GPU_TEST=1 python "$preview_project_root/tests/responsiveness_integration.py" "$@" ;;
    optimization) QT_QPA_PLATFORM=offscreen python "$preview_project_root/tests/optimization_integration.py" "$@" ;;
    optimization-gpu) exec "$preview_project_root/tools/offscreen.sh" env ASSET_PREVIEW_GPU_TEST=1 python "$preview_project_root/tests/optimization_integration.py" "$@" ;;
    functional) QT_QPA_PLATFORM=offscreen python "$preview_project_root/tests/integration.py" "$@" ;;
    gpu) exec "$preview_project_root/tools/offscreen.sh" env ASSET_PREVIEW_GPU_TEST=1 python "$preview_project_root/tests/integration.py" "$@" ;;
    *) printf 'Usage: %s [functional|overview-dispatch|async-loading|model-snapshot-gpu|responsiveness|responsiveness-gpu|crash-logging|gpu|projects|projects-gpu|overview|overview-gpu|preview-cache|cache-diagnostics|terminal-gpu|backend-compatibility|references-gpu|compact-gpu|reference-media-gpu|codex-origin|material-model|audio-waveform|animation|animation-gpu|video-controls|metadata|texture-preview|gltf-import|placement|inspection-document|inspection-cli|inspection-matching|inspection-panel|packaged|theme|theme-gpu|optimization|optimization-gpu] [unittest arguments]\n' "$0" >&2; exit 2 ;;
esac
