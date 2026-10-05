#!/usr/bin/env python3
"""Small source-save example: edit SIZE or COLOR while its preview is visible."""
import sys
from pathlib import Path

SIZE = 1.0
COLOR = (0.18, 0.65, 0.55)

def main() -> None:
    output = Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    # Atomic publication is preferred for every generator connected to the app.
    material = output.with_suffix(".mtl")
    material.write_text("newmtl surface\nKd " + " ".join(map(str, COLOR)) + "\n")
    vertices = [(-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1),(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]
    lines = [f"mtllib {material.name}", "usemtl surface"]
    lines += ["v " + " ".join(str(v * SIZE) for v in point) for point in vertices]
    lines += ["f 4 3 2 1", "f 5 6 7 8", "f 1 2 6 5", "f 2 3 7 6", "f 3 4 8 7", "f 4 1 5 8"]
    candidate = output.with_suffix(".obj.tmp")
    candidate.write_text("\n".join(lines) + "\n"); candidate.replace(output)
    print(f"Published cube, size {SIZE}", flush=True)

if __name__ == "__main__": main()
