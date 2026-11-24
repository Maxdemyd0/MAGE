# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Maxdemyd0 and MAGE contributors
"""Writes assets/models/mannequin/mannequin.glb: a skinned 1.8 m figure with idle, walk, run and wave clips.
Faces +Z. The walk and run clips travel forward (root motion); use "in place" to remove that."""
import json, math, pathlib, struct

# name: (parent, world position of the joint)
BONES = {
    "Hips": (None, (0, .95, 0)), "Spine": ("Hips", (0, 1.10, 0)), "Chest": ("Spine", (0, 1.28, 0)),
    "Neck": ("Chest", (0, 1.50, 0)), "Head": ("Neck", (0, 1.58, 0)),
}
for side, x in (("Left", 1), ("Right", -1)):
    BONES[side + "Arm"] = ("Chest", (x * .19, 1.43, 0))
    BONES[side + "ForeArm"] = (side + "Arm", (x * .21, 1.16, 0))
    BONES[side + "Hand"] = (side + "ForeArm", (x * .22, .91, 0))
    BONES[side + "UpLeg"] = ("Hips", (x * .10, .92, 0))
    BONES[side + "Leg"] = (side + "UpLeg", (x * .11, .50, 0))
    BONES[side + "Foot"] = (side + "Leg", (x * .11, .09, 0))
NAMES = list(BONES)
INDEX = {n: i for i, n in enumerate(NAMES)}

pos, nrm, uv, joints, weights, tris = [], [], [], [], [], []


def smooth(t):
    t = min(max(t, 0), 1)
    return t * t * (3 - 2 * t)


def tube(rings, seg=14):
    """rings: (center, rx, rz, {bone: weight}) from bottom to top; closed with poles at both ends."""
    base = len(pos)
    first, last = rings[0], rings[-1]
    full = [(first[0], 1e-4, 1e-4, first[3])] + rings + [(last[0], 1e-4, 1e-4, last[3])]
    for c, rx, rz, w in full:
        for s in range(seg + 1):
            a = 2 * math.pi * s / seg
            pos.append([c[0] + rx * math.sin(a), c[1], c[2] + rz * math.cos(a)])
            uv.append([s / seg, 1 - c[1] / 1.8])
            items = sorted(w.items(), key=lambda kv: -kv[1])[:4]
            total = sum(v for _, v in items)
            items += [(NAMES[0], 0)] * (4 - len(items))
            joints.append([INDEX[n] for n, _ in items])
            weights.append([v / total for _, v in items])
            nrm.append([0, 0, 0])
    for r in range(len(full) - 1):
        for s in range(seg):
            a, b = base + r * (seg + 1) + s, base + r * (seg + 1) + s + 1
            c, d = a + seg + 1, b + seg + 1
            tris.extend([[a, b, c], [b, d, c]])


def limb(chain, radii, end, steps=8):
    """Vertical limb through a chain of joints with weights blending around every joint."""
    points = [BONES[b][1] for b in chain] + [end]
    rings = []
    for i in range(len(chain)):
        p, q = points[i], points[i + 1]
        for k in range(steps + (i == len(chain) - 1)):
            t = k / steps
            c = [p[j] + (q[j] - p[j]) * t for j in range(3)]
            r = radii[i] + (radii[i + 1] - radii[i]) * t
            w = {chain[i]: 1.0}
            if t < .25 and i > 0:
                w = {chain[i]: .5 + .5 * smooth(t / .25), chain[i - 1]: .5 - .5 * smooth(t / .25)}
            elif t > .75 and i + 1 < len(chain):
                w = {chain[i]: 1 - .5 * smooth((t - .75) / .25), chain[i + 1]: .5 * smooth((t - .75) / .25)}
            rings.append((c, r, r, w))
    tube(rings[::-1])


# torso
tube([((0, .86, 0), .13, .10, {"Hips": 1}), ((0, .95, 0), .16, .11, {"Hips": 1}), ((0, 1.03, 0), .145, .10, {"Hips": .5, "Spine": .5}),
      ((0, 1.12, 0), .135, .095, {"Spine": 1}), ((0, 1.20, 0), .145, .10, {"Spine": .5, "Chest": .5}), ((0, 1.30, 0), .165, .115, {"Chest": 1}),
      ((0, 1.40, 0), .175, .11, {"Chest": 1}), ((0, 1.46, 0), .12, .08, {"Chest": 1}), ((0, 1.49, 0), .06, .06, {"Chest": .5, "Neck": .5}),
      ((0, 1.56, 0), .05, .055, {"Neck": .5, "Head": .5})], 20)
