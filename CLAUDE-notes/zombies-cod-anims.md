
Swipes came too fast (a tester): a swing began the tick after the last ended,
and the standing and crawling swipes dealt both of their `fire` notes. Now one
blow a swing (the first note) and `ZOMBIE_SWING_REST60` (45) between swings:
measured, a zombie at a player lands a blow every 2.5 seconds (it was every 1
to 1.1, and twice in a standing swipe). And Perfect Dark's own man's grunts
and death cries (`chrChoke()`) are skipped for a zombie while
`zombiesCodOwnsBlows()`: its cries are Black Ops II's (zombiesAwardHit()'s
death voice, the attack voice as a blow lands, the groans).
, sprinters 142, each on its own clip. Crawlers go at
the crawl's 14 (a walker) or the crawl sprint's 40. A sprinter groans with
the sprint voices.
l numbers.

zombies.inc: `g_ZombieGib` per bot (`ZGIB_*`), set by `zombiesMaybeGib()` from
zombiesAwardHit() (the host's): an arm hit a zombie lives through, one time in
`ZOMBIE_GIB_ARM_ODDS` (3), one arm at most; a killing headshot not by a blast,
one in `ZOMBIE_GIB_HEAD_ODDS` (2). Online in the block's version 7 (protocol
40; the block came to 1132 of its 1280 bytes). Each machine throws its own:
`zombiesCodGibFresh()`, as a zombie is drawn, compares what is off it with what
it has seen (cleared by a respawn), throws the arm from its shoulder joint
(flown and drawn by `zombiesRenderGibs()`, from zombiesRenderRayFx(), never
the game's random) and bursts blood (`sparksCreate()` flesh and blood) where
the head was. The shot test skips a missing arm's capsules and a missing head.
`--zombies-test-gibs 1` takes every zombie's left arm at level frame 600.
.
- **A gun in the hand, tested headless (2026-10-20)**: `--zombies-test-hold W`
  gives player 1 weapon W at level frame 60 and shows it in third person
  (`--zombies-test-third 0`: first), `--zombies-test-turn D` turns them,
  `--zombies-test-side S` moves the camera sideways (it sits behind the body,
  which hides the hand), `--zombies-test-func 1` picks the second function;
  `--rng-seed` keeps the spawn. A body holds a gun as a pistol or a rifle only
  if its definition has `WEAPONFLAG_AICANUSE` (player.c's wield mode): the Ray
  Gun lacked it and hung at the hip as "unarmed". The Ray Gun's second
  function is the Mauler's charge (`bgunTickMaulerCharge()`, three rounds)
  let go as a three-bolt burst (`FUNCFLAG_BURST3`), spent by
  `WEAPONFLAG3_CHARGESPENT` only once the burst is out. The Reaper's muzzle is
  one of three barrels in turn each frame (`WEAPONFLAG2_MINIGUN`), so COD
  aim's muzzle offset jumped 25 units a frame: its middle is taken. Zombies'
  laser sight traces along the barrel, not to the crosshair. `--zombies-test-barrier N`
  puts the camera on barrier N. Its boards are Origins' `p6_anim_zm_barricade_board_01`
  and `_02` (the dump's `model_export/*_lod0.glb`, `glb2obj.py` into `zombies/models/board01`,
  `board02`), each fitted to a board's place; a zombie at the boards deals no damage
  (`zombiesZombieSwipe()`), since a barrier is on a wall and it waits on the player's side.

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

## A DLC map's streamed pictures (2026-10-19)

A Black Ops II weapon's colour, specular and normal maps are streamed, not in
its fastfile: an OpenAssetTools Unlinker dump of `zm_buried.ff` gave the Ray
Gun's model with no `~-gmtl_t6_wpn_zmb_raygun_col`, and 848 "Could not find
data for image" errors. The pictures are in the `.ipak` files beside the
fastfiles, and the Unlinker loads them only with `--search-path` on
`zone/all`, and then only by name: `base`, `zm`, `dlc3`, and one named for
the zone (`zm_buried.ipak`), which does not exist. Buried's own is
`dlczm3.ipak` (the zombie DLC packs are numbered Nuketown 0, Die Rise 1, Mob
of the Dead 2, Buried 3, Origins 4). Copied into a folder of its own as
`zm_buried.ipak` and that folder put first in the search path, the dump has
no errors and every Ray Gun picture:

    Unlinker.exe --search-path "<copy dir>;<game>\zone\all" --include-assets image -o <out> "<game>\zone\all\zm_buried.ff"

`vmconv.py model` and `glb2obj.py` then take that dump's `images/`.

## Black Ops II's own zombies: where they are (2026-10-21)

Tranzit's regular zombies are not in `zm_transit.ff` (its models are the map's
props and machines) but in its Classic mode's zone, `so_zclassic_zm_transit.ff`:
`c_zom_zombie1_body01`/`02` and `c_zom_zombie2_body01`-`03` (each with its
`_g_larmoff`, `_legsoff`, `_upclean`, ... pieces for limbs shot off), heads
`c_zom_zombie_head_a`, `_k`, `_l`, `_n`; a body is one skinned mesh of 73
joints. Dumped as Tranzit is, with `zm.ipak` copied as
`so_zclassic_zm_transit.ipak` into a folder first in `--search-path`. Unlinker's
`-o <dir>` writes the zone's folders straight into `<dir>`, not
`<dir>/t6/<zone>` as a dump without `-o` does: the dump was moved by hand to
`zone\all\zone_dump\t6\so_zclassic_zm_transit`. The zombies' animations
(walk to sprint, `ai_zombie_boardtear_aligned_[lmr]_[1-6]`, the
`barricade_enter`s, crawls, attacks, deaths) are Origins' dump's `xanim/` and
every map's alike; the boards' own are `o_zombie_board_N_pull`/`_repair`.

A zombie converts as the viewmodel does (`vmconv.py model` and `anim`, into the
build's `zombies/characters/`: `zombie1_body01.zvm` and `zombie_head_a.zvm`
and the walk, run, sprint and idle `.zan`s); `zmpose.py` draws a body and head
posed by one, a row of frames, the head's bones that the body has taking the
body's (Call of Duty's attach). The body comes out upright and facing +x with
the viewmodel's root turn as it is. OpenAssetTools gave the head's skin the
rim light's mask (`c_gen_arm_rim_mask_norim_c`) for its colour: `vmconv.py`
takes the model's own `~-g..._c` for a picture named a mask.

## Black Ops II's zombie in the game (2026-10-21)

`port/src/zmchar.c` draws Tranzit's zombie (`zombie1_body01` and
`zombie_head_a`, the `.zan`s named in its table) in place of a zombie's Perfect
Dark body: `zombiesRenderCodZombie()` (zombies.inc), called from both of
`chrRender()`'s `modelRender()`s, true when it drew. The body stays, unseen, for
shots and hit parts. It skins on the CPU into a vertex array per zombie slot
(`ZMCHAR_SLOTS`), each slot's list built once over its own array, as the
machines batch theirs; `tools/zombies/zmpose.py` is the same posing as a PNG.
The animation is chosen by the zombie's smoothed pace over the floor (inches a
second: idle under 8, walk, run from 72, sprint from 125) and played at that
pace over the loop's own root speed (walk 37.6, run 108, sprint 142, which the
`.zan` does not carry: the table in zmchar.c), blending over 12 60ths; a death
is one of `ch_dazed_[a-d]_death`, held at its end.

What went wrong on the way:
- **j_mainroot's animated translation is an offset from its rest** (rest 37
  inches up, the walk's near 0); every other bone's is where the bone is.
  Without it the zombie walked 37 inches under the floor.
- **`mtx4LoadIdentity()` leaves the diagonal's 1s**: a matrix filled column by
  column must clear `m[1][1]` and `m[2][2]` too, or the "left" and "up" axes
  gain a 1 and the zombie lies stretched along the floor.
- **renderdata's fogcolour is the scenario's highlight colour** in Zombies (the
  yellow-green of a zombie's suit), not the room's light: the light is
  `chr->shadecol`.
- **A drawing never calls the game's `rngRandom()`**: it changed every run
  after it (and online would desync machines). A slot's start and its death
  are picked from its number.
- **Spare bots are killed at frame 100 and lie where they are** until a round
  wants them; Perfect Dark fades a corpse, so a CoD zombie goes once
  chrRender()'s alpha (envcolour's low byte, `unk30` 8) is under half.

Testing: `--zombies-test-zombie D` puts the camera D in front of the first
living zombie and the player beyond harm; with `--rng-seed 7 --fixed-step` two
runs match frame for frame, so moving `build-dabs/zombies/characters` aside
gives the same frame with Perfect Dark's bodies to compare. Frame 1100 has two
zombies walking at the camera.

## Barriers as Call of Duty's: torn and climbed through (2026-10-21)

A board tear is aligned to the barrier (`ai_zombie_boardtear_aligned_m_N_grab`,
`_hold`, `_pull`, Origins' dump), and its root motion is where the barrier is:
from its origin - the window's sill, 31 inches over the floor - the zombie
waits 23 inches back (at z -31, its feet), lunges 23 to 32 inches forward to
grab and steps back as it pulls; the boards are 28 to 34 inches in front, on
the room's side of a wall that thick; `barricade_enter_m_v1` carries it 57.8
forward over the sill to the same floor. `vmconv.py anim` now writes the root
motion as a track named `@delta` (translations only), and zmchar.c keeps it
and a board's `tag_animate` (`zmcharAnimRoot()`): a board's
`o_zombie_board_N_pull` is as long as the zombie's pull of it and flies it
off, `_repair` brings it back, and its last frame is where it rests.

Perfect Dark's walls have nothing behind them, so the window stands out from
the wall at true size (`ZOMBIE_BARRIER_*` in zombies.inc): the zombie's place
(`stand`) 30 units out, the origin 23 inches beyond, the boards about 1.6 to
1.75 m out, a pen between walled at the sides and dark at the back. A zombie
held there plays the tear clips on the barrier's own clock (`tear60`, the
board being pulled `pulling`, the climb `enter60`), drawn in the barrier's
frame with the clip's root motion (`zombiesCodBarrier()`,
`zombiesCodBarrierPlace()`); the board goes as its pull begins; after the
last pull it climbs, and `chrMoveToPos()` puts its body where the climb ends.
A player rebuilds from in front of the boards (`ZOMBIE_BARRIER_FRONT`).

