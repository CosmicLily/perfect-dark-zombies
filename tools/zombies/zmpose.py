#!/usr/bin/env python3
"""
A Black Ops II zombie (a body and a head, vmconv.py's .zvm files) posed by an
animation (.zan) and drawn to a PNG, a row of frames side by side, to check a
character before it is in the game:

    python3 tools/zombies/zmpose.py <dir> <body> <head> <anim|-> <out.png> <frame> [...]

e.g. build-dabs/zombies/characters zombie1_body01 zombie_head_a
ai_zombie_walk_v1 walk.png 0 30 60 90. An anim of "-" draws the rest pose.

The head is hung on the body as Call of Duty attaches one: each of its bones
that the body has too (j_head, j_neck and the like) takes the body's, the rest
follow their parents. The animation's root motion is not taken, so a walk
walks on the spot. Seen from the front, Call of Duty's x (the way the
character faces) towards the camera, z up.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from imgio import png_read, png_write  # noqa: E402
from vmpose import load_zvm, load_zan, sample, trs, mul  # noqa: E402

IDENTITY = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]

# the bone whose animated translation is an offset from its rest (the
# others' are where the bone is): a zombie's hips
ROOT_OFFSET_BONE = "j_mainroot"


def pose(models, tracks, frame):
    """each model's skinning matrices, every model one skeleton by bone name"""
    world = {}
    out = []

    for bones, _, _, _ in models:
        mats = []

        for name, parent, t, q, ib in bones:
            if name in world:
                w = world[name]
            else:
                rot, trans = tracks.get(name, ([], []))
                lt = sample(trans, frame, False)

                # the hips' place is kept from their rest, as an offset: the
                # walk's j_mainroot is near 0, the rest 37 inches up
                if lt and name == ROOT_OFFSET_BONE:
                    lt = [a + b for a, b in zip(lt, t)]

                local = trs(lt or t, sample(rot, frame, True) or q)
                pw = world[bones[parent][0]] if parent >= 0 else IDENTITY
                w = mul(pw, local)
                world[name] = w

            mats.append(mul(w, ib))

        out.append(mats)

    return out


def draw(img, zbuf, W, H, x0, cw, models, skins, d, cam):
    """the posed models into img's column x0..x0+cw, seen from cam (x, z)"""
    f = H * 0.9
    light = (0.7, 0.3, 0.6)

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

            # the camera out along +x looking back: the screen's x is y, its y -z
            depth = cam[0] - acc[0]
            sx = x0 + cw / 2 + f * acc[1] / depth
            sy = H / 2 - f * (acc[2] - cam[1]) / depth
            ln = math.sqrt(sum(v * v for v in nacc)) or 1
            shade = 0.4 + 0.6 * max(0.0, (nacc[0] * light[0] + nacc[1] * light[1] + nacc[2] * light[2]) / ln)
            posed.append((sx, sy, depth, shade, uv))

        for first, count, flags, pic in draws:
            tex = pics.get(pic)

            for t in tris[first:first + count]:
                a, b, c = (posed[i] for i in t)
                minx = max(x0, int(min(a[0], b[0], c[0])))
                maxx = min(x0 + cw - 1, int(max(a[0], b[0], c[0])) + 1)
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
                        img[o * 4:o * 4 + 3] = bytes(min(255, int(ch * s)) for ch in col)


def main():
    if len(sys.argv) < 7:
        raise SystemExit(__doc__)

    d, body, head, animname, outpng = sys.argv[1:6]
    frames = [float(v) for v in sys.argv[6:]]
    models = [load_zvm(os.path.join(d, body + ".zvm")), load_zvm(os.path.join(d, head + ".zvm"))]
    tracks = {} if animname == "-" else load_zan(os.path.join(d, animname + ".zan"))[2]

    cw, H = 320, 480
    W = cw * len(frames)
    img = bytearray(b"\x30\x30\x38\xff" * (W * H))
    zbuf = [1e30] * (W * H)

    for k, frame in enumerate(frames):
        draw(img, zbuf, W, H, k * cw, cw, models, pose(models, tracks, frame), d, (120.0, 2.0))

    png_write(outpng, W, H, img)
    print("%s: %d frames" % (outpng, len(frames)))


if __name__ == "__main__":
    main()
