#!/bin/bash
set -euo pipefail
preview_release_dir=${1:?release directory required}
preview_version=$(cat /workspace/VERSION)
preview_archive="asset-preview-${preview_version}-linux-x86_64.tar.gz"
preview_arch_dir=$(mktemp -d /tmp/asset-preview-arch.XXXXXX)
trap 'rm -rf -- "$preview_arch_dir"' EXIT
useradd --create-home --uid 1000 assetpreviewbuilder
cp "$preview_release_dir/$preview_archive" "$preview_arch_dir/"
python - "$preview_release_dir/$preview_archive" "$preview_arch_dir/PKGBUILD" <<'PY'
import hashlib,sys
from pathlib import Path
with open(sys.argv[1],'rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
text=Path('/workspace/packaging/native/PKGBUILD').read_text().replace('@ARCHIVE_SHA256@',digest)
Path(sys.argv[2]).write_text(text)
PY
chown -R assetpreviewbuilder:assetpreviewbuilder "$preview_arch_dir"
runuser -u assetpreviewbuilder -- env PACKAGER=RaresKeY bash -c 'cd "$1"; makepkg --nodeps --noconfirm' asset-preview-build "$preview_arch_dir"
cp "$preview_arch_dir/"*.pkg.tar.zst "$preview_release_dir/"
cp "$preview_arch_dir/PKGBUILD" "$preview_release_dir/PKGBUILD"