Mapping Call of Duty's barrier space into the world: x into the room is away
from the wall, but its y (a zombie looking in, its left) is along
`(-wall.z, wall.x)`, minus the barrier's own "along": with "along" the zombie
came out mirrored (`zombiesBarrierToWorld()`, used for boards and zombie
alike).

The body stays at its place in the pen through the climb, so for those two
seconds shots land on the pen, not on the climbing model. The pen's walls are
drawn, not solid. A barrier in a narrow corridor puts the climb's end 2.35 m
out: on Complex's barrier 6 it is still in the room.
`--zombies-test-zombie 280 --rng-seed 7 --fixed-step` follows the zombie at
barrier 6 on stage 0x1f: frames 262 to 350 a board torn, 1300 to 1395 the
climb.

## Attacks, crawlers and the swipe through the boards (2026-10-21)

- **Attacks** are drawn, not dealt: a zombie within `ZOMBIE_COD_REACH` (75) of a
  player who is up plays `ai_zombie_walk_attack_v1` moving or
  `ai_zombie_attack_v1` standing (a crawler `ai_zombie_attack_crawl`); the
  blow is still the game's own punch (chrPunchInflictDamage()'s 8.5).
- **Crawlers**: a blast a zombie lives through takes its legs one time in
  `ZOMBIE_CRAWLER_ODDS` (zombiesAwardHit(), the host's, `g_ZombieCrawler`);
  its speed is `zombiesChrSpeedScale()` (bot.c's botGetStatScale() asks it
  per zombie), 0.3 of a run. The crawler is Call of Duty's two pieces,
  `_g_upclean` (the upper body, 73 bones) and `_g_legsoff` (the stumps alone,
  16): `_g_legsoff` by itself is only the stumps. zmchar.c draws a look's
  models (`ZMCHAR_LOOK_WHOLE`, `_CRAWLER`), a slot a list for each. Their
  gore pictures are not in the Classic zone: `~-glimbs_c` is common_zm's,
  `~-gchar_gen_gib_decals` so_zclassic's, copied beside the others before
  `vmconv.py model`. A crawler at a barrier tears with
  `ai_zombie_crawl_boardtear_aligned_m_N_*` and climbs with
  `ai_zombie_crawl_barricade_enter_l`; the climb's end is where its own root
  motion finishes (`zmcharAnimRoot()` at its last frame), not 57.8 inches.
  The body it stands in is still a whole one: a crawler is shot where a
  standing zombie's legs would be.
- **The swipe through the boards** (`reachThroughAttacks`): a player up close
  on the room's side across the opening (`zombiesBarrierReaches()`) stops the
  tearing for `ai_zombie_window_attack_arm_l_out`/`_r_out`, an arm each in
  turn, its blow (half a player's health) `ZOMBIE_REACH_HITAT` through. Those
  clips carry no root motion and lunge in the hips' own track (11 to 35
  inches), so the root is at the zombie's waiting spot, 23 inches behind the
  origin; at the grab's spot it lunged twice and came through the boards.
  The blow goes through `chrDamageByImpact()` with `g_ZombieReachThrough`
  set, the one blow from behind the boards zombiesZombieSwipe() lets land.
- **Online** (Zombies block version 6, protocol 39): the crawlers, a bit a
  bot, and each barrier's zombie, tear clock, board pulled, climb and reach;
  the client sets `g_ZombieAtBarrier` from them, so its barriers are drawn
  as the host's.

Testing: `--zombies-test-crawlers 1` makes every zombie a crawler,
`--zombies-test-reach 1` counts player 1 at every barrier's boards (the
`--zombies-test-zombie` camera then follows a zombie at a barrier rather than
one in the room, and lower for a crawler). With the reach test and no camera
the player is worn down by frame 1300 on stage 0x1f; without it, not.

## Shots on the zombie's own bones; its swipes deal its blows (2026-10-21)

This replaces two of the section above's caveats.

- **Shots**: `chrTestHit()` hands a zombie drawn as Black Ops II's to
  `zombiesCodTestHit()`, which tests the shot (camera space, as
  `shotdata->gunpos2d`/`gundir2d`) against capsules on its joints as last
  drawn (`zmcharSlotPose()`, the slot's joints in its own inches; the place
  it was drawn at, `g_ZombieCodPlace`, four vertex units an inch): pelvis,
  torso (lower spine to upper spine, 7.5"; upper spine to neck slim, 4.5"),
  arms, hands, legs (none on a crawler), and the head a 5" ball 0.6 of the
  neck's length past `j_head`. A torso capsule up to the neck at full width
  took head shots from below as torso. The hit is made with `hitCreate()`
  and one of the Perfect Dark body's own box nodes for that hit part
  (`zombiesChrBboxNode()`: the body's tree walked for a bbox), which
  chrHit()'s blood and bruises need; its position is the ray's own at the
  hit's depth. The body under it is never tested: air over a crawler is air.
- **Blows**: `zombiesTickSwings()` (the game's tick, the host's, every zombie,
  drawn or not) begins a swipe when a player is within `ZOMBIE_COD_REACH`
  (150: a bot's own punch reaches 210, so at 75 the zombies stood off and
  seldom swung) - walking (`walk_attack_v1`), standing (`attack_v1`) or
  crawling - and deals a blow at each of its clip's `fire` notes (frames 6;
  25 and 39; 24 and 54) to a player still in reach and in sight
  (`zombiesReachHit()`, half a player's health). `chrPunchInflictDamage()`
  deals nothing while `zombiesCodOwnsBlows()`. The drawing plays the swing
  the tick has; a client, which has no swings, swipes by nearness alone.

Testing: `--zombies-test-shoot head|chest|air` fires a real shot every half
second from the `--zombies-test-zombie` camera at that point of the zombie it
follows, and the hit test logs the part (0, none). On stage 0x1f, seed 7,
fixed step: head 4 of 5 head (one a hand in the way), chest all torso, air
all misses, standing and crawling. A plain run to frame 3000 has three swipes
land and a game over.

Ten bodies since: Tranzit's Classic zone has `zombie3_body01` to `_05` too
(work clothes: grey, orange, blue, green, yellow), each with all its pieces of
its own. zmchar.c's table is 60 models (`ZMCHAR_TEN`/`ZMCHAR_EIGHT` name
them; eight bodies have their own legs and stumps, `ZMCHAR_PIECES`), the slot
key three 6-bit numbers, and the skinning's working arrays are indexed by a
model's place in its look (three at most), not its number: by number, 60
models' worth of vertices was 12 MB of statics. In zmcharDraw()'s skinning
loop `n` is that place, so the normal is `nrm` (a local `n[3]` shadowed it).

A barrier at its own size needs room: `zombiesBarrierFits()` (in
zombiesPlaceBarriers(), before a pad is taken) wants the room to run on from
the wall `ZOMBIE_BARRIER_FRONT` + `ZOMBIE_BARRIER_CLEAR` (the window, a
player rebuilding it, room past them), and the wall to run
`ZOMBIE_BARRIER_OPEN` + 28 either side at the pen's back and at the boards
(zombiesRayToWall() from points walked out from the pad with
func0f065e74()). Complex (0x1f) went from seven barriers to four; a pad that
fails is passed over for the next. Stage 0x22 has no spawn pads and no
barriers either way.

## Black Ops II's sounds came out as static (2026-10-21)

