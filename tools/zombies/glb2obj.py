#!/usr/bin/env python3
"""
A Black Ops II model as an OpenAssetTools fastfile dump writes it (a .glb, its
pictures .dds beside it in images/, or .png from Greyhound) to an OBJ, an MTL
and the colour maps as PNG in the build's zombies/models/<name>/, for Zombies'
machines (zombies.inc, zombiesMeshBuild()). The art stays on the player's
disk, out of the tree.

    python3 tools/zombies/glb2obj.py <machine dir> <outdir> [name]

<machine dir> holds one .glb and an images/ folder: a colour map named as
the model's material names it (".../images/~-gzombie_vending_jugg_col.dds"),
as .dds or as .png. A picture that is not there leaves its material
untextured, and says so.

The OBJ is written as semodel2obj.py writes Greyhound's models, so the game
takes either alike: centimetres (the .glb is in inches), z up with the front
of the machine towards -y (the .glb is y up, its front +z), and triangles
wound as Call of Duty winds them (clockwise; glTF's are anticlockwise). A
skinned model is posed as the file stands its bones: Buried's Pack-a-Punch
puts its machine, sign and fittings on its table that way (tag_part_01 to 03,
17 to 57 inches up), and taken without its bones the machine floated.
"""

import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from imgio import image_read, png_write  # noqa: E402

INCH = 2.54

COMPONENTS = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
NORMALISED = {5121: 255.0, 5123: 65535.0}


def read_glb(path):
    data = open(path, "rb").read()

    if data[:4] != b"glTF":
        raise SystemExit("%s: not a .glb" % path)

    at = 12
    doc = None
    blob = b""

    while at < len(data):
        n, kind = struct.unpack_from("<II", data, at)
        body = data[at + 8:at + 8 + n]
        at += 8 + n

        if kind == 0x4E4F534A:
            doc = json.loads(body)
        elif kind == 0x004E4942:
            blob = body

    return doc, blob


def accessor(doc, blob, index):
    acc = doc["accessors"][index]
    view = doc["bufferViews"][acc["bufferView"]]
    fmt, size = COMPONENTS[acc["componentType"]]
    width = WIDTH[acc["type"]]
    stride = view.get("byteStride", size * width)
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    out = []

    scale = NORMALISED.get(acc["componentType"]) if acc.get("normalized") else None

    for i in range(acc["count"]):
        vals = struct.unpack_from("<" + fmt * width, blob, base + i * stride)
        if scale:
            vals = tuple(v / scale for v in vals)
        out.append(vals if width > 1 else vals[0])

    return out


def mat4_rows(m):
    """A glTF (column major) 4x4 as 3x4 rows"""
    return [[m[0], m[4], m[8], m[12]], [m[1], m[5], m[9], m[13]], [m[2], m[6], m[10], m[14]]]


def quat_matrix(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def node_matrix(node):
    """A node's own transform as 3x4 rows (the last column the translation)"""
    if "matrix" in node:
        return mat4_rows(node["matrix"])

    r = quat_matrix(node.get("rotation", [0, 0, 0, 1]))
    s = node.get("scale", [1, 1, 1])
    t = node.get("translation", [0, 0, 0])
    return [[r[i][0] * s[0], r[i][1] * s[1], r[i][2] * s[2], t[i]] for i in range(3)]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) + (a[i][3] if j == 3 else 0) for j in range(4)]
            for i in range(3)]


