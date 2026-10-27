/**
 * Black Ops II's own zombies (zmchar.h): Tranzit's regular zombie, a body and
 * a head, posed by Call of Duty's animations and skinned here, on the CPU.
 *
 * The files are tools/zombies/vmconv.py's: a .zvm is a skinned model (each
 * bone's name, parent, rest and inverse bind; each vertex's place, normal,
 * picture coordinate and four bones with their weights; its triangles; a
 * draw per picture), a .zan an animation (per bone, rotation and translation
 * keys). tools/zombies/zmpose.py draws the same posing to a PNG, and this
 * follows it step for step:
 *
 * - The head is hung on the body as Call of Duty attaches one: a bone of the
 *   head's that the body has too (j_head, j_neck ...) is the body's, the rest
 *   follow their parents. Body and head are one skeleton by name.
 * - A track's keys hold at either end; rotations are nlerped.
 * - j_mainroot's animated translation is an offset from its rest (the walk's
 *   is near 0, the rest 37 inches up); every other bone's is where it is.
 *
 * A zombie draws through the batched, textured lists the machines use
 * (zombies.inc, zombiesMeshBuildFrom()): sixteen vertices a load, the
 * pictures bound through the model packs' stand-in tile. Each zombie slot
 * has its own vertex and colour arrays and its own list over them, built the
 * first time the slot draws; each frame the slot's vertices are skinned into
 * its arrays and its list is called.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "system.h"
#include "fs.h"
#include "gbiex.h"
#include "objmesh.h"
#include "modelpack.h"
#include "xblatex.h"
#include "zmchar.h"

#define ZMCHAR_DIR       "$E/zombies/characters/"
#define ZMCHAR_MAXBONES  160
#define ZMCHAR_BATCH     16   // vertices a G_VTX loads
#define ZMCHAR_MAXTRIS   64   // triangles a batch gathers
#define ZMCHAR_FIXED     4.0f // a vertex's units an inch
#define ZMCHAR_ROOTBONE  "j_mainroot"

struct zcbone {
	char name[32];
	s32 parent;
	f32 t[3];
	f32 q[4];
	f32 ib[3][4];
	s32 joint; // its bone in the joined skeleton
};

struct zcvert {
	f32 p[3];
	f32 n[3];
	f32 uv[2];
	u8 j[4];
	f32 w[4];
};

struct zcdraw {
	u32 first;
	u32 count;
	u32 flags;
	struct objmaterial mat;
	// its picture as bound, kept: binding reads and decodes the PNG again,
	// some 12 ms, and a slot built anew for a zombie of another look (a
	// crawler's legs gone) hitched the frame by a frame and a half
	const void *tile;
	u32 tilegen; // modelpackGetGeneration() when it was bound
	bool tiled;
};

struct zcmodel {
	s32 numbones;
	struct zcbone *bones;
	s32 numverts;
	struct zcvert *verts;
	s32 numtris;
	u16 *tris;
	s32 numdraws;
	struct zcdraw *draws;
};

// a key: a frame and a value (four for a rotation, three for a translation)
struct zckey {
	u16 frame;
	f32 v[4];
};

struct zctrack {
	s32 numrot;
	struct zckey *rot;
	s32 numtrans;
	struct zckey *trans;
};

struct zcanim {
	bool ok;
	f32 frames;
	f32 rate;
	struct zctrack tracks[ZMCHAR_MAXBONES]; // by joint
	struct zctrack delta;   // "@delta": the root's own motion
	struct zctrack animate; // tag_animate: a board's
};

// the joined skeleton: each joint's parent and rest, from whichever model has it first
struct zcjoint {
	char name[32];
	s32 parent;
	f32 t[3];
	f32 q[4];
};

struct zcslot {
	bool built;
	s32 key;       // the models its list draws (zmcharLookKey())
	Vtx *vtx;
	Col *col;
	Gfx *gdl;
	u8 *srcmodel;  // each of vtx's model (its place in the look's), and its vertex there
	u16 *srcvert;
	s32 numvtx;
};

static const char *const g_ZmcharAnimNames[ZMCHAR_TEAR] = {
	"ai_zombie_idle_v1_delta",
	"ai_zombie_walk_v1",
	"ai_zombie_run_v2",
	"ai_zombie_sprint_v1",
	"ch_dazed_a_death",
	"ch_dazed_b_death",
	"ch_dazed_c_death",
	"ch_dazed_d_death",
	"ai_zombie_walk_attack_v1",
	"ai_zombie_barricade_enter_m_v1",
	"ai_zombie_attack_v1",
	"ai_zombie_crawl_v1",
	"ai_zombie_crawl_sprint",
	"ai_zombie_attack_crawl",
	"ai_zombie_crawl_death_v1",
	"ai_zombie_crawl_death_v2",
	"ai_zombie_window_attack_arm_l_out",
	"ai_zombie_window_attack_arm_r_out",
	"ai_zombie_crawl_barricade_enter_l",
	"ai_zombie_jump_down_48",
	"ai_zombie_jump_down_72",
	"ai_zombie_jump_down_96",
	"ai_zombie_jump_down_127",
	"ai_zombie_jump_down_190",
	"ai_zombie_jump_down_222",
	"ai_zombie_crawl_jump_down_48",
	"ai_zombie_crawl_jump_down_96",
	"ai_zombie_crawl_jump_down_127",
	"ai_zombie_crawl_jump_down_189",
	"ai_zombie_crawl_jump_down_222",
};

// each jump down's height, inches (its name's)
static const f32 g_ZmcharJumpDownHeight[] = { 48, 72, 96, 127, 190, 222 };
static const f32 g_ZmcharCrawlJumpDownHeight[] = { 48, 96, 127, 189, 222 };

// each loop's root motion, inches a second (the .xanim's tag_origin delta:
// these are drawn where the zombie's body is, its motion its own)
static const f32 g_ZmcharAnimSpeed[ZMCHAR_TEAR] = {
	0, 37.6f, 108.3f, 142.1f, 0, 0, 0, 0, 0, 0,
	0, 14.3f, 39.6f, 0, 0, 0, 0, 0, 0,
};

static void zmcharAnimName(s32 anim, char *out, s32 size)
{
	static const char *const parts[3] = { "grab", "hold", "pull" };

	if (anim < ZMCHAR_TEAR) {
		snprintf(out, size, "%s", g_ZmcharAnimNames[anim]);
	} else if (anim < ZMCHAR_CRAWL_TEAR) {
		snprintf(out, size, "ai_zombie_boardtear_aligned_m_%d_%s", (anim - ZMCHAR_TEAR) / 3 + 1,
				parts[(anim - ZMCHAR_TEAR) % 3]);
	} else if (anim < ZMCHAR_BOARD_PULL) {
		snprintf(out, size, "ai_zombie_crawl_boardtear_aligned_m_%d_%s", (anim - ZMCHAR_CRAWL_TEAR) / 3 + 1,
				parts[(anim - ZMCHAR_CRAWL_TEAR) % 3]);
	} else if (anim < ZMCHAR_BOARD_REPAIR) {
		snprintf(out, size, "o_zombie_board_%d_pull", anim - ZMCHAR_BOARD_PULL + 1);
	} else {
		snprintf(out, size, "o_zombie_board_%d_repair", anim - ZMCHAR_BOARD_REPAIR + 1);
	}
}

// the five whole bodies, each one's upper body as a crawler's (Call of Duty's
// _g_upclean), the stumps where the legs were (_g_legsoff: the two gowned
// bodies without their own wear the first gown's, as Call of Duty has them),
// and the four heads
#define ZMCHAR_PIECES 8 // bodies with legs and stumps of their own (the gowns 02 and 03 wear 01's)
#define ZMCHAR_UPPER0 ZMCHAR_BODIES
#define ZMCHAR_STUMP0 (ZMCHAR_UPPER0 + ZMCHAR_BODIES)
#define ZMCHAR_HEAD0  (ZMCHAR_STUMP0 + ZMCHAR_PIECES)
#define ZMCHAR_LARM0  (ZMCHAR_HEAD0 + ZMCHAR_HEADS)  // each body's torso without its left arm
#define ZMCHAR_RARM0  (ZMCHAR_LARM0 + ZMCHAR_BODIES) // ... without its right
#define ZMCHAR_LEGS0  (ZMCHAR_RARM0 + ZMCHAR_BODIES) // the legs under a torso piece (as the stumps)
#define ZMCHAR_MODELS (ZMCHAR_LEGS0 + ZMCHAR_PIECES)

static const char *const g_ZmcharModelNames[ZMCHAR_MODELS] = {
#define ZMCHAR_TEN(piece) \
	"zombie1_body01" piece, "zombie1_body02" piece, "zombie2_body01" piece, "zombie2_body02" piece, "zombie2_body03" piece, \
	"zombie3_body01" piece, "zombie3_body02" piece, "zombie3_body03" piece, "zombie3_body04" piece, "zombie3_body05" piece
#define ZMCHAR_EIGHT(piece) \
	"zombie1_body01" piece, "zombie1_body02" piece, "zombie2_body01" piece, \
	"zombie3_body01" piece, "zombie3_body02" piece, "zombie3_body03" piece, "zombie3_body04" piece, "zombie3_body05" piece
	ZMCHAR_TEN(""),
	ZMCHAR_TEN("_upclean"),
	ZMCHAR_EIGHT("_legsoff"),
	"zombie_head_a", "zombie_head_k", "zombie_head_l", "zombie_head_n",
	ZMCHAR_TEN("_larmoff"),
	ZMCHAR_TEN("_rarmoff"),
	ZMCHAR_EIGHT("_lowclean"),
};

static const u8 g_ZmcharBodyStumps[ZMCHAR_BODIES] = { 0, 1, 2, 2, 2, 3, 4, 5, 6, 7 };

static bool g_ZmcharHave[ZMCHAR_MODELS];

/**
 * The models a look draws, into out (up to 3): its body and head, or a
 * crawler's upper body, stumps and head; a body or head not there the first,
 * a crawler without its pieces whole
 */