# head
tube([((0, 1.55 + .25 * (.5 - .5 * math.cos(math.pi * k / 10)), .01), max(.098 * math.sin(math.pi * k / 10), .01),
       max(.115 * math.sin(math.pi * k / 10), .01), {"Head": 1}) for k in range(1, 10)], 18)
for side, x in (("Left", 1), ("Right", -1)):
    limb([side + "Arm", side + "ForeArm", side + "Hand"], [.055, .045, .035, .02], (x * .225, .76, 0))
    limb([side + "UpLeg", side + "Leg", side + "Foot"], [.085, .062, .045, .05], (x * .11, .0, 0))
    f = BONES[side + "Foot"][1]
    tube([((f[0], 0.0, .05), .045, .12, {side + "Foot": 1}), ((f[0], .03, .05), .05, .125, {side + "Foot": 1}), ((f[0], .07, .03), .04, .08, {side + "Foot": 1})])
# nose, so that the facing direction is visible
tube([((0, 1.655, .125), .012, .02, {"Head": 1}), ((0, 1.69, .12), .008, .012, {"Head": 1})], 8)

for a, b, c in tris:
    u = [pos[b][i] - pos[a][i] for i in range(3)]
    v = [pos[c][i] - pos[a][i] for i in range(3)]
    n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
    for k in (a, b, c):
        for i in range(3):
            nrm[k][i] += n[i]
# share normals across the UV seam
groups = {}
for i, p in enumerate(pos):
    groups.setdefault(tuple(round(c, 5) for c in p), []).append(i)
for ids in groups.values():
    s = [sum(nrm[i][k] for i in ids) for k in range(3)]
    for i in ids:
        nrm[i] = list(s)
for n in nrm:
    l = math.sqrt(sum(c * c for c in n))
    n[:] = [c / l for c in n] if l > 1e-12 else [0, 1, 0]


def quat(x=0, y=0, z=0):
    """Euler degrees, applied X then Y then Z; returns x y z w."""
    def mul(a, b):
        ax, ay, az, aw = a
        bx, by, bz, bw = b
        return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)
    h = lambda d: (math.sin(math.radians(d) / 2), math.cos(math.radians(d) / 2))
    (sx, cx), (sy, cy), (sz, cz) = h(x), h(y), h(z)
    return mul((0, 0, sz, cz), mul((0, sy, 0, cy), (sx, 0, 0, cx)))


def gait(t, stride, knee, arm, elbow, lean, bob, speed, duration):
    p = 2 * math.pi * t
    r = {"Spine": quat(x=lean), "Chest": quat(y=6 * math.sin(p))}
    for side, o in (("Left", 0), ("Right", math.pi)):
        s = math.sin(p + o)
        r[side + "UpLeg"] = quat(x=-stride * s)
        r[side + "Leg"] = quat(x=8 + knee * max(0, math.cos(p + o)) ** 1.5)  # bends while the leg swings forward
        r[side + "Foot"] = quat(x=-6 * s)
        r[side + "Arm"] = quat(x=arm * s, z=(4 if side == "Left" else -4))
        r[side + "ForeArm"] = quat(x=-elbow)
    return r, (0, -.02 + bob * math.cos(2 * p), speed * duration * t)


def idle(t):
    s = math.sin(2 * math.pi * t)
    r = {"Spine": quat(x=1.5 * s), "Chest": quat(x=1.2 * s), "Head": quat(y=7 * s)}
    for side, x in (("Left", 1), ("Right", -1)):
        r[side + "Arm"] = quat(z=x * (5 + s))
        r[side + "ForeArm"] = quat(x=-12)
    return r, (0, .006 * s, 0)


def wave(t):
    up = smooth(t / .2) * smooth((1 - t) / .2)
    r = {"RightArm": quat(z=-150 * up - 5), "RightForeArm": quat(z=up * 25 * math.sin(2 * math.pi * 4 * t), x=-12 * (1 - up)),
         "LeftArm": quat(z=5), "LeftForeArm": quat(x=-12), "Head": quat(y=-10 * up)}
    return r, (0, 0, 0)


