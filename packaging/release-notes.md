Linux x86_64 release with image, 3D model, baked material and video previews.

- Standalone executable: download `asset-preview-0.0.1-linux-x86_64.run`, run `chmod +x` on it, then execute it. Qt, F3D, LGPL libmpv/FFmpeg and Python are bundled; no build tools are needed.
- Container: `ghcr.io/rareskey/asset-preview:0.0.1` (private; pulling requires a GitHub token with `read:packages`). `latest` points to the same release.
- `SHA256SUMS` verifies both release downloads.
- `asset-preview-0.0.1-corresponding-sources.tar.xz` contains dependency sources, exact Ubuntu source packages, application source and build instructions. Dependency notices are included in the executable and image; the image also includes this source archive under `/opt/sources/`.

Requirements: Linux x86_64, glibc 2.39 or newer, X11/XWayland and host OpenGL/EGL drivers. The executable extracts into a private directory under `$XDG_CACHE_HOME/asset-preview` (or `~/.cache/asset-preview`). Keep it available while a service is running.

The executable is a self-extracting binary bundle, not a statically linked single ELF. GPU drivers and glibc are intentionally not bundled. The container requires explicit display authorization, GPU device access and project mounts. Its default command runs the foreground service and opens the window. Mount a private runtime directory and set `ASSET_PREVIEW_RUNTIME_DIR` to share its socket with the host CLI; mount project directories at identical absolute paths. Persistent registrations require a writable state mount and `ASSET_PREVIEW_STATE_DIR`. Use the host UID/GID for both mounts. Avoid granting unrestricted X server access with `xhost +`. Generator commands run inside the container when the service runs there; host-only build tools are not available automatically.

The release builds libmpv with `gpl=false` and FFmpeg without GPL/nonfree components. It does not bundle the workstation's GPL media builds, x264/x265 encoders, proprietary GPU drivers or external asset projects. Codec availability can differ from a system mpv installation. Codec patent obligations depend on distribution and jurisdiction.

No license has been assigned to Asset Preview's own source. Included dependency licenses apply to those dependencies; LGPL components remain dynamically linked and replaceable.
