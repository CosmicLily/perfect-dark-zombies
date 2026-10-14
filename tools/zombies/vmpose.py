#!/usr/bin/env python3
"""
The viewmodel posed as the game poses it (zombies.inc), drawn to a PNG from
the camera, to check vmconv.py's files before they are in the game:

    python3 tools/zombies/vmpose.py <dir> <bottle> <anim> <frame> <out.png>

e.g. build-dabs/zombies/viewmodel bottle_juggernog drink 30 drink30.png. The
arms (arms.zvm) and the bottle are one skeleton, the bottle's root (j_gun)
hung from the arms' tag_weapon as Call of Duty attaches a weapon; each bone
takes its animation track where it has one and its rest where it does not.
"""

import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from imgio import png_read, png_write  # noqa: E402


def load_zvm(path):
    d = open(path, "rb").read()
    nb, nv, nt, nd = struct.unpack_from("<4I", d, 4)
    at = 20
    bones = []
    for _ in range(nb):
        name = d[at:at + 32].split(b"\0")[0].decode()
        vals = struct.unpack_from("<i3f4f12f", d, at + 32)
        bones.append((name, vals[0], vals[1:4], vals[4:8], [list(vals[8 + r * 4:12 + r * 4]) for r in range(3)]))
        at += 32 + 4 + 12 + 16 + 48
    verts = []
    for _ in range(nv):
        v = struct.unpack_from("<3f3f2f4B4f", d, at)
        verts.append((v[0:3], v[3:6], v[6:8], v[8:12], v[12:16]))
        at += struct.calcsize("<3f3f2f4B4f")
    tris = [struct.unpack_from("<3H", d, at + i * 6) for i in range(nt)]
    at += nt * 6
    draws = []
    for _ in range(nd):
        first, count, flags = struct.unpack_from("<3I", d, at)
        pic = d[at + 12:at + 64].split(b"\0")[0].decode()
        draws.append((first, count, flags, pic))
        at += 64
    return bones, verts, tris, draws


def load_zan(path):
    d = open(path, "rb").read()
    frames, fps, ntracks, nnotes = struct.unpack_from("<4H", d, 4)
    at = 12
    tracks = {}
    for _ in range(ntracks):
        name = d[at:at + 32].split(b"\0")[0].decode()
        nr, nt = struct.unpack_from("<2H", d, at + 32)
        at += 36
        rot = []
        for _ in range(nr):
            v = struct.unpack_from("<H4f", d, at)
            rot.append((v[0], v[1:5]))
            at += 18
        trans = []
        for _ in range(nt):
            v = struct.unpack_from("<H3f", d, at)
            trans.append((v[0], v[1:4]))
            at += 14
        tracks[name] = (rot, trans)
    return frames, fps, tracks


def sample(keys, frame, nlerp):
    if not keys:
        return None
    if frame <= keys[0][0] or len(keys) == 1:
        return keys[0][1]
    for (f0, a), (f1, b) in zip(keys, keys[1:]):
        if f0 <= frame <= f1:
            t = (frame - f0) / float(f1 - f0) if f1 > f0 else 0.0
            if nlerp:
                if sum(a[k] * b[k] for k in range(4)) < 0:
                    b = tuple(-v for v in b)
                q = [a[k] + (b[k] - a[k]) * t for k in range(4)]
                n = math.sqrt(sum(v * v for v in q)) or 1
                return tuple(v / n for v in q)
            return tuple(a[k] + (b[k] - a[k]) * t for k in range(3))
    return keys[-1][1]


def trs(t, q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), t[0]],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), t[1]],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), t[2]]]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) + (a[i][3] if j == 3 else 0) for j in range(4)] for i in range(3)]


def pose(models, tracks, frame):
    """every model's skinning matrices, the models one skeleton by bone name"""
    world = {}
    out = []
    for bones, _, _, _ in models:
        mats = []
        for name, parent, t, q, ib in bones:
            rot, trans = tracks.get(name, ([], []))
            lt = sample(trans, frame, False) or t
            lq = sample(rot, frame, True) or q
            local = trs(lt, lq)
            if parent >= 0:
                pw = world[bones[parent][0]]
            elif name == "j_gun":
                pw = world["tag_weapon"]
            else:
                pw = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]
            world[name] = mul(pw, local)
            mats.append(mul(world[name], ib))
        out.append(mats)
    return out