CLIPS = {
    "idle": (3.0, idle),
    "walk": (1.0, lambda t: gait(t, 26, 48, 24, 18, 3, .015, 1.4, 1.0)),
    "run": (0.64, lambda t: gait(t, 46, 90, 50, 85, 12, .04, 4.2, 0.64)),
    "wave": (2.2, wave),
}

blob, views, accessors = bytearray(), [], []


def accessor(rows, fmt, comp, kind, minmax=False):
    while len(blob) % 4:
        blob.append(0)
    rows = [r if isinstance(r, (list, tuple)) else [r] for r in rows]
    data = b"".join(struct.pack("<%d%s" % (len(r), fmt), *r) for r in rows)
    views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
    blob.extend(data)
    a = {"bufferView": len(views) - 1, "componentType": comp, "count": len(rows), "type": kind}
    if minmax:
        a["min"] = [min(r[i] for r in rows) for i in range(len(rows[0]))]
        a["max"] = [max(r[i] for r in rows) for i in range(len(rows[0]))]
    accessors.append(a)
    return len(accessors) - 1


attributes = {"POSITION": accessor(pos, "f", 5126, "VEC3", True), "NORMAL": accessor(nrm, "f", 5126, "VEC3"), "TEXCOORD_0": accessor(uv, "f", 5126, "VEC2"),
              "JOINTS_0": accessor(joints, "B", 5121, "VEC4"), "WEIGHTS_0": accessor(weights, "f", 5126, "VEC4")}
indices = accessor([i for t in tris for i in t], "H", 5123, "SCALAR")
ibm = accessor([[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0] + [-c for c in BONES[n][1]] + [1] for n in NAMES], "f", 5126, "MAT4")

nodes = []
for n in NAMES:
    parent, p = BONES[n]
    o = BONES[parent][1] if parent else (0, 0, 0)
    nodes.append({"name": n, "translation": [p[i] - o[i] for i in range(3)], "children": [INDEX[c] for c in NAMES if BONES[c][0] == n]})
    if not nodes[-1]["children"]:
        del nodes[-1]["children"]
nodes.append({"name": "Mannequin", "mesh": 0, "skin": 0})
nodes.append({"name": "Root", "children": [0, len(NAMES)]})

animations = []
for name, (duration, f) in CLIPS.items():
    count = int(duration * 30) + 1
    frames = [f(i / (count - 1)) for i in range(count)]
    times = accessor([[i / (count - 1) * duration] for i in range(count)], "f", 5126, "SCALAR", True)
    samplers, channels = [], []
    rest = nodes[0]["translation"]
    samplers.append({"input": times, "output": accessor([[rest[i] + fr[1][i] for i in range(3)] for fr in frames], "f", 5126, "VEC3")})
    channels.append({"sampler": 0, "target": {"node": 0, "path": "translation"}})
    for bone in sorted({b for fr in frames for b in fr[0]}):
        samplers.append({"input": times, "output": accessor([fr[0][bone] for fr in frames], "f", 5126, "VEC4")})
        channels.append({"sampler": len(samplers) - 1, "target": {"node": INDEX[bone], "path": "rotation"}})
    animations.append({"name": name, "samplers": samplers, "channels": channels})

while len(blob) % 4:
    blob.append(0)
gltf = {
    "asset": {"version": "2.0", "generator": "MAGE tests/make_sample_character.py", "copyright": "2026 Maxdemyd0 and MAGE contributors, LGPL-3.0-or-later"},
    "scene": 0, "scenes": [{"nodes": [len(nodes) - 1]}], "nodes": nodes,
    "meshes": [{"primitives": [{"attributes": attributes, "indices": indices, "material": 0}]}],
    "materials": [{"name": "body", "pbrMetallicRoughness": {"baseColorFactor": [.75, .33, .12, 1], "metallicFactor": 0, "roughnessFactor": .55}}],
    "skins": [{"joints": list(range(len(NAMES))), "inverseBindMatrices": ibm, "skeleton": 0}],
    "animations": animations, "accessors": accessors, "bufferViews": views, "buffers": [{"byteLength": len(blob)}],
}
text = json.dumps(gltf, separators=(",", ":")).encode()
text += b" " * (-len(text) % 4)
out = pathlib.Path(__file__).resolve().parent.parent / "assets/models/mannequin/mannequin.glb"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(struct.pack("<III", 0x46546C67, 2, 28 + len(text) + len(blob)) + struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(blob), 0x004E4942) + blob)
print(out, out.stat().st_size, "bytes,", len(pos), "vertices,", len(tris), "triangles")
