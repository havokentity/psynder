#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inspect Psynder cooked .psyscene files without booting the engine."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path
from typing import Any


MAGIC = 0x4E435350  # PSCN
VERSION = 1
HEADER = struct.Struct("<IHHIIIIII8I")
CHUNK = struct.Struct("<IIII")
VEC3 = struct.Struct("<fff")
QUAT = struct.Struct("<ffff")

CHUNK_NAMES = {
    0x53525453: "STRS",
    0x564E4553: "SENV",
    0x54584654: "TXFT",
    0x52584654: "TXFR",
    0x53584654: "TXFS",
    0x4D414353: "SCAM",
    0x53454D53: "SMES",
    0x54494C53: "SLIT",
    0x4D414E53: "SNAM",
    0x54414D53: "SMAT",
    0x50534253: "SBSP",
    0x4C544253: "SBTL",
}

LIGHT_KINDS = {0: "point", 1: "directional", 2: "spot"}
OBJECT_KINDS = {1: "camera", 2: "mesh", 3: "light"}
GEOMETRY_FLAGS = {
    1 << 0: "visible",
    1 << 1: "casts_shadow_override",
    1 << 2: "receives_shadow_override",
}


def rgba8(value: int) -> str:
    return f"#{value & 0xff:02x}{(value >> 8) & 0xff:02x}{(value >> 16) & 0xff:02x}{(value >> 24) & 0xff:02x}"


def flag_names(value: int, names: dict[int, str]) -> list[str]:
    result = [name for bit, name in names.items() if value & bit]
    unknown = value & ~sum(names.keys())
    if unknown:
        result.append(f"0x{unknown:x}")
    return result


def get_string(strings: bytes, offset: int) -> str:
    if offset == 0:
        return ""
    if offset < 0 or offset >= len(strings):
        return f"<bad-offset:{offset}>"
    end = strings.find(b"\0", offset)
    if end < 0:
        end = len(strings)
    return strings[offset:end].decode("utf-8", errors="replace")


def read_vec3(blob: bytes, offset: int) -> tuple[float, float, float]:
    return tuple(round(v, 6) for v in VEC3.unpack_from(blob, offset))


def read_quat(blob: bytes, offset: int) -> tuple[float, float, float, float]:
    return tuple(round(v, 6) for v in QUAT.unpack_from(blob, offset))


def chunk_records(blob: bytes, header_bytes: int, count: int) -> list[dict[str, int]]:
    chunks = []
    for i in range(count):
        offset = header_bytes + i * CHUNK.size
        if offset + CHUNK.size > len(blob):
            raise ValueError("chunk table extends beyond file")
        chunk_type, data_offset, byte_count, stride = CHUNK.unpack_from(blob, offset)
        chunks.append(
            {
                "type": chunk_type,
                "name": CHUNK_NAMES.get(chunk_type, f"0x{chunk_type:08x}"),
                "offset": data_offset,
                "bytes": byte_count,
                "stride": stride,
            }
        )
    return chunks


def require_chunk_slice(blob: bytes, chunk: dict[str, int]) -> bytes:
    start = chunk["offset"]
    end = start + chunk["bytes"]
    if start < 0 or end > len(blob) or end < start:
        raise ValueError(f"{chunk['name']} chunk points outside file")
    return blob[start:end]


def array_offsets(chunk: dict[str, int]) -> range:
    stride = chunk["stride"]
    if stride <= 0:
        return range(0)
    return range(chunk["offset"], chunk["offset"] + chunk["bytes"], stride)


