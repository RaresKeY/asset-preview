#!/usr/bin/env python3
"""Small, self-contained glTF 2.0 fixture for exact component inspection.

Only the standard library is used. The fixture has four rendered components,
three named meshes, one repeated mesh, duplicate node names, two primitives in
one component, and a no-semantic-ID component below nonuniform transforms.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


PARTS = {
    2: {"exported_name": "Shell", "name": "Hull.Port", "semantic_id": "hull.port", "mesh": 0},
    4: {"exported_name": "Shell", "name": "Hull.Starboard", "semantic_id": "hull.starboard", "mesh": 0},
    5: {"exported_name": "Cabin", "name": "Crew.Cabin", "semantic_id": "crew.cabin", "mesh": 1},
    8: {"exported_name": "Nozzle", "name": "Engine.Exhaust", "mesh": 2},
}


def fixture_document(revision: int = 1) -> tuple[dict, bytes]:
    """Return the source document and its embedded binary buffer."""
    if revision not in (1, 2):
        raise ValueError("The fixture has exactly two intentional revisions")
    document = {
        "asset": {"version": "2.0", "generator": "Asset Preview stdlib inspection fixture"},
        "scene": 0,
        "scenes": [{"name": "Inspection fixture", "nodes": [0]}],
        "nodes": [
            {"name": "Fixture_Root", "children": [1, 3, 5, 6]},
            {"name": "Port_Frame", "translation": [-1.65, 0, 0], "children": [2]},
            {"name": "Shell", "mesh": 0, "extras": {"semantic_id": "hull.port", "source_manufactured_label": "Hull.Port"}},
            {"name": "Starboard_Frame", "translation": [1.65, 0, 0], "children": [4]},
            {"name": "Shell", "mesh": 0, "extras": {"semantic_id": "hull.starboard", "source_manufactured_label": "Hull.Starboard"}},
            {"name": "Cabin", "mesh": 1, "translation": [0, 1.1, 0], "extras": {"semantic_id": "crew.cabin", "source_manufactured_label": "Crew.Cabin"}},
            {"name": "Engine_Frame", "translation": [0, -0.8, 0.2], "scale": [1.25, 0.7, 0.8],
             "rotation": [0, math.sin(math.radians(12.5)), 0, math.cos(math.radians(12.5))], "children": [7]},
            {"name": "Nozzle_Cradle", "translation": [0, 0.2, 0], "scale": [0.8, 1.4, 0.65],
             "rotation": [0, 0, math.sin(math.radians(7.5)), math.cos(math.radians(7.5))], "children": [8]},
            {"name": "Nozzle", "mesh": 2, "translation": [0, 0, 0.8], "scale": [0.75, 0.85, 1.1],
             "extras": {"source_manufactured_label": "Engine.Exhaust", "part_role": "exhaust"}},
        ],
        "meshes": [],
        "materials": [
            {"name": "Hull coral", "pbrMetallicRoughness": {"baseColorFactor": [0.85, 0.16, 0.08, 1], "metallicFactor": 0.05, "roughnessFactor": 0.7}, "doubleSided": True},
            {"name": "Cabin jade", "pbrMetallicRoughness": {"baseColorFactor": [0.08, 0.72, 0.32, 1], "metallicFactor": 0.1, "roughnessFactor": 0.65}, "doubleSided": True},
            {"name": "Cabin blue", "pbrMetallicRoughness": {"baseColorFactor": [0.08, 0.3, 0.9, 1], "metallicFactor": 0.1, "roughnessFactor": 0.65}, "doubleSided": True},
            {"name": "Nozzle gold", "pbrMetallicRoughness": {"baseColorFactor": [0.95, 0.62, 0.06, 1], "metallicFactor": 0.1, "roughnessFactor": 0.6}, "doubleSided": True},
        ],
        "bufferViews": [],
        "accessors": [],
    }
    binary = bytearray()

    def accessor(values, components, component_type, kind, target):
        while len(binary) % 4:
            binary.append(0)
        offset = len(binary)
        flat = [component for value in values for component in value] if components > 1 else values
        format_code = "f" if component_type == 5126 else "H"
        binary.extend(struct.pack("<" + format_code * len(flat), *flat))
        view = len(document["bufferViews"])
        document["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": len(binary) - offset, "target": target})
        result = {"bufferView": view, "componentType": component_type, "count": len(values), "type": kind}
        if components > 1:
            result["min"] = [min(value[i] for value in values) for i in range(components)]
            result["max"] = [max(value[i] for value in values) for i in range(components)]
        index = len(document["accessors"])
        document["accessors"].append(result)
        return index

    def box(half, material, center=(0, 0, 0)):
        x, y, z = half
        faces = [
            ((1, 0, 0), [(x, -y, -z), (x, y, -z), (x, y, z), (x, -y, z)]),
            ((-1, 0, 0), [(-x, -y, z), (-x, y, z), (-x, y, -z), (-x, -y, -z)]),
            ((0, 1, 0), [(-x, y, -z), (-x, y, z), (x, y, z), (x, y, -z)]),
            ((0, -1, 0), [(-x, -y, z), (-x, -y, -z), (x, -y, -z), (x, -y, z)]),
            ((0, 0, 1), [(-x, -y, z), (x, -y, z), (x, y, z), (-x, y, z)]),
            ((0, 0, -1), [(x, -y, -z), (-x, -y, -z), (-x, y, -z), (x, y, -z)]),
        ]
        positions, normals, indices = [], [], []
        for normal, vertices in faces:
            base = len(positions)
            positions.extend(tuple(point[i] + center[i] for i in range(3)) for point in vertices)
            normals.extend([normal] * 4)
            indices.extend(base + index for index in (0, 1, 2, 0, 2, 3))
        return {"attributes": {"POSITION": accessor(positions, 3, 5126, "VEC3", 34962),
                               "NORMAL": accessor(normals, 3, 5126, "VEC3", 34962)},
                "indices": accessor(indices, 1, 5123, "SCALAR", 34963), "material": material, "mode": 4}

    # The second revision changes actual geometry, while preserving all source identities.
    document["meshes"] = [
        {"name": "Shared hull block", "primitives": [box((0.9 if revision == 1 else 1.05, 0.35, 0.55), 0)]},
        {"name": "Two material cabin", "primitives": [box((0.35, 0.45, 0.4), 1, (-0.35, 0, 0)), box((0.35, 0.45, 0.4), 2, (0.35, 0, 0))]},
        {"name": "Nested exhaust duct", "primitives": [box((0.4, 0.3, 0.85), 3)]},
    ]
    document["buffers"] = [{"byteLength": len(binary)}]
    return document, bytes(binary)


def fixture_bytes(revision: int = 1) -> bytes:
    document, binary = fixture_document(revision)
    encoded = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    encoded += b" " * (-len(encoded) % 4)
    binary += b"\0" * (-len(binary) % 4)
    chunks = struct.pack("<I4s", len(encoded), b"JSON") + encoded
    chunks += struct.pack("<I4s", len(binary), b"BIN\0") + binary
    return struct.pack("<4sII", b"glTF", 2, 12 + len(chunks)) + chunks


def write_fixture(path: Path, revision: int = 1) -> str:
    """Atomically publish one fixture revision and return its exact SHA-256."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    data = fixture_bytes(revision)
    candidate = path.with_name(path.name + ".candidate")
    candidate.write_bytes(data)
    candidate.replace(path)
    return hashlib.sha256(data).hexdigest()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--revision", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    print(json.dumps({"path": str(args.file.absolute()), "sha256": write_fixture(args.file, args.revision), "parts": PARTS}, indent=2))
