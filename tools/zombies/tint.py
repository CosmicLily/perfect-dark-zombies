#!/usr/bin/env python3
"""
A gray picture coloured: Mob of the Dead paints its perk machines in gray (its
"p6_zm_al_" set, whose shader adds the rest), and a machine of it brought into
Zombies would otherwise stand there colourless.

    python3 tools/zombies/tint.py <in.png> <out.png> <r> <g> <b> [strength]

r g b is the colour as 0-255; strength (0 to 1, 0.85 by default) how much of
it. The light of each pixel is kept: dark stays dark, and the brightest
pixels (lettering, highlights) keep some of their white, so a sign still
reads. The input is left as it is.
"""

import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from imgio import image_read, png_write  # noqa: E402


def main():
    if len(sys.argv) < 6:
        raise SystemExit(__doc__)

    src, dst = sys.argv[1], sys.argv[2]
    tint = [int(v) / 255.0 for v in sys.argv[3:6]]
    strength = float(sys.argv[6]) if len(sys.argv) > 6 else 0.85
    width, height, rgba = image_read(src)

    # brightened so the tint's average light is the gray's
    boost = 1.0 / max(0.05, 0.299 * tint[0] + 0.587 * tint[1] + 0.114 * tint[2])

    for i in range(0, len(rgba), 4):
        lum = (0.299 * rgba[i] + 0.587 * rgba[i + 1] + 0.114 * rgba[i + 2]) / 255.0
        # the brightest keep more of their white
        keep = strength * (1.0 - lum ** 6)

        for k in range(3):
            coloured = min(1.0, lum * tint[k] * boost)
            rgba[i + k] = int(255 * ((1 - keep) * lum + keep * coloured) + 0.5)

    png_write(dst, width, height, rgba)
    print("%s: %dx%d tinted %s" % (dst, width, height, sys.argv[3:6]))


if __name__ == "__main__":
    main()
