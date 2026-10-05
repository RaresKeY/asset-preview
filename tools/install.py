#!/usr/bin/env python3
"""Install owned desktop/fish entrypoints without changing global shortcuts or PATH."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARKER = "X-AssetPreview-Managed=true"

def desktop_quote(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$').replace('%', '%%') + '"'

def main() -> None:
    config = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    data = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    desktop = data / "applications/asset-preview.desktop"
    links = [(ROOT / f"fish/{kind}/asset-preview.fish", config / f"fish/{kind}/asset-preview.fish") for kind in ("functions", "completions")]
    # Preflight every destination before changing any of them.
    for source, target in links:
        if (target.exists() or target.is_symlink()) and not (target.is_symlink() and target.resolve() == source):
            raise SystemExit(f"Refusing to overwrite existing integration: {target}")
    if desktop.is_symlink() or (desktop.exists() and MARKER not in desktop.read_text()):
        raise SystemExit(f"Refusing to overwrite an unrelated desktop entry: {desktop}")
    text = f'''[Desktop Entry]
Type=Application
Name=Asset Preview
GenericName=Live Asset Viewer
Comment=Watch images, 3D assets and baked materials update as they are saved
Exec={desktop_quote(str(ROOT / "bin/asset-preview"))} gui
Icon={ROOT / "icon.svg"}
Terminal=false
StartupNotify=false
Categories=Graphics;3DGraphics;
Keywords=Asset;Preview;Live;3D;Model;Material;PNG;GLB;
{MARKER}
'''
    desktop.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", dir=desktop.parent, suffix=".desktop", delete=False) as file:
        temporary = Path(file.name); file.write(text)
    try:
        if shutil.which("desktop-file-validate"):
            subprocess.run(["desktop-file-validate", str(temporary)], check=True)
        temporary.chmod(0o644); temporary.replace(desktop)
    finally:
        temporary.unlink(missing_ok=True)
    for source, target in links:
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.is_symlink(): target.symlink_to(source)
        print(f"Installed {target}")
    print(f"Installed {desktop}")
    if shutil.which("kbuildsycoca6"):
        subprocess.run(["kbuildsycoca6", "--noincremental"], check=True)

if __name__ == "__main__": main()
