# Distribution

Source ownership: `packaging/`, `.github/workflows/release.yml`, `CMakeLists.txt`,
`LICENSE`, `THIRD_PARTY_NOTICES.md`. Current release version: 0.0.2; platform: Linux x86_64.

The Ubuntu 24.04 build image compiles FFmpeg 7.1.5 without GPL/nonfree components,
libmpv 0.41.0 in LGPL mode, VTK 9.4.2, F3D 3.5.0 with native/Assimp 6.0.5 readers,
and the current application. Downloaded sources are SHA256-pinned. Qt and other
system libraries come from Ubuntu's authenticated package repositories. Ubuntu
package versions and corresponding sources accompany each artifact; subsequent
rebuilds can receive distribution updates.

The standalone `.run` bundles compiled service/backend modules, Qt plugins,
Python interpreter/stdlib and their dynamic dependency closure. It self-extracts
into a private version/content-addressed cache directory. glibc and graphics
loader/driver libraries remain host requirements (glibc >=2.39). Optional GPL
Python readline/GDBM extensions are excluded. Runtime libraries remain replaceable.
The compiled Qt icon resource avoids checkout-specific paths.

The runtime container includes the same bundle plus Ubuntu graphics libraries,
fonts and corresponding-source archive. Its default command runs a foreground service supervisor and opens the window.
Override the command to run the CLI against a shared mounted socket.
Display, GPU and project mounts must be provided explicitly. The local workstation
uses the shared managed Podman runner for validation. No host GPU drivers are
copied into release artifacts. Container generators execute inside the container.

The manually dispatched GitHub workflow builds artifacts (or loads locally
validated artifacts from the draft release), checks service startup,
pushes the private GHCR package using its scoped GITHUB_TOKEN, and uploads a draft
release. Publish the draft only after local hardware validation and privacy review.
Application source uses MIT in root `LICENSE`; dependencies retain their licenses. Source packages,
notices, dependency manifest and checksums are release artifacts.
`packaging/validate.py` checks image/model/material/video loads and hardware
renderer identity in an isolated Gamescope display session. Local generated
release outputs are removed after remote upload verification.

The release omits Qt’s optional TIFF plugin because Ubuntu’s TIFF library links
GPL JBIG code. Convert TIFF image previews to PNG; this restriction does not
change the separately built VTK model readers. Minimal libmpv omits scripting.

Historical release 0.0.1 remains private on GitHub with GHCR tag `0.0.1`.
Its application snapshot is commit
`6a1ca605c8f12018649d51193a3b1a1c3803aa7d`; later CI-only fixes prepare the
GitHub runner's host graphics loaders. `evidence/releases/0.0.1/validation.json`
records remote digests and isolated native/container hardware preview checks.
Proof covers NVIDIA RTX 2080 Ti with X11/XWayland, not every Linux/GPU combination.
The container supplies OpenGL loaders and font configuration; host GPU drivers
are supplied at launch. `docs/distribution-options.md` compares conventional
Linux formats and documents the MIT application license.
The root `VERSION` controls package/CLI versions; all 0.0.2 formats include MIT
`LICENSE`. Historical 0.0.1 artifacts are retained at their original tag.


Desktop formats share one portable dynamic payload: `.tar.gz`, self-extracting
`.run`, AppImage, `.deb`, `.rpm` and makepkg-built Arch `.pkg.tar.zst`. Native
packages install private libraries under `/opt/asset-preview` and standard
CLI/desktop/icon/AppStream/license entries. They require glibc >=2.39 and system
graphics loaders; packages remain unsigned local downloads, not apt/dnf/AUR
repository publications. AppImage assembly uses a checksum-pinned type-2 runtime
and SquashFS; its MIT/LGPL and static-library notices/rebuild sources accompany
the corresponding-source archive. Runtime binary downloads are content-checked;
if upstream replaces a continuous asset, refresh the pin and sources together.

Flatpak uses `io.github.RaresKeY.AssetPreview`, Freedesktop 25.08 and the
committed binary-payload manifest. It has X11/IPC/DRI access, broad host and temp
filesystem permissions, a writable private socket prefix and the Flatpak host
command D-Bus permission for generators. This is a trusted developer tool with
host execution; no strict sandbox claim. Its host CLI bridge is a separate small
release archive requiring host Python 3. The `.flatpak` bundle and bridge are
GitHub Release downloads; no public Flathub submission or remote update channel.
`docs/install-linux.md` owns install/remove/update commands and platform limits.

Release 0.0.2 is published privately from frozen tag snapshot
`4bb0516f7825ca3234848470f8a8fb11cdd85db7`, with all desktop formats, the host
Flatpak bridge, verified PKGBUILD, dependency sources and checksums in GitHub
Releases. GHCR `0.0.2` and `latest` share digest
`sha256:45968df011d338526ccf61c923bfc699b1b32659470fd8b38dfc7164bd935bee`.
`evidence/releases/0.0.2/validation.json` records remote hashes, native installs,
AppImage/Flatpak/container NVIDIA GPU previews and Flatpak host child-group
cleanup on hide, shutdown, timeout and normal completion. Source on `main` may
include later UI work beyond the frozen release. Runtime libraries and graphics
drivers remain external as described above; native packages are unsigned.
The temporary OCI transport archive is removed from Releases after private GHCR
publication; generated local candidates are removed after remote verification.
