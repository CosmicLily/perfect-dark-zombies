#!/usr/bin/env python3
"""
The perk drink baked for the game (zombies.inc, zombiesRenderDrink()): the
player's own first person hands posed by Black Ops II's drink, a matrix per
joint a 60th of a second, and each bottle as two meshes the game's OBJ reader
takes. The art stays on the player's disk, out of the tree.

    python3 tools/zombies/vmbake.py <viewmodel dir>
    python3 tools/zombies/vmbake.py <viewmodel dir> grenade
    python3 tools/zombies/vmbake.py <viewmodel dir> gun <name> <clip> ...

<viewmodel dir> is vmconv.py's: arms.zvm, drink.zan and bottle_<perk>.zvm.
Written beside them:

  drink.zpd   "ZPD1", u16 ticks, u16 joints (33), u16 bottle parts (2), u16 0,
              then for each 60th, each hand joint's 3x4 matrix and each
              bottle part's, row major floats in Call of Duty's view space
              (inches, x forward, y left, z up). A hand joint's takes Perfect
              Dark's hand model (its own units, its joint's own frame) there;
              a bottle part's takes the part's OBJ (tenths of an inch, as
              written here) there.
  bottle_<perk>_body.obj, bottle_<perk>_top.obj and bottle_<perk>.mtl:
              the bottle and its cap, which comes off, in the bottle's own
              bind space in tenths of an inch (the game keeps a vertex at a
              quarter of a unit), its pictures vmconv.py's.

grenade bakes the grenade's quick throw (zombies.inc, zombiesRenderThrow())
from pullpin.zan, throw.zan and frag.zvm (Black Ops II's viewmodel_m67_pullpin,
viewmodel_m67_throw and t6_wpn_grenade_frag_view):

  grenade.zpd as drink.zpd, the pin pulled and then the throw one after the
              other, its last u16 the 60th the throw starts at; the parts
              the grenade and its pin (j_grenade_activate_clip).
  frag_body.obj, frag_top.obj and frag.mtl: the grenade and its pin.

gun bakes a gun of Zombies' own held in the hands (zombies.inc,
zombiesRenderGunViewmodel()) from <name>.zvm and <name>_<clip>.zan for each
clip named (Black Ops II's Ray Gun: raygun, from t6_wpn_zmb_raygun_view and
viewmodel_raygun_t6_idle, _fire, _reload and the rest):

  <name>_<clip>.zpd as drink.zpd, one a clip, the parts each of the gun's
              bones that moves a vertex, in the order of <name>.parts.
  <name>.parts the parts' bone names, one a line.
  <name>_<part>.obj and <name>.mtl: each part's vertices, its pictures
              vmconv.py's, a draw without one left the model's own grey.
"""

import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from vmpose import load_zvm, load_zan, pose  # noqa: E402
from retarget import PD, pd_rest, retarget, invert  # noqa: E402

OBJ_UNITS = 10.0  # the OBJs' units to the inch


def bone_positions(arms, skins):
    """Call of Duty's bones posed: bone -> position"""
    P = {}
    for (name, parent, t, q, ib), sm in zip(arms[0], skins):
        bind = invert(ib)
        P[name] = [sm[r][0] * bind[0][3] + sm[r][1] * bind[1][3] + sm[r][2] * bind[2][3] + sm[r][3]
                   for r in range(3)]
    return P


def hand_matrices(P):
    """Perfect Dark's 33 joints as 3x4 rows: the model's units and the joint's
    own frame to Call of Duty's view space"""
    posed = retarget(P)
    Q = pd_rest()
    # each hand scaled so its palm is Call of Duty's (as retarget() places it)
    k = [math.dist(P["j_mid_%s_0" % s], P["j_wrist_%s" % s]) / math.dist(Q[b + 8], Q[b + 2])
         for s, b in (("ri", 0), ("le", 16))]
    out = []

    for j in range(33):
        if j == 0:
            # the root: where the right forearm's parent would be
            rot, pos = posed[1]
            kk = k[0]
            pos = [pos[i] - sum(rot[i][c] * PD[1][1][c] for c in range(3)) * kk for i in range(3)]
        else:
            rot, pos = posed[j]
            kk = k[0 if j < 17 else 1]
        out.append([[rot[r][0] * kk, rot[r][1] * kk, rot[r][2] * kk, pos[r]] for r in range(3)])

    return out


