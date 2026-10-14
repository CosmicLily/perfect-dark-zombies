#!/usr/bin/env python3
"""
A Greyhound/SETools .semodel to an OBJ, an MTL and the colour maps beside it,
for Zombies' machines (zombies.inc, zombiesMachineMesh). The model is read
from wherever the ripping tool put it and written into the build's own
zombies/models/<name>/ - the art is the game's it came from, and stays out of
the tree.

    python3 tools/zombies/semodel2obj.py <model.semodel> <outdir> [name]

Only what a still model needs is kept: positions, the first UV set, normals
and the faces, a draw per mesh under its material, and each material's colour
map (the "_c" image Greyhound writes into _images/<material>/). The skeleton
and weights are read past and dropped.

The format (SETools' SEModel, version 1), little endian:

    "SEModel" u16 version u16 headersize
    u8 data (1 bones, 2 meshes, 4 materials)
    u8 bonedata (1 global matrix, 2 local matrix, 4 scales)
    u8 meshdata (1 uvs, 2 normals, 4 colours, 8 weights)
    u32 bones, meshes, materials; 3 reserved bytes
    bone names, nul-terminated
    per bone: u8 flags, s32 parent, [global pos 3f rot 4f], [local pos 3f rot 4f], [scale 3f]
    per mesh: u8 flags, u8 layers, u8 influences, u32 vertices, u32 faces,
              positions 3f, [uvs 2f per layer], [normals 3f], [colours 4 u8],
              [weights: influences x (bone index, f32)], faces 3 indices,
              layers x s32 material
    per material: name, u8 simple, diffuse, normal, specular (all names)

A bone index is a u8 under 256 bones, u16 under 65536, u32 past that; a face
index the same by the mesh's vertex count.
"""

import os
import shutil
import struct
import sys


class Reader:
    def __init__(self, data):
        self.data = data
        self.at = 0

    def take(self, fmt):
        size = struct.calcsize("<" + fmt)
        vals = struct.unpack_from("<" + fmt, self.data, self.at)
        self.at += size
        return vals

    def one(self, fmt):
        return self.take(fmt)[0]

    def cstr(self):
        end = self.data.index(b"\0", self.at)
        s = self.data[self.at:end].decode("utf-8", "replace")
        self.at = end + 1
        return s


def index_fmt(count):
    return "B" if count <= 0xff else "H" if count <= 0xffff else "I"


def read_semodel(path):
    r = Reader(open(path, "rb").read())

    if r.data[:7] != b"SEModel":
        raise SystemExit("%s: not an SEModel" % path)

    r.at = 7
    version, headersize = r.take("HH")
    data, bonedata, meshdata = r.take("BBB")
    numbones, nummeshes, nummats = r.take("III")
    r.at += 3

    if version != 1:
        raise SystemExit("%s: SEModel version %d, not 1" % (path, version))

    for _ in range(numbones):
        r.cstr()

    for _ in range(numbones):
        r.take("Bi")
        if bonedata & 1:
            r.take("7f")
        if bonedata & 2:
            r.take("7f")
        if bonedata & 4:
            r.take("3f")

    meshes = []

    for _ in range(nummeshes):
        flags, layers, influences, numverts, numfaces = r.take("BBBII")
        pos = [r.take("3f") for _ in range(numverts)]
        uvs = normals = None

        if meshdata & 1:
            uvs = []
            for _ in range(numverts):
                layer = [r.take("2f") for _ in range(layers)]
                uvs.append(layer[0] if layer else (0.0, 0.0))

        if meshdata & 2:
            normals = [r.take("3f") for _ in range(numverts)]

        if meshdata & 4:
            r.at += 4 * numverts

        if meshdata & 8:
            bf = index_fmt(numbones)
            for _ in range(numverts * influences):
                r.take(bf + "f")

        ff = index_fmt(numverts)
        faces = [r.take(ff * 3) for _ in range(numfaces)]
        mats = [r.one("i") for _ in range(layers)]

        meshes.append({"pos": pos, "uvs": uvs, "normals": normals, "faces": faces,
                       "material": mats[0] if mats else -1})

    materials = []

    for _ in range(nummats):
        name = r.cstr()
        simple = r.one("B")
        maps = [r.cstr(), r.cstr(), r.cstr()] if simple else ["", "", ""]
        materials.append({"name": name, "diffuse": maps[0]})

    # Greyhound can append an "SEBlend" block (blend shapes, zeros for a
    # model without any), which a still model has no use for
    if r.data[r.at:r.at + 7] == b"SEBlend":
        pass
    elif r.at != len(r.data):
        print("warning: %d bytes left over" % (len(r.data) - r.at))

    return meshes, materials