static s32 zmcharLookModels(s32 look, s32 out[3])
{
	s32 body = look & 0xf;
	s32 head = (look >> 4) & 0xf;
	const bool crawler = (look & 0x100) != 0;

	if (body >= ZMCHAR_BODIES || !g_ZmcharHave[body]) {
		body = 0;
	}

	if (head >= ZMCHAR_HEADS || !g_ZmcharHave[ZMCHAR_HEAD0 + head]) {
		head = 0;
	}

	{
		const s32 stumps = ZMCHAR_STUMP0 + g_ZmcharBodyStumps[body];
		const s32 legs = ZMCHAR_LEGS0 + g_ZmcharBodyStumps[body];
		s32 torso = -1;
		s32 n = 0;

		// the torso a piece once anything is off it or below it: one arm
		// gone, or the upper body over a crawler's stumps
		if ((look & ZMCHAR_LOOK_NOLARM) && g_ZmcharHave[ZMCHAR_LARM0 + body]) {
			torso = ZMCHAR_LARM0 + body;
		} else if ((look & ZMCHAR_LOOK_NORARM) && g_ZmcharHave[ZMCHAR_RARM0 + body]) {
			torso = ZMCHAR_RARM0 + body;
		} else if (crawler) {
			torso = ZMCHAR_UPPER0 + body;
		}

		if (torso >= 0 && g_ZmcharHave[torso] && g_ZmcharHave[crawler ? stumps : legs]) {
			out[n++] = torso;
			out[n++] = crawler ? stumps : legs;
		} else {
			out[n++] = body;
		}

		if (!(look & ZMCHAR_LOOK_NOHEAD)) {
			out[n++] = ZMCHAR_HEAD0 + head;
		}

		return n;
	}
}

