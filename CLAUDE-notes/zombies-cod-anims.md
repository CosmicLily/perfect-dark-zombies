# Black Ops II's animations in Zombies

## Digest

- **A grenade's quick throw, first and third person (2026-10-19)**: the frag's
  body is on `tag_clip`, not `j_gun`; a gun's hand model holds its left hand
  off with a toggle; a torso clip (`pt_`) needs `torso_stabilizer` keyed by
  hand; Perfect Dark's chest matrix is not where the shoulders' offsets are
  from. Below.

The tools are `tools/zombies/` (each file's docstring says what it reads and
writes); the art stays on the player's disk, in the build's
`zombies/viewmodel/`.

## A grenade's quick throw (2026-10-19)

First person is `vmbake.py <dir> grenade` (`viewmodel_m67_pullpin` then
`viewmodel_m67_throw` onto the hands, `grenade.zpd`, and `frag_body.obj` /
`frag_top.obj`), drawn by `zombiesRenderThrowViewmodel()`. Third person is
`throwbake.py` (`pt_hold_throw` and the stand, crouch and prone throws,
`throw.ztb`), laid on the body by `zombiesPoseThrowArms()` from chr.c. Four
things were wrong once, and none shows in the code.

**The grenade is its clip.** `t6_wpn_grenade_frag_view` has three bones:
`j_gun`, `tag_clip` and `j_grenade_activate_clip` (the pin). Every vertex of
the body (903) is on `tag_clip`; `j_gun` hangs from the arms' `tag_weapon`,
which the m67's clips key once and never move. Baked on `j_gun`, the grenade
stood still at the bottom of the view while the hands went away from it.

**The left hand is switched off.** A gun's hand model carries both hands but
holds the left off with a toggle (`MODELPART_HAND_LEFT`, whose rwdata is the
hand's `handsavedata`); only the fists show both. The pin is pulled with the
left hand, so the throw switches it on for its own draw and back after. The
drink, which wants no left hand, moves the fists' left one behind the eye.

**A torso clip leaves the pelvis's turn in the spine.** In a Black Ops II
body the spine hangs from `torso_stabilizer`, under `j_mainroot`, and the
spine's tracks are in tag_origin's frame: a body clip (`pb_`, the dive's)
keys the stabilizer as `j_mainroot`'s inverse. A torso clip (`pt_`) keys
neither, and left at the bind the stabilizer leaves `j_mainroot`'s bind turn
(about 120 degrees) on the spine: it leaned 60 degrees to the side and both
arms stood straight up. `throwbake.py` keys it as the bind's inverse.

**Perfect Dark's chest matrix is not where the shoulders hang from.** Placed
down the body's own offsets from the chest's matrix (as the dive places
everything from the pelvis), a shoulder came out 33 units from it where the
body has it at 19, and the arms were long. The elbows and hands do go down
their offsets (measured equal to the body's own to the hundredth). So a
shoulder keeps the body's own place against the chest, turned with the
chest's new turn. The long arms also put the body in the third person
camera's way, which faded it out for a frame or two like a wall does.

To check a retarget before the game: both tools take a `check.png` of stick
figures (Call of Duty's skeleton white, Perfect Dark's green), and the bone
directions can be compared numerically from `divebake.py`'s `retarget()` and
`posed_pd()`. A sheet whose shoulders sit apart from the white ones is only
the chest's shape; the directions are what matter.
