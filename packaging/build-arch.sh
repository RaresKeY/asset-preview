#!/bin/bash
set -euo pipefail
preview_release_dir=${1:?release directory required}
preview_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
preview_version=$(cat "$preview_root/VERSION")
preview_archive="asset-preview-${preview_version}-linux-x86_64.tar.gz"
preview_arch_dir=$(mktemp -d /tmp/asset-preview-arch.XXXXXX)
trap 'rm -rf -- "$preview_arch_dir"' EXIT
useradd --create-home --uid 1000 assetpreviewbuilder
cp "$preview_release_dir/$preview_archive" "$preview_arch_dir/"
preview_sha=$(sha256sum "$preview_release_dir/$preview_archive" | cut -d ' ' -f 1)
sed "s/@ARCHIVE_SHA256@/$preview_sha/" "$preview_root/packaging/native/PKGBUILD" > "$preview_arch_dir/PKGBUILD"

chown -R assetpreviewbuilder:assetpreviewbuilder "$preview_arch_dir"
runuser -u assetpreviewbuilder -- env PACKAGER=RaresKeY bash -c 'cd "$1"; makepkg --nodeps --noconfirm' asset-preview-build "$preview_arch_dir"
cp "$preview_arch_dir/"*.pkg.tar.zst "$preview_release_dir/"
cp "$preview_arch_dir/PKGBUILD" "$preview_release_dir/PKGBUILD"
