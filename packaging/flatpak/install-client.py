#!/usr/bin/env python3
"""Install an owned Flatpak CLI bridge symlink without replacing other tools."""
import os
from pathlib import Path
root=Path(__file__).resolve().parents[1]
target=Path.home()/'.local/bin/asset-preview-flatpak'
source=root/'bin/asset-preview-flatpak'
if (target.exists() or target.is_symlink()) and not (target.is_symlink() and target.resolve()==source):
    raise SystemExit('Refusing to replace existing command: '+str(target))
target.parent.mkdir(parents=True,exist_ok=True)
if not target.is_symlink():target.symlink_to(source)
print('Installed '+str(target)+'; keep this extracted directory in place.')