def main():
    d, bottle, animname, frame, outpng = sys.argv[1:6]
    frame = float(frame)
    models = [load_zvm(os.path.join(d, "arms.zvm")), load_zvm(os.path.join(d, bottle + ".zvm"))]
    _, _, tracks = load_zan(os.path.join(d, animname + ".zan"))
    skins = pose(models, tracks, frame)

    W, H = 480, 360
    fov = math.radians(65)
    f = (W / 2) / math.tan(fov / 2)
    zbuf = [1e30] * (W * H)
    img = bytearray(b"\x30\x30\x38\xff" * (W * H))
    light = (0.4, 0.3, 0.85)

    for (bones, verts, tris, draws), mats in zip(models, skins):
        pics = {}
        for _, _, _, pic in draws:
            if pic and pic not in pics:
                pics[pic] = png_read(os.path.join(d, pic))
        posed = []
        for p, n, uv, js, ws in verts:
            acc = [0.0, 0.0, 0.0]
            nacc = [0.0, 0.0, 0.0]
            for j, w in zip(js, ws):
                if w <= 0:
                    continue
                m = mats[j]
                for r in range(3):
                    acc[r] += w * (m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3])
                    nacc[r] += w * (m[r][0] * n[0] + m[r][1] * n[1] + m[r][2] * n[2])
            # x forward, y left, z up: the screen's x is -y, its y -z
            if acc[0] > 0.5:
                sx = W / 2 - f * acc[1] / acc[0]
                sy = H / 2 - f * acc[2] / acc[0]
            else:
                sx = sy = None
            ln = math.sqrt(sum(v * v for v in nacc)) or 1
            shade = 0.45 + 0.55 * max(0.0, -(nacc[0] * light[0] - nacc[1] * light[1] + nacc[2] * light[2]) / ln)
            posed.append((sx, sy, acc[0], shade, uv))

        for first, count, flags, pic in draws:
            tex = pics.get(pic)
            for t in tris[first:first + count]:
                a, b, c = (posed[i] for i in t)
                if a[0] is None or b[0] is None or c[0] is None:
                    continue
                minx = max(0, int(min(a[0], b[0], c[0])))
                maxx = min(W - 1, int(max(a[0], b[0], c[0])) + 1)
                miny = max(0, int(min(a[1], b[1], c[1])))
                maxy = min(H - 1, int(max(a[1], b[1], c[1])) + 1)
                den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
                if abs(den) < 1e-9 or maxx < minx or maxy < miny:
                    continue
                for y in range(miny, maxy + 1):
                    for x in range(minx, maxx + 1):
                        l0 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / den
                        l1 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / den
                        l2 = 1 - l0 - l1
                        if l0 < 0 or l1 < 0 or l2 < 0:
                            continue
                        z = l0 * a[2] + l1 * b[2] + l2 * c[2]
                        o = y * W + x
                        if z >= zbuf[o]:
                            continue
                        u = l0 * a[4][0] + l1 * b[4][0] + l2 * c[4][0]
                        v = l0 * a[4][1] + l1 * b[4][1] + l2 * c[4][1]
                        s = l0 * a[3] + l1 * b[3] + l2 * c[3]
                        if tex:
                            tw, th, tp = tex
                            tx = int((u % 1.0) * tw) % tw
                            ty = int((1.0 - (v % 1.0)) * th) % th
                            q = (ty * tw + tx) * 4
                            if flags & 1 and tp[q + 3] < 100:
                                continue
                            col = tp[q:q + 3]
                        else:
                            col = (200, 200, 200)
                        zbuf[o] = z
                        img[o * 4:o * 4 + 3] = bytes(min(255, int(cc * s)) for cc in col)

    png_write(outpng, W, H, img)
    print("wrote", outpng)


if __name__ == "__main__":
    main()