def write_bottle(d, perk, bottle, topbone="j_top"):
    bones, verts, tris, draws = bottle
    top = [b[0] for b in bones].index(topbone)

    # a vertex is the cap's when the cap's bone holds most of it
    def is_top(v):
        return sum(w for j, w in zip(v[3], v[4]) if j == top) > 0.5

    with open(os.path.join(d, "%s.mtl" % perk), "w") as f:
        for i, (_, _, _, pic) in enumerate(draws):
            f.write("newmtl %s_%d\nmap_Kd %s\n\n" % (perk, i, pic))

    for part, want in (("body", False), ("top", True)):
        out = ["# %s's %s, from vmbake.py" % (perk, part), "mtllib %s.mtl" % perk]
        for p, n, uv, js, ws in verts:
            out.append("v %.4f %.4f %.4f" % tuple(c * OBJ_UNITS for c in p))
        for p, n, uv, js, ws in verts:
            # vmconv.py's v is already OBJ's way up, as glb2obj.py writes it
            out.append("vt %.6f %.6f" % (uv[0], uv[1]))
        for p, n, uv, js, ws in verts:
            out.append("vn %.5f %.5f %.5f" % tuple(n))
        numtris = 0
        for i, (first, count, flags, pic) in enumerate(draws):
            out.append("usemtl %s_%d" % (perk, i))
            for a, b, c in tris[first:first + count]:
                if all(is_top(verts[x]) == want for x in (a, b, c)):
                    a, b, c = a + 1, b + 1, c + 1
                    out.append("f %d/%d/%d %d/%d/%d %d/%d/%d" % (a, a, a, c, c, c, b, b, b))
                    numtris += 1
        with open(os.path.join(d, "%s_%s.obj" % (perk, part)), "w") as f:
            f.write("\n".join(out) + "\n")
        print("%s_%s.obj: %d triangles" % (perk, part, numtris))


def bake(arms, prop, parts, clips):
    """the hands and the prop's parts a 60th at a time through each clip in
    turn: the data, and the 60th each clip starts at"""
    data = bytearray()
    starts = []
    ticks = 0

    for frames, fps, tracks in clips:
        starts.append(ticks)
        n = (frames - 1) * 60 // fps + 1
        for t in range(n):
            skins = pose([arms, prop], tracks, t * fps / 60.0)
            P = bone_positions(arms, skins[0])
            for m in hand_matrices(P):
                data += struct.pack("<12f", *(m[0] + m[1] + m[2]))
            for b in parts:
                sm = skins[1][b]
                m = [[sm[r][c] / OBJ_UNITS for c in range(3)] + [sm[r][3]] for r in range(3)]
                data += struct.pack("<12f", *(m[0] + m[1] + m[2]))
        ticks += n

    return data, starts, ticks


def grenade(d):
    arms = load_zvm(os.path.join(d, "arms.zvm"))
    frag = load_zvm(os.path.join(d, "frag.zvm"))
    names = [b[0] for b in frag[0]]
    # the grenade is its clip (tag_clip, every vertex of its body): j_gun
    # stands still where the arms' tag_weapon holds it
    parts = [names.index("tag_clip"), names.index("j_grenade_activate_clip")]
    clips = [load_zan(os.path.join(d, name + ".zan")) for name in ("pullpin", "throw")]
    data, starts, ticks = bake(arms, frag, parts, clips)

    out = bytearray(b"ZPD1" + struct.pack("<4H", ticks, 33, len(parts), starts[1])) + data
    open(os.path.join(d, "grenade.zpd"), "wb").write(out)
    print("grenade.zpd: %d ticks, the throw from %d, %d bytes" % (ticks, starts[1], len(out)))

    write_bottle(d, "frag", frag, "j_grenade_activate_clip")