// a look made whole, as zmcharLookModels() would draw it: a slot's key
static s32 zmcharLookKey(s32 look)
{
	s32 models[3];
	const s32 n = zmcharLookModels(look, models);
	s32 key = 0;
	s32 i;

	for (i = 0; i < 3; i++) {
		key |= (i < n ? models[i] : 63) << (i * 6);
	}

	return key;
}

static bool g_ZmcharTried = false;
static bool g_ZmcharOk = false;
static struct zcmodel g_ZmcharModels[ZMCHAR_MODELS];
static struct zcjoint g_ZmcharJoints[ZMCHAR_MAXBONES];
static s32 g_ZmcharNumJoints = 0;
static s32 g_ZmcharRootJoint = -1;
static struct zcanim g_ZmcharAnims[ZMCHAR_ANIMCOUNT];
static struct zcslot g_ZmcharSlots[ZMCHAR_SLOTS];
static f32 g_ZmcharSlotPose[ZMCHAR_SLOTS][ZMCHAR_MAXBONES][3];
static bool g_ZmcharSlotPosed[ZMCHAR_SLOTS];

/**
 * Skinning is the zombies' cost: some 0.2 to 0.4 ms each a frame. A slot is
 * skinned once a level frame however many views draw it (split screen), and
 * one further than ZMCHAR_LOD_DIST from the camera every other frame, its
 * last vertices drawn between: at that distance its pose at 30 a second does
 * not show. g_Vars.lvframenum the last frame each slot was skinned.
 */
#define ZMCHAR_LOD_DIST 2000.0f

extern struct g_vars g_Vars;
static s32 g_ZmcharSlotSkinned[ZMCHAR_SLOTS];

s32 zmcharJoint(const char *name)
{
	s32 i;

	for (i = 0; i < g_ZmcharNumJoints; i++) {
		if (strcmp(g_ZmcharJoints[i].name, name) == 0) {
			return i;
		}
	}

	return -1;
}

s32 zmcharJointParent(s32 joint)
{
	return joint >= 0 && joint < g_ZmcharNumJoints ? g_ZmcharJoints[joint].parent : -1;
}

const f32 (*zmcharSlotPose(s32 slot))[3]
{
	return slot >= 0 && slot < ZMCHAR_SLOTS && g_ZmcharSlotPosed[slot] ? g_ZmcharSlotPose[slot] : NULL;
}
static u16 g_ZmcharWhite[XBLATEX_TILE * XBLATEX_TILE];

static u32 zmcharU32(const u8 *p)
{
	u32 v;
	memcpy(&v, p, 4);
	return v;
}

static u16 zmcharU16(const u8 *p)
{
	u16 v;
	memcpy(&v, p, 2);
	return v;
}

static s32 zmcharJointByName(const char *name)
{
	s32 i;

	for (i = 0; i < g_ZmcharNumJoints; i++) {
		if (strcmp(g_ZmcharJoints[i].name, name) == 0) {
			return i;
		}
	}

	return -1;
}

static bool zmcharLoadModel(struct zcmodel *m, const char *name)
{
	char rel[128];
	u8 *data;
	u32 size = 0;
	u32 at = 20;
	s32 i;

	snprintf(rel, sizeof(rel), ZMCHAR_DIR "%s.zvm", name);

	if (fsFileSize(fsFullPath(rel)) <= 20 || (data = fsFileLoad(rel, &size)) == NULL) {
		return false;
	}

	if (size < 20 || memcmp(data, "ZVM1", 4) != 0) {
		sysMemFree(data);
		return false;
	}

	m->numbones = zmcharU32(data + 4);
	m->numverts = zmcharU32(data + 8);
	m->numtris = zmcharU32(data + 12);
	m->numdraws = zmcharU32(data + 16);

	if (m->numbones <= 0 || m->numbones > ZMCHAR_MAXBONES || m->numverts <= 0 || m->numverts > 65535
			|| size < 20 + (u32)m->numbones * 112 + (u32)m->numverts * 52 + (u32)m->numtris * 6 + (u32)m->numdraws * 64) {
		sysLogPrintf(LOG_WARNING, "zmchar: %s is not vmconv.py's", rel);
		sysMemFree(data);
		return false;
	}

	m->bones = calloc(m->numbones, sizeof(*m->bones));
	m->verts = calloc(m->numverts, sizeof(*m->verts));
	m->tris = calloc(m->numtris * 3, sizeof(u16));
	m->draws = calloc(m->numdraws, sizeof(*m->draws));

	for (i = 0; i < m->numbones; i++, at += 112) {
		struct zcbone *b = &m->bones[i];

		memcpy(b->name, data + at, 32);
		b->name[31] = '\0';
		memcpy(&b->parent, data + at + 32, 4);
		memcpy(b->t, data + at + 36, 12);
		memcpy(b->q, data + at + 48, 16);
		memcpy(b->ib, data + at + 64, 48);
	}

	for (i = 0; i < m->numverts; i++, at += 52) {
		struct zcvert *v = &m->verts[i];

		memcpy(v->p, data + at, 12);
		memcpy(v->n, data + at + 12, 12);
		memcpy(v->uv, data + at + 24, 8);
		memcpy(v->j, data + at + 32, 4);
		memcpy(v->w, data + at + 36, 16);
	}

	for (i = 0; i < m->numtris * 3; i++, at += 2) {
		m->tris[i] = zmcharU16(data + at);
	}

	for (i = 0; i < m->numdraws; i++, at += 64) {
		struct zcdraw *d = &m->draws[i];
		char pic[53];

		d->first = zmcharU32(data + at);
		d->count = zmcharU32(data + at + 4);
		d->flags = zmcharU32(data + at + 8);
		memcpy(pic, data + at + 12, 52);
		pic[52] = '\0';

		snprintf(d->mat.name, sizeof(d->mat.name), "%s_%d", name, i);

		if (pic[0]) {
			snprintf(rel, sizeof(rel), ZMCHAR_DIR "%s", pic);
			d->mat.kind = OBJMAT_IMAGE;
			d->mat.alpha = d->flags & 1;
			snprintf(d->mat.image, sizeof(d->mat.image), "%s", fsFullPath(rel));
		}
	}

	sysMemFree(data);

	return true;
}

