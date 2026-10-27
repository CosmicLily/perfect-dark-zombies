#!/usr/bin/env python3
"""
A Black Ops II map's textures for port/src/codmap.c: each material of C2M's
export that has a colour map in an OpenAssetTools dump gets a texture number
of the arena's mod, a placeholder for it in Perfect Dark's own texture format
(textures/<num>.bin: CODMAP_TEX square, RGBA16, the picture's mean colour) and
the picture itself beside it as the mod's own texture pack
(textures/<num>.png, which the game draws in the placeholder's place at its
full size - CLAUDE-notes/texture-packs.md, "The mod gets an index of its own").
codmap.c reads textures/codmap.txt, a line a material: name, number.

    python3 tools/zombies/codmaptex.py <C2M's map folder> <the dump's t6/> <the mod's folder> <zone>...
    python3 tools/zombies/codmaptex.py --props <Greyhound's xmodels/> <x0> <x1> <y0> <y1> <C2M's map folder> <the mod's folder>

The zones are the dump's folders looked in for the pictures, in order
(zm_transit so_zclassic_zm_transit common_zm for Tranzit). The second form
adds, after the map's own, the materials of the props C2M's _xmodels.json
places inside the box (the map's inches), from Greyhound's export of them: each
model's least detailed OBJ (the one codmap.c places), its .mtl's map_Kd
picture, already a PNG. The art stays on the player's disk, out of the tree.
"""

import json
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from imgio import image_read, png_write  # noqa: E402

FIRST_NUM = 0xe00  # past the ROM's 3503 (and Japan's 3511), under the 12 bits a room's command has
CODMAP_TEX = 32    # the placeholder's side: a room's texture coordinates are in its texels
MAX_SIDE = 1024    # a bigger picture is halved till it fits


