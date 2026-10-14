"""
Pictures for tools/zombies: PNG in and out, and the DXT1/DXT5 DDS files a
fastfile dump (OpenAssetTools' Unlinker) writes, in plain Python with no
packages beyond the standard library.

An image here is (width, height, rgba), rgba a bytearray of width * height * 4,
the top row first.
"""

import struct
import zlib


def png_write(path, width, height, rgba):
    rows = bytearray()
    stride = width * 4

    for y in range(height):
        rows.append(0)
        rows += rgba[y * stride:(y + 1) * stride]

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(rows), 6)))
        f.write(chunk(b"IEND", b""))


def png_read(path):
    data = open(path, "rb").read()

    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)

    at = 8
    idat = bytearray()
    width = height = depth = ctype = None

    while at < len(data):
        n, kind = struct.unpack_from(">I4s", data, at)
        body = data[at + 8:at + 8 + n]
        at += 12 + n

        if kind == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (0, 2, 4, 6) or interlace:
                raise ValueError("%s: an 8-bit, non-interlaced PNG is wanted" % path)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break

    bpp = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(bytes(idat))
    stride = width * bpp
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    pos = 0

    for y in range(height):
        kind = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride

        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0

            if kind == 1:
                line[i] = (line[i] + a) & 0xff
            elif kind == 2:
                line[i] = (line[i] + b) & 0xff
            elif kind == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xff
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xff

        for x in range(width):
            px = line[x * bpp:(x + 1) * bpp]
            o = (y * width + x) * 4

            if ctype == 0:
                out[o:o + 4] = bytes((px[0], px[0], px[0], 255))
            elif ctype == 2:
                out[o:o + 4] = bytes((px[0], px[1], px[2], 255))
            elif ctype == 4:
                out[o:o + 4] = bytes((px[0], px[0], px[0], px[1]))
            else:
                out[o:o + 4] = px

        prev = line

    return width, height, out


def _rgb565(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _colour_block(block, alpha_ok):
    c0, c1, bits = struct.unpack_from("<HHI", block, 0)
    a = _rgb565(c0)
    b = _rgb565(c1)

    if c0 > c1 or not alpha_ok:
        pal = [a + (255,), b + (255,),
               tuple((2 * a[k] + b[k]) // 3 for k in range(3)) + (255,),
               tuple((a[k] + 2 * b[k]) // 3 for k in range(3)) + (255,)]
    else:
        pal = [a + (255,), b + (255,),
               tuple((a[k] + b[k]) // 2 for k in range(3)) + (255,),
               (0, 0, 0, 0)]

    return [pal[(bits >> (2 * i)) & 3] for i in range(16)]


def _alpha_block(block):
    a0, a1 = block[0], block[1]
    bits = int.from_bytes(block[2:8], "little")

    if a0 > a1:
        pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]

    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]


def dds_read(path):
    """A DXT1 or DXT5 DDS's top mip level"""
    data = open(path, "rb").read()

    if data[:4] != b"DDS ":
        raise ValueError("%s: not a DDS" % path)

    height, width = struct.unpack_from("<II", data, 12)
    fourcc = data[84:88]

    if fourcc not in (b"DXT1", b"DXT5"):
        raise ValueError("%s: %s, only DXT1 and DXT5 are read" % (path, fourcc))

    size = 8 if fourcc == b"DXT1" else 16
    out = bytearray(width * height * 4)
    at = 128
    bw = (width + 3) // 4
    bh = (height + 3) // 4

    for by in range(bh):
        for bx in range(bw):
            block = data[at:at + size]
            at += size

            if fourcc == b"DXT1":
                px = _colour_block(block, True)
            else:
                alpha = _alpha_block(block[0:8])
                px = [c[:3] + (alpha[i],) for i, c in enumerate(_colour_block(block[8:16], False))]

            for i in range(16):
                x = bx * 4 + (i & 3)
                y = by * 4 + (i >> 2)

                if x < width and y < height:
                    o = (y * width + x) * 4
                    out[o:o + 4] = bytes(px[i])

    return width, height, out


def image_read(path):
    return dds_read(path) if path.lower().endswith(".dds") else png_read(path)
