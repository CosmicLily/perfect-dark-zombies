#!/usr/bin/env python3
"""
Black Ops II's raw animations (an OpenAssetTools fastfile dump's xanim/ files,
"viewmodel_zombie_perksacola_drink" and the like), read into bone tracks.

    python3 tools/zombies/xanim.py <file> [...]     prints what each holds

The layout, little endian, as worked out from the dumps (version 19):

    u16 version (19), u16 frames, u16 bones, u8 flags, u8 ?, u16 framerate
        flags: 1 looping (keys run to frame `frames`, the first again),
               2 the root's motion follows (a player body's pb_*, a prop's)
    when flags & 2, tag_origin's motion: rotation keys (below) of one s16
        each, the yaw quaternion's z, then translation keys (below);
        pb_dive_prone's carries the body 119 forward and 44 up and down
    u8[(bones + 7) / 8] a bitfield (unknown; set on a few bones)
    u8[(bones + 7) / 8] simple: the bone's rotations are about z alone
    bone names, nul terminated
    per bone:
        u16 rotation keys
            the frame of each when more than one and fewer than the frames
            (frames + 1 looping): u8, u16 past 256; a key every frame has none
            each key: s16 z when simple, else 3 s16 (x, y, z); w is the
            positive root of the rest; a short is a 32767th
        u16 translation keys
            one: 3 f32
            more: frames as above, u8 flag, f32 min[3], f32 size[3], and
                  per key u16[3] when the flag is 0 (u8[3] otherwise),
                  min + size * value / 65535 (/ 255)
    u8 notetracks, each a nul terminated name and a u16 frame
       ("sndnt#evt_perk_bottle_open" 16, "sndnt#evt_perk_swallow" 29,
       "sndnt#evt_bottle_break" 57 in the drink)

Every animation in common_zm and zm_prison reads to its last byte but three
of the dreamcatcher's (flags 0x84).

A bone with no keys keeps its pose in the model; one with one key holds it.
A track is local to the bone's parent, as the model's skeleton has it.
"""

import math
import struct
import sys


class XAnim:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.version, self.frames, numbones = struct.unpack_from("<HHH", d, 0)
        self.flags = d[6]
        self.framerate = struct.unpack_from("<H", d, 8)[0]

        if self.version != 19:
            raise ValueError("%s: xanim version %d, 19 is read" % (path, self.version))

        nb = (numbones + 7) // 8
        at = 10
        self.delta_yaw = []    # [(frame, z)]: the yaw quaternion's z
        self.delta_trans = []  # [(frame, (x, y, z))]: tag_origin's motion

        if self.flags & 2:
            n = struct.unpack_from("<H", d, at)[0]
            at += 2
            frames, at = self._indices(d, at, n)
            for f in frames:
                self.delta_yaw.append((f, struct.unpack_from("<h", d, at)[0] / 32767.0))
                at += 2
            self.delta_trans, at = self._translations(d, at)

        simple = d[at + nb:at + 2 * nb]
        at += 2 * nb
        self.names = []

        for _ in range(numbones):
            e = d.index(b"\0", at)
            self.names.append(d[at:e].decode())
            at = e + 1

        self.rot = {}    # name -> [(frame, (x, y, z, w))]
        self.trans = {}  # name -> [(frame, (x, y, z))]

        for i, name in enumerate(self.names):
            is_simple = (simple[i >> 3] >> (i & 7)) & 1
            n = struct.unpack_from("<H", d, at)[0]
            at += 2

            if n:
                frames, at = self._indices(d, at, n)
                keys = []

                for f in frames:
                    if is_simple:
                        z = struct.unpack_from("<h", d, at)[0] / 32767.0
                        at += 2
                        q = (0.0, 0.0, z, math.sqrt(max(0.0, 1.0 - z * z)))
                    else:
                        x, y, z = (v / 32767.0 for v in struct.unpack_from("<hhh", d, at))
                        at += 6
                        q = (x, y, z, math.sqrt(max(0.0, 1.0 - x * x - y * y - z * z)))

                    keys.append((f, q))

                self.rot[name] = keys

            keys, at = self._translations(d, at)
            if keys:
                self.trans[name] = keys

        # what happens when: "sndnt#evt_perk_swallow" at frame 29 of the drink
        self.notes = []
        count = d[at] if at < len(d) else 0
        at += 1

        for _ in range(count):
            e = d.index(b"\0", at)
            name = d[at:e].decode()
            frame = struct.unpack_from("<H", d, e + 1)[0]
            self.notes.append((frame, name))
            at = e + 3

        self.leftover = len(d) - at

    def _indices(self, d, at, n):
        """the frames of n keys: none written for one key or a key every frame"""
        # a looping animation (flag 1) keys one frame more, its first again
        slots = self.frames + (self.flags & 1)
        if n <= 1:
            return [0] * n, at
        if n >= slots:
            return list(range(n)), at
        idx = "B" if slots <= 256 else "H"
        return list(struct.unpack_from("<%d%s" % (n, idx), d, at)), at + struct.calcsize(idx) * n

    def _translations(self, d, at):
        n = struct.unpack_from("<H", d, at)[0]
        at += 2

        if n == 1:
            return [(0, struct.unpack_from("<3f", d, at))], at + 12
        if n == 0:
            return [], at

        frames, at = self._indices(d, at, n)
        flag = d[at]
        at += 1
        lo = struct.unpack_from("<3f", d, at)
        size = struct.unpack_from("<3f", d, at + 12)
        at += 24
        fmt, top = ("<3H", 65535.0) if flag == 0 else ("<3B", 255.0)
        step = struct.calcsize(fmt)
        keys = []

        for f in frames:
            v = struct.unpack_from(fmt, d, at)
            at += step
            keys.append((f, tuple(lo[k] + size[k] * v[k] / top for k in range(3))))

        return keys, at

    def sample(self, name, frame):
        """(quaternion or None, translation or None) of a bone at a frame"""

        def at_frame(keys, lerp):
            if not keys:
                return None
            if frame <= keys[0][0] or len(keys) == 1:
                return keys[0][1]
            for (f0, a), (f1, b) in zip(keys, keys[1:]):
                if f0 <= frame <= f1:
                    return lerp(a, b, (frame - f0) / float(f1 - f0) if f1 > f0 else 0.0)
            return keys[-1][1]

        def nlerp(a, b, t):
            if sum(a[k] * b[k] for k in range(4)) < 0:
                b = tuple(-v for v in b)
            q = [a[k] + (b[k] - a[k]) * t for k in range(4)]
            n = math.sqrt(sum(v * v for v in q)) or 1.0
            return tuple(v / n for v in q)

        def lerp(a, b, t):
            return tuple(a[k] + (b[k] - a[k]) * t for k in range(3))

        return at_frame(self.rot.get(name), nlerp), at_frame(self.trans.get(name), lerp)


def main():
    for path in sys.argv[1:]:
        a = XAnim(path)
        print("%s: %d frames at %d fps, %d bones, %d rotating, %d moving, %d bytes left; notes %s"
              % (path.split("/")[-1], a.frames, a.framerate, len(a.names), len(a.rot), len(a.trans),
                 a.leftover, a.notes))


if __name__ == "__main__":
    main()
