# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Maxdemyd0 and MAGE contributors
"""Writes assets/models/sample/sample.glb, a model that exercises every imported material feature:
crate = base colour + normal + metallic-roughness maps, panel = alpha cut-out (double sided), top = emissive map."""
import json, math, pathlib, struct, zlib

N = 128


def png(pixel):
    raw = b"".join(b"\0" + bytes(c for x in range(N) for c in pixel(x, y)) for y in range(N))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def rim(x, y):
    return min(x, y, N - 1 - x, N - 1 - y) < 12


def base(x, y):
    if rim(x, y):
        return (190, 190, 195, 255)
    if x < 40 and y < 40:
        return (200, 30, 25, 255)  # red marker: top left of the texture
    g = 150 + int(18 * math.sin(x * 0.35 + 3 * math.sin(y * 0.05)))
    return (g, int(g * 0.66), int(g * 0.4), 255)


def metal_rough(x, y):
    return (0, 60, 255, 255) if rim(x, y) else (0, 190, 0, 255)  # G roughness, B metallic


def normal(x, y):
    if rim(x, y):
        return (128, 128, 255, 255)
    nx = 0.6 * math.cos((x - 12) * 2 * math.pi / 26)
    nz = math.sqrt(1 - nx * nx)
    return (int(128 + 127 * nx), 128, int(128 + 127 * nz), 255)


def cutout(x, y):
    inside = (x % 32 - 16) ** 2 + (y % 32 - 16) ** 2 < 110
    return (40, 120, 60, 0 if inside else 255)


def emissive(x, y):
    on = abs((x + y) % 64 - 32) < 10
    return (255, 120, 20, 255) if on else (0, 0, 0, 255)


blob, views, accessors, meshes = bytearray(), [], [], []


def view(data):
    while len(blob) % 4:
        blob.append(0)
    views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
    blob.extend(data)
    return len(views) - 1


def mesh(quads, material):
    """quads: (origin, u axis, v axis) with the surface facing u x v."""
    pos, nrm, uv, idx = [], [], [], []
    for o, u, v in quads:
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        l = math.sqrt(sum(c * c for c in n))
        b = len(pos)
        for s, t in ((0, 0), (1, 0), (1, 1), (0, 1)):
            pos.append([o[i] + u[i] * s + v[i] * t for i in range(3)])
            nrm.append([c / l for c in n])
            uv.append([s, 1 - t])
        idx += [b, b + 1, b + 2, b, b + 2, b + 3]
    pack = lambda rows: b"".join(struct.pack("<%df" % len(r), *r) for r in rows)
    ids = []
    for rows, kind in ((pos, "VEC3"), (nrm, "VEC3"), (uv, "VEC2")):
        a = {"bufferView": view(pack(rows)), "componentType": 5126, "count": len(rows), "type": kind}
        if rows is pos:
            a["min"] = [min(p[i] for p in pos) for i in range(3)]
            a["max"] = [max(p[i] for p in pos) for i in range(3)]
        ids.append(len(accessors))
        accessors.append(a)
    accessors.append({"bufferView": view(struct.pack("<%dH" % len(idx), *idx)), "componentType": 5123, "count": len(idx), "type": "SCALAR"})
    meshes.append({"primitives": [{"attributes": {"POSITION": ids[0], "NORMAL": ids[1], "TEXCOORD_0": ids[2]}, "indices": len(accessors) - 1, "material": material}]})


mesh([((-.6, 0, .6), (1.2, 0, 0), (0, 1.2, 0)), ((.6, 0, .6), (0, 0, -1.2), (0, 1.2, 0)),
      ((.6, 0, -.6), (-1.2, 0, 0), (0, 1.2, 0)), ((-.6, 0, -.6), (0, 0, 1.2), (0, 1.2, 0)),
      ((-.6, 0, -.6), (1.2, 0, 0), (0, 0, 1.2))], 0)
mesh([((-.6, 1.2, .6), (1.2, 0, 0), (0, 0, -1.2))], 2)
mesh([((.9, 0, .4), (1.2, 0, 0), (0, 1.6, 0))], 1)

images = [{"bufferView": view(png(f)), "mimeType": "image/png"} for f in (base, metal_rough, normal, cutout, emissive)]
gltf = {
    "asset": {"version": "2.0", "generator": "MAGE tests/make_sample_model.py", "copyright": "2026 Maxdemyd0 and MAGE contributors, LGPL-3.0-or-later"},
    "extensionsUsed": ["KHR_materials_emissive_strength"],
    "scene": 0, "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "sample", "children": [1, 2, 3]}] + [{"mesh": i} for i in range(3)],
    "meshes": meshes, "accessors": accessors, "bufferViews": views, "images": images,
    "textures": [{"source": i} for i in range(5)],
    "materials": [
        {"name": "crate", "normalTexture": {"index": 2}, "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicRoughnessTexture": {"index": 1}}},
        {"name": "panel", "alphaMode": "MASK", "doubleSided": True, "pbrMetallicRoughness": {"baseColorTexture": {"index": 3}, "metallicFactor": 0, "roughnessFactor": 0.6}},
        {"name": "top", "emissiveFactor": [1, 1, 1], "emissiveTexture": {"index": 4}, "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4}},
         "pbrMetallicRoughness": {"baseColorFactor": [0.05, 0.05, 0.05, 1], "metallicFactor": 0}}],
    "buffers": [{"byteLength": len(blob)}],
}
while len(blob) % 4:
    blob.append(0)
gltf["buffers"][0]["byteLength"] = len(blob)
text = json.dumps(gltf, separators=(",", ":")).encode()
text += b" " * (-len(text) % 4)
out = pathlib.Path(__file__).resolve().parent.parent / "assets/models/sample/sample.glb"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(struct.pack("<III", 0x46546C67, 2, 28 + len(text) + len(blob)) + struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(blob), 0x004E4942) + blob)
print(out, out.stat().st_size)