A tester heard television static whenever a zombie groaned or attacked. The
`.zsn`s are little endian, but `n_alRaw16Pull()` (n_load.c) reads a raw wave
big endian, as the cartridge's own are, so every voice and every Ray Gun
sound played byte-swapped: white noise as long as the voice. `zmSfxLoad()`
now swaps them as they load. To check a sound's playback, `--audio-dump FILE`
gives the mixed output (s16 stereo at 22020); a voice's zero-crossing rate
there near 0.4 is a byte-swapped one (the files' own are under 0.1).

## A zombie down a ledge, and slower swipes (2026-10-21)

A zombie off a ledge falls under Perfect Dark's own gravity (`chr->manground`,
its feet, above `chr->ground`, the floor under it). `zombiesCodFall()` takes a
fall deeper than `ZOMBIE_COD_DROP` (30 inches) as Black Ops II's jump down:
`zmcharJumpDown()` picks the clip nearest the drop (`ai_zombie_jump_down_48`
to `_222`, a crawler's `ai_zombie_crawl_jump_down_*`; Tranzit's Classic zone
and Origins' dumps, `vmconv.py anim` into `zombies/characters/`) and finds
from its root where it leaves the ledge (a tenth of the way down) and lands.
In the air the clip goes as far down its fall as the zombie is down its own,
and the body is drawn at `manground`: drawn at `ground`, as it is otherwise,
it was already on the floor below as it stepped off. Down, it plays the
landing out at the clip's pace. Only a zombie that is drawn is animated, so a
headless test must look at the one that falls: `--zombies-test-follow N` puts
the camera beside zombie N, `--zombies-shot-step S` takes a picture every S
frames from `--screenshot-frame` on. Following one changes which are off screen
(ticked every fourth frame), so the run is not the unfollowed one: stage
0x32, seed 7, following 5 shows a 157-inch drop at frame 1832.

Swipes were still too quick (a tester): `ZOMBIE_SWING_REST60` is 90, and a
swipe through a barrier's boards, which came back to back, now rests
`ZOMBIE_REACH_REST60` (120) after each, tearing at the boards meanwhile.
## Mob of the Dead's perk machines in their colours (2026-10-21)

PhD Flopper (`p6_zm_al_vending_nuke`) and Deadshot (`p6_zm_al_vending_ads`,
`_ads_logo`) are Mob of the Dead's machines. Their colour maps (`_c`) are
gray all over, and they had been tinted one colour each with `tint.py`
(purple, orange), which looked wrong. The colour is in a second picture,
`_m`: the emblem, trim, buttons and sign in colour, black where the gray
shows, no alpha. Both stream from `dlczm2.ipak` (Mob of the Dead's), dumped
as Buried's are, with the ipak copied as `zm_prison.ipak`. `tools/zombies/almerge.py`
lays the mask over the gray as far as it is bright, into the build's
`models/phdflopper/phdflopper_1.png` and `models/deadshot/deadshot_0.png`,
`_1.png`. The other machines' pictures were coloured already.
## The machines' lights, glass and blood (2026-10-22)

The machines no longer cast a glow (the coloured halo round the sign and the
pool on the floor were taken out with `zombiesRenderMachineGlow()`), and an
`_on` picture is drawn as it is, not darkened by the room, rather than
brightened by its own shade on top: they looked like lamps.

A machine's list is drawn in the opaque pass in one render mode, and
`modelpackBindMaterial()`'s alpha was never used, so glass (`mc_global_glass`,
Cherry's `..._glass_frosted_clear_center`) was a gray slab over the front and
Cherry's blood decal a dark block. A cut-out inside the list (an alpha compare,
`G_RM_AA_ZB_TEX_EDGE2`, then the opaque mode set back after the draw) hid that
draw and **every draw after it**: Deadshot and PhD vanished whole. Materials
named `glass` or `blood` are now skipped instead. The blood's picture had also
lost its alpha on conversion (the build's `electriccherry_2.png` is now
`~-gglo_dec_mature_blood_splt_~c028c2d0`, Mob of the Dead's ipak dump).
`--zombies-test-perk N` circles machine N for screenshots.
## Electric Cherry's burst (2026-10-22)

Mob of the Dead's effect is `fx_alcatraz_electric_cherry_player` (and `_sm`,
`_down`; the map's `mp/configstrings` csv names every effect it loads), but
the Unlinker writes no effect assets (`--include-assets fx` dumped nothing).
Its pictures come with the ipak dump: `fxbake.py <images> <out> cherry` makes
`cherry_ring.i8` (fxt_fx_emp_ring_wave), `cherry_flare.i8`
(fxt_light_flare_cool) and `cherry_bolts.i8` (fxt_env_lighting_bolt_ground's
eight bolts), each small enough for one texture load; they are uncompressed
32-bit DDS, which `imgio.py` now reads. `zombiesCherryBurst()` starts one as
the reload's shock lands; `zombiesRenderCherryFx()` draws it with the Ray
Gun's effects: a flash, the ring widening to the shock's reach, a bolt to
each zombie it reaches. For the player who reloads it is anchored at the gun
and the ring at the chest: from the eye the burst's middle is under the
camera and the floor's ring below the view. Its clock stands still while
none is drawn, so a new burst restarts it (one aged out at its first frame
otherwise). `--zombies-test-cherry` fires one every 90 frames.
Matched against Black Ops II (2026-10-22, a 1080p capture of a Mob of the
Dead player reloading with Cherry, stepped a tenth of a second at a time):
the burst is tall violet-white bolts standing all round the player, floor to
well over the head (in first person they fill the view's front and sides),
a low white-blue haze, white sparkles drifting, about 0.8 s, and the shocked
zombies crackling. So: six bolts standing in a ring a stride out, turned to
the camera about the upright (`ZOMBIE_CHERRY_STAND60`), eighteen sparkles,
the colour `ZOMBIE_CHERRY_R/G/B` (the pictures are white), the floor ring
faint. Bolts hung at the screen's edges were tried first and dropped: the
standing ones give that look themselves. Each struck zombie crackles where
it is now (`bot[]`), drawn 25 units toward the camera (at its middle the
body hid it). The sound is `zmb_cherry_explode` (the taser mine's blast,
`tazer_mine.LN50.pc.snd.wav` in the Origins dump), baked as
`cherry_explode.zsn`: `ZMSFX_CHERRY_EXPLODE`. `--zombies-cherry-reach R`
and `--zombies-cherry-cam D` (a camera behind player 1) go with
`--zombies-test-cherry`.
## A zombie's blow, and the perks against Black Ops II (2026-10-22)

A blow (`zombiesReachHit()`, the open swipes' and the barrier's) was meant
to be half a player's health, but `chrDamage()` turned it on the way: the
fist's side (`FUNCFLAG_BLUNTIMPACT`: 0.4 from the front, 0.7 the side), the
torso doubled, the player's MP handicap divided out (a test run's halved it),
the simulant damage slider, and the health scale it was sent times (so
Juggernog's 2.5 was taken back off). Measured with `--zombies-test-reach 1`:
a blow took 1.02 of a player's health, one hit down with Juggernog or
without. Now `chrDamage()` takes `zombiesBlowDamage()` as it is, just before
the player's health: 0.51 a blow, two hits down, 0.204 with Juggernog, five.
Instrument the player's own damage line in `chrDamage()` when a hit's size
is in doubt; reading the scalings one by one missed the handicap twice.

The swing begins only once a player has been in reach for
`ZOMBIE_SWING_WINDUP60` (a third of a second) and lands only in the reach
itself (it had 15 to spare): begun the tick one came in reach, the walking
swipe's blow (its frame 6) caught a player running past.

Perks: a limit of four (`ZOMBIE_PERK_LIMIT`, the machines offer nothing past
it), Speed Cola rebuilds boards twice as fast, Stamin-Up is 7% (it was 15%).
## The perks' sounds and the drink's left hand (2026-10-22)

Black Ops II's own, as `zmsfx.c` slots: `perk_buy` (zmb_cha_ching),
`perk_dispense`, `perk_deny`, the drink's `perk_open`, `perk_swallow`,
`perk_break`, `perk_belch` (evt_perk_bottle_open, evt_perk_swallow,
evt_bottle_break, evt_belch: the common bank, dumped with
`zmb_common.all.sabs`/`.sabl` copied as `zmb_buried.all.*` into a folder first
in `--search-path`, `--include-assets soundbank` on zm_buried.ff), each perk's
`sting_*` and `jingle_*` (mus_perks_*_sting/_jingle: FLAC in Tranzit Classic's
dump, decoded with MSYS2's ffmpeg to WAV for `sndbake.py`; Electric Cherry's
jingle is Mob of the Dead's `cherry_jingle`, and it has no sting). The drink's
open, swallow and break are on its clip's notes (frames 16, 29, 57); the
belch as the perk takes. `zombiesTickJingles()` plays a machine's jingle near
a player every 1.5 to 4 minutes. The drink drew the left hand behind the eye,
so the cap came off by itself: it is drawn now (the hand model's left-hand
toggle switched on, as the grenade's throw does). `--zombies-test-drink P`
buys perk P at frame 200.
The machines' hums are Black Ops II's (`perk_hum`, `pap_hum`: the common
bank's `perksacola/hum/hum_loop` and `packa_loop`), the only looped ones of
`zmsfx.c`: a raw wave's loop (`ALRawLoop`, read in by `snd.c` as the ADPCM
loop is, count -1) and an envelope of decay -1 (`n_sndplayer.c`'s sustain).
`zombiesTickHums()` starts one from its machine as a player comes within 650
and stops it past 850. `psIsChannelFree()` knows only the AI's channels
(0-7): a hum's channel is checked in `g_PsChannels` itself. Quick Revive
alone goes once its third bottle is spent (`zombiesTickQrMachine()`:
deregistered from its rooms, disabled, and no longer placed). The HUD's
perk icons were already the game's own (perkicons.py `--iwi`).
The drink seen in third person is Black Ops II's own body clip now,
`p_zombie_perkacola_drink` (zm_buried's xanim, 65 frames: a whole body's clip,
so `throwbake.py` keeps its own torso_stabilizer), baked as `throw.ztb`'s
fifth clip (`ZTCLIP_DRINK`; `throwbake.py` takes several xanim dirs, comma
separated, as Windows paths: an MSYS `/d/...` one is not found). It plays
through `zombiesPoseThrowArms()` two of the drink's 60ths a frame; the old
IK arm (`zombiesPoseDrinkArm()`) only runs without the file. The clip's note
says the bottle is in the left hand, but retargeted it is our right arm that
drinks (in the left the bottle hung at the hip). A test run's 60ths go two a
frame: `--screenshot-frame` steps are half the drink's.
**The drink clip on the body is off (`ZOMBIE_DRINKBODY_CLIP` 0).** Its check
sheet has the retargeted right arm curl over the head to drink, as Black Ops
II's right wrist comes within 14 inches of the head at frames 32-40 (the left
stays at the hip, whatever its `anim_gunhand "left"` note says). Laid on the
body in the game, at frame 33 an arm stood out straight in front instead
(seen from behind, `--zombies-test-turn 90`/`270`); not found why. The bent
arm (`zombiesPoseDrinkArm()`) brings the bottle to the mouth meanwhile.
`throw.ztb` now has an eighth part, the head's turn in the chest's (j_neck to
j_head, from the bind's), which `zombiesThrowBodyHead()` lays on every
matrix under the body's head spot about the neck: the throws tip the head 3
to 38 degrees. Seen working in a drink; not yet looked at in a throw.
## The box's, Pack-a-Punch's, the wall buys' and the power-ups' sounds (2026-10-22)

Black Ops II's, in `zmsfx.c` slots `ZMSFX_BOX_OPEN` to `ZMSFX_PU_DBLPOINTS_OFF`
(names in zmb_buried.all.aliases.csv: zmb_lid_open/_close, zmb_music_box,
zmb_box_spin, zmb_no_cha_ching, zmb_weap_wall, zmb_perks_packa_upgrade/
_ready/_deny/_upgrade_crnch/_knuckle_0, zmb_spawn_powerup,
zmb_powerup_grabbed, zmb_full_ammo, zmb_insta_kill, evt_nuked,
evt_carpenter_end, the double points' off). Most are in the common bank's
dump (`commonsnd_out`), the power-up spawn and grab in zm_buried's own sound
dump, the box spin a FLAC. `zombiesCodOr()` plays one, or the game's own
stand-in when its file is not there. The box's leaving sounds (poof, land,
move) are baked but unused: the box never moves (no teddy bear yet).
The power-up announcer is Richtofen, as in Tranzit, Die Rise and Buried
(Samantha is Black Ops I's and Origins'): Tranzit's `vox_zmba_powerup_*`
(`ZMSFX_ANNOUNCER` + the power-up). They are in the English-language zone,
not the map's: `--include-assets soundbank` on
`zone/english/en_so_zclassic_zm_transit.ff` dumps the
`zmb_classic_transit.english` bank (its aliases.csv and the FLACs under
`devraw/english/sound/vox/scripted/zmc/`). The common English bank dumped
under Buried's name gave only the zombies' own voices.
## Round music, going down, a power-up's hum, and sounds past 5.5 seconds (2026-10-22)

Round music is Tranzit's (the global `mus_zombie_round_over`, 9.2 s, and
`_round_start`, 14 s): `zombiesRoundMusic()` plays one as a sound with the
level's music paused for its length (`musicPauseFor()`, music.c: the sting's
pause with no track). The break between rounds is Black Ops II's ten seconds
(`ZOMBIE_ROUND_BREAK` 600, was five). Going down: `evt_player_final_hit` as a
player drops (even when it ends the game), `evt_laststand_loop` while down,
one of three `evt_player_swiped` for a blow taken (`zombiesTickLastStandSound()`,
every machine's own). A power-up lying there hums (`zmb_spawn_powerup_loop`,
`zombiesTickDropHums()`); not seen in a run yet.

**The sound player will not play anything past 5.5 seconds.**
`sndpEnvTooLong()` (n_sndplayer.c) stops a sound whose envelope is longer,
and a watchdog (`unk48`, snd.c) resets the player when one runs a second past
its envelope capped at 5.5 s. The round start played three seconds and then
the whole mix went silent. `zmSfxIsOurs()` lets these sounds through both,
as GoldenEye's bank sounds are let through the first.
## Tranzit's game over song, and finding a sound no alias names (2026-10-26)

Tranzit's game over song is in **`zmb_patch.all.sabs`** (the patch's bank),
the FLAC at byte 31153505, 21.0 s, stereo 48 kHz. No dumped aliases.csv names
it: Tranzit's own `zmb_classic_transit.all` has no `mus_zombie_game_over`
(Buried's "Dead or Undead" was used first, and was wrong). Found by sound: the
YouTube video "TranZit - Game Over Song" played in the browser pane through a
Web Audio analyser (not to the speakers), its loudness every 0.1 s, matched
against every 18-40 s FLAC stream in every `zmb_*.sabs` (each stream starts
"fLaC"; its STREAMINFO gives the length). The right one scored 0.887 lined up
at its start; every other was 0.6 or less and lined up mid-track. The banks
that had to be looked in beyond the map's own: `zmb_patch`,
`zmb_returned_tranzit`, `zmb_highrise`, `zmb_rt_load`, `zmb_code_post_gfx`.
## The zombies' cost, and the crawlers' lag (2026-10-26)

Timed in `zmcharDraw()` (`sysGetMicroseconds()`, a log line every two
seconds) on a seeded match started on round 50 with `--zombies-test-crawlers 1`
and `--zombies-test-zombie 300` (the camera among them):
- **The hitches were picture binding.** `modelpackBindMaterial()` reads and
  decodes the PNG every call, and a slot's list is built anew for every
  zombie of a new look (a crawler losing its legs, a respawn): 45 ms a build,
  a frame and a half each. The models' 116 PNGs are 16 pictures (each body
  piece has its own copy), so `zmcharBindAll()` binds them once as the
  zombies load, sharing a tile between files of the same bytes (FNV-1a), and
  each `struct zcdraw` keeps its tile (rebound only for a new pack's
  generation). A build is now 0.3-0.5 ms.
- **The steady cost is skinning**, 0.2 ms a whole zombie and 0.35 a crawler
  (three models, some 9,000 vertices) on screen. `zmchar.c` is now built at
  `-O2 -finline-functions` (it made little difference: the loop is memory
  bound); a slot is skinned once a frame however many views draw it, and one
  further than `ZMCHAR_LOD_DIST` (2000) every other frame.
**The drink clip, second look (2026-10-26):** still off. What is ruled out:
the body's joint offsets are `divebake.py`'s PD table exactly; at clip frame
32 each hand's place *from its shoulder* in the chest's frame is the
converter's to the unit (right hand (-26, 184, 216): up by the face and in
front of it, where Black Ops II's right wrist is 14 inches from the head).
The shoulders sit some 450 units lower against the chest matrix's origin
than the converter's `pos[2]` (the chest matrix is not where its offsets
start, as the throw found), which does not change the arms. Yet drawn, the
bottle hand reaches forward at chest height. So the matrices as posed are
right and something after them (the mesh's binding to the arm's second
matrices, or a later pose) is not: look there next. Watch it with
`--zombies-test-hold 2 --zombies-test-third 1 --zombies-test-turn 180
--zombies-test-drink 0`; clip frame F is level frame 218 + 2F. The grenade
throw in third person was checked and is right (`--zombies-test-throw F`
throws for player 1 every 120 frames). `--zombies-cherry-cam D` with D below
0 puts the camera in front of player 1 (but the first person drink is drawn
over it).
**Power-ups as Black Ops II's own models (2026-10-26).** The six drops draw
`zombie_ammocan`, `zombie_skull`, `zombie_x2_icon`, `zombie_bomb`,
`zombie_carpenter` and `zombie_firesale` (`zombiesRenderPowerup()`), from the
OpenAssetTools dump's `model_export/*.glb` (common_zm has the skull, x2 and
bomb; so_zclassic_zm_transit the ammo can and hammer; zm_buried the fire
sale) through `tools/zombies/glb2obj.py` into `build-dabs/zombies/models/
pu_<name>/`. Give glb2obj a folder with the .glb and an `images/` of that
zone's .dds plus common_zm's. Every one but the ammo can's body is the
material `mc_mtl_x2icon_gold`, whose picture only common_zm has: the
carpenter's, the fire sale's and the ammo can's second material came out
untextured, so `pu_x2_icon_0.png` was copied in and a `map_Kd` added to their
.mtl by hand. The diamonds over a drop are drawn only for a drop without its
model; with one, a green glow (fx_zombie_powerup_on's look) is drawn with the
Ray Gun's sprites, pulled towards the eye by most of the model's size, or the
model hid all of it but its lowest edge. `--zombies-test-powerups K` lays out
kinds K to K+3 in front of player 1 at frame 200 (`--zombies-test-turn 90` on
Complex faces open floor from the spawn). Timing is powerup_timeout's: 15 s
whole, then 40 blinks (15 at 0.5 s, 10 at 0.25, 15 at 0.1), 26.5 s in all.
**Solo Quick Revive's machine leaving (2026-10-26).** As _zm_perks'
revive_solo_fx: the machine goes once the third solo bottle is *drunk*, not
once its life is used (ours waited for that before, and then just vanished).
Two seconds on, `box_move`; it rises 40 inches over three seconds rocking
ten degrees a half-second swing about its own side axis through its foot
(`zombiesQrLeavePose()`, in the drawn matrix only: the prop and its collision
stay put until it goes); then a medium smoke burst stands in for the
poltergeist effect (effect files do not dump) with `box_poof`, and the prop is
disabled as before. `--zombies-test-qrleave F --zombies-test-perk 1` films it.
## Zombies that walk like Call of Duty's, and jump up ladders (2026-10-26)

A zombie is a simulant given AIBOTCMD_ATTACK on a player; the swipes and
their damage are ours (`zombiesTickSwings()`), but the walking was the
simulant's, and that is what looked like a simulant. Three things in it, each
behind `zombiesIsActive()` so other modes are untouched:
- **Facing.** bot.c turns a simulant to its target whenever
  `botIsAboutToAttack()` (nearly always for a zombie), while it walks its
  route: zombies walked sideways and backwards round every corner. Now a zombie
  faces the way it walks (`chrGetRotY()`) and turns to the player only within
  twice a swipe's reach and in sight (`zombiesFacesTarget()`).
- **Evades.** `botTickEvade()` (random jumps and rolls) and `botTryDodge()`
  (rolling when shot, from chraction.c) return at once for zombies. The
  navmesh brain's jump links (simbrain.c) are crossings, kept.
- **Re-pathing.** `botcmdTickDistMode()` re-issues `chrGoToProp()` every 15
  60ths for a zombie (60 otherwise): a second-old way sent it where the player
  had been.

**Ladders.** Black Ops II's zombies have no ladders: they jump up a ledge
(`ai_zombie_jump_up_48/96/127/222`, `ai_zombie_crawl_jump_up_*`: Origins'
dump has the 48 and 96, Tranzit's the 127 and 222; `vmconv.py anim` into
`characters/`, `ZMCHAR_JUMPUP`/`ZMCHAR_CRAWL_JUMPUP` at the end of zmchar's
table) and the jump down was already ours (a ladder's head is walked off).
`zombiesTickClimb()` (host, each zombie before its orders) takes a zombie
whose GOPOS has it `onladder` while standing on its floor: the ladder's head
from `cdFindLadderDist()`'s `top`, asked again above each tile; the floor at
the top past the ladder's face (the normal faces the zombie, so the ledge is
`-normal`), 1 to 3 x 45 past the face, within -60..+40 of the head, else it
climbs as before. Held there (botApplyHold) for the clip's length, drawn at
the foot carried by the clip's root with its rise stretched to the ledge's
(`zombiesCodClimbPlace()`), then `chrMoveToPos()` onto the top. Pipes (0x29)
has one a zombie reaches at round 5; `--zombies-test-follow 0
--zombies-follow-climb --screenshot-frame 99999 --zombies-shot-step 20`
films the first jump from its start. Stock (waypoint) movement only: modern
(navmesh) movement crosses its ladder links its own way.
## Black Ops II's Town as an arena (2026-10-26)

`port/src/codmap.c`: `--codmap-convert <C2M's exported_maps/black_ops_2/zm>`
writes each `g_CodMapRows` map whose C2M export is there into
`$E/mods/Black Ops II Maps/` as a Stage Loader map (bg, tiles, pads, mp setup,
and a `modconfig.txt` maps block) and exits. C2M (github.com/vugi99/C2M, the
maintained fork of Husky) reads the map out of the *running* Steam game, so the
player loads it and exports; Plutonium is a different process C2M will not
find. OpenAssetTools' dump has no level geometry, only `maps/mp/*.d3dbsp.ents`.

The first cut, only to walk round: Town is the box x -2168..4785, y
-4578..2416 inches of Tranzit's export (80,475 triangles), turned into our
axes (x, z up as y, y as -z) at 2.54 an inch about the box's middle; a 12x12
grid of 1500-unit rooms with a portal the level's height between every two
neighbours, so the portal walk and `bgFindRoomsByPos()` need nothing from the
map; each room's triangles in one flat colour a material, lit in the vertices,
no texture (the state at the head of the leaf: G_TEXTURE off, G_CC_SHADE, no
lighting, no culling); every triangle a collision tile (a floor at normal y
0.6 and over, a wall otherwise, ceilings included) in every room it reaches
within 60 of, but materials named as decals, foliage, leaves, grass, vines,
fx or sky. The players start at `town_standard_player_spawns`, stood on the
highest floor triangle under them. No waypoints yet (Zombies places its
machines and box from pads and walls, and most found no wall). Load it with
`Mod.MapMods=Black Ops II Maps` and `--boot-map "Town (BO2)"` (stage 0x51 here).

Next: textures (Greyhound's export of the search string into `_images`; the
rooms drawn at run time as `gebeanstage.c` draws an HD level, or the pictures
as mod textures), waypoints from the entity file's 5,350 `node_pathnode`s,
and Zombies' parts from its zones, spawners, barriers and box spots.
**Town's textures (2026-10-26).** `tools/zombies/codmaptex.py` gives each
material of C2M's `_matdata.json` with a colour map in the OpenAssetTools dump
(161 of Town's 163: the dump's `images/` hold them under C2M's own names, so no
Greyhound export is needed for textures) a texture number from 0x0e00 up, and
writes the mod's `textures/<num>.png` (the picture, halved to 1024 at most) and
`textures/<num>.bin` (a 32x32 placeholder of its mean colour) and
`textures/codmap.txt`. **The zlib texture record is paletted only:**
`texAlignIndices()` steps by palette indices and leaves its step unset for
any other format, so an RGBA16 placeholder came out as noise; it is
RGBA16_CI8 with one colour. codmap.c writes a G_TEXTURE on and a `0xc0`
subcommand 4 (one tile, wrap) before each material's triangles (the loader
adopts the G_TEXTURE and patches its lod), G_CC_MODULATERGB, texture
coordinates in the placeholder's texels (32 a texel) moved by whole repeats
near 0; untextured ones G_TEXTURE off and G_CC_SHADE. The PNGs are the
textures, so `texpackHaveReplacements()` serves a Black Ops II stage's own pack
whatever `Mod.LoadTextures` says (`codmapStageWantsPack()`). Materials named
caulk, shadow or clip (the "SHADOW PRIMARY" tool surface) are dropped whole.
**Town's props (2026-10-26).** `--codmap-props <Greyhound's xmodels/>` places
every model `_xmodels.json` puts in the box: 1,273 of Town's 1,639 (the other
366 are the lab's, under Town, which Greyhound did not export from Town). Each
model's least detailed OBJ (146,000 triangles in all; the most detailed would
be 744,000 against the map's 80,000), **in centimetres: Greyhound writes cm,
C2M's placements are inches** (unscaled, every prop was 2.54 times too big);
turned Rz*Ry*Rx (Maya's xyz order, as C2M's import scripts set
rotateX/Y/Z), scaled, moved, then into ours as the map is. Solid only when
over 24 inches across or high. Its materials' pictures are already PNG
(`_images/<material>/`, the .mtl's `map_Kd`); `codmaptex.py --props`
numbers them after the map's (0x0ea1 up, 289 in all). The placement JSON has
a space after each colon. Breakable storefront windows stand alone in the
street: their walls are likely script brushmodels, not in C2M's world export.
**Town's waypoints (2026-10-26).** A map's entity file has its
`node_pathnode`s and not their links (the compiler works those out), so
codmap.c links two nodes within 380 inches across and 40 up or down when no
solid triangle crosses the line between them at a knee's height or a head's
(Moller-Trumbore against a 400-unit grid of the solid triangles) and there is
floor under its middle; groups are each grid room's nodes that link to each
other (`navFindRoute()` walks a group's own links). Town: 842 nodes, 6,029
links, 171 groups; the pads file is the spawns, then a pad a node. Zombies
reached a standing player in round 1. Next, Town's layout from the entities
(Survival Town is script_string `zstandard_*`): perk machines are
`script_struct` `zm_perk_machine` (model and `script_noteworthy` specialty),
the box `zbarrier_zmcore_MagicBox` (`town_chest*`), windows
`zbarrier_zmcore_BasicWoodBarrier` (`*_baricade*`; the `labs_` ones are under
Town), wall buys `weapon_upgrade` (`zombie_weapon_upgrade`). Zombies mode
places everything from the spawn pads (`zombiesPlaceBarriers()`,
`zombiesReserveBoxPad()`, `zombiesPlacePerks()`, `zombiesPlaceWallBuys()`);
`zombiesPlaceMachineObj()` takes a pad, a wall distance and a wall direction,
so a pad on the spot with the facing passes straight through.
**Town's own Zombies layout (2026-10-26).** codmap.c writes, after the
nodes' pads, a pad for each spot of Survival Town (19: five perk machines,
Pack-a-Punch, the first `town_chest` box, twelve windows; a window's pad is
100 units inside it, inside being the side with a roof over it), and the mod's
`zombies/<arena>.txt` (`perk <id> <pad> <x> <z>`, `pap`, `box`, `barrier <pad>
<x> <z> <dist>`; x z a machine's back, or out through a window).
`zombiesLayout()` reads it once a stage (`modloaderGetStageModDir()` and
`modloaderGetStageMapName()`), and where there is one: Juggernog through
`zombiesFindJuggSpot()` (the player's place ZOMBIE_JUGG_FRONT before it); the
other perks through `zombiesPlacePerkMachine()` with a wall distance that puts
the machine on its pad, and no perk the layout lacks; Pack-a-Punch and no
cornering in `zombiesCornerMachines()`; the box's pad in
`zombiesReserveBoxPad()`; the windows built straight in
`zombiesPlaceBarriers()` (zone -1, always open). Eight windows of Town's twelve:
`ZOMBIE_BARRIER_MAX` is in the online scenario block, left alone. Not yet: wall
buys (`weapon_upgrade`, `zombie_weapon_upgrade`), and zombies coming in at the
zones' own spawners rather than the player spawns.
**Town was the wrong size: C2M's map OBJ is centimetres (2026-10-27).** Its
placements, entities and spawns are inches, and so is the row's box; the OBJ
was read as inches, so the map was 2.54 times too big and the box cut out
another patch, and every prop and entity floated or sank (props on the
ground: 6% read as inches, 60% as cm/2.54, measured against the map's floors).
Read now in cm/2.54; the box takes 136,701 triangles; rooms 1000 across (1500
put two over 32767 vertices), 18x18; and a `zmin` of -200 inches leaves out
Tranzit's lab under Town (map, props and nodes alike).
**Town's sky, fog, light, lava and holes (2026-10-27).** The maps line takes
a `fog` row (`modloaderParseFog()`): Town's is a 22000 far plane, fog from
992 of the z range, an amber sky 6b5a46 with clouds 8a7660. Light: a low sun
from the west in the vertices, and `shadePass()` halves any face with a solid
triangle over it (a ray up from its middle); **it must run before
`writeBg()`**: first run after it, it changed nothing on screen. `"grass"` in
the walk-through list made `afr_ground_grass_01` (7,753 ground triangles)
holes to fall through; only decals, `_dec`, leaves, vines, fx and atmos are
walk-through now. Lava: C2M leaves `rock_lava_molten` and `rock_lava_cooling`
empty; codmaptex.py builds them from the dump's `rock_lava_cooling_c` crust
and the `rock_lava_ember` red mask as orange heat, drawn unshaded. Its floors
carry GEOFLAG_0400 (nothing of Perfect Dark's sets it), and
`zombiesTickLava()` burns a player on one (`floorflags`, not `isfalling`; the
prop's y is no foot height) a fifth of their health every half second, the
blow's way so Juggernog counts. Exposed lava is round Pack-a-Punch; the big
lava triangles west of Town lie under the cooled crust and are never the
floor. `--zombies-test-at X,Z` (one argument, its own word) puts player 1
there at frame 150.
**Town, second look pass, and the horde put down unseen (2026-10-27).** Props
draw their LOD1 (335,000 triangles) and are solid from their least detailed
model (a second placement, `hide`: solid, not drawn); LOD1's own tiles
overran `convertTiles()` loading the tiles file. **bg.c holds at most 350
rooms** (`g_BgForceOnscreenRooms[350]`): 750-unit rooms (576) smashed the
stack, so rooms stay 1000 (18x18) and one over CODMAP_PIECE_TRIS (9000) gives
the rest to pieces after the grid, drawn with it through bg.c's
`g_BgRoomPieces` (row padding bytes = the room it is drawn with), which is
turned on for `codmapStageIs()` as for a remake (9 pieces in Town). Not
drawn but solid (`unseen`): glass, and foliage, grass and gobo cards but bark
(cut-outs the opaque rooms cannot draw: grey slabs). Not drawn at all:
`atmos_` effect cards (an orange slab). Zombies: `zombiesPutDownHorde()` kills
every bot at the start; those are now flagged `g_ZombieHidden` until
`zombiesSpawnOne()` sends them in, and `zombiesRenderCodZombie()` draws
nothing for one (`zombiesHideCorpse()`), so the match no longer opens on the
horde dropping dead round the players.
**Town's room state, and what does not work in it (2026-10-27).** A
codmap room's list must stay one cycle with no render mode of its own: two
cycles with `G_RM_PASS`/`*_OPA_SURF2` (to let `gfxReplaceGbiCommands()` fog it)
drew no room at all, and **the port's renderer knows no G_SETBLENDCOLOR
(0xf9)**: it logged "Unknown GBI opcode 0xf9" and ended every room's list
there. So there is no fog on the rooms yet, and an alpha test has only the
threshold 0, which cut nothing out of the chain-link fence: chain-link and
barbed fence materials are hidden (`unseen`) instead, as glass and foliage
cards are; decals stay drawn (hidden, one left a hole in the bank's floor).
codmaptex.py's codmap.txt has a third column, 1 when a picture is see-through
in 2% or more of it; many of Black Ops II's opaque maps (bricks, cars) are,
so only fence-like names may use it.
**The renderer takes G_SETBLENDCOLOR, and Town has fog and cut-outs
(2026-10-27).** `gfx_pc.cpp`'s interpreter had no case for 0xf9 and ended any
list holding one (stock emits it only under DEBUG line mode); it is accepted
and passed over now (the shaders' alpha test is fixed, not the blend
colour's). With that out of the way the two-cycle room state works: each
material sets `G_CYC_2CYCLE`, `G_RM_PASS` with `G_RM_AA_ZB_OPA_SURF2` (or
`TEX_EDGE2` for a cut-out) and a `..., G_CC_PASS2` combine, as a ROM room
does, and `gfxReplaceGbiCommands()` fogs it on a stage with fog. The earlier
"two cycles drew nothing" was the 0xf9 in the same list, not the cycles.
Cut-outs (fence, chain, barbed, `_holes`, leaf, branch, shrub, bush, grass but
ground) draw under TEX_EDGE2: the shaders drop texels under 0.19 alpha. Only
glass and gobo cards are hidden now.
**Town's glass and decals, blended (2026-10-27).** A room now has a second
leaf (the room header's +12, a 20-byte leaf after the first: list, and its
own vertex and colour bases at its first vertex, so its G_VTX offsets count
from there): the translucent one, which bg.c draws after the props. Triangles
sort opaque first, then glass (`blend` 1, G_RM_AA_ZB_XLU_SURF2), then decals
and `_dec` (`blend` 2, G_RM_AA_ZB_XLU_DECAL2), both `G_CC_MODULATERGBA`; the
opaque list ends at the first blended triangle. The bank's floor by its
papers has no floor in C2M's export: the decal drawn opaque had hidden the
hole, blended it shows the fog colour through.
**Falling through Town (2026-10-27).** Two causes, two fixes. A flat decal is
sometimes the only floor C2M exported (the bank's papers): `writeTiles()`
makes any upward-facing decal (`blend` 2, normal y 0.6 and over) a floor tile
(4,900 more in Town). And for holes the export left, `zombiesTickFallNet()`
(Black Ops II stages only, `codmapStageIs()`) keeps each player's last place
standing on real ground (not falling, `vv_ground` over -29999) and puts them
back there once they have nothing under them (**`vv_ground` is held at
exactly -30000 for none**) or have fallen for two seconds, before
bondwalk.c's four-second fall death. `--zombies-test-under` with
`--zombies-test-at` starts a player falling under the floor there.bondwalk.c's four fall deaths (four seconds of fall, and three
`vv_manground <= -30000` checks that kill at once, missed by the tick's net)
now ask `zombiesFallNetCatch()` first, which on a Black Ops II map in Zombies
puts the player back at their last safe place and cancels the fall. Placed at
8500,8500 (outside Town, ground -2^32) the player used to die on the spot.
## A Black Ops II map's rooms popping in and out (2026-10-27)

codmap.c's rooms are squares of a 1000-unit grid and its portals the squares'
open sides. bgTickPortalsWalk() narrows the screen box at every portal, so on
Town whole squares the camera could see were dropped and came back as the
player walked ("when I walk around it just bugs out"). A codmap stage now goes
through bgTickPortalsSpectate() instead (every room in the screen box, cut at
the far plane, which the fog hides), with the 200-room load budget.

That drew past the frame's vtx pool (Town wanted 2 MB of a 1.6 MB pool), and
anything past the pool's guard is served from ONE scratch block every such
allocation shares (gfxVtxTake()). roommesh.c's gfxmeshdraw record landed there,
was written over before the renderer read it, and gfx_mesh_begin() crashed on a
pointer of -1. roomMeshBegin() now leaves a room on the CPU path when its record
is scratch (gfxAllocationIsScratch()), and a codmap stage starts with three
times the pool. Any new record the renderer follows a pointer out of, taken
from gfxAllocate(), needs the same check.

## A mod map's pictures preloaded, a player off the map, and the names (2026-10-27)

**Preload (port/src/mappreload.c).** Choosing a mod map (the arena list, or
--boot-map) queues its mod's every texture-pack picture for decoding
(texpackPreloadStage()); they land in texpack's kept store through the frame's
poll, the same as when first drawn. A box at the bottom right (drawn at the end
of lvRender(), over menus and matches) shows the map's name and the share kept;
mappreloadFinish(), at the stage load in pdmain.c, waits for the rest (at most
30 s). Town: 291 pictures, 190 MB kept, 0.7 s from a cold start - the kept
store's 512 MB budget (Mod.TexturePackCacheMB) holds it. `--mappreload-show`
holds the box at half to look at it. The mod's index is built by
texpackModUse() at the preload: a stage load that mounted the mod's directory
at another index would rebuild it and drop the kept pictures.

**Off the map.** A player who went through a floor onto something under it, or
off the map's edge, stands on ground, so the fall net (ground -30000 or a long
fall) never caught them: from there nothing of the map is drawn (its faces
point up, or it is past the fog) and the zombies, whose rooms are not in view,
vanish too - just the sky and the fog colour. zombiesPlayerInBounds() asks the
waypoints (Black Ops II's path nodes cover every floor a zombie walks): more
than 300 under the lowest within 1000 of the player, or none within 2500, is
out; a second out puts them back where they last stood in bounds, and a spot
out of bounds is never recorded as the one to come back to.

**Names.** The arena tab is "Custom Maps" (arenatabs.c TABKEY_MODS; the flat
list's group too, setup.c), and an arena whose map name already ends in ")"
- "Town (BO2)" - is not given its mod's name after it (modloader.c).

## The zombie AI against Black Ops II's own scripts (2026-10-27)

Black Ops II's Zombies rules are in its GSC scripts. Decompiled copies are
published by Plutonium (github.com/plutoniummod/t6-scripts, under
ZM/Core/maps/mp/zombies/): _zm.gsc (round flow), _zm_spawner.gsc (spawning,
attacks), _zm_ai_basic.gsc (the chase), _zm_utility.gsc (run speeds),
_zm_zonemgr.gsc, _zm_blockers.gsc. They are Activision's code: read them from a
scratch folder, never commit them; our C follows their rules in our own words.
The movement itself (pathing, steering) is engine code, not in any script.

Checked and already the same: the round's zombie count (default_max_zombie_func
and round_spawning's multiplier), the spawn delay (2 s, 0.95 a round, 0.08 the
least), health (150, +100 to round 9, then +10%), 24 out at once, ten seconds
between rounds. The chase in solo is the nearest player, picked again every 1
to 3 seconds (find_flesh), which a single player makes moot.

Changed to theirs:
- **Run speed** (set_run_speed()): zombie_move_speed is 1 in round 1, then the
  last round's number times 8; a zombie draws from it to 34 above, walking to
  35, running to 70, sprinting past it. Ours was the round times 8 give or
  take 20, a round early.
- **The last zombie sprints** (zombie_speed_up()): from round 4, once 4 or
  fewer are left to come and 3 or fewer are out, the one left alone sprints.
  zombiesTickSpeedUp().
- **Stuck zombies** (round_spawn_failsafe()): every 30 s (40 for a crawler) one
  that has gone under 24 inches is killed with no points and put back in the
  count, unless it was at a barrier's boards in the last 8 s.
  zombiesTickFailsafe().
- **The swipe**: meleeattackdist 64 inches (ZOMBIE_COD_REACH 162.6, was 150),
  meleedamage 60 of a player's 100 (ZOMBIE_REACH_BLOW 0.60, was 0.51; still
  two hits down, five with Juggernog's 250).

**Windows** (_zm_blockers.gsc): a board goes up the moment Interact is held,
then one a second (do_post_chunk_repair_delay(), ZOMBIE_BARRIER_FIX60 60, was
33), and Speed Cola does not quicken it: the wait takes no perk, and
replace_chunk()'s quicker board asks for "speciality_fastreload", a perk
nobody has. Ten points a board, counted first and paid while under the
round's cap, 50 times the round up to 500 (it was 500 from round 1). Tearing
is the tear animations' own pace already. **Several zombies at a window** (blocker_attack_spots()): three spots, the middle
then 22 inches to each side (the count and spacing are the window asset's,
not in the scripts: ZOMBIE_BARRIER_SPOTS, ZOMBIE_BARRIER_SPOTSIDE). Each spot
is its own zombie, tear clock, swipe and climb (struct zombiebarrierspot). The
boards are still a stack that comes off from the top: a zombie takes hold of
the highest board no other has (BO2's "targetted_by_zombie"), and whoever pulls
first pulls the top one, one that held it taking the puller's board instead.
With every board gone the others climb in too, each at its own offset. Online
the scenario block carries the middle spot only (its layout unchanged); the
side spots are the host's. `--zombies-test-barrier N` logs a side spot taken.

**Spawning** (_zm_zonemgr.gsc's manage_zones()): zombies come in only in the
active zones, the ones a player stands in and those joined to one by a door
bought (zombiesFindActiveComps(), the player's zone by the nearest waypoint),
not every zone opened so far. A player's own spawn and the zombies' scatter
when everyone is down still use every open zone. With no pad in an active zone
the open ones are used, never one behind a shut door. An arena without doors is
one zone: there the barriers near a player are still preferred. The log says
"player 1 is in zone N" as it changes. Carpark (--boot-stage 0x3d) has locked
doors to try it on.

**Health** (_zm_playerhealth.gsc's playerhealthregen()): 2.4 seconds after the
last hit health is full again at once; if it went to a fifth or less ("very
hurt", which lasts until it is full) it waits until 5 seconds after the hit and
refills a tenth every 0.05 s. Ours refilled from nothing over 1.25 s after the
2.4. The script's "player_is_invulnerable" flag is checked by nothing in
Zombies; the one guard is the same zombie not landing again for 0.4 s
(remove_ignore_attacker()), which our 1.5 s between swipes covers.
`--zombies-log-regen` logs each refill. Without Juggernog very hurt is never
reached: a blow leaves 40%, the next downs.

Testing Zombies on a stock arena: `--boot-stage 0x3c --mpsims 1 --mpscenario 6`
(Warehouse). `--boot-map` knows only mod maps, and --mpscenario does nothing
without --mpsims.

## Windows in the walls (2026-10-27)

A window is a hole in the wall with its pen behind, as Call of Duty's, on a
renderer with a stencil (OpenGL; `bar->inwall`). Perfect Dark's walls are a
surface with nothing behind, so the hole is cut on the screen each frame, not
in the level:

- **The cut** (`zombiesRenderWindowHoles()`, called from bgRenderScene() right
  after a room's opaque pass, for the windows whose wall is in that room): the
  opening, a unit in front of the wall, is drawn with `G_STENCIL_MARK_EXT`
  (stencil set where it passes the depth test, no colour: whatever stands in
  front still hides the window), then again with `G_STENCIL_PUNCH_EXT` (only
  where marked, the mark cleared, the depth range squeezed to 1 so the far
  plane's depth is written). gbiex.h's `gSPStencilEXT`, gfx_pc.cpp's
  `G_STENCIL_EXT`, the API's `set_stencil_mode` (OpenGL only; Vulkan's is null
  and its windows stand out from the wall as before). The depth clear clears
  the stencil too. A frame that ends with the stencil on is logged and closed.
- **Draw order**: rooms and props are drawn a room at a time, and most props
  (zombies too) before their room's walls (RENDERPASS_OPA_PREBG). The window's
  pen and a zombie at an in-wall window get PROPFLAG_RENDERPOSTBG
  (zombiesTickBarriers()), so they come after the cut; the pen is a closed box
  behind the opening, so the rooms drawn after cannot show in the hole.
- **Where things are**: the window's origin, the sill, is `bar->origin`,
  ZOMBIE_BARRIER_INWALL (28 inches behind the wall's face, the boards on it)
  instead of ZOMBIE_BARRIER_ORIGIN (out in the room); ZB_INSIDE()/ZB_FRONT()
  follow it. The zombie's bot still stands in front of the boards (its spot's
  stand) and is drawn behind in the window's frame (zombiesCodBarrierPlace());
  while it is there its perimeter is off (nothing to walk into). One killed
  there is drawn where it fell, behind the wall (`g_ZombieDiedBehind`), until it
  comes again.
- **Shots**: bgTestHitInRoom() lets a shot through a wall where it meets an
  in-wall window's opening (`zombiesWindowHoleAt()`), so the zombie behind is
  hit; `--zombies-test-shoot` logs "through window N's hole". chrHit() no
  longer bruises a zombie drawn as Black Ops II's (its Perfect Dark model is
  not what is seen, and walking its lists crashed on such a shot).
- Recesses the arena already has are still preferred for a window
  (zombiesFindNiche()); without a stencil, a window on a plain wall is built out
  with a block of wall round it.

## Power-ups at Black Ops II's size, and test windows that must not be minimized (2026-10-28)

Black Ops II spawns a power-up as a plain script_model 40 inches over the drop
point and never scales it (_zm_powerups.gsc). Our power-up meshes are a unit to
the centimetre, as every Call of Duty model here is (Juggernog's machine is 253
tall, Call of Duty's 100 inches), so zombiesRenderPowerup() draws each at its
own size (`k = 1 / ZOMBIE_MESH_FIXED`): Max Ammo 60 long, skull 59, Double
Points 55, Carpenter 77, Nuke 108, Fire Sale 195. They had each been squeezed
to 40. The middle floats ZOMBIE_POWERUP_FLOAT (40 inches) over the floor, the
glow is sized to the model (zombiesPowerupLongest()), and the pick-up range is
powerup_grab()'s 64 inches from the feet (ZOMBIE_DROP_RANGE 127 sideways).

**A test run started minimized (`Start-Process -WindowStyle Minimized`) on
Windows draws nothing on OpenGL**: --screenshot-frame then saves black, or
black over coloured noise, from frames the game drew correctly. Today's chase
of "black frames" near the in-wall windows was only that: the same runs in a
normal window are fine, and `--zombies-no-holes` (no window cut) was as black
minimized. Start screenshot runs in a normal window.

## The Mystery Box moves, as Black Ops II's (2026-10-28)

From _zm_magicbox.gsc: the spin is 4 s (40 cycles), a gun waits 12 s
(treasure_chest_timeout()). The box has up to ZOMBIE_BOX_SPOTS places
(zombiesReserveBoxPad(): its first, then each pad furthest from those taken,
while 6 spawn pads stay for the zombies). A spin ends on the teddy bear by
treasure_chest_weapon_spawn()'s odds (zombiesBoxRollsTeddy(): not before 4
guns since the last move, 15% to the 8th, sure at the 8th before any move,
then 30%/50%); never on a Fire Sale. The player gets the cost back; the bear
(zombies/models/teddybear, Buried's zombie_teddybear via glb2obj.py, drawn in
place of the gun object) waits 2.5 s and rises 500 inches over 4 s; the box
goes for 7 s (propDisable) and comes back at another place (zombiesBoxPutAt()).
Guns offered off a Fire Sale count toward the move. Checked against the
scripts and already the same: perk prices and limit, Quick Revive's solo 500,
Pack-a-Punch 5000 and its ammo, the box's 950 and Fire Sale's 10, power-up
timers and 4 drops a round, kill points. Kept on purpose: the Ray Gun's 1 in 8
(BO2 picks evenly; our pool is larger and a tester never saw one). Not synced
online: a client keeps the box where it started. Test: `--zombies-test-box`
(camera on it), `--zombies-test-teddy F` (a spin at frame F that must move it).

**The box's own model** (2026-10-28): p6_anim_zm_magic_box_lod0.glb (TranZit's
zone; its pictures ~-gp6_zm_magic_box_c, _hay_c and generic_objective are in
Buried's images/) through glb2obj.py into zombies/models/magicbox/: 243 x 67 x
49 cm. zombiesRenderBoxMesh() draws it over the crate, which stays for
collision at ZOMBIE_BOX_LENGTH 243; its floor is g_ZombieBoxFloor, and with the
model the gun floats 40 inches over the floor as Black Ops II floats it.

**The box's own animations** (2026-10-28): its skeleton is tag_origin,
tag_animate (the body, 794 triangles) and j_hinge (the lid, 519, hinged 17.34
inches up and 12.26 back). glb2obj.py `--joint=NAME` writes one bone's
triangles (zombies/models/magicbox_body, magicbox_lid); vmconv.py `anim` writes
o_zombie_magic_box_{open,close,leave,arrive,weapon_rise,teddy_rise}.zan into
zombies/characters/, loaded as ZMCHAR_BOX + ZMCHAR_BOX_*. zmchar.c keeps a
j_hinge track beside tag_animate's; zmcharAnimBox() reads the body's or the
lid's. zombiesRenderBoxMesh() draws the two pieces rigidly (no skinning: each
vertex is one bone's), the lid turned about the hinge by j_hinge, both moved by
tag_animate. Played: open on a buy, close on a take or a timeout, leave after
the teddy bear (7.5 s, then the poof and the box put at its next place), arrive
there (5 s, the thud on its box_impact note at frame 140). The guns switch on
o_zombie_magic_box_weapon_rise's 40 "switch" notes (g_ZombieBoxSwitches). The guns going
round rise by weapon_rise's own motion from the box's floor
(magic_box_do_weapon_rise(): BO2 carries them on zbarrier pieces 3 and 4, then
spawns the gun 40 inches up and sinks it 85% over its 12 s, as ours does).
Not used: teddy_rise's motion (BO2's script moves the bear with moveto()).

**The box's lights** (2026-10-28): Black Ops II puts them on with effects
(default_pandora_fx_func()'s lght_marker on a tag_origin over the box, and
treasure_chest_glowfx()'s magicbox_glow client field, fx_zmb_tranzit_marker_glow),
whose files are not in the dump. zombiesRenderBoxGlows() draws them with the Ray
Gun's glow sprite, additively, in the effects pass beside the power-ups' glow:
the beam, upright and turned to the camera, pale blue, 1600 up in overlapping
100-high pieces (one quad the beam's height is the sprite's bright middle far
over the box and nothing at it) and fading to its top, while the box is at its
place; and with the lid up a warm glow at its mouth and a short shaft. The "?"
marks have no glow map of their own (the box's generic_objective material is a
black placeholder for Black Ops II's objective outline).

**The box's other places are taken after the windows** (2026-10-28):
zombiesReserveBoxSpots(), called after zombiesPlaceBarriers(), not inside
zombiesReserveBoxPad(). Taken first, the three pads furthest apart were the
ones the windows stand beside, and Warehouse came up with no window at all.
They now also keep 350 off any window or wall buy. Checks done the same day:
a spin landing on a gun (`--zombies-test-boxspin F`) floats it 39.4 inches over
the floor at the end of weapon_rise, as BO2's 40; windows in walls on Carpark
and Villa; windows per arena 3, 2, 5, 3, 3 (Warehouse, Carpark, Complex, Villa,
Skedar). `--zombies-shot-step` now works with --zombies-test-barrier and
--zombies-test-zombie too.

## Pack-a-Punch as Black Ops II's script has it (2026-10-28)

Our machine is the buildable one (p6_anim_zm_buildable_pap_on, TranZit and
Buried); the dump has no animation of its own for it (the PaP clips there,
fxanim_zom_tomb_packapunch_pc*, are Origins' machine's). What Black Ops II
moves is the gun (_zm_perks.gsc's third_person_weapon_upgrade(),
pap_weapon_move_in() and _out()): out in front 25 inches (interact_offset) and
held half a second, in over half a second, the upgrade sound as it gets in,
ready 4.35 s after it came out, out again over half a second, then all the way
back in over the 15 s to take it. ZOMBIE_PAP_HOLD60 30, ZOMBIE_PAP_WORK60 261
(was 180), ZOMBIE_PAP_INAT for the sound (it played as the gun set off), the
offer's creep to fully in (it stopped a quarter out). `--zombies-test-pap F`
puts player 1's gun in at frame F; watch with `--zombies-test-perk 9`.

## Stuck zombies (2026-10-28)

`--zombies-test-stuck` (player 1 unhurt; a zombie away from windows and player
1 that has gone under 2 feet in 5 s is logged once a spot, with its room and
command) over 90 s on five arenas. What it found: round_spawn_failsafe()'s 30
second look sent back zombies swiping at a player who stood still - all six on
Carpark - so a zombie within ZOMBIE_FAILSAFE_FIGHT (330) of a player who is up
is never counted stuck; and the zombies really stuck, away from every player,
stood there the whole 30 s, so ZOMBIE_UNSTICK60 (10 s) sends one back sooner
(killed with no points, put back in the count, as the failsafe). Snags seen
again and again, for a fix to the arenas' paths some day: Warehouse 3842 95 -727
(room 45) and 4145 296 1242 (room 44); Complex -2394 107 -1462 (room 31) and
-4434 67 -1022 (room 20). The failsafe's log names the window and spot a zombie
was held at, -1 when none.

**The knife no longer reloads the gun** (2026-10-28): the quick knife swaps to
the Combat Knife and back, and Perfect Dark's unequip puts a gun's loaded
rounds back in the reserve (bondgun.c, the loop over loadedammo) while the
equip loads a full magazine out of it (bgun0f098df8(); only the magnums,
shotgun and crossbow remember theirs, WEAPONFLAG2_UNEQUIPPEDRELOAD). Each
hand's loaded count is kept as the knife comes out (g_ZombieKnifeLoaded) and
zombiesKnifeKeepLoaded() puts the magazine back to it once the gun is up again,
the rest back in the reserve. `--zombies-test-knife F`: 3 loaded, a knife at
frame F; the log says what it came back with (8) and was put back to (3).

## The players' own spawn points (2026-10-28)

As Call of Duty's: every player starts, and comes back, at one spot of four
together (zombiesReservePlayerPads(), zombiesPlayerSpot()): one spawn pad of
the start zone, the one nearest the middle of the zone's pads that has no wall
near it (one no window could use), kept back from the spawn list, and three
more 90 out ahead, left and right of it where the floor goes on and no wall is
between; player N at the Nth (zombiesChooseSpawnLocation()). Windows keep
350 off the pad. They had spawned at any pad of the open zones, the windows'
own among them. Taking four whole pads instead cost Warehouse and Villa every
window - an arena has 10 to 17 spawn pads, and machines, wall buys, windows
and the box's places all want them. Even one shifts which feature gets which
wall: windows came out Warehouse 2, Carpark 1, Complex 4, Villa 1, Skedar 3
(were 3, 2, 5, 3, 3), Villa's walls going to its wall buys.

## The round's opening, and no "Zombies" box (2026-10-28)

Black Ops II opens a match on "Round" and a big number in the middle of the
screen, then the number shrinks into the bottom-left corner. `zombiesRoundIntro()`
and `zombiesRenderRoundIntro()` in zombies.inc do that: the HUD is not drawn
while `TICKMODE_MPSWIRL` turns the camera round, so the first first-person frame
starts it (a flag, reset when `lvframenum` goes backwards). The stock scenario
name box ("Zombies", bottom left, scenarios.c) is skipped for Zombies.

The middle of the screen under the HUD's left alignment is not `viewWidth/2`:
the alignment is an offset (the pillar's width, cut to 16:9's under
`G_ASPECT_WIDE_EXT`, none when centred), and the view is 320x220, not 4:3.
Taking it as 4:3 put the title ~90px right of centre at 1920x1009.

## A gun's damage on a zombie is Black Ops II's weapon file (2026-10-28)

Perfect Dark's damage numbers were made for simulants: by round 10 a zombie
has some seven times a round 1 zombie's health, and Perfect Dark's rifles hit
barely harder than its pistols, so the assault rifles "did nothing".
`zombiesBulletDamage()` (zombies.inc, called from chrDamage() in place of the
old `zombiesBulletScale()` doubling) gives each Perfect Dark gun the weapon file
of the nearest Black Ops II gun, read out of the dump
(`zone_dump/t6/zm_*/weapons/<gun>_zm`, backslash-separated key\value): `damage`,
`minDamage`, `maxDamageRange`/`minDamageRange` (inches, ×2.54 for our units),
and `locHead`, `locTorsoUpper`, `locTorsoLower`, `locRightArmUpper`.
`_zm.gsc`'s actor_damage_override passes that through untouched but for the
perks, so nothing else multiplies it. Pack-a-Punch takes the `_upgraded_zm` file
instead of doubling. The shotgun's six pellets share the 870's `shotCount` (4).
Guns with no row (launchers, tranquilizer, laser) keep the old path.
`--zombies-log-damage` prints each hit in Black Ops II's units; with
`--zombies-test-zombie 300 --zombies-test-shoot head` the Falcon (M1911, 20,
head 3.5) takes three to kill a round 1 zombie, as Black Ops II's does.

## The drink and the throw with a gun that has no hands (2026-10-28)

The first-person drink and grenade throw are drawn on the character's hand model
(`zombiesDrinkHandModel()`). A gun without WEAPONFLAG_HASHANDS (the RC-P120, a
GoldenEye gun with its glove) never loaded one, so the drink fell back to the
HUD's bottle. In Zombies the gun loader (bondgun.c) now loads the hands for every
gun and sets up the right hand's model, but keeps its command list in
`g_BgunSpareHandCmds` (`bgunSpareHandCmds()`) instead of `unk0dd0`: a set
`unk0dd0` is what draws hands on the gun. `zombiesHandsSetUp()` runs that list
before the drink, throw or knife draws the hands. Test with
`--zombies-test-hold 13 --zombies-test-third 0 --zombies-test-drink 0`.

## The box's teddy facing out, and its fakes at the places it is not (2026-10-28)

The teddy bear is drawn at the box's yaw + pi (it faced along the box before,
then backwards). Black Ops II's zbarrier `zmcore_magicbox` has as its first piece
`p6_anim_zm_magic_box_fake`: footlockers with a teddy on them, shown at each box
place without the box. It is converted from Buried's `model_export` into
`build-dabs/zombies/models/magicbox_fake/` (its pictures `~-gzombie_teddybear_c`
and `~~-gp6_zm_footlocker_old_c-rg~8bd9e7e1` copied into the input's images/).
`zombiesCacheBoxFakes()` stands the box at each place once (zombiesBoxPutAt())
and puts it back; `zombiesRenderBoxFakes()` draws them from bg.c's room pass.
`--zombies-test-fake K` circles place K. Not yet checked: the footlocker reads
greyish and see-through, and it may want its own colour map looked at.
**Town from the newer C2M (2026-10-28).** C2M's update writes, in
`exported_maps/<game>/<map>/`, `<map>.obj` **in inches** (the old one was cm),
`_static_models.json` (Name, then Position, RotationDegrees and ModelScale, each
an object of X/Y/Z), `_materials.json` (a material's colour map at
Textures/colorMap/Name, the props' materials with the map's) and
`_entities.txt` (the old `_mapEnts.txt`'s format); every model goes flat into
`exported_models/<game>/` as `<name>.obj` (LOD0) and `<name>_LOD1..3.obj`, in
inches; the pictures as PNG in `exported_images/<game>/`. codmap.c sets
`g_NewC2m` when `_static_models.json` is there and reads all of it, the models
from `../../exported_models/<game>` by default; codmaptex.py takes the pictures
from `exported_images` first (Town: 393 materials, none missing, no `--props`
pass). Town: 1,272 props, 132 placements with no model (the lab's, which Town
cannot reach). A standalone map (Nuketown) exports every model; a piece of a
bigger one exports only what the game has loaded, and Town's 127 of 150 was all
that is reachable. **The converter's window says "Not responding" for minutes
while it works: closing it there loses the run silently.**
**Town's fog.** A fog row's fogmin/fogmax are depth-buffer thousandths,
z' = far(d - near) / (d(far - near)): `992 1000` with near 15 and far 22000 was
whole only at the far plane, and the box's cut edge (8,800 units from its
middle) showed through. `988 998` starts at about 1,200 units and is whole by
5,600, as Town's own haze hides the rest of Tranzit.