// Each model's bones into the joined skeleton: a name the body has is the body's
static void zmcharJoin(void)
{
	s32 k;
	s32 i;

	for (k = 0; k < ZMCHAR_MODELS; k++) {
		struct zcmodel *m = &g_ZmcharModels[k];

		if (!g_ZmcharHave[k]) {
			continue;
		}

		for (i = 0; i < m->numbones; i++) {
			struct zcbone *b = &m->bones[i];
			s32 j = zmcharJointByName(b->name);

			if (j < 0 && g_ZmcharNumJoints < ZMCHAR_MAXBONES) {
				struct zcjoint *jt = &g_ZmcharJoints[g_ZmcharNumJoints];

				j = g_ZmcharNumJoints++;
				memcpy(jt->name, b->name, sizeof(jt->name));
				jt->parent = b->parent >= 0 && b->parent < i ? m->bones[b->parent].joint : -1;
				memcpy(jt->t, b->t, sizeof(jt->t));
				memcpy(jt->q, b->q, sizeof(jt->q));
			}

			b->joint = j;
		}
	}

	g_ZmcharRootJoint = zmcharJointByName(ZMCHAR_ROOTBONE);
}

static bool zmcharLoadAnim(struct zcanim *a, s32 anim)
{
	char name[64];
	char rel[128];
	u8 *data;
	u32 size = 0;
	u32 at = 12;
	s32 numtracks;
	s32 t;

	zmcharAnimName(anim, name, sizeof(name));
	snprintf(rel, sizeof(rel), ZMCHAR_DIR "%s.zan", name);

	if (fsFileSize(fsFullPath(rel)) <= 12 || (data = fsFileLoad(rel, &size)) == NULL) {
		return false;
	}

	if (size < 12 || memcmp(data, "ZAN1", 4) != 0) {
		sysMemFree(data);
		return false;
	}

	a->frames = zmcharU16(data + 4);
	a->rate = zmcharU16(data + 6);
	numtracks = zmcharU16(data + 8);

	for (t = 0; t < numtracks && at + 36 <= size; t++) {
		char bone[33];
		s32 nr = zmcharU16(data + at + 32);
		s32 nt = zmcharU16(data + at + 34);
		s32 j;
		struct zctrack *track;
		s32 k;

		memcpy(bone, data + at, 32);
		bone[32] = '\0';
		at += 36;

		if (at + nr * 18 + nt * 14 > size) {
			break;
		}

		if (strcmp(bone, "@delta") == 0) {
			track = &a->delta;
		} else if (strcmp(bone, "tag_animate") == 0) {
			track = &a->animate;
		} else {
			j = zmcharJointByName(bone);
			track = j >= 0 ? &a->tracks[j] : NULL;
		}

		if (track == NULL) {
			at += nr * 18 + nt * 14;
			continue;
		}

		track->numrot = nr;
		track->rot = nr ? calloc(nr, sizeof(struct zckey)) : NULL;

		for (k = 0; k < nr; k++, at += 18) {
			track->rot[k].frame = zmcharU16(data + at);
			memcpy(track->rot[k].v, data + at + 2, 16);
		}

		track->numtrans = nt;
		track->trans = nt ? calloc(nt, sizeof(struct zckey)) : NULL;

		for (k = 0; k < nt; k++, at += 14) {
			track->trans[k].frame = zmcharU16(data + at);
			memcpy(track->trans[k].v, data + at + 2, 12);
		}
	}

	sysMemFree(data);
	a->ok = a->frames > 0 && a->rate > 0;

	return a->ok;
}

/**
 * Every draw's picture bound as the zombies load, once a game. The models'
 * 116 pictures are 16 (each body piece carries its own copy of its body's),
 * so a file whose bytes are one already bound shares that one's tile: 16
 * decodes, where binding as each slot was first built read and decoded every
 * copy, some 45 ms a new piece, a hitch whenever a zombie of a new look (a
 * crawler's legs gone) was first drawn.
 */
static void zmcharBindAll(void)
{
	struct { u64 hash; const void *tile; } seen[64];
	s32 numseen = 0;
	s32 numbound = 0;
	s32 i;
	s32 d;

	for (i = 0; i < ZMCHAR_MODELS; i++) {
		if (!g_ZmcharHave[i]) {
			continue;
		}

		for (d = 0; d < g_ZmcharModels[i].numdraws; d++) {
			struct zcdraw *draw = &g_ZmcharModels[i].draws[d];
			u64 hash = 14695981039346656037ull;
			s32 alpha;
			s32 soft;
			s32 k;
			FILE *f;

			if (draw->mat.kind != OBJMAT_IMAGE) {
				continue;
			}

			// FNV-1a over the file's bytes
			f = fopen(draw->mat.image, "rb");

			if (f) {
				u8 buf[65536];
				size_t n;

				while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
					size_t b;

					for (b = 0; b < n; b++) {
						hash = (hash ^ buf[b]) * 1099511628211ull;
					}
				}

				fclose(f);
			}

			for (k = 0; k < numseen && seen[k].hash != hash; k++);

			if (k < numseen) {
				draw->tile = seen[k].tile;
			} else {
				draw->tile = modelpackBindMaterial(&draw->mat, &alpha, &soft);
				numbound++;

				if (numseen < (s32)ARRAYCOUNT(seen)) {
					seen[numseen].hash = hash;
					seen[numseen].tile = draw->tile;
					numseen++;
				}
			}

			draw->tilegen = modelpackGetGeneration();
			draw->tiled = true;
		}
	}

	sysLogPrintf(LOG_NOTE, "zmchar: %d pictures bound for the zombies' draws", numbound);
}

