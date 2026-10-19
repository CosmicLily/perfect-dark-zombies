#!/usr/bin/env python3
"""
Black Ops II's third person grenade throw retargeted onto Perfect Dark's body
(g_SkelChr), baked for the game (zombies.inc, zombiesPoseThrowArms()): the
chest and the arms through Call of Duty's torso clips, laid over whatever the
body's own animation has the legs and the pelvis doing.

    python3 tools/zombies/throwbake.py <xanim dir> <body.glb> <out.ztb> [check.png]

<xanim dir> is an OpenAssetTools dump of common_zm's xanim/ (CLIPS below);
<body.glb> any Black Ops II body with its standard human skeleton (zm_prison's
c_zom_inmate_body1_lod0.glb), as divebake.py takes. With check.png, a sheet of
stick figures from the side, every fourth frame: Call of Duty's skeleton
white, Perfect Dark's retargeted onto it green. The art stays on the player's
disk, out of the tree.

The pt_ clips are a torso's alone (Call of Duty lays them over the legs'):
the spine, the neck and head, the shoulders, arms and fingers. The legs and
the pelvis are left at the bind here, and the retarget is divebake.py's. What
is kept is relative, so the body's own pose carries it: the chest's turn in
the pelvis's frame, and each arm part's in the chest's.

out.ztb, little endian:
    "ZTB1", u16 clips (CLIPS' order), u16 parts (7: the chest, part 2, then
    parts 3 to 8, the arms)
    per clip: u16 frames, u16 0, f32 frames a second, then per frame the
    chest's 3x3 turn in the pelvis's axes and each arm part's in the chest's,
    row major, each from the part's rest axes.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from divebake import (PD, add, apply, draw_sheet, hinge_signs, leg_length, load_skeleton,  # noqa: E402
                      matmul, pose, posed_pd, retarget, scale, to_pd, transpose)
from xanim import XAnim  # noqa: E402

CLIPS = ("pt_hold_throw", "pt_grenade_stand_throw", "pt_crouch_grenade_throw", "pt_prone_grenade_throw")
IDENTITY = ([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0])


class Torso:
    """A torso clip with torso_stabilizer keyed as a body's clip keys it: the
    inverse of j_mainroot's turn (the bind's, which a torso clip does not
    move), so the spine's tracks are in tag_origin's frame. Left at the bind,
    the spine leaned 60 degrees to the side and both arms stood up."""

    def __init__(self, anim, bones):
        self.anim = anim
        self.frames = anim.frames
        self.framerate = anim.framerate
        x, y, z, w = [q for n, p, t, q in bones if n == "j_mainroot"][0]
        self.stabilizer = (-x, -y, -z, w)

    def sample(self, name, f):
        if name == "torso_stabilizer":
            return self.stabilizer, None
        return self.anim.sample(name, f)


def main():
    xdir, glb, out = sys.argv[1:4]
    check = sys.argv[4] if len(sys.argv) > 4 else None
    bones = load_skeleton(glb)
    signs = hinge_signs(bones)
    names = {b[0]: b[1] for b in bones}
    cod_leg = sum(sum(v * v for v in t) ** 0.5 for n, p, t, q in bones if n in ("j_knee_le", "j_ankle_le"))
    k = leg_length() / cod_leg

    data = bytearray(b"ZTB1" + struct.pack("<2H", len(CLIPS), 7))
    panels = []

    for name in CLIPS:
        a = Torso(XAnim(os.path.join(xdir, name)), bones)
        data += struct.pack("<2Hf", a.frames, 0, a.framerate)

        for f in range(a.frames):
            w = pose(bones, a, f, IDENTITY)
            R, pelvis = retarget(w, signs, k)
            chest = matmul(transpose(R[1]), R[2])
            data += struct.pack("<9f", *(chest[0] + chest[1] + chest[2]))

            for j in range(3, 9):
                rel = matmul(transpose(R[2]), R[j])
                data += struct.pack("<9f", *(rel[0] + rel[1] + rel[2]))

            if check and f % 4 == 0:
                cod = []
                for n, (p, r) in w.items():
                    par = names.get(n)
                    if par and par in w and n.startswith("j_") and not any(
                            s in n for s in ("bulge", "twist", "raise", "_1", "_2", "_3", "head_end", "helmet")):
                        cod.append((scale(to_pd(p), k), scale(to_pd(w[par][0]), k)))
                pos = posed_pd(R, pelvis)
                pd = [(pos[j], pos[PD[j][0]], (60, 230, 90) if j % 2 else (240, 170, 60)) for j in PD if PD[j][0]]
                for j in (7, 8):
                    pd.append((pos[j], add(pos[j], apply(R[j], [0, 0, 120])), (240, 80, 80)))
                panels.append((cod, pd))

        print("%s: %d frames at %d" % (name, a.frames, a.framerate))

    open(out, "wb").write(data)
    print("%s: %d bytes" % (out, len(data)))

    if check:
        draw_sheet(check, panels)
        print("wrote", check)


if __name__ == "__main__":
    main()
