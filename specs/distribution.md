# Distribution

Source ownership: `packaging/`, `.github/workflows/release.yml`, `CMakeLists.txt`,
`THIRD_PARTY_NOTICES.md`. Release version: 0.0.1; platform: Linux x86_64.

The Ubuntu 24.04 build image compiles FFmpeg 7.1.1 without GPL/nonfree components,
libmpv 0.41.0 in LGPL mode, VTK 9.4.2, F3D 3.5.0 with native/Assimp readers,
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
fonts and corresponding-source archive. It runs the CLI as its entrypoint.
Display, GPU and project mounts must be provided explicitly. The local workstation
uses the shared managed Podman runner for validation. No host GPU drivers are
copied into release artifacts. Container generators execute inside the container.

The manually dispatched GitHub workflow builds artifacts, checks service startup,
pushes the private GHCR package using its scoped GITHUB_TOKEN, and uploads a draft
release. Publish the draft only after local hardware validation and privacy review.
No application license is inferred from dependency licensing. Source packages,
notices, dependency manifest and checksums are release artifacts. Local generated
release outputs are removed after remote upload verification.