bool zmcharReady(void)
{
	s32 i;

	if (g_ZmcharTried) {
		return g_ZmcharOk;
	}

	g_ZmcharTried = true;

	// the body and head, without which nothing; a crawler's pieces if there
	for (i = 0; i < ZMCHAR_MODELS; i++) {
		g_ZmcharHave[i] = zmcharLoadModel(&g_ZmcharModels[i], g_ZmcharModelNames[i]);

		if (!g_ZmcharHave[i] && (i == 0 || i == ZMCHAR_HEAD0)) {
			sysLogPrintf(LOG_NOTE, "zmchar: no %s" ZMCHAR_DIR "%s.zvm: the zombies keep Perfect Dark's bodies",
					"", g_ZmcharModelNames[i]);
			return false;
		}
	}

	zmcharJoin();

	for (i = 0; i < ZMCHAR_ANIMCOUNT; i++) {
		zmcharLoadAnim(&g_ZmcharAnims[i], i);
	}

	// walking at the least: without it there is nothing to draw them by
	g_ZmcharOk = g_ZmcharAnims[ZMCHAR_WALK].ok && g_ZmcharRootJoint >= 0;

	memset(g_ZmcharWhite, 0xff, sizeof(g_ZmcharWhite));

	if (g_ZmcharOk) {
		zmcharBindAll();
	}

	sysLogPrintf(LOG_NOTE, "zmchar: Black Ops II's zombie, %d and %d vertices, %d bones joined%s",
			g_ZmcharModels[0].numverts, g_ZmcharModels[1].numverts, g_ZmcharNumJoints,
			g_ZmcharOk ? "" : ", but no walk: Perfect Dark's bodies kept");

	return g_ZmcharOk;
}

f32 zmcharAnimFrames(s32 anim)
{
	return zmcharReady() && anim >= 0 && anim < ZMCHAR_ANIMCOUNT && g_ZmcharAnims[anim].ok ? g_ZmcharAnims[anim].frames : 0;
}

f32 zmcharAnimRate(s32 anim)
{
	return zmcharReady() && anim >= 0 && anim < ZMCHAR_ANIMCOUNT && g_ZmcharAnims[anim].ok ? g_ZmcharAnims[anim].rate : 30;
}

f32 zmcharAnimSpeed(s32 anim)
{
	return anim >= 0 && anim < ZMCHAR_TEAR ? g_ZmcharAnimSpeed[anim] : 0;
}

// A track's value at frame: its keys held at either end, a rotation nlerped
static bool zmcharSample(const struct zckey *keys, s32 n, f32 frame, bool rot, f32 *out)
{
	s32 k;

	if (n <= 0) {
		return false;
	}

	if (n == 1 || frame <= keys[0].frame) {
		memcpy(out, keys[0].v, sizeof(f32) * 4);
		return true;
	}

	for (k = 0; k < n - 1; k++) {
		const struct zckey *a = &keys[k];
		const struct zckey *b = &keys[k + 1];

		if (frame >= a->frame && frame <= b->frame) {
			const f32 t = b->frame > a->frame ? (frame - a->frame) / (f32)(b->frame - a->frame) : 0;
			s32 c;

			if (rot) {
				const f32 sign = a->v[0] * b->v[0] + a->v[1] * b->v[1] + a->v[2] * b->v[2] + a->v[3] * b->v[3] < 0 ? -1 : 1;
				f32 len = 0;

				for (c = 0; c < 4; c++) {
					out[c] = a->v[c] + (b->v[c] * sign - a->v[c]) * t;
					len += out[c] * out[c];
				}

				len = len > 0 ? 1.0f / sqrtf(len) : 1;

				for (c = 0; c < 4; c++) {
					out[c] *= len;
				}
			} else {
				for (c = 0; c < 3; c++) {
					out[c] = a->v[c] + (b->v[c] - a->v[c]) * t;
				}
			}

			return true;
		}
	}

	memcpy(out, keys[n - 1].v, sizeof(f32) * 4);
	return true;
}

bool zmcharAnimRoot(s32 anim, f32 frame, f32 pos[3], f32 q[4])
{
	const struct zcanim *a;
	f32 v[4];
	bool any = false;

	pos[0] = pos[1] = pos[2] = 0;
	q[0] = q[1] = q[2] = 0;
	q[3] = 1;

	if (!zmcharReady() || anim < 0 || anim >= ZMCHAR_ANIMCOUNT || !g_ZmcharAnims[anim].ok) {
		return false;
	}

	a = &g_ZmcharAnims[anim];

	if (zmcharSample(a->delta.trans, a->delta.numtrans, frame, false, v)) {
		pos[0] += v[0];
		pos[1] += v[1];
		pos[2] += v[2];
		any = true;
	}

	if (zmcharSample(a->animate.trans, a->animate.numtrans, frame, false, v)) {
		pos[0] += v[0];
		pos[1] += v[1];
		pos[2] += v[2];
		any = true;
	}

	if (zmcharSample(a->animate.rot, a->animate.numrot, frame, true, v)) {
		memcpy(q, v, sizeof(f32) * 4);
		any = true;
	}

	return any;
}