def inspect_scene(path: Path) -> dict[str, Any]:
    blob = path.read_bytes()
    if len(blob) < HEADER.size:
        raise ValueError("file is too small for a psyscene header")

    fields = HEADER.unpack_from(blob, 0)
    magic, version, header_bytes = fields[:3]
    file_bytes, chunk_count, transform_count, camera_count, mesh_count, flags = fields[3:9]
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x}, expected PSCN")
    if version != VERSION:
        raise ValueError(f"unsupported psyscene version {version}")
    if file_bytes and file_bytes != len(blob):
        raise ValueError(f"header file_bytes={file_bytes}, actual={len(blob)}")

    chunks = chunk_records(blob, header_bytes, chunk_count)
    by_name = {chunk["name"]: chunk for chunk in chunks}
    strings = require_chunk_slice(blob, by_name["STRS"]) if "STRS" in by_name else b"\0"

    transforms = []
    translations = by_name.get("TXFT")
    rotations = by_name.get("TXFR")
    scales = by_name.get("TXFS")
    for index in range(transform_count):
        item: dict[str, Any] = {"index": index}
        if translations and index * translations["stride"] + 12 <= translations["bytes"]:
            item["translation"] = read_vec3(blob, translations["offset"] + index * translations["stride"])
        if rotations and index * rotations["stride"] + 16 <= rotations["bytes"]:
            item["rotation_xyzw"] = read_quat(blob, rotations["offset"] + index * rotations["stride"])
        if scales and index * scales["stride"] + 12 <= scales["bytes"]:
            item["scale"] = read_vec3(blob, scales["offset"] + index * scales["stride"])
        transforms.append(item)

    environments = []
    if "SENV" in by_name:
        for offset in array_offsets(by_name["SENV"]):
            color, clear_color, clear_depth = struct.unpack_from("<IBB", blob, offset)
            environments.append(
                {
                    "clear_color_rgba8": f"0x{color:08x}",
                    "clear_color": bool(clear_color),
                    "clear_depth": bool(clear_depth),
                    "preview": rgba8(color),
                }
            )

    cameras = []
    if "SCAM" in by_name:
        for offset in array_offsets(by_name["SCAM"]):
            transform_index = struct.unpack_from("<I", blob, offset)[0]
            fov_y_rad, near_z, far_z = struct.unpack_from("<fff", blob, offset + 28)
            cameras.append(
                {
                    "transform": transform_index,
                    "look_at": read_vec3(blob, offset + 4),
                    "up": read_vec3(blob, offset + 16),
                    "fov_y_deg": round(fov_y_rad * 57.29577951308232, 4),
                    "near_z": near_z,
                    "far_z": far_z,
                    "active": bool(struct.unpack_from("<B", blob, offset + 48)[0]),
                }
            )

    object_names: dict[tuple[int, int], str] = {}
    if "SNAM" in by_name:
        for offset in array_offsets(by_name["SNAM"]):
            kind, object_index, name_offset, _reserved = struct.unpack_from("<IIII", blob, offset)
            name = get_string(strings, name_offset)
            if name:
                object_names[(kind, object_index)] = name

    meshes = []
    if "SMES" in by_name:
        for index, offset in enumerate(array_offsets(by_name["SMES"])):
            transform_index, mesh_name, material_name, group_name, flags_raw, mobility = struct.unpack_from(
                "<IIIIIB", blob, offset
            )
            meshes.append(
                {
                    "index": index,
                    "name": object_names.get((2, index), get_string(strings, group_name) or f"mesh#{index}"),
                    "group": get_string(strings, group_name),
                    "mesh": get_string(strings, mesh_name),
                    "material": get_string(strings, material_name),
                    "transform": transform_index,
                    "flags": flag_names(flags_raw, GEOMETRY_FLAGS),
                    "mobility": "dynamic" if mobility else "static",
                }
            )

    lights = []
    if "SLIT" in by_name:
        for index, offset in enumerate(array_offsets(by_name["SLIT"])):
            transform_index, kind, casts_shadow = struct.unpack_from("<IBB", blob, offset)
            color = struct.unpack_from("<I", blob, offset + 8)[0]
            intensity, light_range, inner_cone, outer_cone = struct.unpack_from("<ffff", blob, offset + 12)
            lights.append(
                {
                    "index": index,
                    "name": object_names.get((3, index), f"light#{index}"),
                    "kind": LIGHT_KINDS.get(kind, f"unknown:{kind}"),
                    "transform": transform_index,
                    "color_rgba8": f"0x{color:08x}",
                    "preview": rgba8(color),
                    "intensity": intensity,
                    "range": light_range,
                    "inner_cone_deg": inner_cone,
                    "outer_cone_deg": outer_cone,
                    "casts_shadow": bool(casts_shadow),
                }
            )

    materials = []
    if "SMAT" in by_name:
        for index, offset in enumerate(array_offsets(by_name["SMAT"])):
            name, base_texture, albedo, flags_raw = struct.unpack_from("<IIII", blob, offset)
            alpha_cutoff, reflectivity, roughness, emissive = struct.unpack_from("<ffff", blob, offset + 16)
            materials.append(
                {
                    "index": index,
                    "name": get_string(strings, name) or f"material#{index}",
                    "base_color_texture": get_string(strings, base_texture),
                    "albedo_rgba8": f"0x{albedo:08x}",
                    "preview": rgba8(albedo),
                    "flags": f"0x{flags_raw:x}",
                    "alpha_cutoff": alpha_cutoff,
                    "reflectivity": reflectivity,
                    "roughness": roughness,
                    "emissive": emissive,
                }
            )

    return {
        "path": str(path),
        "header": {
            "file_bytes": file_bytes,
            "chunk_count": chunk_count,
            "transform_count": transform_count,
            "camera_count": camera_count,
            "mesh_instance_count": mesh_count,
            "flags": f"0x{flags:x}",
        },
        "chunks": chunks,
        "environment": environments,
        "transforms": transforms,
        "cameras": cameras,
        "meshes": meshes,
        "lights": lights,
        "object_names": [
            {
                "kind": OBJECT_KINDS.get(kind, f"unknown:{kind}"),
                "index": index,
                "name": name,
            }
            for (kind, index), name in sorted(object_names.items())
        ],
        "materials": materials,
    }


def print_summary(scene: dict[str, Any]) -> None:
    header = scene["header"]
    print(f"{scene['path']}")
    print(
        "  "
        f"{header['transform_count']} transforms, "
        f"{len(scene['meshes'])} meshes, "
        f"{len(scene['lights'])} lights, "
        f"{len(scene['materials'])} materials, "
        f"{len(scene['cameras'])} cameras"
    )
    if scene["environment"]:
        env = scene["environment"][0]
        print(f"  environment clear={env['preview']} depth={env['clear_depth']}")
    for mesh in scene["meshes"]:
        print(
            f"  mesh[{mesh['index']}] {mesh['name']}: "
            f"mesh={mesh['mesh'] or '<none>'} material={mesh['material'] or '<none>'} "
            f"transform={mesh['transform']}"
        )
    for light in scene["lights"]:
        print(
            f"  light[{light['index']}] {light['name']} ({light['kind']}): "
            f"color={light['preview']} intensity={light['intensity']:.3g} "
            f"range={light['range']:.3g} transform={light['transform']}"
        )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", type=Path, help="Path to a .psyscene file")
    parser.add_argument("--json", action="store_true", help="Print full JSON instead of a compact summary")
    args = parser.parse_args(argv)

    try:
        scene = inspect_scene(args.scene)
    except (OSError, ValueError, struct.error) as exc:
        print(f"scene_inspect: {exc}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(scene, indent=2))
    else:
        print_summary(scene)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
