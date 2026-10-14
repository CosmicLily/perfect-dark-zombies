#!/usr/bin/env python3
"""
Black Ops II's first person viewmodel pieces for Zombies' perk drink
(zombies.inc, the viewmodel): the arms and a perk bottle, each a skinned model
from an OpenAssetTools dump (.glb), and the animations that move them (xanim,
tools/zombies/xanim.py), written for the game into the build's
zombies/viewmodel/. The art stays on the player's disk.

    python3 tools/zombies/vmconv.py model <in.glb> <images dir> <out.zvm>
    python3 tools/zombies/vmconv.py anim <in.xanim> <out.zan>

Everything is in Call of Duty's own viewmodel space: inches, x forward, y left,
z up, the camera at the origin. The .glb is that space turned y up at its root
(OpenAssetTools' rotation of -90 degrees about x on the skeleton's first bone:
tag_view on the arms, j_gun on a bottle); the turn is taken back off, so the
animations, whose tracks are in the game's space, pose it directly.

model (.zvm), little endian:
    "ZVM1", u32 bones, vertices, triangles, draws
    per bone: char name[32], s32 parent (-1 for the root), f32 rest
        translation[3], f32 rest rotation[4] (x y z w, local to the parent),
        f32 inverse bind[12] (3x4 rows)
    per vertex (52 bytes): f32 position[3], f32 normal[3], f32 uv[2] (v up
        the picture, as the game binds a PNG), u8 bone[4], f32 weight[4]
    per triangle: u16[3]
    per draw: u32 first triangle, u32 triangles, u32 flags (1: the picture's
        alpha is kept), char picture[52] (a PNG beside the .zvm)

animation (.zan):
    "ZAN1", u16 frames, u16 framerate, u16 tracks, u16 notes
    per track: char bone[32], u16 rotation keys, u16 translation keys, then
        (u16 frame, f32 x y z w) each, then (u16 frame, f32 x y z) each
    per note: u16 frame, char name[46]
"""

import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from glb2obj import read_glb, accessor, node_matrix, mul, mat4_rows  # noqa: E402
from imgio import image_read, png_write  # noqa: E402
from xanim import XAnim  # noqa: E402

# y up to z up: the turn OpenAssetTools put on the root, taken off
R = [[1, 0, 0, 0], [0, 0, -1, 0], [0, 1, 0, 0]]


