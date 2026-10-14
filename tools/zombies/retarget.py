#!/usr/bin/env python3
"""
The perk drink's arms retargeted onto Perfect Dark's own first person hands,
checked as a stick figure before it is in the game:

    python3 tools/zombies/retarget.py <viewmodel dir> <anim> <frame> <out.png>

The Call of Duty skeleton (arms.zvm, posed by the .zan as vmpose.py poses it)
is drawn white, and Perfect Dark's hand skeleton retargeted onto it green.

Perfect Dark's hand file (hand file 1253 dumped in the game: 33 matrices,
both hands) is a forearm (1, 17), a wrist (2, 18), a thumb of two joints and
four fingers of three, every bone a position offset from its parent with no
rotation of its own: its rest pose is the model's own axes, fingers along +z.

Each Perfect Dark joint takes its partner's frame: the direction from the
joint to the next one along the bone, and the hand's sideways line (index
knuckle to little finger knuckle), the same two things read off each
skeleton, so the rest poses need not agree. Positions follow Perfect Dark's
own bone lengths from the wrist, which is put at Call of Duty's, the hand
scaled so its palm is Call of Duty's length.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from imgio import png_write  # noqa: E402
from vmpose import load_zvm, load_zan, pose  # noqa: E402

# Perfect Dark's hand skeleton: joint -> (parent, offset from the parent)
PD = {
    0: (-1, (0.00, -20.38, -74.14)),
    1: (0, (-0.13, -14.12, -329.58)), 2: (1, (0.13, 14.12, 329.58)),
    3: (2, (28.40, -33.97, 41.48)), 4: (3, (21.67, -24.49, 22.00)),
    5: (2, (28.64, 3.66, 94.92)), 6: (5, (-2.13, -7.44, 39.38)), 7: (6, (-1.66, -1.48, 26.93)),
    8: (2, (8.82, 9.08, 95.84)), 9: (8, (-0.15, -12.05, 49.72)), 10: (9, (-1.10, -2.83, 26.93)),
    11: (2, (-7.45, 4.84, 95.75)), 12: (11, (0.67, -6.00, 40.75)), 13: (12, (0.49, -1.96, 22.72)),
    14: (2, (-23.59, -2.68, 95.97)), 15: (14, (1.70, -2.07, 35.10)), 16: (15, (1.15, 0.40, 17.15)),
    17: (0, (234.47, -14.12, -329.58)), 18: (17, (-0.13, 14.12, 329.58)),
    19: (18, (-28.40, -33.97, 41.48)), 20: (19, (-21.67, -24.49, 22.00)),
    21: (18, (-28.64, 3.66, 94.92)), 22: (21, (2.13, -7.44, 39.38)), 23: (22, (1.73, -1.66, 26.94)),
    24: (18, (-8.82, 9.08, 95.84)), 25: (24, (0.15, -12.05, 49.72)), 26: (25, (1.10, -2.83, 26.93)),
    27: (18, (7.45, 4.84, 95.75)), 28: (27, (-0.67, -6.00, 40.75)), 29: (28, (-0.49, -1.96, 22.72)),
    30: (18, (23.59, -2.68, 95.97)), 31: (30, (-1.70, -2.07, 35.10)), 32: (31, (-1.15, 0.40, 17.15)),
}

# Perfect Dark joint -> (Call of Duty bone, the bone its direction runs to);
# s is the side, "ri" or "le"
def mapping(side, base):
    return {
        base + 1: ("j_elbow_%s" % side, "j_wrist_%s" % side),
        base + 2: ("j_wrist_%s" % side, "j_mid_%s_0" % side),
        # Perfect Dark's thumb starts at its knuckle: Call of Duty's _0 is
        # the root of the thumb's own bone in the palm
        base + 3: ("j_thumb_%s_1" % side, "j_thumb_%s_2" % side),
        base + 4: ("j_thumb_%s_2" % side, None),
        base + 5: ("j_index_%s_0" % side, "j_index_%s_1" % side),
        base + 6: ("j_index_%s_1" % side, "j_index_%s_2" % side),
        base + 7: ("j_index_%s_2" % side, None),
        base + 8: ("j_mid_%s_0" % side, "j_mid_%s_1" % side),
        base + 9: ("j_mid_%s_1" % side, "j_mid_%s_2" % side),
        base + 10: ("j_mid_%s_2" % side, None),
        base + 11: ("j_ring_%s_0" % side, "j_ring_%s_1" % side),
        base + 12: ("j_ring_%s_1" % side, "j_ring_%s_2" % side),
        base + 13: ("j_ring_%s_2" % side, None),
        base + 14: ("j_pinky_%s_0" % side, "j_pinky_%s_1" % side),
        base + 15: ("j_pinky_%s_1" % side, "j_pinky_%s_2" % side),
        base + 16: ("j_pinky_%s_2" % side, None),
    }


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def norm(v):
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def frame(a, lat):
    """columns x = a, z = a x lat, y = z x x"""
    x = norm(a)
    z = norm(cross(x, lat))
    y = cross(z, x)
    return [[x[i], y[i], z[i]] for i in range(3)]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def transpose(a):
    return [[a[j][i] for j in range(3)] for i in range(3)]


def apply(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


def pd_rest():
    q = {}
    for j in sorted(PD):
        p, off = PD[j]
        q[j] = list(off) if p < 0 else [q[p][i] + off[i] for i in range(3)]
    return q


def retarget(P):
    """Perfect Dark's joints posed: joint -> (rotation, position), in Call of
    Duty's space; P is Call of Duty's bone -> position"""
    Q = pd_rest()
    out = {}

    for side, base in (("ri", 0), ("le", 16)):
        m = mapping(side, base)
        lat_c = norm(sub(P["j_index_%s_0" % side], P["j_pinky_%s_0" % side]))
        lat_p = norm(sub(Q[base + 5], Q[base + 14]))
        k = (math.dist(P["j_mid_%s_0" % side], P["j_wrist_%s" % side]) /
             math.dist(Q[base + 8], Q[base + 2]))
        rot = {}

        for j, (c, c2) in m.items():
            if c not in P:
                continue
            # the direction along the bone, out of the joint where it has a
            # joint after it, else into it
            if c2 and c2 in P:
                a_c = sub(P[c2], P[c])
            else:
                parent_c = [cc for jj, (cc, cc2) in m.items() if cc2 == c][0]
                a_c = sub(P[c], P[parent_c])
            child = [jj for jj in PD if PD[jj][0] == j and jj in m]
            child = [jj for jj in child if jj in (j + 1,)] or child
            if j == base + 2:
                a_p = sub(Q[base + 8], Q[j])
            elif child:
                a_p = sub(Q[child[0]], Q[j])
            else:
                a_p = sub(Q[j], Q[PD[j][0]])
            rot[j] = matmul(frame(a_c, lat_c), transpose(frame(a_p, lat_p)))

        # positions from the wrist: Perfect Dark's own lengths, scaled
        pos = {base + 2: P["j_wrist_%s" % side]}
        pos[base + 1] = sub(pos[base + 2], [v * k for v in apply(rot[base + 1], PD[base + 2][1])])

        for j in sorted(m):
            if j in pos or j not in rot:
                continue
            p = PD[j][0]
            if j == base + 3:
                # Perfect Dark's thumb sits higher on the palm than Call of
                # Duty's: the thumb follows its partner's knuckle, or it
                # misses the bottle
                pos[j] = P[m[j][0]]
                continue
            pos[j] = [pos[p][i] + v * k for i, v in enumerate(apply(rot[p], PD[j][1]))]

        for j in rot:
            out[j] = (rot[j], pos[j])

    return out


def main():
    d, animname, frame_no, outpng = sys.argv[1:5]
    arms = load_zvm(os.path.join(d, "arms.zvm"))
    _, _, tracks = load_zan(os.path.join(d, animname + ".zan"))

    # Call of Duty's bones posed: positions from the skinning matrices'
    # inverse binds taken back off (world = skin * bind)
    skins = pose([arms], tracks, float(frame_no))[0]
    P = {}
    for (name, parent, t, q, ib), sm in zip(arms[0], skins):
        # world translation = skin applied to the bone's bind origin
        bind = invert(ib)
        P[name] = [sm[r][0] * bind[0][3] + sm[r][1] * bind[1][3] + sm[r][2] * bind[2][3] + sm[r][3] for r in range(3)]

    posed = retarget(P)
    W, H = 640, 480
    f = (W / 2) / math.tan(math.radians(65) / 2)
    img = bytearray(b"\x20\x20\x28\xff" * (W * H))

    def proj(v):
        if v[0] < 0.5:
            return None
        return (W / 2 - f * v[1] / v[0], H / 2 - f * v[2] / v[0])

    def line(a, b, col):
        pa, pb = proj(a), proj(b)
        if not pa or not pb:
            return
        n = int(max(abs(pb[0] - pa[0]), abs(pb[1] - pa[1]))) + 1
        for i in range(n + 1):
            x = int(pa[0] + (pb[0] - pa[0]) * i / n)
            y = int(pa[1] + (pb[1] - pa[1]) * i / n)
            for dx in (0, 1):
                for dy in (0, 1):
                    if 0 <= x + dx < W and 0 <= y + dy < H:
                        o = ((y + dy) * W + x + dx) * 4
                        img[o:o + 3] = bytes(col)

    names = [b[0] for b in arms[0]]
    for name, parent, t, q, ib in arms[0]:
        if parent >= 0 and (name.startswith("j_") and not name.startswith("j_sleave")):
            line(P[name], P[names[parent]], (230, 230, 230))

    for j, (rot, pos) in posed.items():
        p = PD[j][0]
        if p in posed:
            line(pos, posed[p][1], (60, 230, 90))
        # each joint's own bone axis, short, to show its turn
        tip = [pos[i] + rot[i][2] * 1.2 for i in range(3)]
        line(pos, tip, (240, 90, 60))

    png_write(outpng, W, H, img)
    print("wrote", outpng)


def invert(m):
    a = [row[:3] for row in m]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
           - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    inv = [[(a[(j + 1) % 3][(i + 1) % 3] * a[(j + 2) % 3][(i + 2) % 3]
             - a[(j + 1) % 3][(i + 2) % 3] * a[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)]
           for i in range(3)]
    t = [-sum(inv[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [inv[i] + [t[i]] for i in range(3)]


if __name__ == "__main__":
    main()