s32 zmcharJumpDown(f32 inches, bool crawler, f32 *leave, f32 *land)
{
	// each one's leave and land, found once from its root's fall
	static f32 found[ZMCHAR_TEAR - ZMCHAR_JUMPDOWN][2];
	static bool done[ZMCHAR_TEAR - ZMCHAR_JUMPDOWN];
	const f32 *heights = crawler ? g_ZmcharCrawlJumpDownHeight : g_ZmcharJumpDownHeight;
	const s32 count = crawler ? ARRAYCOUNT(g_ZmcharCrawlJumpDownHeight) : ARRAYCOUNT(g_ZmcharJumpDownHeight);
	const s32 first = crawler ? ZMCHAR_CRAWL_JUMPDOWN : ZMCHAR_JUMPDOWN;
	s32 best = -1;
	s32 k;

	for (k = 0; k < count; k++) {
		const s32 anim = first + k;

		if (!zmcharReady() || !g_ZmcharAnims[anim].ok) {
			continue;
		}

		if (best < 0 || fabsf(heights[k] - inches) < fabsf(heights[best - first] - inches)) {
			best = anim;
		}
	}

	if (best < 0) {
		return -1;
	}

	k = best - ZMCHAR_JUMPDOWN;

	if (!done[k]) {
		const f32 frames = g_ZmcharAnims[best].frames;
		f32 pos[3];
		f32 q[4];
		f32 bottom = 0;
		f32 f;

		done[k] = true;
		found[k][0] = 0;
		found[k][1] = frames - 1;

		for (f = 0; f < frames; f += 0.25f) {
			zmcharAnimRoot(best, f, pos, q);
			bottom = MIN(bottom, pos[2]);
		}

		// leaving: a tenth of the way down; landed: all but the last inch
		for (f = 0; f < frames; f += 0.25f) {
			zmcharAnimRoot(best, f, pos, q);

			if (pos[2] < bottom * 0.1f) {
				found[k][0] = f;
				break;
			}
		}

		for (f = found[k][0]; f < frames; f += 0.25f) {
			zmcharAnimRoot(best, f, pos, q);

			if (pos[2] <= bottom + 1) {
				found[k][1] = f;
				break;
			}
		}
	}

	*leave = found[k][0];
	*land = found[k][1];

	return best;
}

// A joint's local rotation and translation in an animation at a frame
static void zmcharLocal(const struct zcanim *a, s32 j, f32 frame, f32 q[4], f32 t[3])
{
	const struct zcjoint *jt = &g_ZmcharJoints[j];
	f32 v[4];

	memcpy(q, jt->q, sizeof(f32) * 4);
	memcpy(t, jt->t, sizeof(f32) * 3);

	if (a == NULL || !a->ok) {
		return;
	}

	if (zmcharSample(a->tracks[j].rot, a->tracks[j].numrot, frame, true, v)) {
		memcpy(q, v, sizeof(f32) * 4);
	}

	if (zmcharSample(a->tracks[j].trans, a->tracks[j].numtrans, frame, false, v)) {
		if (j == g_ZmcharRootJoint) {
			t[0] += v[0];
			t[1] += v[1];
			t[2] += v[2];
		} else {
			memcpy(t, v, sizeof(f32) * 3);
		}
	}
}

// out = a * b, both rotation-and-translation 3x4
static void zmcharMul(const f32 a[3][4], const f32 b[3][4], f32 out[3][4])
{
	s32 i;
	s32 j;

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 4; j++) {
			out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0);
		}
	}
}

// Every joint's world (the zombie's own space), the pose blended from one
// animation into another
static void zmcharPose(s32 anim, f32 frame, s32 from, f32 fromframe, f32 blend, f32 world[][3][4])
{
	const struct zcanim *a = anim >= 0 && anim < ZMCHAR_ANIMCOUNT ? &g_ZmcharAnims[anim] : NULL;
	const struct zcanim *b = blend > 0 && from >= 0 && from < ZMCHAR_ANIMCOUNT && g_ZmcharAnims[from].ok
		? &g_ZmcharAnims[from] : NULL;
	s32 j;

	for (j = 0; j < g_ZmcharNumJoints; j++) {
		f32 q[4];
		f32 t[3];
		f32 x;
		f32 y;
		f32 z;
		f32 w;
		f32 local[3][4];

		zmcharLocal(a, j, frame, q, t);

		if (b) {
			f32 q2[4];
			f32 t2[3];
			f32 len = 0;
			f32 sign;
			s32 c;

			zmcharLocal(b, j, fromframe, q2, t2);
			sign = q[0] * q2[0] + q[1] * q2[1] + q[2] * q2[2] + q[3] * q2[3] < 0 ? -1 : 1;

			for (c = 0; c < 4; c++) {
				q[c] = q[c] + (q2[c] * sign - q[c]) * blend;
				len += q[c] * q[c];
			}

			len = len > 0 ? 1.0f / sqrtf(len) : 1;

			for (c = 0; c < 4; c++) {
				q[c] *= len;
			}

			for (c = 0; c < 3; c++) {
				t[c] = t[c] + (t2[c] - t[c]) * blend;
			}
		}

		x = q[0];
		y = q[1];
		z = q[2];
		w = q[3];

		local[0][0] = 1 - 2 * (y * y + z * z);
		local[0][1] = 2 * (x * y - z * w);
		local[0][2] = 2 * (x * z + y * w);
		local[0][3] = t[0];
		local[1][0] = 2 * (x * y + z * w);
		local[1][1] = 1 - 2 * (x * x + z * z);
		local[1][2] = 2 * (y * z - x * w);
		local[1][3] = t[1];
		local[2][0] = 2 * (x * z - y * w);
		local[2][1] = 2 * (y * z + x * w);
		local[2][2] = 1 - 2 * (x * x + y * y);
		local[2][3] = t[2];

		if (g_ZmcharJoints[j].parent >= 0 && g_ZmcharJoints[j].parent < j) {
			zmcharMul(world[g_ZmcharJoints[j].parent], local, world[j]);
		} else {
			memcpy(world[j], local, sizeof(local));
		}
	}
}