def apply(m, p, w):
    return tuple(m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + (m[i][3] if w else 0) for i in range(3))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    src, outdir = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(os.path.normpath(outdir))
    glbs = [f for f in os.listdir(src) if f.lower().endswith(".glb")]

    if len(glbs) != 1:
        raise SystemExit("%s: one .glb is wanted, found %s" % (src, glbs))

    doc, blob = read_glb(os.path.join(src, glbs[0]))
    os.makedirs(outdir, exist_ok=True)

    # every node's place; a mesh node's is where its mesh goes, unless it is
    # skinned, when its vertices go where its bones put them (glTF ignores a
    # skinned mesh node's own transform)
    identity = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]
    world = {}
    placed = []

    def walk(index, parent):
        node = doc["nodes"][index]
        m = mul(parent, node_matrix(node))
        world[index] = m

        for c in node.get("children", []):
            walk(c, m)

    scene = doc["scenes"][doc.get("scene", 0)]

    for root in scene["nodes"]:
        walk(root, identity)

    for index, node in enumerate(doc["nodes"]):
        if "mesh" not in node or index not in world:
            continue

        if "skin" in node:
            # each joint's matrix in the pose the file stands it in: the
            # joint's place after its inverse bind matrix
            skin = doc["skins"][node["skin"]]
            ibms = accessor(doc, blob, skin["inverseBindMatrices"]) if "inverseBindMatrices" in skin \
                else [(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)] * len(skin["joints"])
            joints = [mul(world[j], mat4_rows(ibm)) for j, ibm in zip(skin["joints"], ibms)]
            placed.append((node["mesh"], identity, joints))
        else:
            placed.append((node["mesh"], world[index], None))

    # each material's colour map, converted once
    mats = []

    for i, mat in enumerate(doc.get("materials", [])):
        mname = mat.get("name", "mat%d" % i).replace("/", "_")
        pic = None
        tex = mat.get("pbrMetallicRoughness", {}).get("baseColorTexture")

        if tex is not None:
            img = doc["images"][doc["textures"][tex["index"]]["source"]]
            stem = os.path.splitext(os.path.basename(img.get("uri", img.get("name", ""))))[0]

            for ext in (".png", ".dds"):
                p = os.path.join(src, "images", stem + ext)

                if os.path.exists(p):
                    pic = "%s_%d.png" % (name, i)
                    w, h, rgba = image_read(p)

                    # Opaque but for glass. A "~~" picture is a colour map
                    # with another in its alpha (Black Ops II packs a gloss
                    # map there): Buried Pack-a-Punch's table came out 99%
                    # clear, and the machine stood on nothing
                    if "glass" not in mname.lower():
                        rgba[3::4] = b"\xff" * (w * h)

                    png_write(os.path.join(outdir, pic), w, h, rgba)
                    break
            else:
                print("warning: %s has no picture (%s)" % (mname, stem))

        mats.append((mname, pic))

    with open(os.path.join(outdir, name + ".mtl"), "w") as f:
        for mname, pic in mats:
            f.write("newmtl %s\n" % mname)
            if pic:
                f.write("map_Kd %s\n" % pic)
            f.write("\n")

    out = ["# %s, from %s" % (name, glbs[0]), "mtllib %s.mtl" % name]
    base = 1
    numtris = 0
    lo = [1e30] * 3
    hi = [-1e30] * 3

    for mi, (mesh, m, joints) in enumerate(placed):
        for pi, prim in enumerate(doc["meshes"][mesh]["primitives"]):
            if prim.get("mode", 4) != 4:
                continue

            attrs = prim["attributes"]
            pos = accessor(doc, blob, attrs["POSITION"])
            nrm = accessor(doc, blob, attrs["NORMAL"]) if "NORMAL" in attrs else [(0, 1, 0)] * len(pos)
            uvs = accessor(doc, blob, attrs["TEXCOORD_0"]) if "TEXCOORD_0" in attrs else [(0, 0)] * len(pos)
            idx = accessor(doc, blob, prim["indices"]) if "indices" in prim else list(range(len(pos)))

            if joints and "JOINTS_0" in attrs and "WEIGHTS_0" in attrs:
                # each vertex's own matrix: its bones' blended by its weights
                jids = accessor(doc, blob, attrs["JOINTS_0"])
                wts = accessor(doc, blob, attrs["WEIGHTS_0"])
                vmats = []

                for js, ws in zip(jids, wts):
                    total = sum(ws) or 1.0
                    vm = [[0.0] * 4 for _ in range(3)]

                    for j, w in zip(js, ws):
                        if w > 0:
                            for r in range(3):
                                for c in range(4):
                                    vm[r][c] += joints[j][r][c] * w / total

                    vmats.append(vm)
            else:
                vmats = [m] * len(pos)

            out.append("o mesh%d_%d" % (mi, pi))

            for p, vm in zip(pos, vmats):
                x, y, z = apply(vm, p, True)
                # y up, front +z, inches -> z up, front -y, centimetres
                v = (x * INCH, -z * INCH, y * INCH)
                for k in range(3):
                    lo[k] = min(lo[k], v[k])
                    hi[k] = max(hi[k], v[k])
                out.append("v %.5f %.5f %.5f" % v)

            for u, v in uvs:
                out.append("vt %.6f %.6f" % (u, 1.0 - v))

            for n, vm in zip(nrm, vmats):
                x, y, z = apply(vm, n, False)
                ln = math.sqrt(x * x + y * y + z * z) or 1.0
                out.append("vn %.5f %.5f %.5f" % (x / ln, -z / ln, y / ln))

            if "material" in prim:
                out.append("usemtl %s" % mats[prim["material"]][0])

            for t in range(0, len(idx) - 2, 3):
                # clockwise, as Call of Duty winds them
                a, c, b = idx[t] + base, idx[t + 1] + base, idx[t + 2] + base
                out.append("f %d/%d/%d %d/%d/%d %d/%d/%d" % (a, a, a, b, b, b, c, c, c))
                numtris += 1

            base += len(pos)

    out.insert(1, "# bounds %.2f %.2f %.2f to %.2f %.2f %.2f" % tuple(lo + hi))

    with open(os.path.join(outdir, name + ".obj"), "w") as f:
        f.write("\n".join(out) + "\n")

    print("%s: %d vertices, %d triangles, %d materials; bounds %.1f %.1f %.1f to %.1f %.1f %.1f cm"
          % ((name, base - 1, numtris, len(mats)) + tuple(lo + hi)))


if __name__ == "__main__":
    main()
