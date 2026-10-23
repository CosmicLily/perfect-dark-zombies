#!/usr/bin/env python3
"""
Black Ops II's effect pictures for Zombies (zombies.inc, zombiesRenderRayFx()),
as the game's intensity textures: each picture's light (its alpha times its
brightness) a byte a pixel, top row first. The art stays on the player's
disk, out of the tree.

    python3 tools/zombies/fxbake.py <images dir> <out dir>
    python3 tools/zombies/fxbake.py <images dir> <out dir> cherry

From zm_buried's images/: fxt_fx_raygun_ring.dds (the ring a Ray Gun bolt
leaves where it lands, 64 square) to raygun_ring.i8.

cherry, from zm_prison's images/ (Mob of the Dead's, dumped with its ipak:
CLAUDE-notes/zombies-cod-anims.md), Electric Cherry's burst, each made small
enough for one load of the N64's texture memory (4 KB):
    fxt_fx_emp_ring_wave.dds (256 square) to cherry_ring.i8, 64 square
    fxt_light_flare_cool.dds (256 square) to cherry_flare.i8, 64 square
    fxt_env_lighting_bolt_ground.dds (512 square, eight bolts four across)
        to cherry_bolts.i8, the eight one after another, each 32 by 64
"""

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from imgio import image_read  # noqa: E402

PICTURES = (("fxt_fx_raygun_ring.dds", "raygun_ring.i8", 64),)


def light(rgba, w, h):
    """Each pixel's light, as an intensity texture's"""
    out = [0] * (w * h)

    for i in range(w * h):
        r, g, b, a = rgba[i * 4:i * 4 + 4]
        out[i] = (max(r, g, b) * a) // 255

    return out


def shrink(src, sw, x0, y0, w, h, ow, oh):
    """Box-filtered: the w by h part of src at x0, y0, made ow by oh"""
    out = bytearray(ow * oh)
    fx = w // ow
    fy = h // oh

    for y in range(oh):
        for x in range(ow):
            total = 0

            for v in range(fy):
                row = (y0 + y * fy + v) * sw + x0 + x * fx
                total += sum(src[row:row + fx])

            out[y * ow + x] = total // (fx * fy)

    return out


def cherry(src, dst):
    for name, out, size in (("fxt_fx_emp_ring_wave.dds", "cherry_ring.i8", 64),
                            ("fxt_light_flare_cool.dds", "cherry_flare.i8", 64)):
        w, h, rgba = image_read(os.path.join(src, name))
        data = shrink(light(rgba, w, h), w, 0, 0, w, h, size, size)

        with open(os.path.join(dst, out), "wb") as f:
            f.write(data)

        print("%s: %dx%d from %dx%d" % (out, size, size, w, h))

    w, h, rgba = image_read(os.path.join(src, "fxt_env_lighting_bolt_ground.dds"))
    lit = light(rgba, w, h)
    fw = w // 4
    fh = h // 2
    data = bytearray()

    for k in range(8):
        data += shrink(lit, w, (k % 4) * fw, (k // 4) * fh, fw, fh, 32, 64)

    with open(os.path.join(dst, "cherry_bolts.i8"), "wb") as f:
        f.write(data)

    print("cherry_bolts.i8: 8 of 32x64 from %dx%d" % (w, h))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    src, dst = sys.argv[1], sys.argv[2]

    if len(sys.argv) > 3 and sys.argv[3] == "cherry":
        cherry(src, dst)
        return

    for name, out, size in PICTURES:
        w, h, rgba = image_read(os.path.join(src, name))

        if (w, h) != (size, size):
            raise SystemExit("%s: %dx%d, %d square wanted" % (name, w, h, size))

        data = bytearray(light(rgba, w, h))

        with open(os.path.join(dst, out), "wb") as f:
            f.write(data)

        print("%s: %dx%d" % (out, w, h))


if __name__ == "__main__":
    main()