// A slot's arrays and list for a look: the machines' batches, every draw of its models
static bool zmcharBuildSlot(struct zcslot *s, s32 look)
{
	s32 models[3];
	const s32 nummodels = zmcharLookModels(look, models);
	s32 maxvtx = 0;
	s32 maxgfx = 8;
	s32 numgfx = 0;
	s32 n;
	s32 k;
	s32 d;

	// another look in the slot: the last one's let go
	free(s->vtx);
	free(s->col);
	free(s->srcmodel);
	free(s->srcvert);
	free(s->gdl);
	s->vtx = NULL;
	s->col = NULL;
	s->srcmodel = NULL;
	s->srcvert = NULL;
	s->gdl = NULL;
	s->numvtx = 0;

	s->built = true;
	s->key = zmcharLookKey(look);

	for (n = 0; n < nummodels && (k = models[n]) >= 0; n++) {
		maxvtx += g_ZmcharModels[k].numtris * 3;
		maxgfx += g_ZmcharModels[k].numtris * 3 + g_ZmcharModels[k].numdraws * 16;
	}

	s->vtx = calloc(maxvtx, sizeof(Vtx));
	s->col = calloc(maxvtx, sizeof(Col));
	s->srcmodel = calloc(maxvtx, sizeof(u8));
	s->srcvert = calloc(maxvtx, sizeof(u16));
	s->gdl = calloc(maxgfx, sizeof(Gfx));

	if (!s->vtx || !s->col || !s->srcmodel || !s->srcvert || !s->gdl) {
		free(s->vtx);
		free(s->col);
		free(s->srcmodel);
		free(s->srcvert);
		free(s->gdl);
		s->vtx = NULL;
		s->gdl = NULL;
		return false;
	}

	for (n = 0; n < nummodels && (k = models[n]) >= 0; n++) {
		const struct zcmodel *m = &g_ZmcharModels[k];

		for (d = 0; d < m->numdraws; d++) {
			struct zcdraw *draw = &m->draws[d];
			const void *tile = NULL;
			s32 alpha = 0;
			s32 soft = 0;
			s32 slots[ZMCHAR_BATCH];
			u8 tris[ZMCHAR_MAXTRIS * 3];
			s32 numslots = 0;
			s32 numtris = 0;
			u32 t;
			Gfx *gdl = &s->gdl[numgfx];

			// bound once, again only for a new pack (struct zcdraw)
			if (draw->mat.kind == OBJMAT_IMAGE) {
				if (!draw->tiled || draw->tilegen != modelpackGetGeneration()) {
					draw->tile = modelpackBindMaterial(&draw->mat, &alpha, &soft);
					draw->tilegen = modelpackGetGeneration();
					draw->tiled = true;
				}

				tile = draw->tile;
			}

			if (!tile) {
				tile = g_ZmcharWhite;
			}

			gDPPipeSync(gdl++);
			gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
			gDPLoadTextureBlock(gdl++, tile, G_IM_FMT_RGBA, G_IM_SIZ_16b,
					XBLATEX_TILE, XBLATEX_TILE, 0,
					G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR,
					XBLATEX_TILE_MASK, XBLATEX_TILE_MASK, G_TX_NOLOD, G_TX_NOLOD);
			numgfx = (s32)(gdl - s->gdl);

			for (t = draw->first; t <= draw->first + draw->count && t <= (u32)m->numtris; t++) {
				const bool last = t == draw->first + draw->count || t == (u32)m->numtris;
				s32 corner[3] = { -1, -1, -1 };
				s32 need = 0;
				s32 c;
				s32 j;

				if (!last) {
					for (c = 0; c < 3; c++) {
						for (j = 0; j < numslots; j++) {
							if (slots[j] == m->tris[t * 3 + c]) {
								corner[c] = j;
							}
						}

						if (corner[c] < 0) {
							for (j = 0; j < c; j++) {
								if (m->tris[t * 3 + j] == m->tris[t * 3 + c] && corner[j] < 0) {
									break;
								}
							}

							if (j == c) {
								need++;
							}
						}
					}
				}

				// the batch out: before a triangle it has no room for, and at the end
				if ((last || numslots + need > ZMCHAR_BATCH || numtris == ZMCHAR_MAXTRIS) && numtris > 0) {
					gdl = &s->gdl[numgfx];

					for (j = 0; j < numslots; j++) {
						const struct zcvert *v = &m->verts[slots[j]];
						Vtx *out = &s->vtx[s->numvtx + j];

						s->srcmodel[s->numvtx + j] = (u8)n; // its place in the look's models
						s->srcvert[s->numvtx + j] = (u16)slots[j];
						out->flags = 0;
						out->colour = (u8)(j * 4);
						out->s = (s16)(v->uv[0] * XBLATEX_TILE_SCALE);
						out->t = (s16)(v->uv[1] * XBLATEX_TILE_SCALE);
						s->col[s->numvtx + j].a = 0xff;
					}

					gSPColor(gdl++, &s->col[s->numvtx], numslots);
					gSPVertex(gdl++, &s->vtx[s->numvtx], numslots, 0);

					for (j = 0; j < numtris; j += 4) {
						u8 *q = &tris[j * 3];
						s32 n = numtris - j;

						gSPTri4(gdl++, q[0], q[1], q[2],
								n > 1 ? q[3] : 0, n > 1 ? q[4] : 0, n > 1 ? q[5] : 0,
								n > 2 ? q[6] : 0, n > 2 ? q[7] : 0, n > 2 ? q[8] : 0,
								n > 3 ? q[9] : 0, n > 3 ? q[10] : 0, n > 3 ? q[11] : 0);
					}

					numgfx = (s32)(gdl - s->gdl);
					s->numvtx += numslots;
					numslots = 0;
					numtris = 0;

					for (c = 0; c < 3; c++) {
						corner[c] = -1;
					}
				}

				if (last) {
					break;
				}

				for (c = 0; c < 3; c++) {
					const s32 vi = m->tris[t * 3 + c];

					for (j = 0; j < numslots && corner[c] < 0; j++) {
						if (slots[j] == vi) {
							corner[c] = j;
						}
					}

					if (corner[c] < 0) {
						corner[c] = numslots;
						slots[numslots++] = vi;
					}
				}

				tris[numtris * 3 + 0] = (u8)corner[0];
				tris[numtris * 3 + 1] = (u8)corner[1];
				tris[numtris * 3 + 2] = (u8)corner[2];
				numtris++;
			}
		}
	}

	gSPEndDisplayList(&s->gdl[numgfx++]);

	return true;
}

