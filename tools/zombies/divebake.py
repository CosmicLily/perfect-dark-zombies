#!/usr/bin/env python3
"""
Black Ops II's dolphin dive and prone retargeted onto Perfect Dark's body
(g_SkelChr), baked for the game (zombies.inc, zombiesPoseDive()): the dive,
its landing, getting down to prone and up again, lying prone and crawling,
each joint's turn at every frame of Call of Duty's.

    python3 tools/zombies/divebake.py <xanim dir> <body.glb> <out.zbd> [check.png]

<xanim dir> is an OpenAssetTools dump of common_zm's xanim/ (CLIPS below);
<body.glb> any Black Ops II body with its standard human skeleton (zm_prison's
c_zom_inmate_body1_lod0.glb). With check.png, a sheet of stick figures from
the side: Call of Duty's skeleton white, Perfect Dark's retargeted onto it
green, every eighth frame of the short clips. The art stays on the player's
disk, out of the tree.

The player body's skeleton is the glb's with torso_stabilizer between
j_mainroot and j_spinelower: the stabilizer takes the pelvis's turn back off
(its key is j_mainroot's inverse) and j_spinelower's track is in its frame.
j_mainroot's translation is an offset from its bind (standing it is 4 inches
down, prone 32). The dive's root motion (tag_origin: 119 inches forward, 18.8
degrees to the left of its x, a hop of 44 inches and a yaw of 17.8 degrees)
is the game's to do, but for the hop: the body is posed on the floor where
the dive left it, the hop in it, the clips all turned so the dive goes
straight forward, and the landing and getting up where the dive's yaw left
the body.

Perfect Dark's chr skeleton is fifteen joints, an animation part each, every
one a position offset from its parent and no turn of its own: its rest pose
is the model's own axes (the model faces +z, +x is its left: chr.c's
lshoulderjoint 2 is part 3's matrix), arms and legs straight out along x.
Each joint takes its partner's frame, as retarget.py does the hands: the
direction along the bone and a second line - a limb's hinge (the Call of Duty
joint's own z, which a knee or elbow turns about), the hips' or shoulders'
line across the pelvis or chest - read off both skeletons; the turn is
F_cod * F_pd_rest^T. A hand keeps its forearm's turn. Positions are the
game's to work out from the body's own offsets (bodies differ); the pelvis's
place is Call of Duty's, scaled by the legs' lengths.

Laid out, Call of Duty's body is behind its origin (the pelvis 25 inches
back, the feet 60), and a third person camera behind the player had the
thighs in its lens. So a body lying down is moved forward by as much as puts
it midway, head to ankles, on the player: as the dive or getting down lays
it out, all the while it lies or crawls, and gone again as it gets up.

out.zbd, little endian:
    "ZBD2", u16 clips (CLIPS' order), u16 parts (14), f32 leg length (the
    model units the pelvis's place is in: thigh + shin of the body here),
    f32 the model's units to Call of Duty's inch
    per clip: u16 frames, u16 flags (1: loops, its frame `frames` its first),
    f32 frames a second, f32 inches a loop carries the body (a crawl's), then
    per frame f32 pelvis[3] (the model's units, x left, y up from the floor,
    z forward), then parts 1 to 14 each a 3x3 turn, row major: the joint's
    rest axes into the model's.
"""

import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from glb2obj import read_glb  # noqa: E402
from imgio import png_write  # noqa: E402
from xanim import XAnim  # noqa: E402

CLIPS = ("pb_dive_prone", "pb_dive_prone_land", "pb_prone2crouch", "pb_crouch2prone",
         "pb_prone_aim", "pb_prone_crawl", "pb_prone_crawl_back", "pb_prone_crawl_left",
         "pb_prone_crawl_right")
DIVE, LAND, GETUP, GETDOWN = 0, 1, 2, 3

# Perfect Dark's chr skeleton: animation part -> (parent part, offset in
# model units), dumped from the player's body
PD = {
    1: (0, (1.02, 35.46, 0)),
    2: (1, (-2.22, 429.41, 0)),
    3: (2, (147.57, 299.0, 0)), 5: (3, (213.79, 43.69, -17.10)), 7: (5, (218.06, 16.55, 16.97)),
    4: (2, (-143.51, 303.61, 0)), 6: (4, (-215.76, 43.62, -18.27)), 8: (6, (-224.73, 12.48, 19.37)),
    9: (1, (83.75, -85.63, 0)), 11: (9, (441.24, -5.09, 0)), 13: (11, (381.32, -3.69, -29.61)),
    10: (1, (-78.08, -87.6, 0)), 12: (10, (-450.97, -14.73, 0)), 14: (12, (-369.57, -9.29, -34.54)),
}


