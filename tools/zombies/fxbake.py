#!/usr/bin/env python3
"""
Black Ops II's effect pictures for Zombies' own guns' shots (zombies.inc,
zombiesRenderRayFx()), as the game's intensity textures: each picture's
light (its alpha times its brightness) a byte a pixel, top row first. The art
stays on the player's disk, out of the tree.

    python3 tools/zombies/fxbake.py <images dir> <out dir>

From zm_buried's images/: fxt_fx_raygun_ring.dds (the ring a Ray Gun bolt
leaves where it lands, 64 square) to raygun_ring.i8.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from imgio import image_read  # noqa: E402

PICTURES = (("fxt_fx_raygun_ring.dds", "raygun_ring.i8", 64),)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    src, dst = sys.argv[1], sys.argv[2]

    for name, out, size in PICTURES:
        w, h, rgba = image_read(os.path.join(src, name))

        if (w, h) != (size, size):
            raise SystemExit("%s: %dx%d, %d square wanted" % (name, w, h, size))

        data = bytearray(w * h)

        for i in range(w * h):
            r, g, b, a = rgba[i * 4:i * 4 + 4]
            data[i] = (max(r, g, b) * a) // 255

        with open(os.path.join(dst, out), "wb") as f:
            f.write(data)

        print("%s: %dx%d" % (out, w, h))


if __name__ == "__main__":
    main()