def write_parts(d, name, model, parts):
    """each part's own OBJ: the vertices its bone holds most of"""
    bones, verts, tris, draws = model

    with open(os.path.join(d, "%s.mtl" % name), "w") as f:
        for i, (_, _, _, pic) in enumerate(draws):
            f.write("newmtl %s_%d\n" % (name, i))
            f.write("map_Kd %s\n\n" % pic if pic else "Kd 0.55 0.55 0.55\n\n")

    def owner(v):
        return max(range(4), key=lambda i: v[4][i])

    for b in parts:
        bname = bones[b][0]
        out = ["# %s's %s, from vmbake.py" % (name, bname), "mtllib %s.mtl" % name]
        for p, n, uv, js, ws in verts:
            out.append("v %.4f %.4f %.4f" % tuple(c * OBJ_UNITS for c in p))
        for p, n, uv, js, ws in verts:
            out.append("vt %.6f %.6f" % (uv[0], uv[1]))
        for p, n, uv, js, ws in verts:
            out.append("vn %.5f %.5f %.5f" % tuple(n))
        numtris = 0
        for i, (first, count, flags, pic) in enumerate(draws):
            out.append("usemtl %s_%d" % (name, i))
            for a, bb, c in tris[first:first + count]:
                if all(verts[x][3][owner(verts[x])] == b for x in (a, bb, c)):
                    a, bb, c = a + 1, bb + 1, c + 1
                    out.append("f %d/%d/%d %d/%d/%d %d/%d/%d" % (a, a, a, c, c, c, bb, bb, bb))
                    numtris += 1
        with open(os.path.join(d, "%s_%s.obj" % (name, bname)), "w") as f:
            f.write("\n".join(out) + "\n")
        print("%s_%s.obj: %d triangles" % (name, bname, numtris))


def gun(d, name, clips):
    arms = load_zvm(os.path.join(d, "arms.zvm"))
    model = load_zvm(os.path.join(d, name + ".zvm"))
    bones, verts = model[0], model[1]
    held = set()

    for p, n, uv, js, ws in verts:
        held.add(js[max(range(4), key=lambda i: ws[i])])

    parts = sorted(held)

    with open(os.path.join(d, name + ".parts"), "w") as f:
        f.write("\n".join(bones[b][0] for b in parts) + "\n")

    for clip in clips:
        data, starts, ticks = bake(arms, model, parts, [load_zan(os.path.join(d, "%s_%s.zan" % (name, clip)))])
        out = bytearray(b"ZPD1" + struct.pack("<4H", ticks, 33, len(parts), 0)) + data
        open(os.path.join(d, "%s_%s.zpd" % (name, clip)), "wb").write(out)
        print("%s_%s.zpd: %d ticks, %d parts" % (name, clip, ticks, len(parts)))

    write_parts(d, name, model, parts)


def main():
    d = sys.argv[1]

    if sys.argv[2:] == ["grenade"]:
        grenade(d)
        return

    if len(sys.argv) > 4 and sys.argv[2] == "gun":
        gun(d, sys.argv[3], sys.argv[4:])
        return

    arms = load_zvm(os.path.join(d, "arms.zvm"))
    bottles = sorted(f[:-4] for f in os.listdir(d) if f.startswith("bottle_") and f.endswith(".zvm"))
    bottle = load_zvm(os.path.join(d, bottles[0] + ".zvm"))
    frames, fps, tracks = load_zan(os.path.join(d, "drink.zan"))
    ticks = (frames - 1) * 60 // fps + 1
    names = [b[0] for b in bottle[0]]
    parts = [names.index("j_gun"), names.index("j_top")]

    data = bytearray(b"ZPD1" + struct.pack("<4H", ticks, 33, len(parts), 0))

    for t in range(ticks):
        skins = pose([arms, bottle], tracks, t * fps / 60.0)
        P = bone_positions(arms, skins[0])
        for m in hand_matrices(P):
            data += struct.pack("<12f", *(m[0] + m[1] + m[2]))
        for b in parts:
            sm = skins[1][b]
            # the OBJ's tenths of an inch to the bind's inches first
            m = [[sm[r][c] / OBJ_UNITS for c in range(3)] + [sm[r][3]] for r in range(3)]
            data += struct.pack("<12f", *(m[0] + m[1] + m[2]))

    open(os.path.join(d, "drink.zpd"), "wb").write(data)
    print("drink.zpd: %d ticks, %d bytes" % (ticks, len(data)))

    for name in bottles:
        write_bottle(d, name, load_zvm(os.path.join(d, name + ".zvm")))


if __name__ == "__main__":
    main()
