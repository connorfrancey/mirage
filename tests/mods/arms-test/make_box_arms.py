"""Writes content/meshes/CH_TKY_Crim_Fixer_1P.SK_UpperBody.glb: "box arms" for Faith's first-person arms (step 31's
skeletal mesh replacement). A box along each upper arm, forearm and hand, each skinned to its bone, the elbow end of
each forearm box half on the upper arm (soft vertices). The joints are where Faith's are in her arms' reference pose
(CH_TKY_Crim_Fixer_1P.SK_UpperBody, in game units; only the bone names matter to the game, the places put the boxes on
her arms). glTF is Y up and in meters: the game's (x, y, z) is the file's (x, z, y) times 50 (`mirage model`'s default).

Run it again after changing it, then `uv run mirage model <the .glb> --convert`."""

import json
import math
import struct
from pathlib import Path

OUT = Path(__file__).parent / "content" / "meshes" / "CH_TKY_Crim_Fixer_1P.SK_UpperBody.glb"
SCALE = 50.0

# Faith's joints in her arms' reference pose (game units), left side; the right side mirrors X
LEFT = {
    "Arm": (16.3964, -148.453, -1.1723),
    "ForeArm": (33.5132, -130.879, -2.60999),
    "Hand": (46.9679, -117.066, 14.5383),
    "HandMiddle1": (51.5307, -113.023, 21.2424),
}


def file_space(p):
    return (p[0] / SCALE, p[2] / SCALE, p[1] / SCALE)


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    n = math.sqrt(sum(x * x for x in a))
    return tuple(x / n for x in a)


positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []


def box(start, end, half_width, half_height, bone, start_bone=None):
    """A box from start to end (file space), its faces outward and counter-clockwise (glTF's front). Vertices at the
    start end are half on start_bone when given."""
    d = sub(end, start)
    length = math.sqrt(sum(x * x for x in d))
    x = norm(d)
    y = norm(cross((0.0, 1.0, 0.0) if abs(x[1]) < 0.9 else (1.0, 0.0, 0.0), x))
    z = cross(x, y)
    centre = tuple((s + e) / 2 for s, e in zip(start, end))
    half = (length / 2, half_width, half_height)
    axes = (x, y, z)
    # (normal axis, sign, the two axes across the face, their cross product along the normal)
    for n_axis, sign in ((0, 1), (0, -1), (1, 1), (1, -1), (2, 1), (2, -1)):
        a_axis, b_axis = (n_axis + 1) % 3, (n_axis + 2) % 3
        if sign < 0:
            a_axis, b_axis = b_axis, a_axis
        base = len(positions)
        normal = tuple(sign * c for c in axes[n_axis])
        for ca, cb in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            local = [0.0, 0.0, 0.0]
            local[n_axis] = sign * half[n_axis]
            local[a_axis] = ca * half[a_axis]
            local[b_axis] = cb * half[b_axis]
            p = tuple(centre[k] + sum(local[i] * axes[i][k] for i in range(3)) for k in range(3))
            positions.append(p)
            normals.append(normal)
            uvs.append(((ca + 1) / 2, (cb + 1) / 2))
            at_start = local[0] < 0
            if start_bone is not None and at_start:
                joints.append((bone, start_bone, 0, 0))
                weights.append((0.5, 0.5, 0.0, 0.0))
            else:
                joints.append((bone, 0, 0, 0))
                weights.append((1.0, 0.0, 0.0, 0.0))
        indices.extend((base, base + 1, base + 2, base, base + 2, base + 3))


names = []
for side, mirror in (("Left", 1.0), ("Right", -1.0)):
    pts = {k: file_space((mirror * v[0], v[1], v[2])) for k, v in LEFT.items()}
    first = len(names)
    names += [side + "Arm", side + "ForeArm", side + "Hand"]
    hand_end = tuple(h + 2 * (m - h) for h, m in zip(pts["Hand"], pts["HandMiddle1"]))
    box(pts["Arm"], pts["ForeArm"], 3.5 / SCALE, 3.5 / SCALE, first)
    box(pts["ForeArm"], pts["Hand"], 3.0 / SCALE, 2.5 / SCALE, first + 1, start_bone=first)
    box(pts["Hand"], hand_end, 4.0 / SCALE, 1.5 / SCALE, first + 2)
joint_places = []
for side, mirror in (("Left", 1.0), ("Right", -1.0)):
    for k in ("Arm", "ForeArm", "Hand"):
        v = LEFT[k]
        joint_places.append(file_space((mirror * v[0], v[1], v[2])))

# The binary buffer: each array 4-byte aligned
blob = bytearray()
views, accessors = [], []


def add(data, target=None):
    while len(blob) % 4:
        blob.append(0)
    view = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
    if target:
        view["target"] = target
    blob.extend(data)
    views.append(view)
    return len(views) - 1


def accessor(view, ctype, count, atype, **extra):
    accessors.append({"bufferView": view, "componentType": ctype, "count": count, "type": atype, **extra})
    return len(accessors) - 1


FLOAT, UBYTE, USHORT = 5126, 5121, 5123
pos = accessor(add(b"".join(struct.pack("<3f", *p) for p in positions), 34962), FLOAT, len(positions), "VEC3",
               min=[min(p[k] for p in positions) for k in range(3)], max=[max(p[k] for p in positions) for k in range(3)])
nrm = accessor(add(b"".join(struct.pack("<3f", *n) for n in normals), 34962), FLOAT, len(normals), "VEC3")
tex = accessor(add(b"".join(struct.pack("<2f", *t) for t in uvs), 34962), FLOAT, len(uvs), "VEC2")
jnt = accessor(add(b"".join(struct.pack("<4B", *j) for j in joints), 34962), UBYTE, len(joints), "VEC4")
wgt = accessor(add(b"".join(struct.pack("<4f", *w) for w in weights), 34962), FLOAT, len(weights), "VEC4")
idx = accessor(add(b"".join(struct.pack("<H", i) for i in indices), 34963), USHORT, len(indices), "SCALAR")
# Inverse bind matrices: each joint is a translation to its place, column-major
ibm = accessor(add(b"".join(struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -p[0], -p[1], -p[2], 1)
                            for p in joint_places)), FLOAT, len(joint_places), "MAT4")

gltf = {
    "asset": {"version": "2.0", "generator": "tests/mods/arms-test/make_box_arms.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, *range(1, len(names) + 1)]}],
    "nodes": [{"name": "BoxArms", "mesh": 0, "skin": 0}]
    + [{"name": n, "translation": list(p)} for n, p in zip(names, joint_places)],
    "skins": [{"joints": list(range(1, len(names) + 1)), "inverseBindMatrices": ibm}],
    "meshes": [{"name": "BoxArms", "primitives": [{
        "attributes": {"POSITION": pos, "NORMAL": nrm, "TEXCOORD_0": tex, "JOINTS_0": jnt, "WEIGHTS_0": wgt},
        "indices": idx}]}],
    "buffers": [{"byteLength": len(blob)}],
    "bufferViews": views,
    "accessors": accessors,
}
text = json.dumps(gltf, separators=(",", ":")).encode()
text += b" " * (-len(text) % 4)
while len(blob) % 4:
    blob.append(0)
OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_bytes(struct.pack("<3I", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob))
                + struct.pack("<2I", len(text), 0x4E4F534A) + text + struct.pack("<2I", len(blob), 0x004E4942) + bytes(blob))
print(f"{OUT}: {len(positions)} vertices, {len(indices) // 3} triangles, {len(names)} bones")
