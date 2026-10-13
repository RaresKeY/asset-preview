#!/usr/bin/env python3
"""Connect Asset Workshop's finite surface fixture without changing the viewer."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


VIEWS = (
    ("model", "surface-fixture.glb", "Surface fixture · saved host GLB"),
    ("open-model", "surface-fixture-open.glb", "Surface fixture · sampled open GLB"),
    ("host-material", "host-material.png", "Surface fixture · native material diagnostic"),
    ("cut-edges", "cut-edges.png", "Surface fixture · derived native edges"),
    ("closed-leaf", "closed-leaf.png", "Surface fixture · native closed leaf"),
    ("open-leaf", "open-leaf.png", "Surface fixture · native open leaf"),
    ("seat-contact", "seat-contact.png", "Surface fixture · native seat contact"),
)


def absolute(value: str | Path) -> Path:
    # Preserve published filenames, including atomic replacement of symlinks.
    return Path(os.path.abspath(os.path.expanduser(str(value))))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_json(path: Path) -> dict:
    if path.stat().st_size > 1024 * 1024:
        raise ValueError(f"Manifest/receipt exceeds 1 MiB: {path}")
    result = json.loads(path.read_text())
    if not isinstance(result, dict):
        raise ValueError(f"Expected a JSON object: {path}")
    return result


def plan(workshop: Path, preview: Path, tools: Path, runner: Path,
         prefix: str, generate: bool, timeout: int) -> dict:
    construct = workshop / "constructs/surface-ownership"
    output = construct / "examples/exports"
    inputs = [construct / name for name in (
        "examples/fixture.json", "scripts/build.sh", "src/build_fixture.py",
        "src/canonical.py", "src/publish.py", "src/ownership.py", "canonical-pin.json",
    )]
    inputs += [tools / "tools/geometry-authoring/src" / name for name in (
        "surface_ownership.py", "surface_topology_blender.py",
    )]
    commands = []
    for suffix, filename, label in VIEWS:
        entry_id = f"{prefix}-{suffix}"
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", entry_id):
            raise ValueError("Prefix produces an invalid preview ID (maximum 64 characters)")
        command = [str(preview / "bin/asset-preview"), "add", str(output / filename),
                   "--id", entry_id, "--label", label, "--cwd", str(workshop)]
        if generate and suffix == "model":
            for path in inputs:
                command += ["--watch", str(path)]
            command += ["--timeout", str(timeout), "--exec", "/usr/bin/env",
                        f"GAME_DEV_TOOLS_ROOT={tools}", f"GODOT_PODMAN_RUNNER={runner}",
                        str(construct / "scripts/build.sh"), "--capture"]
        commands.append(command)
    return {"output": str(output), "source": str(inputs[0]),
            "pin": str(construct / "canonical-pin.json"),
            "inputs": [str(path) for path in inputs], "commands": commands}


def check_publication(connection: dict) -> dict:
    """Check byte coherence, not surface geometry, animation or appearance."""
    output = Path(connection["output"])
    manifest_path = output / "manifest.json"
    manifest = read_json(manifest_path)
    if manifest.get("schema") != "surface-fixture-artifacts/v1":
        raise ValueError("Unsupported fixture publication manifest")
    files = {}
    for item in manifest["files"]:
        name = item["path"]
        if not isinstance(name, str) or Path(name).name != name or name in {".", "..", ""}:
            raise ValueError("Manifest contains an escaping filename")
        if name in files:
            raise ValueError("Manifest contains a duplicate filename")
        path = output / name
        if not path.is_file() or path.is_symlink() or sha256(path) != item["sha256"]:
            raise ValueError(f"Missing or changed publication file: {name}")
        files[name] = item["sha256"]
    required = {filename for _, filename, _ in VIEWS} | {
        "surface-fixture.blend", "surface-fixture.surface-ownership.json",
        "surface-fixture.audit.json", "surface-fixture-open.surface-ownership.json",
        "surface-fixture-open.audit.json", "surface-fixture.native.json",
    }
    if not required <= files.keys():
        raise ValueError("Run the owner fixture with --capture to publish all proof views")
    source_hash = sha256(Path(connection["source"]))
    if manifest["source"]["sha256"] != source_hash:
        raise ValueError("Fixture declaration changed after publication")
    if manifest["canonical"] != read_json(Path(connection["pin"])):
        raise ValueError("Canonical pin changed after publication")
    receipt = read_json(output / "surface-fixture.surface-ownership.json")
    if receipt["source"]["sha256"] != source_hash or receipt["artifact"]["sha256"] != files["surface-fixture.glb"]:
        raise ValueError("Receipt is bound to different source/export bytes")
    if manifest["native_sha256"] != files["surface-fixture.blend"] or manifest["exported_sha256"] != files["surface-fixture.glb"]:
        raise ValueError("Manifest native/export identity is inconsistent")
    # A publisher can atomically replace the manifest during the checks above.
    if read_json(manifest_path) != manifest:
        raise ValueError("Publication changed while being checked; retry")
    return {"publication": "byte-coherent", "files": len(files),
            "geometry_verdict": "not checked by this connector",
            "owner_proof_scope": manifest.get("proof_scope"),
            "output": str(output)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    workspace = Path.home() / "workspace"
    parser.add_argument("--workshop", default=os.environ.get("ASSET_WORKSHOP_PROJECT", str(workspace / "asset-workshop")))
    parser.add_argument("--preview", default=os.environ.get("ASSET_PREVIEW_PROJECT", str(workspace / "asset-preview")))
    parser.add_argument("--tools", default=os.environ.get("GAME_DEV_TOOLS_ROOT", str(workspace / "game-dev-tools")))
    parser.add_argument("--runner", default=os.environ.get("GODOT_PODMAN_RUNNER", str(workspace / "godot-podman/bin/godot-podman")))
    parser.add_argument("--id-prefix", default="workshop-surface-fixture")
    parser.add_argument("--generate", action="store_true", help="Attach the finite owner build to the model view only")
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("--print-plan", action="store_true", help="Print exact argv and inputs; do not connect or run")
    parser.add_argument("--check", action="store_true", help="Check published file hashes; do not connect or run")
    args = parser.parse_args()
    if not 1 <= args.timeout <= 3600:
        parser.error("timeout must be between 1 and 3600 seconds")
    connection = plan(*(absolute(getattr(args, key)) for key in ("workshop", "preview", "tools", "runner")),
                      args.id_prefix, args.generate, args.timeout)
    if args.print_plan:
        print(json.dumps(connection, indent=2))
    elif args.check:
        print(json.dumps(check_publication(connection), indent=2))
    else:
        if args.generate:
            for path in connection["inputs"]:
                if not Path(path).is_file():
                    parser.error(f"Missing owner input: {path}")
        for command in connection["commands"]:
            subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
