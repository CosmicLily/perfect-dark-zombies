#ifndef _IN_ZMCHAR_H
#define _IN_ZMCHAR_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "types.h"

/**
 * Black Ops II's own zombies (zmchar.c): a body and a head, each a skinned
 * model (tools/zombies/vmconv.py's .zvm, in the build's zombies/characters/),
 * posed by Call of Duty's animations (.zan) and skinned on the CPU, drawn in
 * place of a zombie's Perfect Dark body (zombies.inc).
 *
 * Everything here is in Call of Duty's own space: inches, x the way the
 * zombie faces, y its left, z up, the origin between its feet. The caller's
 * matrix takes that into the world.
 */
enum {
	ZMCHAR_IDLE,
	ZMCHAR_WALK,
	ZMCHAR_RUN,
	ZMCHAR_SPRINT,
	ZMCHAR_DEATH_A,
	ZMCHAR_DEATH_B,
	ZMCHAR_DEATH_C,
	ZMCHAR_DEATH_D,
	ZMCHAR_ATTACK,
	ZMCHAR_ENTER,                                  // over a barrier's sill once its boards are gone
	ZMCHAR_STAND_ATTACK,                           // a swipe standing; ZMCHAR_ATTACK is one walking
	ZMCHAR_CRAWL,                                  // a crawler's ...
	ZMCHAR_CRAWL_SPRINT,
	ZMCHAR_CRAWL_ATTACK,
	ZMCHAR_CRAWL_DEATH_A,
	ZMCHAR_CRAWL_DEATH_B,
	ZMCHAR_WINDOW_L,                               // a swipe through a barrier's boards, each arm
	ZMCHAR_WINDOW_R,
	ZMCHAR_CRAWL_ENTER,                            // a crawler's climb in
	ZMCHAR_JUMPDOWN,                               // + one of 6: down a ledge, by its height (zmcharJumpDownHeight())
	ZMCHAR_CRAWL_JUMPDOWN = ZMCHAR_JUMPDOWN + 6,   // + one of 5: a crawler's
	ZMCHAR_TEAR = ZMCHAR_CRAWL_JUMPDOWN + 5,       // + board * 3 + ZMCHAR_TEAR_*: a board torn off
	ZMCHAR_CRAWL_TEAR = ZMCHAR_TEAR + 6 * 3,       // + board * 3 + ZMCHAR_TEAR_*: a crawler's
	ZMCHAR_BOARD_PULL = ZMCHAR_CRAWL_TEAR + 6 * 3, // + board: the board's own, going
	ZMCHAR_BOARD_REPAIR = ZMCHAR_BOARD_PULL + 6,   // + board: and coming back
	ZMCHAR_ANIMCOUNT = ZMCHAR_BOARD_REPAIR + 6
};

enum {
	ZMCHAR_TEAR_GRAB,
	ZMCHAR_TEAR_HOLD,
	ZMCHAR_TEAR_PULL
};

#define ZMCHAR_SLOTS 64 // zombies drawn at once, each with vertices of its own

/**
 * What a zombie is drawn as: one of Tranzit's ten bodies (two in a
 * straitjacket, three in a hospital gown, five in a worker's clothes), one of its four heads, whole or a
 * crawler with its legs blown off (the body's _g_upclean and the stumps). A
 * body or head whose files are not there is drawn as the first.
 */
#define ZMCHAR_BODIES 10
#define ZMCHAR_HEADS  4
#define ZMCHAR_LOOK(body, head, crawler) ((body) | (head) << 4 | ((crawler) ? 0x100 : 0))

// ... and what has been shot off it, or'd in: an arm (Call of Duty's
// _g_larmoff or _g_rarmoff torso over its _g_lowclean legs) or the head
#define ZMCHAR_LOOK_NOLARM 0x200
#define ZMCHAR_LOOK_NORARM 0x400
#define ZMCHAR_LOOK_NOHEAD 0x800

// Whether the zombie's files are there (read the first time it is asked)
bool zmcharReady(void);

// An animation's length in its own frames (0 when it is not there) and its
// frames a second
f32 zmcharAnimFrames(s32 anim);
f32 zmcharAnimRate(s32 anim);

// How far, in inches, an animation's loop carries the zombie a second (its
// root motion, Call of Duty's: the walk 37.6, the run 108, the sprint 142)
f32 zmcharAnimSpeed(s32 anim);

/**
 * Where an animation's root is at frame, in inches (its tag_origin motion,
 * plus tag_animate's place on a board's), and tag_animate's turn (x y z w;
 * none for a zombie's). False when the animation carries neither. A board
 * tear's and a board's are aligned to the barrier: from its origin, the
 * window's sill.
 */
bool zmcharAnimRoot(s32 anim, f32 frame, f32 pos[3], f32 q[4]);

/**
 * The jump down a ledge nearest a drop of inches, a crawler's or not (an
 * animation, -1 none there), and where in it the zombie leaves the ledge and
 * lands (frames: its root going down, and all the way down)
 */
s32 zmcharJumpDown(f32 inches, bool crawler, f32 *leave, f32 *land);

// A joint of the zombie's skeleton by name (j_head, j_knee_le ...), -1 none,
// and its parent
s32 zmcharJoint(const char *name);
s32 zmcharJointParent(s32 joint);

// Where a slot's joints were as it was last drawn (inches, Call of Duty's
// space, as zmcharDraw()'s mtx takes them), NULL when it has not been
const f32 (*zmcharSlotPose(s32 slot))[3];

/**
 * One zombie, slot its own (0 to ZMCHAR_SLOTS - 1) and drawn as look (a
 * crawler as a whole one when its pieces are not there): posed by anim at frame
 * (blended over from `from` at its frame `fromframe` by blend, 1 all `from`),
 * drawn under mtx (Call of Duty's space into the view, a float matrix) in
 * the room's light (prim). The gdl, or as it was when the zombie is not there.
 */
Gfx *zmcharDraw(Gfx *gdl, s32 slot, s32 look, Mtxf *mtx, s32 anim, f32 frame, s32 from, f32 fromframe, f32 blend,
		u8 r, u8 g, u8 b);

#endif
