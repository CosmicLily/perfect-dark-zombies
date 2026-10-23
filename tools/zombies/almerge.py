#!/usr/bin/env python3
"""
A Mob of the Dead perk machine's picture in its colours (the p6_zm_al_vending_
machines: PhD Flopper's "nuke", Deadshot's "ads" and its logo). Their colour
map (_c) is gray all over; what is coloured on them - the perk's emblem, the
trim, the buttons, the sign - is in a second picture (_m), black wherever the
gray shows. Both stream from the map pack's .ipak (dlczm2.ipak, Mob of the
Dead's: dumped as Buried's are, see CLAUDE-notes/zombies-cod-anims.md), not
from zm_prison.ff. The mask's colour is laid over the gray as far as it is
bright; it has no alpha of its own.

    python3 tools/zombies/almerge.py <_c.dds|png> <_m.dds|png> <out.png>
"""

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from imgio import image_read, png_write  # noqa: E402

LOW = 16    # a mask this dark (its brightest channel) leaves the gray
HIGH = 64   # and this bright is all colour


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)

    cw, ch, c = image_read(sys.argv[1])
    mw, mh, m = image_read(sys.argv[2])

    if (cw, ch) != (mw, mh):
        sys.exit("the colour map is %dx%d, the mask %dx%d" % (cw, ch, mw, mh))

    out = bytearray(c)

    for i in range(cw * ch):
        o = i * 4
        bright = max(m[o], m[o + 1], m[o + 2])
        w = min(1.0, max(0.0, (bright - LOW) / float(HIGH - LOW)))

        for k in range(3):
            out[o + k] = int(round(c[o + k] * (1 - w) + m[o + k] * w))

    png_write(sys.argv[3], cw, ch, bytes(out))
    print("%s: %dx%d" % (sys.argv[3], cw, ch))


if __name__ == "__main__":
    main()
