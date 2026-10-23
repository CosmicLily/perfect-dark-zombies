#!/usr/bin/env python3
"""
Black Ops II's sounds baked for the game (port/src/zmsfx.c): each one mono,
16-bit and at the game's synth rate (22020 Hz, port/src/audio.c), so that
played at its key's base note it sounds as Call of Duty's does.

    python3 tools/zombies/sndbake.py <out dir> <name>=<wav>[:gain][+<wav>[:gain]...] ...

Each <name> is written to <out dir>/<name>.zsn from the WAVs after it, mixed
together (a gain each, 1 if none), every channel of each averaged to one.
The WAVs are an OpenAssetTools Unlinker dump of a zombies zone's sound bank
(its `sound/` folder): `--include-assets soundbank` on zm_buried.ff gives
Buried's own and the zombies' voices, and the Ray Gun's are in
zmb_common.all.sabs/.sabl, which the Unlinker only reads under the zone's
own bank name - copied as zmb_buried.all.sabs/.sabl into a folder put first
in --search-path (the same trick as a DLC's .ipak, CLAUDE-notes). FLAC
entries are left out: nothing here decodes them.

A .zsn is "ZSN1", u32 rate, u32 sample count, then the samples, s16 little
endian. The art stays on the player's disk, in the build's zombies/sound/.

Downsampling averages every input sample under each output one (a box
filter), so 48000 to 22020 does not alias into a hiss.
"""
import os
import struct
import sys


RATE = 22020


def read_wav(path):
    # The chunks read by hand: the Unlinker's RIFF header gives the whole
    # file as 48 bytes, and Python's wave believes it (a sound of one sample)
    with open(path, "rb") as f:
        blob = f.read()

    if blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError("%s: not a WAV" % path)

    channels = width = rate = 0
    frames = b""
    at = 12

    while at + 8 <= len(blob):
        tag = blob[at:at + 4]
        size, = struct.unpack_from("<I", blob, at + 4)
        body = blob[at + 8:at + 8 + size]

        if tag == b"fmt ":
            _, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", body, 0)
            width = bits // 8
        elif tag == b"data":
            frames = body

        at += 8 + size + (size & 1)

    if width != 2:
        raise ValueError("%s: %d-byte samples, only 16-bit read" % (path, width))

    count = len(frames) // (2 * channels)
    data = struct.unpack("<%dh" % (count * channels), frames[:count * 2 * channels])
    mono = [sum(data[i * channels:(i + 1) * channels]) / channels for i in range(count)]

    return rate, mono


def resample(src, rate):
    if rate == RATE:
        return list(src)

    ratio = rate / RATE
    out = []
    n = int(len(src) / ratio)

    for i in range(n):
        a = i * ratio
        b = a + ratio

        if ratio > 1:
            lo = int(a)
            hi = min(int(b) + 1, len(src))
            total = 0.0
            weight = 0.0

            for j in range(lo, hi):
                w = min(b, j + 1) - max(a, j)

                if w > 0:
                    total += src[j] * w
                    weight += w

            out.append(total / weight if weight > 0 else 0.0)
        else:
            j = int(a)
            f = a - j
            s0 = src[j] if j < len(src) else 0.0
            s1 = src[j + 1] if j + 1 < len(src) else s0
            out.append(s0 + (s1 - s0) * f)

    return out


def bake(outdir, spec):
    name, layers = spec.split("=", 1)
    mixed = []

    for layer in layers.split("+"):
        path, gain = layer, 1.0

        if ":" in layer[2:]:
            cut = layer.rindex(":")
            path, gain = layer[:cut], float(layer[cut + 1:])

        rate, mono = read_wav(path)
        samples = resample(mono, rate)

        if len(samples) > len(mixed):
            mixed.extend([0.0] * (len(samples) - len(mixed)))

        for i, s in enumerate(samples):
            mixed[i] += s * gain

    pcm = [max(-32768, min(32767, int(round(s)))) for s in mixed]
    out = os.path.join(outdir, name + ".zsn")

    with open(out, "wb") as f:
        f.write(b"ZSN1")
        f.write(struct.pack("<II", RATE, len(pcm)))
        f.write(struct.pack("<%dh" % len(pcm), *pcm))

    print("%s: %d samples, %.2f s" % (out, len(pcm), len(pcm) / RATE))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)

    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)

    for spec in sys.argv[2:]:
        bake(outdir, spec)


if __name__ == "__main__":
    main()