def inverse(m):
    """A 3x4 affine matrix's inverse"""
    a = [row[:3] for row in m]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
           - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    inv = [[(a[(j + 1) % 3][(i + 1) % 3] * a[(j + 2) % 3][(i + 2) % 3]
             - a[(j + 1) % 3][(i + 2) % 3] * a[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)]
           for i in range(3)]
    t = [-sum(inv[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [inv[i] + [t[i]] for i in range(3)]


def to_quat(m):
    """The rotation of a 3x4 matrix (normalised columns), as x y z w"""
    c = [[m[r][k] for r in range(3)] for k in range(3)]
    s = [math.sqrt(sum(v * v for v in col)) or 1.0 for col in c]
    r = [[m[i][k] / s[k] for k in range(3)] for i in range(3)]
    tr = r[0][0] + r[1][1] + r[2][2]

    if tr > 0:
        q = math.sqrt(tr + 1.0) * 2
        return ((r[2][1] - r[1][2]) / q, (r[0][2] - r[2][0]) / q, (r[1][0] - r[0][1]) / q, 0.25 * q)
    if r[0][0] > r[1][1] and r[0][0] > r[2][2]:
        q = math.sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2
        return (0.25 * q, (r[0][1] + r[1][0]) / q, (r[0][2] + r[2][0]) / q, (r[2][1] - r[1][2]) / q)
    if r[1][1] > r[2][2]:
        q = math.sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2
        return ((r[0][1] + r[1][0]) / q, 0.25 * q, (r[1][2] + r[2][1]) / q, (r[0][2] - r[2][0]) / q)
    q = math.sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2
    return ((r[0][2] + r[2][0]) / q, (r[1][2] + r[2][1]) / q, 0.25 * q, (r[1][0] - r[0][1]) / q)


def name_field(s, n):
    b = s.encode()[:n - 1]
    return b + b"\0" * (n - len(b))


def model(src, imgdir, out):
    doc, blob = read_glb(src)
    skin = doc["skins"][0]
    joints = skin["joints"]
    jindex = {node: i for i, node in enumerate(joints)}
    parent_of = {c: i for i, n in enumerate(doc["nodes"]) for c in n.get("children", [])}
    ibms = accessor(doc, blob, skin["inverseBindMatrices"])

    # each bone's bind in the game's space, and its rest relative to its parent
    bind = [mul(R, inverse(mat4_rows(m))) for m in ibms]
    bones = []

    for i, node in enumerate(joints):
        p = parent_of.get(node)
        parent = jindex.get(p, -1)
        local = mul(inverse(bind[parent]), bind[i]) if parent >= 0 else bind[i]
        bones.append((doc["nodes"][node]["name"], parent, [local[k][3] for k in range(3)], to_quat(local),
                      inverse(bind[i])))

    verts = []
    tris = []
    draws = []
    pictures = {}
    outdir = os.path.dirname(out)
    stem = os.path.splitext(os.path.basename(out))[0]

    for n in doc["nodes"]:
        if "mesh" not in n:
            continue

        for prim in doc["meshes"][n["mesh"]]["primitives"]:
            a = prim["attributes"]
            pos = accessor(doc, blob, a["POSITION"])
            nrm = accessor(doc, blob, a["NORMAL"])
            uvs = accessor(doc, blob, a["TEXCOORD_0"])
            js = accessor(doc, blob, a["JOINTS_0"])
            ws = accessor(doc, blob, a["WEIGHTS_0"])
            idx = accessor(doc, blob, prim["indices"])
            base = len(verts)

            for p, nn, uv, j, w in zip(pos, nrm, uvs, js, ws):
                total = sum(w) or 1.0
                verts.append(([p[0], -p[2], p[1]], [nn[0], -nn[2], nn[1]], (uv[0], 1.0 - uv[1]),
                              tuple(j), tuple(v / total for v in w)))

            first = len(tris)

            for t in range(0, len(idx) - 2, 3):
                tris.append((idx[t] + base, idx[t + 1] + base, idx[t + 2] + base))

            mat = doc["materials"][prim["material"]]
            mname = mat.get("name", "")
            keep_alpha = "alpha" in mname.lower() or "glass" in mname.lower()
            pic = ""
            tex = mat.get("pbrMetallicRoughness", {}).get("baseColorTexture")

            if tex is not None:
                img = doc["images"][doc["textures"][tex["index"]]["source"]]
                istem = os.path.splitext(os.path.basename(img.get("uri", "")))[0]
                key = (istem, keep_alpha)

                if key not in pictures:
                    for ext in (".png", ".dds"):
                        p = os.path.join(imgdir, istem + ext)
                        if os.path.exists(p):
                            pic = "%s_%d.png" % (stem, len(pictures))
                            w, h, rgba = image_read(p)
                            if not keep_alpha:
                                rgba[3::4] = b"\xff" * (w * h)
                            png_write(os.path.join(outdir, pic), w, h, rgba)
                            pictures[key] = pic
                            break
                    else:
                        print("warning: %s has no picture %s" % (mname, istem))
                        pictures[key] = ""

                pic = pictures[key]

            draws.append((first, len(tris) - first, 1 if keep_alpha else 0, pic))

    if len(verts) > 65535:
        raise SystemExit("%s: %d vertices, more than a u16 names" % (src, len(verts)))

    with open(out, "wb") as f:
        f.write(b"ZVM1" + struct.pack("<4I", len(bones), len(verts), len(tris), len(draws)))

        for name, parent, t, q, ib in bones:
            f.write(name_field(name, 32) + struct.pack("<i3f4f12f", parent, *t, *q, *[v for row in ib for v in row]))

        for p, nn, uv, j, w in verts:
            f.write(struct.pack("<3f3f2f4B4f", *p, *nn, *uv, *j, *w))

        for t in tris:
            f.write(struct.pack("<3H", *t))

        for first, count, flags, pic in draws:
            f.write(struct.pack("<3I", first, count, flags) + name_field(pic, 52))

    print("%s: %d bones, %d vertices, %d triangles, %d draws, %d pictures"
          % (out, len(bones), len(verts), len(tris), len(draws), len([p for p in pictures.values() if p])))


def anim(src, out):
    a = XAnim(src)
    tracks = sorted(set(a.rot) | set(a.trans), key=a.names.index)

    with open(out, "wb") as f:
        f.write(b"ZAN1" + struct.pack("<4H", a.frames, a.framerate, len(tracks), len(a.notes)))

        for name in tracks:
            rot = a.rot.get(name, [])
            trans = a.trans.get(name, [])
            f.write(name_field(name, 32) + struct.pack("<2H", len(rot), len(trans)))

            for frame, q in rot:
                f.write(struct.pack("<H4f", frame, *q))

            for frame, t in trans:
                f.write(struct.pack("<H3f", frame, *t))

        for frame, note in a.notes:
            f.write(struct.pack("<H", frame) + name_field(note, 46))

    print("%s: %d frames at %d fps, %d tracks, %d notes" % (out, a.frames, a.framerate, len(tracks), len(a.notes)))


def main():
    if len(sys.argv) >= 5 and sys.argv[1] == "model":
        model(sys.argv[2], sys.argv[3], sys.argv[4])
    elif len(sys.argv) >= 4 and sys.argv[1] == "anim":
        anim(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