# --- small vector and matrix helpers (3x3 as rows) ---

def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def add(a, b):
    return [a[i] + b[i] for i in range(3)]


def scale(a, k):
    return [v * k for v in a]


def dot(a, b):
    return sum(a[i] * b[i] for i in range(3))


def norm(v):
    n = math.sqrt(dot(v, v)) or 1.0
    return [x / n for x in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def transpose(a):
    return [[a[j][i] for j in range(3)] for i in range(3)]


def apply(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


def column(m, c):
    return [m[0][c], m[1][c], m[2][c]]


def quat(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def yaw(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def frame(a, lat):
    """columns x = a, z = a x lat, y = z x x"""
    x = norm(a)
    z = norm(cross(x, lat))
    y = cross(z, x)
    return [[x[i], y[i], z[i]] for i in range(3)]


def to_pd(v):
    """Call of Duty's x forward, y left, z up to the model's x left, y up, z forward"""
    return [v[1], v[2], v[0]]


def rot_to_pd(m):
    p = [[0, 1, 0], [0, 0, 1], [1, 0, 0]]
    return matmul(matmul(p, m), transpose(p))


# --- Call of Duty's side ---

def load_skeleton(path):
    """bone -> (parent or None, rest translation, rest quaternion), parents first,
    in tag_origin's space (z up, x forward)"""
    doc, _ = read_glb(path)
    nodes = doc["nodes"]
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i
    origin = [i for i, n in enumerate(nodes) if n.get("name") == "tag_origin"][0]
    bones = []

    def walk(i):
        for c in nodes[i].get("children", []):
            n = nodes[c]
            p = nodes[i]["name"] if i != origin else None
            bones.append((n["name"], p, n.get("translation", [0, 0, 0]), n.get("rotation", [0, 0, 0, 1])))
            if n["name"] == "j_mainroot":
                bones.append(("torso_stabilizer", "j_mainroot", [0, 0, 0], [0, 0, 0, 1]))
            walk(c)

    walk(origin)
    return [(n, "torso_stabilizer" if n == "j_spinelower" else p, t, q) for n, p, t, q in bones]


def pose(bones, anim, f, tag):
    """bone -> (position, rotation) at frame f, tag_origin at tag (rotation, position)"""
    world = {}
    for name, parent, t, q in bones:
        aq, at = anim.sample(name, f) if anim else (None, None)
        lq = quat(aq or q)
        if name == "j_mainroot":
            lt = add(t, at) if at else t
        else:
            lt = at or t
        pp, pr = world[parent] if parent else (tag[1], tag[0])
        world[name] = (add(pp, apply(pr, lt)), matmul(pr, lq))
    return world


def hinge_signs(bones):
    """each knee's and elbow's z against the way the limb bends at the bind"""
    w = pose(bones, None, 0, ([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0]))
    signs = {}
    for top, mid, end in limbs():
        bend = cross(sub(w[mid][0], w[top][0]), sub(w[end][0], w[mid][0]))
        signs[mid] = 1.0 if dot(column(w[mid][1], 2), bend) >= 0 else -1.0
    return signs


def limbs():
    out = []
    for s in ("le", "ri"):
        out.append(("j_shoulder_" + s, "j_elbow_" + s, "j_wrist_" + s))
        out.append(("j_hip_" + s, "j_knee_" + s, "j_ankle_" + s))
    return out


# --- the retarget ---

def pd_rest():
    q = {0: [0.0, 0.0, 0.0]}
    for j in sorted(PD):
        p, off = PD[j]
        q[j] = add(q[p], off)
    return q


def leg_length():
    return (sum(math.dist([0, 0, 0], PD[j][1]) for j in (11, 13, 12, 14))) / 2


def retarget(w, signs, k):
    """parts -> turn, and the pelvis's place; w is Call of Duty's pose in the
    model's axes, k the model's units to the inch"""
    P = {n: to_pd(p) for n, (p, r) in w.items()}
    Z = {n: to_pd(column(r, 2)) for n, (p, r) in w.items()}
    Q = pd_rest()
    fwd = [0, 0, 1]
    R = {}

    def hinge(mid):
        return scale(Z[mid], signs[mid])

    # pelvis and chest: up the body, across the hips / shoulders
    R[1] = matmul(frame(sub(P["j_spine4"], P["j_mainroot"]), sub(P["j_hip_le"], P["j_hip_ri"])),
                  transpose(frame(sub(Q[2], Q[1]), sub(Q[9], Q[10]))))
    R[2] = matmul(frame(sub(P["j_neck"], P["j_spine4"]), sub(P["j_shoulder_le"], P["j_shoulder_ri"])),
                  transpose(frame([0, 1, 0], sub(Q[3], Q[4]))))

    for s, up, fore, hand, thigh, shin, foot in (("le", 3, 5, 7, 9, 11, 13), ("ri", 4, 6, 8, 10, 12, 14)):
        h = hinge("j_elbow_" + s)
        # the rest arm bends its forearm forward about this
        hp = cross(PD[fore][1], fwd)
        R[up] = matmul(frame(sub(P["j_elbow_" + s], P["j_shoulder_" + s]), h), transpose(frame(PD[fore][1], hp)))
        R[fore] = matmul(frame(sub(P["j_wrist_" + s], P["j_elbow_" + s]), h), transpose(frame(PD[hand][1], hp)))
        R[hand] = R[fore]

        h = hinge("j_knee_" + s)
        # and its shin back
        hp = cross(PD[shin][1], scale(fwd, -1))
        R[thigh] = matmul(frame(sub(P["j_knee_" + s], P["j_hip_" + s]), h), transpose(frame(PD[shin][1], hp)))
        R[shin] = matmul(frame(sub(P["j_ankle_" + s], P["j_knee_" + s]), h), transpose(frame(PD[foot][1], hp)))
        # the foot points forward at rest, its toes along +z
        R[foot] = matmul(frame(sub(P["j_ball_" + s], P["j_ankle_" + s]), h), transpose(frame(fwd, hp)))

    # the hips' middle where Call of Duty's is
    mid = scale(add(PD[9][1], PD[10][1]), 0.5)
    pelvis = sub(scale(P["j_mainroot"], k), apply(R[1], mid))
    return R, pelvis


def posed_pd(R, pelvis):
    """each part's place from the pelvis down the offsets"""
    pos = {1: pelvis}
    for j in sorted(PD):
        if j != 1:
            p = PD[j][0]
            pos[j] = add(pos[p], apply(R[p], PD[j][1]))
    return pos


# --- the clips ---

def clip_poses(bones, xdir):
    """each clip: (its XAnim, [w per frame]), in Call of Duty's axes"""
    anims = [XAnim(os.path.join(xdir, name)) for name in CLIPS]
    dive = anims[0]
    end = dive.delta_trans[-1][1]
    travel = math.atan2(end[1], end[0])

    def at(keys, f):
        if not keys:
            return None
        if f <= keys[0][0]:
            return keys[0][1]
        for (f0, a), (f1, b) in zip(keys, keys[1:]):
            if f0 <= f <= f1:
                t = (f - f0) / float(f1 - f0)
                return a + (b - a) * t if not isinstance(a, tuple) else tuple(a[i] + (b[i] - a[i]) * t for i in range(3))
        return keys[-1][1]

    def yaw_at(f):
        z = at(dive.delta_yaw, f) or 0.0
        return 2 * math.asin(max(-1.0, min(1.0, z)))

    final = yaw_at(dive.frames - 1)
    out = []

    for i, a in enumerate(anims):
        ws = []
        for f in range(a.frames):
            if i == DIVE:
                hop = at(dive.delta_trans, f)
                tag = (yaw(yaw_at(f) - travel), [0, 0, hop[2] if hop else 0.0])
            else:
                tag = (yaw(final - travel), [0, 0, 0])
            ws.append(pose(bones, a, f, tag))
        out.append((a, ws))

    return out


# --- the check ---

def draw_sheet(path, panels):
    """panels: [(cod segments, pd segments)] in the model's axes, drawn side on
    (z to the right, y up)"""
    cols = 6
    pw, ph = 200, 160
    rows = (len(panels) + cols - 1) // cols
    W, H = cols * pw, rows * ph
    img = bytearray(b"\x20\x20\x28\xff" * (W * H))

    def line(ox, oy, a, b, col, s):
        pa = (ox + pw * 0.5 + a[2] * s, oy + ph * 0.85 - a[1] * s)
        pb = (ox + pw * 0.5 + b[2] * s, oy + ph * 0.85 - b[1] * s)
        n = int(max(abs(pb[0] - pa[0]), abs(pb[1] - pa[1]))) + 1
        for i in range(n + 1):
            x = int(pa[0] + (pb[0] - pa[0]) * i / n)
            y = int(pa[1] + (pb[1] - pa[1]) * i / n)
            if ox <= x < ox + pw and oy <= y < oy + ph:
                o = (y * W + x) * 4
                img[o:o + 3] = bytes(col)

    for i, (cod, pd) in enumerate(panels):
        ox, oy = (i % cols) * pw, (i // cols) * ph
        for x in range(pw):
            o = ((oy + int(ph * 0.85)) * W + ox + x) * 4
            img[o:o + 3] = b"\x50\x50\x50"
        for a, b in cod:
            line(ox, oy, a, b, (230, 230, 230), 0.065)
        for a, b, col in pd:
            line(ox, oy, a, b, col, 0.065)

    png_write(path, W, H, img)


def main():
    xdir, glb, out = sys.argv[1:4]
    check = sys.argv[4] if len(sys.argv) > 4 else None
    bones = load_skeleton(glb)
    signs = hinge_signs(bones)
    names = {b[0]: b[1] for b in bones}
    cod_leg = sum(math.dist([0, 0, 0], t) for n, p, t, q in bones if n in ("j_knee_le", "j_ankle_le"))
    legs = leg_length()
    k = legs / cod_leg
    clips = clip_poses(bones, xdir)

    data = bytearray(b"ZBD2" + struct.pack("<2H2f", len(clips), 14, legs, k))
    panels = []

    # the body midway, head to ankles, on the player while it lies (above)
    def middle(w):
        ankles = scale(add(to_pd(w["j_ankle_le"][0]), to_pd(w["j_ankle_ri"][0])), 0.5)
        return (to_pd(w["j_head"][0])[2] + ankles[2]) * 0.5 * k

    dive = clips[DIVE][1]
    laid = [middle(w) for w in dive[len(dive) // 2:]] + [middle(w) for w in clips[LAND][1]]
    forward = -sum(laid) / len(laid)
    print("laid out, the body's middle is %.1f units from the origin; moved forward %.1f" % (-forward, forward))

    def shift(c, t, ws):
        if c in (DIVE, GETDOWN):
            return max(0.0, min(forward, -middle(ws[t])))
        if c == GETUP:
            return forward * (1.0 - t / float(len(ws) - 1))
        return forward

    for c, (a, ws) in enumerate(clips):
        loops = a.flags & 1
        travel = math.hypot(*a.delta_trans[-1][1][:2]) if loops and a.delta_trans else 0.0
        data += struct.pack("<2H2f", len(ws), loops, a.framerate, travel)
        for t, w in enumerate(ws):
            R, pelvis = retarget(w, signs, k)
            pelvis[2] += shift(c, t, ws)
            data += struct.pack("<3f", *pelvis)
            for j in range(1, 15):
                data += struct.pack("<9f", *(R[j][0] + R[j][1] + R[j][2]))

            if check and t % 8 == 0 and not loops or check and loops and t % 40 == 0:
                cod = []
                for n, (p, r) in w.items():
                    par = names.get(n)
                    if par and par in w and n.startswith("j_") and not any(
                            s in n for s in ("bulge", "twist", "raise", "_1", "_2", "_3", "head_end", "helmet")):
                        cod.append((scale(to_pd(p), k), scale(to_pd(w[par][0]), k)))
                pos = posed_pd(R, pelvis)
                pd = []
                for j in PD:
                    if PD[j][0]:
                        col = (60, 230, 90) if j % 2 or j == 2 else (240, 170, 60)
                        pd.append((pos[j], pos[PD[j][0]], col))
                # each foot's and hand's rest +z, to show its turn
                for j in (7, 8, 13, 14):
                    pd.append((pos[j], add(pos[j], apply(R[j], [0, 0, 120])), (240, 80, 80)))
                panels.append((cod, pd))

        print("%s: %d frames%s" % (CLIPS[c], len(ws), ", loops, %.1f inches a loop" % travel if loops else ""))

    open(out, "wb").write(data)
    print("%s: %d bytes, legs %.1f units, %.2f units to the inch" % (out, len(data), legs, k))

    if check:
        draw_sheet(check, panels)
        print("wrote", check)


if __name__ == "__main__":
    main()
