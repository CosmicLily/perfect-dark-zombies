#!/usr/bin/env python3
"""
Perk icons cut from a sheet of them on a plain background (Black Ops II's "HD
Perks Shaders" sheet), into the build's zombies/perks/<name>.png that
port/src/zombieperks.c draws (70 square, the badge on a transparent field).
The art stays on the player's disk, out of the tree.

    python3 tools/zombies/perkicons.py <sheet.png> <outdir> <row> <col>=<name> ...
    python3 tools/zombies/perkicons.py --iwi <outdir> <file.iwi>=<name> ...

The second takes the game's own icon textures, which are better (from_iwi()).

<row> is 0 for the sheet's first row of badges, 1 for the next; <col> counts
the badges along it from 0. E.g. the second row's Deadshot, PhD and Electric
Cherry:

    perkicons.py sheet.png build/zombies/perks 1 0=deadshot 1=phdflopper 4=electriccherry

The background is the colour at the sheet's corner. Each badge is found as a
run of columns and rows standing out from it, cut square about its middle,
scaled to 70, and the background is taken out from the edges inwards (a fill,
so the badge's own dark pixels stay).
"""

import sys
from collections import deque

sys.path.insert(0, __import__("os").path.dirname(__file__))
from imgio import png_read, png_write  # noqa: E402

SIZE = 70


def runs(flags, mingap=4, minlen=20):
    out = []
    start = None
    gap = 0

    for i, f in enumerate(flags + [False] * (mingap + 1)):
        if f:
            if start is None:
                start = i
            gap = 0
            end = i
        elif start is not None:
            gap += 1
            if gap > mingap:
                if end - start + 1 >= minlen:
                    out.append((start, end))
                start = None

    return out


def from_iwi(outdir, picks):
    """
    The game's own icons instead (perkicons.py --iwi <outdir> <file.iwi>=<name>
    ...): Black Ops II's HUD textures, specialty_*_zombies.iwi and Electric
    Cherry's minimap_icon_electric_cherry.iwi, 64 square DXT5 with their own
    alpha, written as they are - sharper than a sheet's copies, which were a
    compressed picture of them on a background to take out.
    """
    from imgio import iwi_read

    for pick in picks:
        src, name = pick.rsplit("=", 1)
        w, h, rgba = iwi_read(src)
        path = "%s/%s.png" % (outdir, name)
        png_write(path, w, h, rgba)
        print("wrote", path, "(%dx%d from %s)" % (w, h, src))


def main():
    if sys.argv[1] == "--iwi":
        from_iwi(sys.argv[2], sys.argv[3:])
        return

    sheet, outdir, row = sys.argv[1], sys.argv[2], int(sys.argv[3])
    picks = dict((int(a.split("=")[0]), a.split("=")[1]) for a in sys.argv[4:])
    w, h, p = png_read(sheet)
    bg = p[0:3]

    def far(x, y, tol=28):
        o = (y * w + x) * 4
        return max(abs(p[o + c] - bg[c]) for c in range(3)) > tol

    # the rows of badges: lines with enough standing out, past the title
    rowflags = [sum(1 for x in range(0, w, 2) if far(x, y)) > 20 for y in range(h)]
    bands = runs(rowflags, mingap=6, minlen=40)
    # the title's lines are wider than tall; badges are near square
    y0, y1 = bands[-2 + row] if len(bands) >= 2 else bands[row]

    colflags = [any(far(x, y) for y in range(y0, y1 + 1)) for x in range(w)]
    cols = runs(colflags, mingap=6, minlen=30)
    print("row %d at y %d-%d, badges at %s" % (row, y0, y1, cols))

    for col, name in picks.items():
        x0, x1 = cols[col]
        # the badge edge to edge, as the first six were cut
        side = max(x1 - x0, y1 - y0) + 1
        cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
        out = bytearray(SIZE * SIZE * 4)

        # a box filter down to SIZE
        for oy in range(SIZE):
            for ox in range(SIZE):
                sx0 = cx - side / 2 + ox * side / SIZE
                sy0 = cy - side / 2 + oy * side / SIZE
                acc = [0, 0, 0]
                n = 0
                for sy in range(int(sy0), int(sy0 + side / SIZE) + 1):
                    for sx in range(int(sx0), int(sx0 + side / SIZE) + 1):
                        if 0 <= sx < w and 0 <= sy < h:
                            o = (sy * w + sx) * 4
                            for c in range(3):
                                acc[c] += p[o + c]
                            n += 1
                o = (oy * SIZE + ox) * 4
                for c in range(3):
                    out[o + c] = acc[c] // max(n, 1)
                out[o + 3] = 255

        # the background out, from the edges in
        def isbg(x, y, tol=30):
            o = (y * SIZE + x) * 4
            return max(abs(out[o + c] - bg[c]) for c in range(3)) <= tol

        seen = set()
        q = deque((x, y) for x in range(SIZE) for y in (0, SIZE - 1))
        q.extend((x, y) for y in range(SIZE) for x in (0, SIZE - 1))

        while q:
            x, y = q.popleft()
            if (x, y) in seen or not (0 <= x < SIZE and 0 <= y < SIZE) or not isbg(x, y):
                continue
            seen.add((x, y))
            out[(y * SIZE + x) * 4 + 3] = 0
            q.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))

        path = "%s/%s.png" % (outdir, name)
        png_write(path, SIZE, SIZE, out)
        print("wrote", path, "(%d cleared)" % len(seen))


if __name__ == "__main__":
    main()