def rzip(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    body = c.compress(data) + c.flush()
    n = len(data)
    return bytes([0x11, 0x73, (n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff]) + body


def placeholder(rgb):
    """Perfect Dark's zlib texture record: header byte (zlib, one lod), then
    format, palette count and entries, width, height, and the texels. The zlib
    kind is paletted only (texAlignIndices() steps by a palette index), so the
    picture is RGBA16_CI8 with one colour, every index 0."""
    r, g, b = rgb
    colour = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1
    head = bytes([0x41, 0x09, 0x00, colour >> 8, colour & 0xff, CODMAP_TEX, CODMAP_TEX])
    return head + rzip(bytes(CODMAP_TEX * CODMAP_TEX))


def halve(w, h, rgba):
    nw, nh = max(1, w // 2), max(1, h // 2)
    out = bytearray(nw * nh * 4)

    for y in range(nh):
        for x in range(nw):
            for c in range(4):
                s = 0
                for dy in (0, 1):
                    for dx in (0, 1):
                        s += rgba[((2 * y + dy) * w + 2 * x + dx) * 4 + c]
                out[(y * nw + x) * 4 + c] = s // 4

    return nw, nh, out


def cutout(rgba):
    """1 when enough of the picture is see-through to be drawn cut out (a
    fence's mesh, a branch's twigs): codmap.c draws it with an alpha test"""
    n = len(rgba) // 4
    step = max(1, n // 8192)
    clear = sum(1 for i in range(0, n, step) if rgba[i * 4 + 3] < 128)
    return 1 if clear * step > n * 0.02 else 0


def mean(rgba):
    n = len(rgba) // 4
    step = max(1, n // 4096)
    tot = [0, 0, 0]
    k = 0

    for i in range(0, n, step):
        for c in range(3):
            tot[c] += rgba[i * 4 + c]
        k += 1

    return tuple(t // max(1, k) for t in tot)


def prop_obj(models, name):
    """A model's least detailed OBJ in Greyhound's export, None for none"""
    best = None
    for lod in range(8):
        p = os.path.join(models, name, "%s_LOD%d.obj" % (name, lod))
        if os.path.isfile(p):
            best = p
    return best


def props(models, box, mapdir, moddir):
    stem = os.path.basename(os.path.normpath(mapdir))
    outdir = os.path.join(moddir, "textures")
    table = os.path.join(outdir, "codmap.txt")
    lines = [l for l in open(table).read().split("\n") if l.strip()] if os.path.isfile(table) else []
    have = {l.split()[0] for l in lines}
    num = max([int(l.split()[1], 16) for l in lines] + [FIRST_NUM - 1]) + 1
    place = json.load(open(os.path.join(mapdir, stem + "_xmodels.json")))
    names = sorted({e["Name"] for e in place if "Name" in e
                    and box[0] <= float(e["PosX"]) <= box[1] and box[2] <= float(e["PosY"]) <= box[3]})
    added = 0

    for name in names:
        obj = prop_obj(models, name)
        if not obj:
            continue
        folder = os.path.dirname(obj)
        for mtl in [f for f in os.listdir(folder) if f.endswith(".mtl")]:
            cur = None
            for line in open(os.path.join(folder, mtl), errors="replace"):
                bits = line.strip().split(None, 1)
                if not bits:
                    continue
                if bits[0] == "newmtl":
                    cur = bits[1]
                elif bits[0] == "map_Kd" and cur and cur not in have and len(bits) > 1:
                    rel = bits[1].replace("\\\\", "/").replace("\\", "/")
                    src = os.path.normpath(os.path.join(folder, rel))
                    if not os.path.isfile(src):
                        continue
                    try:
                        w, h, rgba = image_read(src)
                    except Exception as e:
                        print("%s: %s" % (src, e))
                        continue
                    while max(w, h) > MAX_SIDE:
                        w, h, rgba = halve(w, h, rgba)
                    png_write(os.path.join(outdir, "%04x.png" % num), w, h, rgba)
                    open(os.path.join(outdir, "%04x.bin" % num), "wb").write(placeholder(mean(rgba)))
                    lines.append("%s %04x %d" % (cur, num, cutout(rgba)))
                    have.add(cur)
                    print("%04x %s %dx%d" % (num, cur, w, h), flush=True)
                    num += 1
                    added += 1
                    if num > 0xfff:
                        print("out of texture numbers")
                        open(table, "w").write("\n".join(lines) + "\n")
                        return

    open(table, "w").write("\n".join(lines) + "\n")
    print("%d props' materials added, %d in all" % (added, len(lines)))


# Tranzit's lava: the cooled crust's picture with the ember mask's red glow
# turned to an orange-yellow heat over it, how much of each by the material
LAVA = {"rock_lava_molten": (0.3, 1.3), "rock_lava_cooling": (0.8, 0.45)}


def find_dds(dump, zones, name):
    for z in zones:
        p = os.path.join(dump, z, "images", name + ".dds")
        if os.path.isfile(p):
            return p
    return None


def lava(dump, zones, mix):
    crust_p = find_dds(dump, zones, "~-grock_lava_cooling_c") or find_dds(dump, zones, "rock_lava_cooling_c")
    ember_p = find_dds(dump, zones, "rock_lava_ember")
    if not crust_p or not ember_p:
        return None
    w, h, crust = image_read(crust_p)
    ew, eh, ember = image_read(ember_p)
    out = bytearray(w * h * 4)
    crustk, glowk = mix
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 4
            e = ember[((y % eh) * ew + (x % ew)) * 4] / 255.0
            g = (e ** 0.8) * glowk
            out[i] = min(255, int(crust[i] * crustk + 255 * g))
            out[i + 1] = min(255, int(crust[i + 1] * crustk + 130 * g * g))
            out[i + 2] = min(255, int(crust[i + 2] * crustk + 25 * g * g))
            out[i + 3] = 255
    return w, h, out


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--props":
        if len(sys.argv) < 9:
            print(__doc__)
            sys.exit(1)
        props(sys.argv[2], [float(v) for v in sys.argv[3:7]], sys.argv[7], sys.argv[8])
        return

    if len(sys.argv) < 5:
        print(__doc__)
        sys.exit(1)

    mapdir, dump, moddir = sys.argv[1], sys.argv[2], sys.argv[3]
    zones = sys.argv[4:]
    stem = os.path.basename(os.path.normpath(mapdir))
    mats = json.load(open(os.path.join(mapdir, stem + "_matdata.json")))["Materials"]
    outdir = os.path.join(moddir, "textures")
    os.makedirs(outdir, exist_ok=True)

    lines = []
    num = FIRST_NUM
    missing = []

    for name in sorted(mats):
        colour = mats[name].get("Color Map")

        # C2M leaves Tranzit's two lava materials empty: their pictures are
        # put together from the dump's (lava())
        if not colour and name in LAVA:
            got = lava(dump, zones, LAVA[name])
            if got:
                w, h, rgba = got
                png_write(os.path.join(outdir, "%04x.png" % num), w, h, rgba)
                open(os.path.join(outdir, "%04x.bin" % num), "wb").write(placeholder(mean(rgba)))
                lines.append("%s %04x %d" % (name, num, cutout(rgba)))
                print("%04x %s %dx%d (lava)" % (num, name, w, h), flush=True)
                num += 1
            continue

        if not colour:
            continue

        src = None
        for z in zones:
            p = os.path.join(dump, z, "images", colour + ".dds")
            if os.path.isfile(p):
                src = p
                break

        if not src:
            missing.append(colour)
            continue

        try:
            w, h, rgba = image_read(src)
        except Exception as e:  # a format imgio does not read
            missing.append("%s (%s)" % (colour, e))
            continue

        while max(w, h) > MAX_SIDE:
            w, h, rgba = halve(w, h, rgba)

        png_write(os.path.join(outdir, "%04x.png" % num), w, h, rgba)
        open(os.path.join(outdir, "%04x.bin" % num), "wb").write(placeholder(mean(rgba)))
        lines.append("%s %04x %d" % (name, num, cutout(rgba)))
        print("%04x %s %dx%d" % (num, name, w, h), flush=True)
        num += 1

        if num > 0xfff:
            print("out of texture numbers")
            break

    open(os.path.join(outdir, "codmap.txt"), "w").write("\n".join(lines) + "\n")
    print("%d materials textured, %d pictures missing: %s" % (len(lines), len(missing), ", ".join(missing[:10])))


if __name__ == "__main__":
    main()