def colour_map(modeldir, matname):
    """The material's colour picture as Greyhound wrote it: _images/<mat>/*_c.*"""
    d = os.path.join(modeldir, "_images", matname)

    if not os.path.isdir(d):
        return None

    for f in sorted(os.listdir(d)):
        stem = os.path.splitext(f)[0].lower()
        if stem.endswith("_c") and f.lower().endswith((".png", ".tga")):
            return os.path.join(d, f)

    return None


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    src, outdir = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(os.path.normpath(outdir))
    meshes, materials = read_semodel(src)
    os.makedirs(outdir, exist_ok=True)

    lo = [1e30] * 3
    hi = [-1e30] * 3

    for m in meshes:
        for p in m["pos"]:
            for k in range(3):
                lo[k] = min(lo[k], p[k])
                hi[k] = max(hi[k], p[k])

    mtl = []
    modeldir = os.path.dirname(os.path.abspath(src))

    for i, mat in enumerate(materials):
        mtl.append("newmtl %s" % mat["name"])
        pic = colour_map(modeldir, mat["name"])

        if pic:
            base = "%s_%d%s" % (name, i, os.path.splitext(pic)[1].lower())
            shutil.copyfile(pic, os.path.join(outdir, base))
            mtl.append("map_Kd %s" % base)
        else:
            print("warning: no colour map for %s" % mat["name"])

        mtl.append("")

    with open(os.path.join(outdir, name + ".mtl"), "w") as f:
        f.write("\n".join(mtl))

    out = ["# %s, from %s" % (name, os.path.basename(src)),
           "# bounds %.2f %.2f %.2f to %.2f %.2f %.2f" % tuple(lo + hi),
           "mtllib %s.mtl" % name]
    base = 1
    numtris = 0

    for mi, m in enumerate(meshes):
        n = len(m["pos"])
        out.append("o mesh%d" % mi)

        for p in m["pos"]:
            out.append("v %.5f %.5f %.5f" % p)
        for u in (m["uvs"] or [(0.0, 0.0)] * n):
            # SEModel's v runs down the picture; OBJ's runs up
            out.append("vt %.6f %.6f" % (u[0], 1.0 - u[1]))
        for nn in (m["normals"] or [(0.0, 0.0, 1.0)] * n):
            out.append("vn %.5f %.5f %.5f" % nn)

        if 0 <= m["material"] < len(materials):
            out.append("usemtl %s" % materials[m["material"]]["name"])

        for a, b, c in m["faces"]:
            out.append("f %d/%d/%d %d/%d/%d %d/%d/%d" % (a + base, a + base, a + base,
                                                         b + base, b + base, b + base,
                                                         c + base, c + base, c + base))
        base += n
        numtris += len(m["faces"])

    with open(os.path.join(outdir, name + ".obj"), "w") as f:
        f.write("\n".join(out) + "\n")

    print("%s: %d meshes, %d vertices, %d triangles, %d materials" %
          (name, len(meshes), base - 1, numtris, len(materials)))
    print("bounds %.2f %.2f %.2f to %.2f %.2f %.2f" % tuple(lo + hi))

    for m in meshes:
        print("  mesh: %d vertices, %d triangles, material %s" %
              (len(m["pos"]), len(m["faces"]),
               materials[m["material"]]["name"] if 0 <= m["material"] < len(materials) else "-"))


if __name__ == "__main__":
    main()
