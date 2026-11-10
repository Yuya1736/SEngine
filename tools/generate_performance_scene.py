"""Generate the deterministic 500-entity rendering acceptance scene."""

import json
from pathlib import Path


def transform(x: float, y: float, z: float, sx: float = 1.0, sy: float = 1.0,
              sz: float = 1.0, rotation=None) -> dict:
    return {
        "position": [x, y, z],
        "rotationEuler": rotation or [0.0, 0.0, 0.0],
        "scale": [sx, sy, sz],
    }


entities = [
    {
        "id": 0,
        "name": "Performance Camera",
        "components": {
            "transform": transform(0.0, 15.0, 27.0, rotation=[-29.0, 0.0, 0.0]),
            "camera": {"fov": 60.0, "near": 0.1, "far": 300.0, "primary": True},
            "tag": "MainCamera",
        },
    },
    {
        "id": 1,
        "name": "Sun",
        "components": {
            "transform": transform(0.0, 20.0, 0.0),
            "directionalLight": {
                "direction": [-0.35, -1.0, -0.25],
                "color": [1.0, 0.96, 0.9],
                "intensity": 1.1,
            },
        },
    },
    {
        "id": 2,
        "name": "Ground",
        "components": {
            "transform": transform(0.0, -0.05, 0.0, 60.0, 1.0, 60.0),
            "meshRenderer": {
                "mesh": "builtin:plane",
                "material": "0b6c8a22-61f4-4f4e-9e5b-100000000010",
                "visible": True,
            },
        },
    },
]

materials = [
    "0b6c8a22-61f4-4f4e-9e5b-100000000011",
    "0b6c8a22-61f4-4f4e-9e5b-100000000012",
    "0b6c8a22-61f4-4f4e-9e5b-100000000015",
    "0b6c8a22-61f4-4f4e-9e5b-100000000016",
]

next_id = 3
for row in range(10):
    for column in range(10):
        x = (column - 4.5) * 2.2
        z = (row - 4.5) * 2.2
        entities.append(
            {
                "id": next_id,
                "name": f"Render Cube {row:02d}-{column:02d}",
                "components": {
                    "transform": transform(x, 0.55, z, 0.8, 0.8, 0.8,
                                           rotation=[0.0, (row * 17 + column * 11) % 360, 0.0]),
                    "meshRenderer": {
                        "mesh": "builtin:cube",
                        "material": materials[(row + column) % len(materials)],
                        "visible": True,
                    },
                    "tag": "PerformanceCube",
                },
            }
        )
        next_id += 1

while next_id < 500:
    entities.append(
        {
            "id": next_id,
            "name": f"Ambient Entity {next_id:03d}",
            "components": {"transform": transform(0.0, 0.0, 0.0)},
        }
    )
    next_id += 1

root = Path(__file__).resolve().parents[1]
output = root / "assets" / "scenes" / "performance.scene.json"
output.write_text(json.dumps({"version": 1, "entities": entities}, indent=2) + "\n",
                  encoding="utf-8")
print(f"Wrote {len(entities)} entities to {output}")
