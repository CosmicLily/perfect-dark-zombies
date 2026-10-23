
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