// A slot's joints posed and its models' vertices skinned into its arrays:
// each its place and a shade from its turned normal, a light from above, in
// front and to the left of the zombie
static void zmcharSkin(s32 slot, struct zcslot *s, const s32 *models, s32 nummodels,
		s32 anim, f32 frame, s32 from, f32 fromframe, f32 blend)
{
	static f32 world[ZMCHAR_MAXBONES][3][4];
	// by a model's place in the look (three at the most), not its number
	static f32 skin[3][ZMCHAR_MAXBONES][3][4];
	static f32 pos[3][65536 / 4][3];
	static u8 shade[3][65536 / 4];
	s32 n;
	s32 k;
	s32 i;

	zmcharPose(anim, frame, from, fromframe, blend, world);

	// the joints kept, for a shot's test against them (zombies.inc)
	for (i = 0; i < g_ZmcharNumJoints; i++) {
		g_ZmcharSlotPose[slot][i][0] = world[i][0][3];
		g_ZmcharSlotPose[slot][i][1] = world[i][1][3];
		g_ZmcharSlotPose[slot][i][2] = world[i][2][3];
	}

	g_ZmcharSlotPosed[slot] = true;
	g_ZmcharSlotSkinned[slot] = g_Vars.lvframenum;

	for (n = 0; n < nummodels && (k = models[n]) >= 0; n++) {
		const struct zcmodel *m = &g_ZmcharModels[k];

		for (i = 0; i < m->numbones; i++) {
			zmcharMul(world[m->bones[i].joint], m->bones[i].ib, skin[n][i]);
		}

		for (i = 0; i < m->numverts && i < (s32)ARRAYCOUNT(pos[n]); i++) {
			const struct zcvert *v = &m->verts[i];
			f32 p[3] = { 0, 0, 0 };
			f32 nrm[3] = { 0, 0, 0 };
			f32 len;
			f32 lit;
			s32 c;
			s32 w;

			for (w = 0; w < 4; w++) {
				const f32 (*sm)[4];

				if (v->w[w] <= 0 || v->j[w] >= m->numbones) {
					continue;
				}

				sm = skin[n][v->j[w]];

				for (c = 0; c < 3; c++) {
					p[c] += v->w[w] * (sm[c][0] * v->p[0] + sm[c][1] * v->p[1] + sm[c][2] * v->p[2] + sm[c][3]);
					nrm[c] += v->w[w] * (sm[c][0] * v->n[0] + sm[c][1] * v->n[1] + sm[c][2] * v->n[2]);
				}
			}

			len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
			lit = len > 0 ? (nrm[0] * 0.55f + nrm[1] * 0.3f + nrm[2] * 0.78f) / len : 0;

			memcpy(pos[n][i], p, sizeof(p));
			shade[n][i] = (u8)(255.0f * (0.62f + 0.38f * (lit > 0 ? lit : 0)));
		}
	}

	for (i = 0; i < s->numvtx; i++) {
		const s32 k2 = s->srcmodel[i];
		const s32 vi = s->srcvert[i];
		const f32 *p = pos[k2][vi];
		Vtx *out = &s->vtx[i];

		out->x = (s16)(p[0] * ZMCHAR_FIXED);
		out->y = (s16)(p[1] * ZMCHAR_FIXED);
		out->z = (s16)(p[2] * ZMCHAR_FIXED);
		s->col[i].r = s->col[i].g = s->col[i].b = shade[k2][vi];
	}
}

Gfx *zmcharDraw(Gfx *gdl, s32 slot, s32 look, Mtxf *mtx, s32 anim, f32 frame, s32 from, f32 fromframe, f32 blend,
		u8 r, u8 g, u8 b)
{
	struct zcslot *s;
	s32 models[3];
	s32 nummodels;
	bool rebuilt = false;
	bool again = true;

	if (!zmcharReady() || slot < 0 || slot >= ZMCHAR_SLOTS) {
		return gdl;
	}

	s = &g_ZmcharSlots[slot];
	nummodels = zmcharLookModels(look, models);

	// its list built the first time, and again for a zombie of another look
	if (!s->built || s->key != zmcharLookKey(look)) {
		rebuilt = true;

		if (!zmcharBuildSlot(s, look)) {
			return gdl;
		}
	}

	if (!s->gdl) {
		return gdl;
	}

	// not again if skinned already this frame (another view), or if far and
	// skinned last frame: its vertices as they are (ZMCHAR_LOD_DIST)
	if (!rebuilt && g_ZmcharSlotPosed[slot]) {
		const f32 dist2 = mtx->m[3][0] * mtx->m[3][0] + mtx->m[3][1] * mtx->m[3][1] + mtx->m[3][2] * mtx->m[3][2];
		const s32 since = g_Vars.lvframenum - g_ZmcharSlotSkinned[slot];

		if (since == 0 || (since == 1 && dist2 > ZMCHAR_LOD_DIST * ZMCHAR_LOD_DIST)) {
			again = false;
		}
	}

	if (again) {
		zmcharSkin(slot, s, models, nummodels, anim, frame, from, fromframe, blend);
	}
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_2CYCLE);
	gDPSetRenderMode(gdl++, G_RM_PASS, G_RM_AA_ZB_OPA_SURF2);
	gDPSetCombineLERP(gdl++, TEXEL0, 0, SHADE, 0, 0, 0, 0, 1,
			COMBINED, 0, PRIMITIVE, 0, 0, 0, 0, 1);
	gDPSetPrimColor(gdl++, 0, 0, r, g, b, 0xff);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gDPSetTexturePersp(gdl++, G_TP_PERSP);
	gDPSetTextureLOD(gdl++, G_TL_TILE);
	gDPSetTextureLUT(gdl++, G_TT_NONE);
	gDPSetTextureConvert(gdl++, G_TC_FILT);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gSPClearGeometryMode(gdl++, G_LIGHTING | G_TEXTURE_GEN | G_FOG | G_CULL_BOTH);
	gSPSetGeometryMode(gdl++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
	gSPMatrix(gdl++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW | G_MTX_FLOATS);
	gSPDisplayList(gdl++, s->gdl);
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);

	return gdl;
}
