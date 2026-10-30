/**
 * A Black Ops II map, exported by C2M from the running game (its geometry as
 * an OBJ in inches, z up, and its entities), as a Perfect Dark arena for the
 * Stage Loader: `--codmap-convert <C2M's exported_maps/black_ops_2/zm>` writes
 * the bg, tiles, pads and multiplayer setup of every row of g_CodMapRows whose
 * map is there into $E/mods/Black Ops II Maps/ and exits.
 *
 * The level is cut out of its map by the row's box (Town is one corner of
 * Tranzit), turned into Perfect Dark's axes (x, y up, -z) and scale (2.54 an
 * inch) about the box's middle, and dealt by the middle of each triangle into
 * a grid of rooms CODMAP_CELL wide, every two neighbours joined by a portal
 * the height of the level: the portal walk then needs no knowledge of the
 * map, and every place is in a room. Each room draws its triangles in one
 * colour a material (lit from above in the vertices: no textures yet), and
 * every triangle is a collision tile in each room it reaches into - a floor
 * where it faces up, a wall otherwise - but those of a material named as
 * foliage or a decal. The players start where the row's spawn points stand.
 * CLAUDE-notes/zombies-cod-anims.md, "Black Ops II's Town as an arena".
 */

#ifndef PLATFORM_N64

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <zlib.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "constants.h"
#include "system.h"
#include "fs.h"

#define SEG_BG 0x0f000000u
#define CODMAP_SCALE 2.54f    // our units an inch
#define CODMAP_CELL  1000.0f  // a room's side: 18x18 for Town, under bg.c's 350 rooms (750's 576 overran g_BgForceOnscreenRooms)
#define CODMAP_PIECE_TRIS 9000 // a room's triangles at most; the rest go into pieces drawn with it (32767 vertices)
#define CODMAP_REACH 60.0f    // a tile goes in every room it comes this near
#define CODMAP_FLOOR_NY 0.6f  // a triangle facing up this much is a floor

#define FLOOR_FLAGS (0x0001 | 0x0002 | 0x0008 | 0x0010)
#define GEOFLAG_LAVA 0x0400 // GEOFLAG_0400, which nothing of Perfect Dark's sets
#define WALL_FLAGS  (0x0004 | 0x0008 | 0x0010)

struct codmaprow {
	const char *key;    // the files' name: bg_<key>.seg
	const char *name;   // the arena's
	const char *map;    // C2M's folder and file stem
	f32 box[4];         // x0 x1 y0 y1, the map's inches
	f32 zmin;           // and nothing under this (Town: Tranzit's lab under it)
	const char *spawns; // the targetname of its player spawns
	const char *fog;    // the maps block's fog row (modloader.c's modloaderParseFog())
};

static const struct codmaprow g_CodMapRows[] = {
	{ "bo2town", "Town (BO2)", "zm_transit", { -2168, 4785, -4578, 2416 }, -200, "town_standard_player_spawns",
		// Tranzit's dusk: a brown haze closing in past a street or two, under a
		// dull amber sky with a high, dirty cloud layer. fogmin and fogmax are
		// depth-buffer fractions (a thousandth), z' = far(d - near) / (d(far -
		// near)): 988 is about 1,200 units, 998 about 5,600, so the haze is whole
		// well before the box's cut edge (8,800 from its middle) as Town's own
		// hides the rest of Tranzit. 992 to 1000 was whole only at the far plane,
		// and the cut showed through it
		"15 22000 0 0 0 988 998 6b5a46 1 6000 0 8a7660 0 -5000 0 000000 0" },
};

/* ------------------------------------------------------------------------ */

typedef struct {
	uint8_t *v;
	size_t n, cap;
} cbuf;

static void cbGrow(cbuf *b, size_t n)
{
	if (b->n + n > b->cap) {
		size_t cap = b->cap ? b->cap : 256;

		while (cap < b->n + n) {
			cap *= 2;
		}

		b->v = realloc(b->v, cap);
		b->cap = cap;
	}
}

static void cbPut(cbuf *b, const void *p, size_t n) { cbGrow(b, n); memcpy(b->v + b->n, p, n); b->n += n; }
static void cbZeros(cbuf *b, size_t n) { cbGrow(b, n); memset(b->v + b->n, 0, n); b->n += n; }
static void cbU8(cbuf *b, uint32_t v) { uint8_t c = (uint8_t)v; cbPut(b, &c, 1); }
static void cbU16(cbuf *b, uint32_t v) { cbU8(b, v >> 8); cbU8(b, v); }
static void cbU32(cbuf *b, uint32_t v) { cbU16(b, v >> 16); cbU16(b, v); }
static void cbF32(cbuf *b, f32 f) { uint32_t u; memcpy(&u, &f, 4); cbU32(b, u); }
static void cbPad(cbuf *b, size_t a) { while (b->n % a) { cbU8(b, 0); } }
static void set16(uint8_t *b, size_t o, uint32_t v) { b[o] = v >> 8; b[o + 1] = v; }
static void set32(uint8_t *b, size_t o, uint32_t v) { set16(b, o, v >> 16); set16(b, o + 2, v); }
static void setf32(uint8_t *b, size_t o, f32 f) { uint32_t u; memcpy(&u, &f, 4); set32(b, o, u); }

// Rare's 1173 header, five bytes, then raw deflate (geconvert.c's rzip1173())
static cbuf rzip(const uint8_t *data, size_t n)
{
	cbuf out = {0};
	z_stream zs;
	uLong bound;

	memset(&zs, 0, sizeof(zs));
	deflateInit2(&zs, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
	bound = deflateBound(&zs, (uLong)n);
	cbZeros(&out, 5 + bound);
	out.v[0] = 0x11;
	out.v[1] = 0x73;
	out.v[2] = (n >> 16) & 0xff;
	out.v[3] = (n >> 8) & 0xff;
	out.v[4] = n & 0xff;
	zs.next_in = (Bytef *)data;
	zs.avail_in = (uInt)n;
	zs.next_out = out.v + 5;
	zs.avail_out = (uInt)bound;
	deflate(&zs, Z_FINISH);
	out.n = 5 + zs.total_out;
	deflateEnd(&zs);
	return out;
}

/* ------------------------------------------------------------------------ */

struct ctri {
	f32 p[3][3];   // our axes and units
	f32 uv[3][2];  // the OBJ's, v down
	f32 n[3];
	u32 colour;    // the vertices', lit
	s32 mat;
	u8 solid;
	u8 hide;       // solid but not drawn: a prop's simplest model, its collision
	u8 scan;       // the collision bo2scan read out of the game (readScan())
};

struct cmat {
	char name[96];
	u32 rgb;
	s32 texnum;    // the mod's texture for it (tools/zombies/codmaptex.py), -1 none
	u8 solid;
	u8 nodraw;     // neither drawn nor solid
	u8 glow;       // lit by itself, whatever light it is in (molten lava)
	u8 unseen;     // solid but not drawn: glass and decals, which the rooms cannot draw blended
	u8 cut;        // its picture see-through in places: drawn cut out (an alpha test)
	u8 blend;      // drawn blended by its picture's alpha, in the room's translucent leaf: 1 glass, 2 a decal
};

// codmaptex.py's textures/codmap.txt: a material's name and its texture number
static char (*g_TexNames)[96];
static s32 *g_TexNums;
static u8 *g_TexCut;
static s32 g_NumTexNames;

#define CODMAP_TEX 32 // codmaptex.py's placeholder side: a texel is 32 of a vertex's s or t

static void texTableRead(const char *path)
{
	FILE *f = fopen(path, "rb");
	char name[96];
	u32 num;

	g_NumTexNames = 0;

	if (!f) {
		sysLogPrintf(LOG_WARNING, "codmap: no %s, so no textures", path);
		return;
	}

	{
		char line[256];

		while (fgets(line, sizeof(line), f)) {
			s32 cut = 0;

			if (sscanf(line, "%95s %x %d", name, &num, &cut) < 2) {
				continue;
			}

			g_TexNames = realloc(g_TexNames, sizeof(*g_TexNames) * (g_NumTexNames + 1));
			g_TexNums = realloc(g_TexNums, sizeof(*g_TexNums) * (g_NumTexNames + 1));
			g_TexCut = realloc(g_TexCut, g_NumTexNames + 1);
			snprintf(g_TexNames[g_NumTexNames], sizeof(g_TexNames[0]), "%s", name);
			g_TexNums[g_NumTexNames] = (s32)num;
			g_TexCut[g_NumTexNames] = cut != 0;
			g_NumTexNames++;
		}
	}

	fclose(f);
}

static struct ctri *g_Tris;
static s32 g_NumTris, g_MaxTris;
static struct cmat *g_Mats;
static s32 g_NumMats, g_MaxMats;

static u32 matHash(const char *s)
{
	u32 h = 2166136261u;

	while (*s) {
		h = (h ^ (u8)*s++) * 16777619u;
	}

	return h;
}

// A material's flat colour: an earthy one picked by its name, until the
// textures come
static s32 matFind(const char *name)
{
	// Not walked on or into: decals, leaves and effects. Ground grass
	// (afr_ground_grass_01) is ground: with "grass" here Town had holes to fall
	// through
	static const char *const nosolid[] = { "decal", "_dec", "foliage", "fol_", "leaf", "leaves", "vine", "fx_", "atmos_", "sky" };
	// Black Ops II's tools' own surfaces, which its renderer never draws: a
	// shadow caster's caulk, written "SHADOW PRIMARY" in yellow
	// and its effect cards: atmos_glow_fire is a glow over a fire, drawn
	// added, and opaque it was an orange slab
	static const char *const nodraw[] = { "caulk", "shadow", "clip", "atmos_" };
	static const u32 palette[] = {
		0x8a7a66, 0x6f6a60, 0x7d6b55, 0x5f5a52, 0x8f8576, 0x6b5b4a, 0x777a74, 0x857060,
		0x6a6f6a, 0x9a8c78, 0x5c5248, 0x807568,
	};
	s32 i;
	u32 h;
	char kind[96];

	for (i = 0; i < g_NumMats; i++) {
		if (strcmp(g_Mats[i].name, name) == 0) {
			return i;
		}
	}

	if (g_NumMats == g_MaxMats) {
		g_MaxMats = g_MaxMats ? g_MaxMats * 2 : 256;
		g_Mats = realloc(g_Mats, sizeof(*g_Mats) * g_MaxMats);
	}

	h = matHash(name);
	snprintf(g_Mats[g_NumMats].name, sizeof(g_Mats[g_NumMats].name), "%s", name);
	g_Mats[g_NumMats].rgb = palette[h % ARRAYCOUNT(palette)];
	g_Mats[g_NumMats].solid = 1;
	g_Mats[g_NumMats].texnum = -1;

	g_Mats[g_NumMats].cut = 0;

	// What a surface is - solid, glass, a decal, foliage - is its base
	// material's: the newer C2M merges a surface's layers into one name, the
	// base first and what is laid over it after "__"
	// (concrete_sidewalk_dirty__glass_decal_broken_shards). Read whole, that
	// sidewalk was glass, and glass is not walked on: Town had holes to fall
	// through in front of its shops (F3 2026-10-28, -165 -755). The texture is
	// still the whole name's
	{
		const char *layers = strstr(name, "__");

		snprintf(kind, sizeof(kind), "%.*s", layers ? (int)(layers - name) : (int)strlen(name), name);
	}

	// The picture: the whole name's, else the base's. C2M's _materials.json
	// names only the base materials, and a merged surface's name is in none
	// of its rows: 560 of Town's 956 surfaces were drawn a flat colour
	for (i = 0; i < g_NumTexNames * 2; i++) {
		const s32 t = i % g_NumTexNames;
		const char *want = i < g_NumTexNames ? name : kind;

		if (i == g_NumTexNames && g_Mats[g_NumMats].texnum >= 0) {
			break;
		}

		if (strcmp(g_TexNames[t], want) == 0) {
			g_Mats[g_NumMats].texnum = g_TexNums[t];
			// Black Ops II keeps more than cover in many colour maps' alpha
			// (bricks, floors, whole cars came out holed): cut out only what is
			// meant to be seen through
			// (branches and leaves are not wholly clear between twigs, and with
			// no blend colour to test against they stayed slabs: hidden below)
			g_Mats[g_NumMats].cut = g_TexCut[t] && (strstr(kind, "fence") || strstr(kind, "chain")
					|| strstr(kind, "barbed") || strstr(kind, "_holes") || strstr(kind, "leaf")
					|| strstr(kind, "branch") || strstr(kind, "shrub") || strstr(kind, "bush")
					|| (strstr(kind, "grass") && !strstr(kind, "ground")));
		}
	}

	for (i = 0; i < (s32)ARRAYCOUNT(nosolid); i++) {
		if (strstr(kind, nosolid[i])) {
			g_Mats[g_NumMats].solid = 0;
		}
	}

	g_Mats[g_NumMats].nodraw = 0;
	// glass, and the cut-out cards of trees, bushes and grass (a branch is a
	// picture with see-through between its twigs, and drawn opaque it was a
	// grey slab): the rooms draw opaque alone. A tree keeps its bark
	// Glass and decals want blending, which a room's opaque list has not (a
	// decal cut out flickers against the wall under it); the rest of what is
	// see-through in places (fences, branches, grass) is drawn cut out
	// (decals stay drawn: some are the only floor there is, and hidden they
	// left holes)
	// Chain-link mesh too: the alpha test does not cut it in the port's
	// renderer, and drawn it was a black sheet; its posts and rails are props
	// (chain-link, branches, leaves and grass are cut out now: above; glass
	// and decals are blended, below)
	g_Mats[g_NumMats].unseen = strstr(kind, "gobo") != NULL;
	g_Mats[g_NumMats].blend = strstr(kind, "glass") ? 1 : strstr(kind, "decal") || strstr(kind, "_dec") ? 2 : 0;
	g_Mats[g_NumMats].glow = strstr(kind, "lava_molten") || strstr(kind, "lava_cooling") || strstr(kind, "atmos_glow");

	for (i = 0; i < (s32)ARRAYCOUNT(nodraw); i++) {
		if (strstr(kind, nodraw[i])) {
			g_Mats[g_NumMats].nodraw = 1;
		}
	}

	return g_NumMats++;
}

static void triAdd(const f32 a[3], const f32 b[3], const f32 c[3], const f32 *uv[3], s32 mat)
{
	struct ctri *t;
	f32 u[3], v[3], len, lit;
	u32 rgb = g_Mats[mat].rgb;
	s32 k;

	if (g_NumTris == g_MaxTris) {
		g_MaxTris = g_MaxTris ? g_MaxTris * 2 : 65536;
		g_Tris = realloc(g_Tris, sizeof(*g_Tris) * g_MaxTris);
	}

	t = &g_Tris[g_NumTris];
	memcpy(t->p[0], a, sizeof(f32) * 3);
	memcpy(t->p[1], b, sizeof(f32) * 3);
	memcpy(t->p[2], c, sizeof(f32) * 3);

	for (k = 0; k < 3; k++) {
		t->uv[k][0] = uv[k] ? uv[k][0] : 0;
		t->uv[k][1] = uv[k] ? 1 - uv[k][1] : 0;
	}

	for (k = 0; k < 3; k++) {
		u[k] = b[k] - a[k];
		v[k] = c[k] - a[k];
	}

	t->n[0] = u[1] * v[2] - u[2] * v[1];
	t->n[1] = u[2] * v[0] - u[0] * v[2];
	t->n[2] = u[0] * v[1] - u[1] * v[0];
	len = sqrtf(t->n[0] * t->n[0] + t->n[1] * t->n[1] + t->n[2] * t->n[2]);

	if (len < 0.01f) {
		return; // no area
	}

	for (k = 0; k < 3; k++) {
		t->n[k] /= len;
	}

	// Tranzit's low, hazy sun from the west and above, either face alike, and
	// the sky's own light on everything; a textured face is its picture so
	// lit, an untextured one its flat colour. Under a roof it is darker still
	// (shadePass())
	lit = 0.5f + 0.5f * fabsf(t->n[1] * 0.72f - t->n[0] * 0.6f + t->n[2] * 0.35f);

	if (g_Mats[mat].texnum >= 0) {
		rgb = 0xffffff;
	}

	if (g_Mats[mat].glow) {
		lit = 1;
	}
	t->colour = (u32)(((rgb >> 16) & 0xff) * lit) << 24 | (u32)(((rgb >> 8) & 0xff) * lit) << 16
		| (u32)((rgb & 0xff) * lit) << 8 | 0xff;
	t->mat = mat;
	t->solid = g_Mats[mat].solid;
	t->hide = 0;
	t->scan = 0;
	g_NumTris++;
}

/* ------------------------------------------------------------------------ */

static void codToOurs(const f32 cod[3], const f32 *centre, f32 out[3])
{
	out[0] = (cod[0] - centre[0]) * CODMAP_SCALE;
	out[1] = cod[2] * CODMAP_SCALE;
	out[2] = -(cod[1] - centre[1]) * CODMAP_SCALE;
}

/**
 * C2M's newer export (2026-10-28): the map's OBJ, its placements
 * (_static_models.json: Position, RotationDegrees and ModelScale, each an
 * object of X, Y and Z), _materials.json and _entities.txt beside it, every
 * model in exported_models/<game>/ as <name>.obj and <name>_LOD1..3.obj, all of
 * it in inches. The older export wrote the map in centimetres, _xmodels.json
 * (PosX..RotZ, Scale) and _mapEnts.txt, its props from Greyhound in cm. Set by
 * codmapConvertRow() from which placements file is there.
 */
static s32 g_NewC2m;

// Town's collision is the game's own, read by bo2scan (readScan())
static s32 g_ScanCollision;

// a prop being placed keeps its collision beside the scan's (Survival's wrecks)
static s32 g_PropKeepCollision;

// The OBJ's triangles whose middle is in the row's box, into g_Tris
static s32 readObj(const char *path, const struct codmaprow *row, const f32 *centre)
{
	FILE *f = fopen(path, "rb");
	char line[1024];
	f32 *verts = NULL;
	s32 numverts = 0, maxverts = 0;
	f32 *uvs = NULL;
	s32 numuvs = 0, maxuvs = 0;
	s32 mat = -1;

	if (!f) {
		sysLogPrintf(LOG_ERROR, "codmap: no %s", path);
		return 0;
	}

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == 'v' && line[1] == ' ') {
			if (numverts == maxverts) {
				maxverts = maxverts ? maxverts * 2 : 1 << 20;
				verts = realloc(verts, sizeof(f32) * 3 * maxverts);
			}

			sscanf(line + 2, "%f %f %f", &verts[numverts * 3], &verts[numverts * 3 + 1], &verts[numverts * 3 + 2]);

			// The older C2M wrote the map in centimetres; its entities and placements,
			// and the row's box, are inches (read as inches, Town was 2.54 times
			// its size and the box cut out somewhere else)
			if (!g_NewC2m) {
				verts[numverts * 3] /= 2.54f;
				verts[numverts * 3 + 1] /= 2.54f;
				verts[numverts * 3 + 2] /= 2.54f;
			}
			numverts++;
		} else if (line[0] == 'v' && line[1] == 't' && line[2] == ' ') {
			if (numuvs == maxuvs) {
				maxuvs = maxuvs ? maxuvs * 2 : 1 << 20;
				uvs = realloc(uvs, sizeof(f32) * 2 * maxuvs);
			}

			uvs[numuvs * 2] = uvs[numuvs * 2 + 1] = 0;
			sscanf(line + 3, "%f %f", &uvs[numuvs * 2], &uvs[numuvs * 2 + 1]);
			numuvs++;
		} else if (strncmp(line, "usemtl ", 7) == 0) {
			char *e = line + strlen(line);

			while (e > line + 7 && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) {
				*--e = 0;
			}

			mat = matFind(line + 7);
		} else if (line[0] == 'f' && line[1] == ' ') {
			s32 idx[16];
			s32 tidx[16];
			s32 n = 0;
			char *s = line + 2;
			f32 cx = 0, cy = 0, cz = 0;
			s32 k;

			while (*s && n < 16) {
				while (*s == ' ') {
					s++;
				}

				if (!*s || *s == '\n' || *s == '\r') {
					break;
				}

				idx[n] = atoi(s);
				idx[n] = idx[n] < 0 ? numverts + idx[n] : idx[n] - 1;
				tidx[n] = -1;

				while (*s && *s != ' ' && *s != '/') {
					s++;
				}

				if (*s == '/' && s[1] != '/') {
					tidx[n] = atoi(s + 1);
					tidx[n] = tidx[n] < 0 ? numuvs + tidx[n] : tidx[n] - 1;

					if (tidx[n] >= numuvs) {
						tidx[n] = -1;
					}
				}

				n++;

				while (*s && *s != ' ') {
					s++;
				}
			}

			if (n < 3 || mat < 0 || g_Mats[mat].nodraw) {
				continue;
			}

			for (k = 0; k < n; k++) {
				if (idx[k] < 0 || idx[k] >= numverts) {
					n = 0;
					break;
				}

				cx += verts[idx[k] * 3];
				cy += verts[idx[k] * 3 + 1];
				cz += verts[idx[k] * 3 + 2];
			}

			if (n < 3) {
				continue;
			}

			cx /= n;
			cy /= n;
			cz /= n;

			if (cx < row->box[0] || cx > row->box[1] || cy < row->box[2] || cy > row->box[3] || cz < row->zmin) {
				continue;
			}

			for (k = 1; k < n - 1; k++) {
				f32 a[3], b[3], c[3];

				codToOurs(&verts[idx[0] * 3], centre, a);
				codToOurs(&verts[idx[k] * 3], centre, b);
				codToOurs(&verts[idx[k + 1] * 3], centre, c);
				{
					const f32 *uv[3] = {
						tidx[0] >= 0 ? &uvs[tidx[0] * 2] : NULL,
						tidx[k] >= 0 ? &uvs[tidx[k] * 2] : NULL,
						tidx[k + 1] >= 0 ? &uvs[tidx[k + 1] * 2] : NULL,
					};

					triAdd(a, b, c, uv, mat);
				}
			}
		}
	}

	fclose(f);
	free(verts);
	free(uvs);
	sysLogPrintf(LOG_NOTE, "codmap: %s: %d vertices, %d triangles in the box, %d materials", path, numverts, g_NumTris, g_NumMats);

	return g_NumTris > 0;
}

/* ------------------------------------------------------------------------ */

/**
 * The props: every model C2M's _xmodels.json places in the row's box, from
 * Greyhound's export of it (--codmap-props <its xmodels/>): its least
 * detailed OBJ (the most detailed of Town's props come to 744,000 triangles,
 * the least to 146,000, against the map's 80,000), in the model's own inches,
 * z up; turned by the placement's angles (Maya's order, as C2M's own import
 * scripts set them: x, then y, then z), scaled and moved, then into ours as the
 * map is. A prop is solid when it is bigger than CODMAP_PROP_SOLID across or
 * high: bottles and papers are not.
 */
#define CODMAP_PROP_SOLID 24.0f // inches

struct cmodel {
	char name[96];
	s32 detail;       // drawn (LOD1) or its collision (the least detailed)
	s32 tried;
	s32 numtris;
	f32 (*p)[3][3];
	f32 (*uv)[3][2];
	s32 *mat;
	u8 solid;
};

static struct cmodel *g_Models;
static s32 g_NumModels, g_MaxModels;

static struct cmodel *modelGet(const char *dir, const char *name, s32 detail)
{
	char path[1024];
	struct cmodel *m;
	FILE *f = NULL;
	char line[1024];
	f32 *verts = NULL, *uvs = NULL;
	s32 numverts = 0, maxverts = 0, numuvs = 0, maxuvs = 0, maxtris = 0;
	f32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
	s32 mat = -1;
	s32 i, lod;

	for (i = 0; i < g_NumModels; i++) {
		if (strcmp(g_Models[i].name, name) == 0 && g_Models[i].detail == detail) {
			return g_Models[i].numtris ? &g_Models[i] : NULL;
		}
	}

	if (g_NumModels == g_MaxModels) {
		g_MaxModels = g_MaxModels ? g_MaxModels * 2 : 128;
		g_Models = realloc(g_Models, sizeof(*g_Models) * g_MaxModels);
	}

	m = &g_Models[g_NumModels++];
	memset(m, 0, sizeof(*m));
	snprintf(m->name, sizeof(m->name), "%s", name);
	m->detail = detail;

	// the second most detailed (LOD1: Town's props come to 337,000 triangles,
	// against 146,000 at the least detailed, whose cars were boxes), else the
	// least detailed there is
	// (the newer C2M's are side by side in one folder, its most detailed with no
	// _LOD in the name)
	if (detail) {
		if (g_NewC2m) {
			snprintf(path, sizeof(path), "%s/%s_LOD1.obj", dir, name);
		} else {
			snprintf(path, sizeof(path), "%s/%s/%s_LOD1.obj", dir, name, name);
		}

		f = fopen(path, "rb");
	}

	for (lod = 7; lod >= 0 && !f; lod--) {
		if (g_NewC2m && lod == 0) {
			snprintf(path, sizeof(path), "%s/%s.obj", dir, name);
		} else if (g_NewC2m) {
			snprintf(path, sizeof(path), "%s/%s_LOD%d.obj", dir, name, lod);
		} else {
			snprintf(path, sizeof(path), "%s/%s/%s_LOD%d.obj", dir, name, name, lod);
		}

		f = fopen(path, "rb");
	}

	if (!f) {
		return NULL;
	}

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == 'v' && line[1] == ' ') {
			if (numverts == maxverts) {
				maxverts = maxverts ? maxverts * 2 : 4096;
				verts = realloc(verts, sizeof(f32) * 3 * maxverts);
			}

			sscanf(line + 2, "%f %f %f", &verts[numverts * 3], &verts[numverts * 3 + 1], &verts[numverts * 3 + 2]);

			// Greyhound writes centimetres; the map's placements are inches, as
			// the newer C2M's models are
			for (i = 0; i < 3; i++) {
				if (!g_NewC2m) {
					verts[numverts * 3 + i] /= 2.54f;
				}
				lo[i] = verts[numverts * 3 + i] < lo[i] ? verts[numverts * 3 + i] : lo[i];
				hi[i] = verts[numverts * 3 + i] > hi[i] ? verts[numverts * 3 + i] : hi[i];
			}

			numverts++;
		} else if (line[0] == 'v' && line[1] == 't' && line[2] == ' ') {
			if (numuvs == maxuvs) {
				maxuvs = maxuvs ? maxuvs * 2 : 4096;
				uvs = realloc(uvs, sizeof(f32) * 2 * maxuvs);
			}

			uvs[numuvs * 2] = uvs[numuvs * 2 + 1] = 0;
			sscanf(line + 3, "%f %f", &uvs[numuvs * 2], &uvs[numuvs * 2 + 1]);
			numuvs++;
		} else if (strncmp(line, "usemtl ", 7) == 0) {
			char *e = line + strlen(line);

			while (e > line + 7 && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) {
				*--e = 0;
			}

			mat = matFind(line + 7);
		} else if (line[0] == 'f' && line[1] == ' ' && mat >= 0 && !g_Mats[mat].nodraw) {
			s32 vi[3], ti[3];

			if (sscanf(line + 2, "%d/%d/%*d %d/%d/%*d %d/%d/%*d", &vi[0], &ti[0], &vi[1], &ti[1], &vi[2], &ti[2]) != 6) {
				continue;
			}

			if (m->numtris == maxtris) {
				maxtris = maxtris ? maxtris * 2 : 1024;
				m->p = realloc(m->p, sizeof(*m->p) * maxtris);
				m->uv = realloc(m->uv, sizeof(*m->uv) * maxtris);
				m->mat = realloc(m->mat, sizeof(*m->mat) * maxtris);
			}

			for (i = 0; i < 3; i++) {
				const s32 v = vi[i] - 1, t = ti[i] - 1;

				if (v < 0 || v >= numverts) {
					break;
				}

				memcpy(m->p[m->numtris][i], &verts[v * 3], sizeof(f32) * 3);
				m->uv[m->numtris][i][0] = t >= 0 && t < numuvs ? uvs[t * 2] : 0;
				m->uv[m->numtris][i][1] = t >= 0 && t < numuvs ? uvs[t * 2 + 1] : 0;

				// the newer C2M writes a model's t the other way up from its map's
				// (triAdd() turns it over again): a car's paint came out a jumble
				if (g_NewC2m) {
					m->uv[m->numtris][i][1] = 1.0f - m->uv[m->numtris][i][1];
				}
			}

			if (i == 3) {
				m->mat[m->numtris++] = mat;
			}
		}
	}

	fclose(f);
	free(verts);
	free(uvs);

	m->solid = (hi[0] - lo[0] > CODMAP_PROP_SOLID || hi[1] - lo[1] > CODMAP_PROP_SOLID
			|| hi[2] - lo[2] > CODMAP_PROP_SOLID);

	return m->numtris ? m : NULL;
}

// One placement's triangles into g_Tris
static s32 propPlace(const struct cmodel *m, const f32 pos[3], const f32 rot[3], f32 scale, const f32 *centre, s32 collision)
{
	f32 r[3][3];
	f32 c[3], s[3];
	s32 t, k, i;

	for (i = 0; i < 3; i++) {
		c[i] = cosf(rot[i] * (f32)M_PI / 180.0f);
		s[i] = sinf(rot[i] * (f32)M_PI / 180.0f);
	}

	// Rz * Ry * Rx
	r[0][0] = c[2] * c[1];
	r[0][1] = c[2] * s[1] * s[0] - s[2] * c[0];
	r[0][2] = c[2] * s[1] * c[0] + s[2] * s[0];
	r[1][0] = s[2] * c[1];
	r[1][1] = s[2] * s[1] * s[0] + c[2] * c[0];
	r[1][2] = s[2] * s[1] * c[0] - c[2] * s[0];
	r[2][0] = -s[1];
	r[2][1] = c[1] * s[0];
	r[2][2] = c[1] * c[0];

	for (t = 0; t < m->numtris; t++) {
		f32 out[3][3];
		const f32 *uv[3];

		for (k = 0; k < 3; k++) {
			f32 cod[3];

			for (i = 0; i < 3; i++) {
				cod[i] = pos[i] + scale * (r[i][0] * m->p[t][k][0] + r[i][1] * m->p[t][k][1] + r[i][2] * m->p[t][k][2]);
			}

			codToOurs(cod, centre, out[k]);
			uv[k] = m->uv[t][k];
		}

		triAdd(out[0], out[1], out[2], uv, m->mat[t]);

		// drawn and not solid, or solid (if big enough) and not drawn
		if (g_NumTris > 0) {
			g_Tris[g_NumTris - 1].solid = collision && m->solid && g_Tris[g_NumTris - 1].solid;
			g_Tris[g_NumTris - 1].hide = collision;
			g_Tris[g_NumTris - 1].scan = collision && g_PropKeepCollision;
		}
	}

	return m->numtris;
}

static char *jsonValue(char *s)
{
	while (*s == ' ' || *s == ':' || *s == '"' || *s == '\t') {
		s++;
	}

	return s;
}

// An object's X, Y and Z after key in [s, end) (the newer C2M's placements)
static void jsonXyz(const char *s, const char *end, const char *key, f32 out[3])
{
	static const char *const axes[] = { "\"X\"", "\"Y\"", "\"Z\"" };
	const char *at = strstr(s, key);
	const char *close;
	s32 i;

	if (!at || at >= end || (close = strchr(at, '}')) == NULL) {
		return;
	}

	for (i = 0; i < 3; i++) {
		const char *k = strstr(at, axes[i]);

		if (k && k < close) {
			out[i] = (f32)atof(jsonValue((char *)k + 3));
		}
	}
}

static void placeOne(const char *modeldir, const char *name, const f32 pos[3], const f32 rot[3], f32 scale,
		const struct codmaprow *row, const f32 *centre, s32 *placed, s32 *missing, s32 *tris)
{
	const struct cmodel *m;

	if (!name[0] || pos[0] < row->box[0] || pos[0] > row->box[1] || pos[1] < row->box[2] || pos[1] > row->box[3]
			|| pos[2] < row->zmin) {
		return;
	}

	m = modelGet(modeldir, name, 1);

	if (!m) {
		(*missing)++;
		return;
	}

	*tris += propPlace(m, pos, rot, scale, centre, 0);

	// its collision from its simplest model: LOD1's tiles were too many for
	// the tiles file (convertTiles() overran)
	if (m->solid && (m = modelGet(modeldir, name, 0)) != NULL) {
		propPlace(m, pos, rot, scale, centre, 1);
	}

	(*placed)++;
}

// Every placement in the row's box (C2M's _xmodels.json, one object a prop;
// the newer C2M's _static_models.json, one a "Name" to the next)
static void readProps(const char *jsonpath, const char *modeldir, const struct codmaprow *row, const f32 *centre)
{
	FILE *f = fopen(jsonpath, "rb");
	char *text, *at;
	long len;
	s32 placed = 0, missing = 0, tris = 0;

	if (!f) {
		sysLogPrintf(LOG_WARNING, "codmap: no %s, so no props", jsonpath);
		return;
	}

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	text = malloc(len + 1);
	len = (long)fread(text, 1, len, f);
	text[len] = 0;
	fclose(f);

	if (g_NewC2m) {
		for (at = strstr(text, "\"Name\""); at; ) {
			char *next = strstr(at + 6, "\"Name\"");
			char *end = next ? next : text + len;
			char name[96] = "";
			f32 pos[3] = { 0, 0, 0 }, rot[3] = { 0, 0, 0 }, scale[3] = { 1, 1, 1 };

			sscanf(jsonValue(at + 6), "%95[^\"]", name);
			jsonXyz(at, end, "\"Position\"", pos);
			jsonXyz(at, end, "\"RotationDegrees\"", rot);
			jsonXyz(at, end, "\"ModelScale\"", scale);
			placeOne(modeldir, name, pos, rot, scale[0], row, centre, &placed, &missing, &tris);
			at = next;
		}

		free(text);
		sysLogPrintf(LOG_NOTE, "codmap: %d props placed (%d triangles), %d with no model exported", placed, tris, missing);
		return;
	}

	for (at = strchr(text, '{'); at; at = strchr(at + 1, '{')) {
		char *end = strchr(at, '}');
		char name[96] = "";
		f32 pos[3] = { 0, 0, 0 }, rot[3] = { 0, 0, 0 }, scale = 1;
		static const char *const keys[] = { "\"PosX\"", "\"PosY\"", "\"PosZ\"", "\"RotX\"", "\"RotY\"", "\"RotZ\"", "\"Scale\"" };
		f32 *dst[] = { &pos[0], &pos[1], &pos[2], &rot[0], &rot[1], &rot[2], &scale };
		char *k;
		s32 i;

		if (!end) {
			break;
		}

		*end = 0;
		k = strstr(at, "\"Name\"");

		// a value: past the key, the colon, any space and the opening quote
		if (k) {
			k = jsonValue(k + 6);
			sscanf(k, "%95[^\"]", name);
		}

		for (i = 0; i < 7; i++) {
			k = strstr(at, keys[i]);

			if (k) {
				*dst[i] = (f32)atof(jsonValue(k + strlen(keys[i])));
			}
		}

		*end = '}';
		placeOne(modeldir, name, pos, rot, scale, row, centre, &placed, &missing, &tris);
	}

	free(text);
	sysLogPrintf(LOG_NOTE, "codmap: %d props placed (%d triangles), %d with no model exported", placed, tris, missing);
}

/* ------------------------------------------------------------------------ */

struct codspawn {
	f32 pos[3];
	f32 yaw;
};

// The row's player spawns from the map's entities (C2M's _entities.txt or _mapEnts.txt)
static s32 readSpawns(const char *path, const struct codmaprow *row, const f32 *centre, struct codspawn *out, s32 max)
{
	FILE *f = fopen(path, "rb");
	char line[512];
	char origin[128] = "", angles[128] = "", target[128] = "";
	s32 n = 0;

	if (!f) {
		return 0;
	}

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '{') {
			origin[0] = angles[0] = target[0] = 0;
		} else if (line[0] == '}') {
			if (strcmp(target, row->spawns) == 0 && origin[0] && n < max) {
				f32 cod[3] = { 0, 0, 0 };
				f32 ang[3] = { 0, 0, 0 };

				sscanf(origin, "%f %f %f", &cod[0], &cod[1], &cod[2]);
				sscanf(angles, "%f %f %f", &ang[0], &ang[1], &ang[2]);
				codToOurs(cod, centre, out[n].pos);
				out[n].yaw = ang[1] * (f32)M_PI / 180.0f;
				n++;
			}
		} else {
			char key[64], value[256];

			if (sscanf(line, " \"%63[^\"]\" \"%255[^\"]\"", key, value) == 2) {
				if (strcmp(key, "origin") == 0) {
					snprintf(origin, sizeof(origin), "%s", value);
				} else if (strcmp(key, "angles") == 0) {
					snprintf(angles, sizeof(angles), "%s", value);
				} else if (strcmp(key, "targetname") == 0) {
					snprintf(target, sizeof(target), "%s", value);
				}
			}
		}
	}

	fclose(f);
	return n;
}

/* ------------------------------------------------------------------------ */

struct croom {
	f32 x0, z0;         // the cell
	f32 centre[3];
	s32 *tris;          // drawn here: those whose middle is in the cell
	s32 numtris, maxtris;
	cbuf data;          // as written, before compression
	s32 bbox[6];        // about the centre
	s32 drawnwith;      // a piece of this room (1-based), drawn whenever it is; 0 a room of the grid
};

static s32 g_GridX, g_GridZ;
static f32 g_GridX0, g_GridZ0;
static f32 g_MinY, g_MaxY;

static s32 cellOf(f32 x, f32 z)
{
	s32 ix = (s32)floorf((x - g_GridX0) / CODMAP_CELL);
	s32 iz = (s32)floorf((z - g_GridZ0) / CODMAP_CELL);

	ix = ix < 0 ? 0 : ix >= g_GridX ? g_GridX - 1 : ix;
	iz = iz < 0 ? 0 : iz >= g_GridZ ? g_GridZ - 1 : iz;

	return ix * g_GridZ + iz;
}

static void roomAddTri(struct croom *r, s32 t)
{
	if (r->numtris == r->maxtris) {
		r->maxtris = r->maxtris ? r->maxtris * 2 : 256;
		r->tris = realloc(r->tris, sizeof(s32) * r->maxtris);
	}

	r->tris[r->numtris++] = t;
}

static void gfxWords(cbuf *b, const Gfx *g)
{
	cbU32(b, (u32)g->words.w0);
	cbU32(b, (u32)g->words.w1);
}

static s16 s16of(f32 v)
{
	s32 i = (s32)floorf(v + 0.5f);

	return i < -32768 ? -32768 : i > 32767 ? 32767 : i;
}

// The opaque materials first, then the blended (glass, then decals); a
// texture's triangles together in each
static int triTexCompare(const void *a, const void *b)
{
	const struct cmat *ma = &g_Mats[g_Tris[*(const s32 *)a].mat];
	const struct cmat *mb = &g_Mats[g_Tris[*(const s32 *)b].mat];

	if (ma->blend != mb->blend) {
		return ma->blend < mb->blend ? -1 : 1;
	}

	return ma->texnum < mb->texnum ? -1 : ma->texnum > mb->texnum;
}

/**
 * A room as Perfect Dark's room format has it (geconvert.c's roomPack()): its
 * header, one opaque leaf, its vertices (x y z, flags, colour slot, s t), its
 * colours and the leaf's list - the state, then batches of up to sixteen
 * vertices, each loaded with its colours and drawn four triangles a command.
 */
static void roomWrite(struct croom *r, u32 baseptr)
{
	cbuf words = {0};
	cbuf vtx = {0};
	cbuf col = {0};
	s32 numvtx = 0;
	s32 mn[3] = { 32767, 32767, 32767 }, mx[3] = { -32768, -32768, -32768 };
	size_t vtxat, colat, gdlat;
	size_t xlugdl = 0;  // where the translucent leaf's list starts in words, 0 none
	s32 xlubase = 0;    // and its first vertex
	s32 i = 0;
	s32 c;

	// The state that holds throughout: both faces. Each material sets two
	// cycles and a render mode as a ROM room's list does, so that the stage's
	// fog takes it (gfxReplaceGbiCommands(), gfxreplace.c's group 1, turns
	// G_RM_PASS into G_RM_FOG_SHADE_A on a stage with fog)
	{
		Gfx g[2];

		gSPClearGeometryMode(&g[0], G_LIGHTING | G_TEXTURE_GEN | G_CULL_BOTH);
		gSPSetGeometryMode(&g[1], G_SHADE | G_SHADING_SMOOTH);

		for (c = 0; c < 2; c++) {
			gfxWords(&words, &g[c]);
		}
	}

	// a texture's triangles together
	qsort(r->tris, r->numtris, sizeof(s32), triTexCompare);

	while (i < r->numtris) {
		f32 batch[16][3];
		s16 bst[16][2];
		u32 bcol[16];
		u8 idx[64][3];
		s32 nb = 0, nt = 0;
		const s32 texnum = g_Mats[g_Tris[r->tris[i]].mat].texnum;
		const s32 blend = g_Mats[g_Tris[r->tris[i]].mat].blend;
		s32 start;
		s32 k;

		// the first blended triangle: the opaque list ends, and the
		// translucent leaf's begins (bg.c draws it after the props, as a ROM
		// room's second leaf), its vertex offsets from its own first
		if (blend && !xlugdl) {
			Gfx g[2];

			cbU32(&words, 0xb8000000u);
			cbU32(&words, 0);
			xlugdl = words.n;
			xlubase = numvtx;
			gSPClearGeometryMode(&g[0], G_LIGHTING | G_TEXTURE_GEN | G_CULL_BOTH);
			gSPSetGeometryMode(&g[1], G_SHADE | G_SHADING_SMOOTH);
			gfxWords(&words, &g[0]);
			gfxWords(&words, &g[1]);
		}

		start = numvtx - (xlugdl ? xlubase : 0);

		// a new texture: the room's command for it (texLoadFromGdl() loads
		// it there and patches the G_TEXTURE before it), the picture times
		// the vertex colour; or none, the vertex colour alone
		if (i == 0 || g_Mats[g_Tris[r->tris[i - 1]].mat].texnum != texnum
				|| g_Mats[g_Tris[r->tris[i - 1]].mat].blend != blend) {
			// a cut-out (a fence's mesh, a branch's twigs): its picture's
			// clear texels not drawn
			const s32 cut = texnum >= 0 && g_Mats[g_Tris[r->tris[i]].mat].cut;
			Gfx g[4];

			// a cut-out under G_RM_AA_ZB_TEX_EDGE2: the renderer drops its texels
			// under a fifth alpha (gfx_opengl.cpp's texture edge)
			gDPPipeSync(&g[0]);
			gDPSetCycleType(&g[1], G_CYC_2CYCLE);

			// glass blended over what is behind it, a decal over the face it
			// lies on (the decal mode keeps it off that face's depth)
			if (blend == 1) {
				gDPSetRenderMode(&g[2], G_RM_PASS, G_RM_AA_ZB_XLU_SURF2);
			} else if (blend == 2) {
				gDPSetRenderMode(&g[2], G_RM_PASS, G_RM_AA_ZB_XLU_DECAL2);
			} else if (cut) {
				gDPSetRenderMode(&g[2], G_RM_PASS, G_RM_AA_ZB_TEX_EDGE2);
			} else {
				gDPSetRenderMode(&g[2], G_RM_PASS, G_RM_AA_ZB_OPA_SURF2);
			}

			gDPSetAlphaCompare(&g[3], G_AC_NONE);
			gfxWords(&words, &g[0]);
			gfxWords(&words, &g[1]);
			gfxWords(&words, &g[2]);
			gfxWords(&words, &g[3]);

			if (texnum >= 0) {
				gSPTexture(&g[0], 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
				gfxWords(&words, &g[0]);
				// subcmd 4: one tile, wrapping both ways
				cbU32(&words, 0xc0000000u | 4);
				cbU32(&words, (u32)texnum & 0xfff);
				if (cut || blend) {
					gDPSetCombineMode(&g[1], G_CC_MODULATERGBA, G_CC_PASS2);
				} else {
					gDPSetCombineMode(&g[1], G_CC_MODULATERGB, G_CC_PASS2);
				}
				gfxWords(&words, &g[1]);
			} else {
				gSPTexture(&g[0], 0, 0, 0, G_TX_RENDERTILE, G_OFF);
				gDPSetCombineMode(&g[1], G_CC_SHADE, G_CC_PASS2);
				gfxWords(&words, &g[0]);
				gfxWords(&words, &g[1]);
			}
		}

		for (; i < r->numtris && nt < 64; i++) {
			const struct ctri *t = &g_Tris[r->tris[i]];
			s32 got[3];
			s32 added = nb;
			s32 ok = 1;
			s16 st[3][2];
			f32 base[2];

			if (g_Mats[t->mat].texnum != texnum || g_Mats[t->mat].blend != blend) {
				break;
			}

			// the texture's coordinates in its placeholder's texels, a
			// thirty-second each, moved by whole repeats to near 0 so that
			// they fit sixteen bits
			for (c = 0; c < 2; c++) {
				base[c] = floorf(MIN(t->uv[0][c], MIN(t->uv[1][c], t->uv[2][c])));
			}

			for (k = 0; k < 3; k++) {
				for (c = 0; c < 2; c++) {
					st[k][c] = s16of((t->uv[k][c] - base[c]) * CODMAP_TEX * 32);
				}
			}

			for (k = 0; k < 3 && ok; k++) {
				s32 j = 0;

				while (j < added && !(batch[j][0] == t->p[k][0] && batch[j][1] == t->p[k][1]
							&& batch[j][2] == t->p[k][2] && bcol[j] == t->colour
							&& bst[j][0] == st[k][0] && bst[j][1] == st[k][1])) {
					j++;
				}

				if (j == added) {
					if (added == 16) {
						ok = 0;
						break;
					}

					memcpy(batch[added], t->p[k], sizeof(batch[added]));
					bst[added][0] = st[k][0];
					bst[added][1] = st[k][1];
					bcol[added] = t->colour;
					added++;
				}

				got[k] = j;
			}

			if (!ok) {
				break;
			}

			nb = added;
			idx[nt][0] = got[0];
			idx[nt][1] = got[1];
			idx[nt][2] = got[2];
			nt++;
		}

		for (k = 0; k < nb; k++) {
			s16 p[3];

			for (c = 0; c < 3; c++) {
				p[c] = s16of(batch[k][c] - r->centre[c]);
				mn[c] = p[c] < mn[c] ? p[c] : mn[c];
				mx[c] = p[c] > mx[c] ? p[c] : mx[c];
				cbU16(&vtx, (u16)p[c]);
			}

			cbU8(&vtx, 0);
			cbU8(&vtx, k << 2);
			cbU16(&vtx, (u16)bst[k][0]);
			cbU16(&vtx, (u16)bst[k][1]);
			cbU32(&col, bcol[k]);
			numvtx++;
		}

		cbU32(&words, (0x07u << 24) | ((((nb - 1) << 2) & 0xff) << 16) | (u32)(nb * 4));
		cbU32(&words, 0x0d000000u | (u32)(start * 4));
		cbU32(&words, (0x04u << 24) | ((u32)(nb - 1) << 20) | (u32)(nb * 12));
		cbU32(&words, 0x0e000000u | (u32)(start * 12));

		for (k = 0; k < nt; k += 4) {
			u32 w0 = 0xb1u << 24, w1 = 0;
			s32 q;

			for (q = 0; q < 4 && k + q < nt; q++) {
				w1 |= (u32)idx[k + q][0] << (8 * q);
				w1 |= (u32)idx[k + q][1] << (8 * q + 4);
				w0 |= (u32)idx[k + q][2] << (4 * q);
			}

			cbU32(&words, w0);
			cbU32(&words, w1);
		}
	}

	cbU32(&words, 0xb8000000u);
	cbU32(&words, 0);

	// an odd count is made even, as the format wants
	if (numvtx & 1) {
		cbZeros(&vtx, 12);
		cbU32(&col, 0);
		numvtx++;
	}

	vtxat = (0x18 + 20 * (xlugdl ? 2 : 1) + 7) & ~7u;
	colat = vtxat + vtx.n;
	gdlat = (colat + col.n + 7) & ~7u;

	cbZeros(&r->data, gdlat);
	set32(r->data.v, 0, baseptr + (u32)vtxat);
	set32(r->data.v, 4, baseptr + (u32)colat);
	set32(r->data.v, 8, baseptr + 0x18);
	set32(r->data.v, 12, xlugdl ? baseptr + 0x18 + 20 : 0);

	// the translucent leaf: its list, and its vertices and colours from its own first
	if (xlugdl) {
		r->data.v[0x18 + 20] = 0;
		set32(r->data.v, 0x18 + 20 + 4, 0);
		set32(r->data.v, 0x18 + 20 + 8, baseptr + (u32)(gdlat + xlugdl));
		set32(r->data.v, 0x18 + 20 + 12, baseptr + (u32)(vtxat + 12 * xlubase));
		set32(r->data.v, 0x18 + 20 + 16, baseptr + (u32)(colat + 4 * xlubase));
	}
	set16(r->data.v, 0x10, 0xffff);
	set16(r->data.v, 0x12, 0);
	set16(r->data.v, 0x14, numvtx < 32767 ? numvtx : 32767);
	set16(r->data.v, 0x16, numvtx < 32767 ? numvtx : 32767);
	// the leaf
	r->data.v[0x18] = 0;
	set32(r->data.v, 0x18 + 4, 0);
	set32(r->data.v, 0x18 + 8, baseptr + (u32)gdlat);
	set32(r->data.v, 0x18 + 12, baseptr + (u32)vtxat);
	set32(r->data.v, 0x18 + 16, baseptr + (u32)colat);
	memcpy(r->data.v + vtxat, vtx.v, vtx.n);
	memcpy(r->data.v + colat, col.v, col.n);
	cbPut(&r->data, words.v, words.n);

	if (numvtx > 32767) {
		sysLogPrintf(LOG_WARNING, "codmap: a room has %d vertices, over the format's 32767", numvtx);
	}

	// its box: what it draws, and its whole cell over the level's height, so
	// that every place is found in a room (bgFindRoomsByPos())
	{
		const f32 cell[6] = { r->x0, g_MinY, r->z0, r->x0 + CODMAP_CELL, g_MaxY, r->z0 + CODMAP_CELL };

		for (c = 0; c < 3; c++) {
			const s32 lo = s16of(cell[c] - r->centre[c]);
			const s32 hi = s16of(cell[3 + c] - r->centre[c]);

			r->bbox[c] = numvtx && mn[c] < lo ? mn[c] : lo;
			r->bbox[3 + c] = numvtx && mx[c] > hi ? mx[c] : hi;
		}
	}

	free(words.v);
	free(vtx.v);
	free(col.v);
}

struct cportal {
	s32 room1, room2; // 1-based
	f32 pts[4][3];
};

static cbuf writeBg(struct croom *rooms, s32 numrooms, const struct cportal *portals, s32 numportals)
{
	const size_t tableat = 0x18;
	const size_t cmdsat = tableat + 20 * (numrooms + 3);
	const size_t portalsat = cmdsat + 8;
	cbuf portalrecs = {0}, groups = {0}, primary = {0}, primz, out = {0}, roomsblob = {0};
	size_t groupsat, inf;
	u32 ptr;
	s32 r, i, c;

	for (i = 0; i < numportals; i++) {
		cbU16(&portalrecs, i + 1);
		cbU16(&portalrecs, portals[i].room1);
		cbU16(&portalrecs, portals[i].room2);
		cbU8(&portalrecs, 0);
		cbU8(&portalrecs, 0);
		cbU8(&groups, 4);
		cbZeros(&groups, 3);

		for (s32 k = 0; k < 4; k++) {
			for (c = 0; c < 3; c++) {
				cbF32(&groups, portals[i].pts[k][c]);
			}
		}
	}

	cbZeros(&portalrecs, 8);
	cbZeros(&groups, 4);

	groupsat = portalsat + portalrecs.n;
	cbZeros(&primary, groupsat + groups.n);
	cbPad(&primary, 4);
	set32(primary.v, 4, SEG_BG + (u32)tableat);
	set32(primary.v, 8, SEG_BG + (u32)portalsat);
	set32(primary.v, 12, SEG_BG + (u32)cmdsat);
	// no visibility commands: the end of the list
	primary.v[cmdsat] = 0;
	primary.v[cmdsat + 1] = 1;
	memcpy(primary.v + portalsat, portalrecs.v, portalrecs.n);
	memcpy(primary.v + groupsat, groups.v, groups.n);
	inf = primary.n;

	ptr = SEG_BG + (u32)inf;

	for (r = 0; r < numrooms; r++) {
		const size_t e = tableat + 20 * (r + 1);
		cbuf z;

		roomWrite(&rooms[r], ptr);
		z = rzip(rooms[r].data.v, rooms[r].data.n);
		set32(primary.v, e, ptr);

		for (c = 0; c < 3; c++) {
			setf32(primary.v, e + 4 + 4 * c, rooms[r].centre[c]);
		}

		primary.v[e + 16] = 128;
		primary.v[e + 17] = 255;
		// a piece: the room it is drawn with (the row's two bytes of padding)
		set16(primary.v, e + 18, (u32)rooms[r].drawnwith);
		cbPut(&roomsblob, z.v, z.n);
		ptr += (u32)z.n;
		free(z.v);
	}

	set32(primary.v, tableat + 20 * (numrooms + 1), ptr);

	primz = rzip(primary.v, primary.n);
	cbU32(&out, (u32)inf);
	cbU32(&out, 0);
	cbU32(&out, (u32)primz.n);
	cbPut(&out, primz.v, primz.n);
	cbPut(&out, roomsblob.v, roomsblob.n);
	cbPad(&out, 2);
	set32(out.v, 4, (u32)(out.n - 0xc));

	// section 2: the textures the rooms use (none yet)
	{
		cbuf tz = rzip(NULL, 0);

		cbU16(&out, 0x8000);
		cbU16(&out, (u32)tz.n);
		cbPut(&out, tz.v, tz.n);
		free(tz.v);
	}

	// section 3: each room's box about its centre, its list's length in 16s, its lights
	{
		cbuf s3 = {0}, s3z;

		for (r = 0; r < numrooms; r++) {
			for (c = 0; c < 6; c++) {
				cbU16(&s3, (u16)rooms[r].bbox[c]);
			}
		}

		for (r = 0; r < numrooms; r++) {
			const s32 v = (s32)(rooms[r].data.n / 16 + 1);

			cbU16(&s3, v < 0xffff ? v : 0xffff);
		}

		cbZeros(&s3, numrooms);
		s3z = rzip(s3.v, s3.n);
		cbU16(&out, 0x8000 | (u32)s3.n);
		cbU16(&out, (u32)s3z.n);
		cbPut(&out, s3z.v, s3z.n);
		free(s3.v);
		free(s3z.v);
	}

	cbPad(&out, 16);
	free(portalrecs.v);
	free(groups.v);
	free(primary.v);
	free(primz.v);
	free(roomsblob.v);

	return out;
}

/**
 * The collision tiles, a room's list after another's (geconvert.c's
 * writeTiles()): each triangle in every room it reaches into, its vertices
 * in the world's units
 */
/**
 * A collision tile's vertices are 16-bit whole numbers, and a floor's height
 * under a point is worked out from them in whole numbers too: Tranzit's ground
 * has triangles thousands of units across, and from one of those the height
 * came out as -4294967296 (F3 2026-10-28, Town at -165 -755: the player fell,
 * and fell again where they were put back). Perfect Dark's own levels have no
 * tile near that size. A floor is written as GEOTYPE_TILE_F, its vertices
 * floats (geconvert.c writes them for GoldenEye levels too wide for 16 bits),
 * whose height is worked out in floats. Cutting the big triangles into small
 * ones instead took Town's tiles file past the 16 MB a compressed file's
 * 24-bit length can say, and the game crashed loading it (convertTiles()).
 * Walls stay 16-bit: floats are 52 bytes a tile to their 32.
 */
static void tileEmit(cbuf *bodies, const f32 tp[3][3], u32 flags, s32 *floors, s32 *walls)
{
	// (and with the game's own collision read, its walls too: merged into runs
	// hundreds of inches long, as 16-bit tiles they stopped a walk only some of
	// the time - out of the bar's window at a walk, held at a run)
	const s32 wide = (flags & 0x0001) != 0 || g_ScanCollision;
	f32 lo[3], hi[3];
	s32 ix0, ix1, iz0, iz1, ix, iz, c, k;
	s16 p[3][3];

	for (c = 0; c < 3; c++) {
		lo[c] = hi[c] = tp[0][c];

		for (k = 1; k < 3; k++) {
			lo[c] = tp[k][c] < lo[c] ? tp[k][c] : lo[c];
			hi[c] = tp[k][c] > hi[c] ? tp[k][c] : hi[c];
		}

		for (k = 0; k < 3; k++) {
			p[k][c] = s16of(tp[k][c]);
		}
	}

	ix0 = (s32)floorf((lo[0] - CODMAP_REACH - g_GridX0) / CODMAP_CELL);
	ix1 = (s32)floorf((hi[0] + CODMAP_REACH - g_GridX0) / CODMAP_CELL);
	iz0 = (s32)floorf((lo[2] - CODMAP_REACH - g_GridZ0) / CODMAP_CELL);
	iz1 = (s32)floorf((hi[2] + CODMAP_REACH - g_GridZ0) / CODMAP_CELL);

	for (ix = ix0 < 0 ? 0 : ix0; ix <= ix1 && ix < g_GridX; ix++) {
		for (iz = iz0 < 0 ? 0 : iz0; iz <= iz1 && iz < g_GridZ; iz++) {
			cbuf *b = &bodies[1 + ix * g_GridZ + iz];
			s32 mn[3], mx[3];

			for (c = 0; c < 3; c++) {
				mn[c] = mx[c] = 0;

				for (k = 1; k < 3; k++) {
					if (p[k][c] < p[mn[c]][c]) mn[c] = k;
					if (p[k][c] > p[mx[c]][c]) mx[c] = k;
				}
			}

			cbU8(b, wide ? 1 : 0);
			cbU8(b, 3);
			cbU16(b, flags);
			cbU16(b, 0);

			for (c = 0; c < 3; c++) {
				cbU8(b, mn[c]);
			}

			for (c = 0; c < 3; c++) {
				cbU8(b, mx[c]);
			}

			cbU16(b, 0x0fff);

			if (wide) {
				// GEOTYPE_TILE_F: two bytes to the floats' alignment, then a
				// coord a vertex
				cbU16(b, 0);

				for (k = 0; k < 3; k++) {
					for (c = 0; c < 3; c++) {
						cbF32(b, tp[k][c]);
					}
				}
			} else {
				for (k = 0; k < 3; k++) {
					for (c = 0; c < 3; c++) {
						cbU16(b, (u16)p[k][c]);
					}
				}
			}

			if (flags & 0x0001) {
				(*floors)++;
			} else {
				(*walls)++;
			}
		}
	}
}

static cbuf writeTiles(struct croom *rooms, s32 numrooms, s32 *outfloors, s32 *outwalls)
{
	cbuf *bodies = calloc(numrooms + 1, sizeof(cbuf));
	cbuf out = {0}, z;
	u32 pos;
	s32 floors = 0, walls = 0;
	s32 t, r;

	for (t = 0; t < g_NumTris; t++) {
		const struct ctri *tri = &g_Tris[t];
		u32 flags;

		// with the game's own collision read (readScan()), it and Survival's wrecks'
		// own (spawned by its script, they are in no scan) alone are solid
		if (g_ScanCollision && !tri->scan) {
			continue;
		}

		// A decal lying flat is ground too: in places it is the only floor
		// the export has (the bank's papers), and walked on as nothing it was
		// a hole to fall through. One on a wall stays nothing
		if (!tri->solid && !(tri->n[1] >= CODMAP_FLOOR_NY && !tri->hide && g_Mats[tri->mat].blend == 2)) {
			continue;
		}

		flags = tri->n[1] >= CODMAP_FLOOR_NY ? FLOOR_FLAGS : WALL_FLAGS;

		// lava burns whoever stands on it (zombies.inc's zombiesTickLava())
		if (flags == FLOOR_FLAGS && strstr(g_Mats[tri->mat].name, "lava_molten")) {
			flags |= GEOFLAG_LAVA;
		}

		tileEmit(bodies, tri->p, flags, &floors, &walls);
	}

	cbU32(&out, (u32)(numrooms + 1));
	pos = 4 + 4 * (numrooms + 1) + 4;

	for (r = 0; r <= numrooms; r++) {
		cbU32(&out, pos);
		pos += (u32)bodies[r].n;
	}

	cbU32(&out, pos);

	for (r = 0; r <= numrooms; r++) {
		cbPut(&out, bodies[r].v, bodies[r].n);
		free(bodies[r].v);
	}

	cbPad(&out, 16);
	free(bodies);
	*outfloors = floors;
	*outwalls = walls;
	z = rzip(out.v, out.n);
	free(out.v);

	return z;
}

// The highest floor under a point, or the point's own height for none
static f32 floorUnder(const f32 pos[3])
{
	f32 best = -1e30f;
	s32 t;

	for (t = 0; t < g_NumTris; t++) {
		const struct ctri *tri = &g_Tris[t];
		const f32 *a = tri->p[0], *b = tri->p[1], *c = tri->p[2];
		f32 det, w0, w1, w2, y;

		if (!tri->solid || tri->n[1] < CODMAP_FLOOR_NY) {
			continue;
		}

		det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);

		if (fabsf(det) < 1e-6f) {
			continue;
		}

		w0 = ((b[2] - c[2]) * (pos[0] - c[0]) + (c[0] - b[0]) * (pos[2] - c[2])) / det;
		w1 = ((c[2] - a[2]) * (pos[0] - c[0]) + (a[0] - c[0]) * (pos[2] - c[2])) / det;
		w2 = 1 - w0 - w1;

		if (w0 < 0 || w1 < 0 || w2 < 0) {
			continue;
		}

		y = w0 * a[1] + w1 * b[1] + w2 * c[1];

		if (y <= pos[1] + 40 && y > best) {
			best = y;
		}
	}

	return best > -1e29f ? best : pos[1];
}

/**
 * The waypoints: Black Ops II's own path nodes (the entities' node_pathnode)
 * in the row's box. A map's file has its nodes and not their links, which its
 * compiler works out, so two nodes are linked here when they are within
 * CODMAP_LINK_REACH of each other across, no more than CODMAP_LINK_RISE apart
 * in height, nothing solid crosses the line between them at a knee's height
 * or a head's, and there is floor under its middle near their height. The
 * groups Perfect Dark routes through first are each room's nodes that link
 * to each other (navFindRoute() walks a group's own links, so a group must be
 * of a piece).
 */
#define CODMAP_LINK_REACH 380.0f // inches
#define CODMAP_LINK_RISE  40.0f  // inches
#define CODMAP_BLOCK_CELL 400.0f // our units: the grid the solid triangles are looked for in

struct cnode {
	f32 pos[3];
	s32 *nb;
	s32 numnb, maxnb;
	s32 group;
};

static struct cnode *g_Nodes;
static s32 g_NumNodes;
static s32 g_NumGroups;

static s32 **g_BlockCells;
static s32 *g_BlockCounts;
static s32 g_BlockX, g_BlockZ;

static void blockGridBuild(void)
{
	s32 *caps;
	s32 t, ix, iz;

	g_BlockX = (s32)ceilf(g_GridX * CODMAP_CELL / CODMAP_BLOCK_CELL);
	g_BlockZ = (s32)ceilf(g_GridZ * CODMAP_CELL / CODMAP_BLOCK_CELL);
	g_BlockCells = calloc(g_BlockX * g_BlockZ, sizeof(*g_BlockCells));
	g_BlockCounts = calloc(g_BlockX * g_BlockZ, sizeof(*g_BlockCounts));
	caps = calloc(g_BlockX * g_BlockZ, sizeof(*caps));

	for (t = 0; t < g_NumTris; t++) {
		const struct ctri *tri = &g_Tris[t];
		f32 lo[2], hi[2];
		s32 x0, x1, z0, z1;

		if (!tri->solid) {
			continue;
		}

		lo[0] = MIN(tri->p[0][0], MIN(tri->p[1][0], tri->p[2][0]));
		hi[0] = MAX(tri->p[0][0], MAX(tri->p[1][0], tri->p[2][0]));
		lo[1] = MIN(tri->p[0][2], MIN(tri->p[1][2], tri->p[2][2]));
		hi[1] = MAX(tri->p[0][2], MAX(tri->p[1][2], tri->p[2][2]));
		x0 = MAX(0, (s32)floorf((lo[0] - g_GridX0) / CODMAP_BLOCK_CELL));
		x1 = MIN(g_BlockX - 1, (s32)floorf((hi[0] - g_GridX0) / CODMAP_BLOCK_CELL));
		z0 = MAX(0, (s32)floorf((lo[1] - g_GridZ0) / CODMAP_BLOCK_CELL));
		z1 = MIN(g_BlockZ - 1, (s32)floorf((hi[1] - g_GridZ0) / CODMAP_BLOCK_CELL));

		for (ix = x0; ix <= x1; ix++) {
			for (iz = z0; iz <= z1; iz++) {
				const s32 c = ix * g_BlockZ + iz;

				if (g_BlockCounts[c] == caps[c]) {
					caps[c] = caps[c] ? caps[c] * 2 : 64;
					g_BlockCells[c] = realloc(g_BlockCells[c], sizeof(s32) * caps[c]);
				}

				g_BlockCells[c][g_BlockCounts[c]++] = t;
			}
		}
	}

	free(caps);
}

static void blockGridFree(void)
{
	s32 c;

	for (c = 0; c < g_BlockX * g_BlockZ; c++) {
		free(g_BlockCells[c]);
	}

	free(g_BlockCells);
	free(g_BlockCounts);
	g_BlockCells = NULL;
	g_BlockCounts = NULL;
}

// Whether a solid triangle crosses the segment a to b (Moller-Trumbore)
static s32 segmentBlocked(const f32 a[3], const f32 b[3])
{
	const f32 d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	s32 x0 = MAX(0, (s32)floorf((MIN(a[0], b[0]) - g_GridX0) / CODMAP_BLOCK_CELL));
	s32 x1 = MIN(g_BlockX - 1, (s32)floorf((MAX(a[0], b[0]) - g_GridX0) / CODMAP_BLOCK_CELL));
	s32 z0 = MAX(0, (s32)floorf((MIN(a[2], b[2]) - g_GridZ0) / CODMAP_BLOCK_CELL));
	s32 z1 = MIN(g_BlockZ - 1, (s32)floorf((MAX(a[2], b[2]) - g_GridZ0) / CODMAP_BLOCK_CELL));
	s32 ix, iz, k;

	for (ix = x0; ix <= x1; ix++) {
		for (iz = z0; iz <= z1; iz++) {
			const s32 c = ix * g_BlockZ + iz;

			for (k = 0; k < g_BlockCounts[c]; k++) {
				const struct ctri *tri = &g_Tris[g_BlockCells[c][k]];
				const f32 *p0 = tri->p[0], *p1 = tri->p[1], *p2 = tri->p[2];
				const f32 e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
				const f32 e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
				const f32 h[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
				const f32 det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
				f32 inv, s[3], u, q[3], v, tt;

				if (fabsf(det) < 1e-6f) {
					continue;
				}

				inv = 1.0f / det;
				s[0] = a[0] - p0[0];
				s[1] = a[1] - p0[1];
				s[2] = a[2] - p0[2];
				u = inv * (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]);

				if (u < 0 || u > 1) {
					continue;
				}

				q[0] = s[1] * e1[2] - s[2] * e1[1];
				q[1] = s[2] * e1[0] - s[0] * e1[2];
				q[2] = s[0] * e1[1] - s[1] * e1[0];
				v = inv * (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]);

				if (v < 0 || u + v > 1) {
					continue;
				}

				tt = inv * (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]);

				if (tt > 0.001f && tt < 0.999f) {
					return 1;
				}
			}
		}
	}

	return 0;
}

/**
 * The light under a roof: a face with anything solid over it (a ceiling, a
 * floor above, an awning, a tree) is drawn at CODMAP_SHADE of its light, as
 * Black Ops II's baked lighting leaves its interiors dim against the street.
 * Each triangle asks once, a ray straight up from just over its middle.
 */
#define CODMAP_SHADE 0.5f

static void shadePass(void)
{
	s32 t, shaded = 0;

	if (!g_BlockCells) {
		blockGridBuild();
	}

	for (t = 0; t < g_NumTris; t++) {
		struct ctri *tri = &g_Tris[t];
		f32 from[3], to[3];
		u32 c = tri->colour;
		s32 k;

		if (g_Mats[tri->mat].glow) {
			continue;
		}

		for (k = 0; k < 3; k++) {
			from[k] = (tri->p[0][k] + tri->p[1][k] + tri->p[2][k]) / 3;
		}

		from[1] += 15;
		to[0] = from[0];
		to[1] = from[1] + 2000;
		to[2] = from[2];

		if (segmentBlocked(from, to)) {
			tri->colour = (u32)(((c >> 24) & 0xff) * CODMAP_SHADE) << 24
				| (u32)(((c >> 16) & 0xff) * CODMAP_SHADE) << 16
				| (u32)(((c >> 8) & 0xff) * CODMAP_SHADE) << 8 | (c & 0xff);
			shaded++;
		}
	}

	sysLogPrintf(LOG_NOTE, "codmap: %d of %d faces under a roof", shaded, g_NumTris);
}

static void nodeLink(s32 a, s32 b)
{
	struct cnode *n = &g_Nodes[a];

	if (n->numnb == n->maxnb) {
		n->maxnb = n->maxnb ? n->maxnb * 2 : 8;
		n->nb = realloc(n->nb, sizeof(s32) * n->maxnb);
	}

	n->nb[n->numnb++] = b;
}

static void readNodes(const char *path, const struct codmaprow *row, const f32 *centre)
{
	FILE *f = fopen(path, "rb");
	char line[512];
	char origin[128] = "", classname[64] = "";
	s32 max = 0, i, j, links = 0;
	const f32 reach = CODMAP_LINK_REACH * CODMAP_SCALE, rise = CODMAP_LINK_RISE * CODMAP_SCALE;

	g_NumNodes = 0;

	if (!f) {
		return;
	}

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '{') {
			origin[0] = classname[0] = 0;
		} else if (line[0] == '}') {
			f32 cod[3] = { 0, 0, 0 };

			if (strcmp(classname, "node_pathnode") != 0 || !origin[0]) {
				continue;
			}

			sscanf(origin, "%f %f %f", &cod[0], &cod[1], &cod[2]);

			if (cod[0] < row->box[0] || cod[0] > row->box[1] || cod[1] < row->box[2] || cod[1] > row->box[3]
					|| cod[2] < row->zmin) {
				continue;
			}

			if (g_NumNodes == max) {
				max = max ? max * 2 : 1024;
				g_Nodes = realloc(g_Nodes, sizeof(*g_Nodes) * max);
			}

			memset(&g_Nodes[g_NumNodes], 0, sizeof(g_Nodes[0]));
			codToOurs(cod, centre, g_Nodes[g_NumNodes].pos);
			g_Nodes[g_NumNodes].pos[1] += 20;
			g_Nodes[g_NumNodes].pos[1] = floorUnder(g_Nodes[g_NumNodes].pos) + 1;
			g_NumNodes++;
		} else {
			char key[64], value[256];

			if (sscanf(line, " \"%63[^\"]\" \"%255[^\"]\"", key, value) == 2) {
				if (strcmp(key, "origin") == 0) {
					snprintf(origin, sizeof(origin), "%s", value);
				} else if (strcmp(key, "classname") == 0) {
					snprintf(classname, sizeof(classname), "%s", value);
				}
			}
		}
	}

	fclose(f);

	if (!g_BlockCells) {
		blockGridBuild();
	}

	for (i = 0; i < g_NumNodes; i++) {
		for (j = i + 1; j < g_NumNodes; j++) {
			const f32 *a = g_Nodes[i].pos, *b = g_Nodes[j].pos;
			const f32 dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
			f32 knee[2][3], head[2][3], mid[3], fl;
			s32 k;

			if (dx * dx + dz * dz > reach * reach || dy > rise || dy < -rise) {
				continue;
			}

			for (k = 0; k < 3; k++) {
				knee[0][k] = a[k];
				knee[1][k] = b[k];
				head[0][k] = a[k];
				head[1][k] = b[k];
				mid[k] = (a[k] + b[k]) / 2;
			}

			knee[0][1] += 50;
			knee[1][1] += 50;
			head[0][1] += 150;
			head[1][1] += 150;

			if (segmentBlocked(knee[0], knee[1]) || segmentBlocked(head[0], head[1])) {
				continue;
			}

			mid[1] += 40;
			fl = floorUnder(mid);

			if (fl < mid[1] - 40 - 80) {
				continue;
			}

			nodeLink(i, j);
			nodeLink(j, i);
			links++;
		}
	}

	// the groups: each room's nodes that link to each other
	g_NumGroups = 0;

	for (i = 0; i < g_NumNodes; i++) {
		g_Nodes[i].group = -1;
	}

	for (i = 0; i < g_NumNodes; i++) {
		s32 *stack;
		s32 top = 0;
		const s32 cell = cellOf(g_Nodes[i].pos[0], g_Nodes[i].pos[2]);

		if (g_Nodes[i].group >= 0) {
			continue;
		}

		stack = malloc(sizeof(s32) * g_NumNodes);
		stack[top++] = i;
		g_Nodes[i].group = g_NumGroups;

		while (top > 0) {
			const s32 n = stack[--top];

			for (j = 0; j < g_Nodes[n].numnb; j++) {
				const s32 m = g_Nodes[n].nb[j];

				if (g_Nodes[m].group < 0 && cellOf(g_Nodes[m].pos[0], g_Nodes[m].pos[2]) == cell) {
					g_Nodes[m].group = g_NumGroups;
					stack[top++] = m;
				}
			}
		}

		free(stack);
		g_NumGroups++;
	}

	sysLogPrintf(LOG_NOTE, "codmap: %d path nodes, %d links, %d groups", g_NumNodes, links, g_NumGroups);
}

/**
 * Town's own Zombies layout, out of the entities, for zombies.inc to place
 * as Black Ops II does in place of choosing spots itself: the Survival set's
 * perk machines (script_string zstandard_*) and Pack-a-Punch, the Mystery
 * Box, and the boarded windows. Each spot is a pad of its own after the path
 * nodes', and zombies/<arena>.txt in the mod names them:
 *
 *   perk <id> <pad> <x> <z>   a machine at the pad, its back that way (zombies.inc's ZOMBIEPERK_*)
 *   pap <pad> <x> <z>         Pack-a-Punch, the same
 *   box <pad> <x> <z>         the Mystery Box
 *   barrier <pad> <x> <z> <d> a window d from the pad, that way (out of the building)
 *
 * A window's inside is the side with a roof over it.
 */
#define CODMAP_MAX_EXTRA 64
#define CODMAP_WINDOW_IN 100.0f // a window's pad, this far inside it

struct cextra {
	f32 pos[3];
	f32 yaw;
};

static struct cextra g_ExtraPads[CODMAP_MAX_EXTRA];
static s32 g_NumExtraPads;
static cbuf g_Layout;

struct cent {
	char classname[64], origin[128], angles[128], targetname[96], noteworthy[96], string[160], model[96];
};

static s32 extraPad(const f32 pos[3], f32 yaw)
{
	if (g_NumExtraPads >= CODMAP_MAX_EXTRA) {
		return -1;
	}

	memcpy(g_ExtraPads[g_NumExtraPads].pos, pos, sizeof(f32) * 3);
	g_ExtraPads[g_NumExtraPads].pos[1] = floorUnder(g_ExtraPads[g_NumExtraPads].pos) + 1;
	g_ExtraPads[g_NumExtraPads].yaw = yaw;

	return g_NumExtraPads++;
}

static void layoutLine(const char *fmt, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	cbPut(&g_Layout, line, strlen(line));
}

// Whether something solid stands over a point (a roof)
static s32 roofed(const f32 p[3])
{
	const f32 up[3] = { p[0], p[1] + 1200, p[2] };
	const f32 from[3] = { p[0], p[1] + 120, p[2] };

	return segmentBlocked(from, up);
}

static void layoutEnt(const struct cent *e, const struct codmaprow *row, const f32 *centre, s32 padbase)
{
	static const struct { const char *specialty; s32 id; } perks[] = {
		{ "specialty_armorvest", 0 }, { "specialty_quickrevive", 1 }, { "specialty_fastreload", 2 },
		{ "specialty_rof", 3 }, { "specialty_longersprint", 4 },
	};
	f32 cod[3] = { 0, 0, 0 }, ang[3] = { 0, 0, 0 }, pos[3], fwd[2];
	s32 i, pad;

	sscanf(e->origin, "%f %f %f", &cod[0], &cod[1], &cod[2]);
	sscanf(e->angles, "%f %f %f", &ang[0], &ang[1], &ang[2]);

	if (cod[0] < row->box[0] || cod[0] > row->box[1] || cod[1] < row->box[2] || cod[1] > row->box[3]) {
		return;
	}

	codToOurs(cod, centre, pos);
	// the way it faces, ours: Call of Duty's yaw from +x to +y, our z its -y
	fwd[0] = cosf(ang[1] * (f32)M_PI / 180.0f);
	fwd[1] = -sinf(ang[1] * (f32)M_PI / 180.0f);

	if (strcmp(e->targetname, "zm_perk_machine") == 0 && strstr(e->string, "zstandard")) {
		if (strcmp(e->noteworthy, "specialty_weapupgrade") == 0) {
			if ((pad = extraPad(pos, 0)) >= 0) {
				layoutLine("pap %d %.4f %.4f\n", padbase + pad, -fwd[0], -fwd[1]);
			}

			return;
		}

		for (i = 0; i < (s32)ARRAYCOUNT(perks); i++) {
			if (strcmp(e->noteworthy, perks[i].specialty) == 0 && (pad = extraPad(pos, 0)) >= 0) {
				layoutLine("perk %d %d %.4f %.4f\n", perks[i].id, padbase + pad, -fwd[0], -fwd[1]);
			}
		}
	} else if (strcmp(e->classname, "zbarrier_zmcore_MagicBox") == 0 && strcmp(e->noteworthy, "town_chest_zbarrier") == 0) {
		if ((pad = extraPad(pos, 0)) >= 0) {
			layoutLine("box %d %.4f %.4f\n", padbase + pad, -fwd[0], -fwd[1]);
		}
	} else if (strncmp(e->classname, "zbarrier_zmcore_BasicWoodBarrier", 32) == 0 && strncmp(e->string, "labs_", 5) != 0) {
		// in from the window: the side under a roof
		f32 in[3], out[3];
		f32 dir[2];

		for (i = 0; i < 3; i++) {
			in[i] = out[i] = pos[i];
		}

		in[0] += fwd[0] * CODMAP_WINDOW_IN;
		in[2] += fwd[1] * CODMAP_WINDOW_IN;
		out[0] -= fwd[0] * CODMAP_WINDOW_IN;
		out[2] -= fwd[1] * CODMAP_WINDOW_IN;
		in[1] = floorUnder(in);
		out[1] = floorUnder(out);

		if (!roofed(in) && roofed(out)) {
			f32 t[3];

			memcpy(t, in, sizeof(t));
			memcpy(in, out, sizeof(t));
			memcpy(out, t, sizeof(t));
			dir[0] = fwd[0];
			dir[1] = fwd[1];
		} else {
			dir[0] = -fwd[0];
			dir[1] = -fwd[1];
		}

		if ((pad = extraPad(in, 0)) >= 0) {
			layoutLine("barrier %d %.4f %.4f %.1f\n", padbase + pad, dir[0], dir[1], CODMAP_WINDOW_IN);
		}
	}
}

static void readLayout(const char *path, const struct codmaprow *row, const f32 *centre, s32 padbase)
{
	FILE *f = fopen(path, "rb");
	char line[512];
	struct cent e;

	g_NumExtraPads = 0;
	g_Layout.n = 0;

	if (!f) {
		return;
	}

	memset(&e, 0, sizeof(e));

	while (fgets(line, sizeof(line), f)) {
		char key[64], value[256];

		if (line[0] == '{') {
			memset(&e, 0, sizeof(e));
		} else if (line[0] == '}') {
			layoutEnt(&e, row, centre, padbase);
		} else if (sscanf(line, " \"%63[^\"]\" \"%255[^\"]\"", key, value) == 2) {
			if (strcmp(key, "classname") == 0) snprintf(e.classname, sizeof(e.classname), "%s", value);
			else if (strcmp(key, "origin") == 0) snprintf(e.origin, sizeof(e.origin), "%s", value);
			else if (strcmp(key, "angles") == 0) snprintf(e.angles, sizeof(e.angles), "%s", value);
			else if (strcmp(key, "targetname") == 0) snprintf(e.targetname, sizeof(e.targetname), "%s", value);
			else if (strcmp(key, "script_noteworthy") == 0) snprintf(e.noteworthy, sizeof(e.noteworthy), "%s", value);
			else if (strcmp(key, "script_string") == 0) snprintf(e.string, sizeof(e.string), "%s", value);
			else if (strcmp(key, "model") == 0) snprintf(e.model, sizeof(e.model), "%s", value);
		}
	}

	fclose(f);
	sysLogPrintf(LOG_NOTE, "codmap: Town's layout: %d spots", g_NumExtraPads);
}

/**
 * What a Survival map's script stands in its roads: each `script_struct`
 * `game_mode_object` whose script_string names zstandard, a model at an origin
 * and angles (pitch, yaw, roll). Survival Town blocks its streets with wrecks
 * so (two wagons, two buses, a truck's cab), and with none the roads ran on out
 * of Town into the rest of Tranzit (F3 2026-10-28, -2476 -1545). Placed as the
 * map's props are, solid
 */
static void readModeObjects(const char *path, const char *modeldir, const struct codmaprow *row, const f32 *centre)
{
	FILE *f = fopen(path, "rb");
	char line[512];
	struct cent e;
	s32 placed = 0, missing = 0, tris = 0;

	if (!f) {
		return;
	}

	memset(&e, 0, sizeof(e));

	while (fgets(line, sizeof(line), f)) {
		char key[64], value[256];

		if (line[0] == '{') {
			memset(&e, 0, sizeof(e));
		} else if (line[0] == '}') {
			f32 pos[3] = { 0, 0, 0 }, ang[3] = { 0, 0, 0 };

			if (strcmp(e.targetname, "game_mode_object") == 0 && strstr(e.string, "zstandard") && e.model[0]
					&& sscanf(e.origin, "%f %f %f", &pos[0], &pos[1], &pos[2]) == 3) {
				f32 rot[3];

				sscanf(e.angles, "%f %f %f", &ang[0], &ang[1], &ang[2]);

				// Call of Duty's angles are pitch, yaw and roll; the
				// placements' rotation is about x (roll), y (pitch), z (yaw)
				rot[0] = ang[2];
				rot[1] = ang[0];
				rot[2] = ang[1];
				g_PropKeepCollision = 1;
				placeOne(modeldir, e.model, pos, rot, 1, row, centre, &placed, &missing, &tris);
				g_PropKeepCollision = 0;
			}
		} else if (sscanf(line, " \"%63[^\"]\" \"%255[^\"]\"", key, value) == 2) {
			if (strcmp(key, "origin") == 0) snprintf(e.origin, sizeof(e.origin), "%s", value);
			else if (strcmp(key, "angles") == 0) snprintf(e.angles, sizeof(e.angles), "%s", value);
			else if (strcmp(key, "targetname") == 0) snprintf(e.targetname, sizeof(e.targetname), "%s", value);
			else if (strcmp(key, "script_string") == 0) snprintf(e.string, sizeof(e.string), "%s", value);
			else if (strcmp(key, "model") == 0) snprintf(e.model, sizeof(e.model), "%s", value);
		}
	}

	fclose(f);
	sysLogPrintf(LOG_NOTE, "codmap: %d of Survival's own objects placed (%d triangles), %d with no model", placed, tris, missing);
}

/**
 * Slopes over stairs. Black Ops II walks its stairs on clip ramps C2M's
 * export has not; Perfect Dark's own levels have slopes where stairs are
 * drawn. Each ramp is a box over a flight (our units) climbing along z from
 * h0 at z0 to h1 at z1, laid in as an invisible, solid floor (held between
 * the two). The line is the steps' front edges, measured with
 * `--zombies-test-probe 2`: every step is under it, so up the flight the
 * ground is the slope's - no step to stop at, and no gap between open treads
 * to fall through (Town's Juggernog stairs, F3 2026-10-28). A plane fitted to
 * the steps in a box was tried first, and the furniture and landings in the
 * box set it far off.
 */
struct codramp {
	const char *key;  // the row's
	f32 x0, x1;       // the flight's sides
	f32 z0, h0;       // its foot: where the slope meets the floor under it
	f32 z1, h1;       // and its head
};

#define CODMAP_RAMP_RAIL 60.0f // its side rails over it: over the 46 a player steps

static const struct codramp g_CodMapRamps[] = {
	{ "bo2town", 2695, 2990, -3598, -142, -2988, 224 }, // the bar's, to its upper floor
	{ "bo2town", -2090, -1825, -512, -142, 172, 305 },  // up to Juggernog, open treads
};

/**
 * Invisible walls where Black Ops II has one the export lacks: an upright
 * quad from (x0, z0) to (x1, z1), y0 to y1. The landing at the head of Town's
 * Juggernog flight ends at z 725 over nothing at all, and walked along it the
 * player fell off its end (F3 video 2026-10-28)
 */
struct codwall {
	const char *key;
	f32 x0, z0, x1, z1;
	f32 y0, y1;
};

static const struct codwall g_CodMapWalls[] = {
	{ "bo2town", -2140, 725, -1740, 725, 280, 480 }, // the end of the Juggernog flight's landing
};

static void addWalls(const struct codmaprow *row)
{
	s32 i, k;

	for (i = 0; i < (s32)ARRAYCOUNT(g_CodMapWalls); i++) {
		const struct codwall *wl = &g_CodMapWalls[i];
		const f32 w[4][3] = {
			{ wl->x0, wl->y0, wl->z0 }, { wl->x1, wl->y0, wl->z1 },
			{ wl->x1, wl->y1, wl->z1 }, { wl->x0, wl->y1, wl->z0 },
		};
		const f32 *uv[3] = { NULL, NULL, NULL };
		const s32 mat = matFind("codmap_ramp");

		if (strcmp(wl->key, row->key) != 0) {
			continue;
		}

		for (k = 0; k < 2; k++) {
			triAdd(w[0], w[1 + k], w[2 + k], uv, mat);
			g_Tris[g_NumTris - 1].hide = 1;
			g_Tris[g_NumTris - 1].solid = 1;
		}
	}
}

static void addRamps(const struct codmaprow *row)
{
	s32 r;

	addWalls(row);

	for (r = 0; r < (s32)ARRAYCOUNT(g_CodMapRamps); r++) {
		const struct codramp *rp = &g_CodMapRamps[r];
		const f32 q[4][3] = {
			{ rp->x0, rp->h0, rp->z0 }, { rp->x1, rp->h0, rp->z0 },
			{ rp->x1, rp->h1, rp->z1 }, { rp->x0, rp->h1, rp->z1 },
		};
		const s32 quad[2][3] = { { 0, 2, 1 }, { 0, 3, 2 } };
		const s32 mat = matFind("codmap_ramp");
		s32 i;

		if (strcmp(rp->key, row->key) != 0) {
			continue;
		}

		for (i = 0; i < 2; i++) {
			const f32 *uv[3] = { NULL, NULL, NULL };

			triAdd(q[quad[i][0]], q[quad[i][1]], q[quad[i][2]], uv, mat);

			// up, whichever way it was wound
			if (g_Tris[g_NumTris - 1].n[1] < 0) {
				g_NumTris--;
				triAdd(q[quad[i][0]], q[quad[i][2]], q[quad[i][1]], uv, mat);
			}

			g_Tris[g_NumTris - 1].hide = 1;
			g_Tris[g_NumTris - 1].solid = 1;
		}

		// and a wall along each side, from the floor at its foot to a rail
		// over a step's height above it, and one under its head: beside a
		// flight there was no floor, or only one far over it, and running up
		// the player slid off the edge through the "cracks"; and from the floor
		// beside it they walked in under it (F3 2026-10-28, -2011 124, eye 55
		// under the Juggernog flight). The head's wall stops under the floor
		// it meets, short of what a player there would bump
		{
			const f32 *uv[3] = { NULL, NULL, NULL };
			f32 w[3][4][3];
			s32 k;

			for (i = 0; i < 2; i++) {
				const f32 x = i ? rp->x1 : rp->x0;
				const f32 side[4][3] = {
					{ x, rp->h0 - 10, rp->z0 }, { x, rp->h0 - 10, rp->z1 },
					{ x, rp->h1 + CODMAP_RAMP_RAIL, rp->z1 }, { x, rp->h0 + CODMAP_RAMP_RAIL, rp->z0 },
				};

				memcpy(w[i], side, sizeof(side));
			}

			{
				const f32 back[4][3] = {
					{ rp->x0, rp->h0 - 10, rp->z1 }, { rp->x1, rp->h0 - 10, rp->z1 },
					{ rp->x1, rp->h1 - 10, rp->z1 }, { rp->x0, rp->h1 - 10, rp->z1 },
				};

				memcpy(w[2], back, sizeof(back));
			}

			for (i = 0; i < 3; i++) {
				for (k = 0; k < 2; k++) {
					triAdd(w[i][0], w[i][1 + k], w[i][2 + k], uv, mat);
					g_Tris[g_NumTris - 1].hide = 1;
					g_Tris[g_NumTris - 1].solid = 1;
				}
			}
		}

		sysLogPrintf(LOG_NOTE, "codmap: ramp %d, %.0f..%.0f across, %.0f at %.0f to %.0f at %.0f",
				r, rp->x0, rp->x1, rp->h0, rp->z0, rp->h1, rp->z1);
	}
}

/**
 * The game's own collision (tools/zombies/bo2scan): Black Ops II traced
 * straight down with the player's own collision every `step` inches over the
 * row's box, each column's surfaces from the top. That is every floor the
 * player stands on there - clip ramps over stairs, barriers' tops, wrecks -
 * which C2M's drawn world has not. With it, it is the map's whole collision:
 * - floors between each four neighbouring samples whose surfaces meet within
 *   CODSCAN_SLOPE (a ramp's next sample is 10 over), flat runs along y merged
 *   into strips, sloped cells two triangles each;
 * - walls on the line between two samples where one has a surface more than
 *   a step (CODSCAN_STEP) over anything the other has near it, from the
 *   other's highest surface under it up to it, runs along a line with the
 *   same foot and head merged (many small walls defeated the slide before).
 * Every drawn triangle stays drawn and is no longer solid.
 */
#define CODSCAN_LAYERS 8
#define CODSCAN_SLOPE  14 // inches between neighbouring samples on one floor
#define CODSCAN_STEP   18 // Black Ops II's step
#define CODSCAN_TALL   80   // over a floor by more than this, a surface is overhead
#define CODSCAN_GROUND -100 // inches: Town's lowest walkable ground is the street at -55
#define CODSCAN_INSET  0  // inches a floor square stops short of its halfway lines (2: the player could not move at all)

struct scanwall {
	s32 axis;   // 0: a line of x, 1: of y
	s32 line;   // its index
	s32 at;     // the index along it
	s32 lo, hi; // inches
};

static s16 *g_ScanH;
static u8 *g_ScanN;
static s32 g_ScanW, g_ScanD, g_ScanStep;
static f32 g_ScanX0, g_ScanY0;

// the layer of column (i, j) nearest h within tol, -1 for none
static s32 scanMatch(s32 i, s32 j, s32 h, s32 tol)
{
	const s32 c = i * g_ScanD + j;
	s32 best = -1, bestd = tol + 1, l;

	for (l = 0; l < g_ScanN[c]; l++) {
		const s32 d = abs(g_ScanH[c * CODSCAN_LAYERS + l] - h);

		if (d < bestd) {
			bestd = d;
			best = l;
		}
	}

	return best;
}

// column (i, j)'s highest surface under h, or none
static s32 scanUnder(s32 i, s32 j, s32 h, s32 *out)
{
	const s32 c = i * g_ScanD + j;
	s32 l;

	for (l = 0; l < g_ScanN[c]; l++) {
		if (g_ScanH[c * CODSCAN_LAYERS + l] < h) {
			*out = g_ScanH[c * CODSCAN_LAYERS + l];
			return 1;
		}
	}

	return 0;
}

static void scanTri(const f32 a[3], const f32 b[3], const f32 c[3], const f32 *centre, s32 mat, s32 floor)
{
	const f32 *uv[3] = { NULL, NULL, NULL };
	f32 pa[3], pb[3], pc[3];
	const s32 before = g_NumTris;

	codToOurs(a, centre, pa);
	codToOurs(b, centre, pb);
	codToOurs(c, centre, pc);
	triAdd(pa, pb, pc, uv, mat);

	if (g_NumTris == before) {
		return;
	}

	// a floor faces up
	if (floor && g_Tris[g_NumTris - 1].n[1] < 0) {
		g_NumTris--;
		triAdd(pa, pc, pb, uv, mat);
	}

	g_Tris[g_NumTris - 1].hide = 1;
	g_Tris[g_NumTris - 1].solid = 1;
	g_Tris[g_NumTris - 1].scan = 1;
}

static int scanWallCompare(const void *a, const void *b)
{
	const struct scanwall *p = a, *q = b;

	if (p->axis != q->axis) return p->axis - q->axis;
	if (p->line != q->line) return p->line - q->line;
	if (p->lo / 4 != q->lo / 4) return p->lo / 4 - q->lo / 4;
	if (p->hi / 4 != q->hi / 4) return p->hi / 4 - q->hi / 4;
	return p->at - q->at;
}

static s32 readScan(const char *path, const struct codmaprow *row, const f32 *centre)
{
	FILE *f = fopen(path, "rb");
	char line[1024];
	char map[64];
	s32 ver, x0, y0, step, ztop, zbot;
	s32 i, j, l, n;
	s32 floors = 0, walls = 0;
	struct scanwall *ws = NULL;
	s32 numws = 0, maxws = 0;
	const s32 mat = matFind("codmap_scan");

	if (!f) {
		return 0;
	}

	if (!fgets(line, sizeof(line), f)
			|| sscanf(line, "bo2scan %d %63s %d %d %d %d %d", &ver, map, &x0, &y0, &step, &ztop, &zbot) != 7
			|| strcmp(map, row->map) != 0 || step <= 0) {
		fclose(f);
		sysLogPrintf(LOG_WARNING, "codmap: %s is not a bo2scan of %s", path, row->map);
		return 0;
	}

	g_ScanStep = step;
	g_ScanX0 = x0;
	g_ScanY0 = y0;
	g_ScanW = (s32)((row->box[1] - x0) / step) + 1;
	g_ScanD = (s32)((row->box[3] - y0) / step) + 1;
	g_ScanH = calloc((size_t)g_ScanW * g_ScanD * CODSCAN_LAYERS, sizeof(s16));
	g_ScanN = calloc((size_t)g_ScanW * g_ScanD, 1);

	while (fgets(line, sizeof(line), f)) {
		char *s = line;
		s32 x, y, h, used;

		if (sscanf(s, "%d %d%n", &x, &y, &used) != 2) {
			continue;
		}

		i = (x - x0) / step;
		j = (y - y0) / step;
		s += used;

		if (i < 0 || j < 0 || i >= g_ScanW || j >= g_ScanD) {
			continue;
		}

		while (sscanf(s, "%d%n", &h, &used) == 1) {
			const s32 c = i * g_ScanD + j;

			s += used;

			// nothing under the row's floor (Town: Tranzit's lab under it)
			// nothing under the map's ground: the scan reaches Tranzit's lab under
			// Town, and through a gap in a street the player stood on its roof
			if (h >= CODSCAN_GROUND && g_ScanN[c] < CODSCAN_LAYERS) {
				g_ScanH[c * CODSCAN_LAYERS + g_ScanN[c]++] = (s16)h;
			}
		}
	}

	fclose(f);

	// Floors: each sample's own square, step across and centred on it, at its
	// height, the runs along y at one height merged into strips. A floor laid
	// between four samples left the last half step before every edge with no
	// floor at all, a crack along each wall and drop to slip into (the bar's
	// upper floor, the Juggernog landing); squares reach the halfway lines,
	// where the walls stand. A ramp is small steps of its own, under the step
	for (i = 0; i < g_ScanW; i++) {
		s32 runh[32], runj[32], numruns = 0, k;

		for (j = 0; j <= g_ScanD; j++) {
			s32 here[CODSCAN_LAYERS], numhere = 0;

			if (j < g_ScanD) {
				const s32 c = i * g_ScanD + j;

				for (l = 0; l < g_ScanN[c]; l++) {
					here[numhere++] = g_ScanH[c * CODSCAN_LAYERS + l];
				}
			}

			// the strips that do not go on through this sample end before it
			for (k = 0; k < numruns; k++) {
				s32 m, goes = 0;

				for (m = 0; m < numhere; m++) {
					if (here[m] == runh[k]) {
						goes = 1;
						here[m] = INT32_MIN;
					}
				}

				if (!goes) {
					// two inches short of the halfway lines: a square meeting a wall's
					// line exactly was touched by a player's circle against the wall,
					// and they were stepped up onto it (the bar's 38-inch sill)
					const f32 xa = x0 + (i - 0.5f) * step + CODSCAN_INSET, xb = x0 + (i + 0.5f) * step - CODSCAN_INSET;
					const f32 ya = y0 + (runj[k] - 0.5f) * step + CODSCAN_INSET, yb = y0 + (j - 0.5f) * step - CODSCAN_INSET;
					const f32 p00[3] = { xa, ya, runh[k] };
					const f32 p10[3] = { xb, ya, runh[k] };
					const f32 p01[3] = { xa, yb, runh[k] };
					const f32 p11[3] = { xb, yb, runh[k] };

					scanTri(p00, p10, p11, centre, mat, 1);
					scanTri(p00, p11, p01, centre, mat, 1);
					floors += 2;
					runh[k] = runh[--numruns];
					runj[k] = runj[numruns];
					k--;
				}
			}

			for (n = 0; n < numhere; n++) {
				if (here[n] != INT32_MIN && numruns < 32) {
					runh[numruns] = here[n];
					runj[numruns] = j;
					numruns++;
				}
			}
		}
	}

	// walls between neighbours a step apart, each line's runs merged
	for (i = 0; i < g_ScanW; i++) {
		for (j = 0; j < g_ScanD; j++) {
			s32 axis;

			for (axis = 0; axis < 2; axis++) {
				const s32 ni = i + (axis == 0), nj = j + (axis == 1);
				s32 side;

				if (ni >= g_ScanW || nj >= g_ScanD) {
					continue;
				}

				// each side's surfaces with nothing near them across the line
				for (side = 0; side < 2; side++) {
					const s32 ai = side ? ni : i, aj = side ? nj : j;
					const s32 bi = side ? i : ni, bj = side ? j : nj;
					const s32 c = ai * g_ScanD + aj;

					for (l = 0; l < g_ScanN[c]; l++) {
						const s32 h = g_ScanH[c * CODSCAN_LAYERS + l];
						s32 lo;

						// and only what stands in the way: a surface more than a
						// player's height over the floor beside it is overhead (a
						// roof's edge, an awning, a balcony), and walled to the
						// street each was an invisible wall under it. Something solid
						// from floor to ceiling has its own surface lower down
						// (bo2scan 2's note of it)
						if (scanMatch(bi, bj, h, CODSCAN_STEP) >= 0 || !scanUnder(bi, bj, h - CODSCAN_STEP, &lo)
								|| h - lo > CODSCAN_TALL) {
							continue;
						}

						if (numws == maxws) {
							maxws = maxws ? maxws * 2 : 65536;
							ws = realloc(ws, sizeof(*ws) * maxws);
						}

						ws[numws].axis = axis;
						// halfway between the two samples, where the squares of floor
						// either side end. Each side its own line
						ws[numws].line = (axis == 0 ? i : j) * 2 + side;
						ws[numws].at = axis == 0 ? j : i;
						ws[numws].lo = lo;
						ws[numws].hi = h;
						numws++;
					}
				}
			}
		}
	}

	qsort(ws, numws, sizeof(*ws), scanWallCompare);

	for (n = 0; n < numws; ) {
		s32 m = n + 1;
		f32 a[3], b[3], c[3], d[3];
		f32 along0, along1, across;

		while (m < numws && ws[m].axis == ws[n].axis && ws[m].line == ws[n].line
				&& ws[m].lo / 4 == ws[n].lo / 4 && ws[m].hi / 4 == ws[n].hi / 4 && ws[m].at == ws[m - 1].at + 1) {
			m++;
		}

		// halfway between the two samples, the run's cells' width
		across = (ws[n].axis == 0 ? x0 : y0) + (ws[n].line / 2 + 0.5f) * step;
		along0 = (ws[n].axis == 0 ? y0 : x0) + (ws[n].at - 0.5f) * step;
		along1 = (ws[n].axis == 0 ? y0 : x0) + (ws[m - 1].at + 0.5f) * step;

		if (ws[n].axis == 0) {
			a[0] = across; a[1] = along0; a[2] = ws[n].lo;
			b[0] = across; b[1] = along1; b[2] = ws[n].lo;
		} else {
			a[0] = along0; a[1] = across; a[2] = ws[n].lo;
			b[0] = along1; b[1] = across; b[2] = ws[n].lo;
		}

		memcpy(c, b, sizeof(c));
		memcpy(d, a, sizeof(d));
		c[2] = d[2] = ws[n].hi;
		scanTri(a, b, c, centre, mat, 0);
		scanTri(a, c, d, centre, mat, 0);
		walls++;
		n = m;
	}

	free(ws);
	free(g_ScanH);
	free(g_ScanN);
	g_ScanCollision = 1;
	sysLogPrintf(LOG_NOTE, "codmap: %s: the game's own collision, %d floor triangles, %d walls (of %d pieces)",
			path, floors, walls, numws);

	return 1;
}

/**
 * The map's own collision, exactly: Black Ops II's clipMap, exported by
 * OpenAssetTools with tools/zombies/oat-clipmap (ClipMapDumperT6, written for
 * this): <map>.d3dbsp.clip.txt holds
 *   m <index> <name> <surfaceFlags> <contentFlags>
 *   b <contents> <mins> <maxs> <numverts> <verts...> <numsides> <planes nx ny nz d...>
 *   t <material> <three corners>     (collision triangles: terrain, buildings)
 *   s <contents> <three corners>     (the static models' collision, in the world)
 * Everything that stops a player - solid (0x1), glass (0x10) and player clip
 * (0x10000, clip_player and clip) - inside the row's box and above its ground
 * becomes the map's collision, invisible; zombie-only clip, triggers and the
 * like are left out. A brush's faces are its axial box's six and its sides'
 * planes, each the brush's corners that lie on it, in order round its middle.
 * This is the collision the game itself walks the player on, stair ramps, the
 * barriers round Survival Town and all: the scan before it (readScan()) only
 * sampled it, and its walls and floors were guessed back from the samples
 */
#define CODCLIP_BLOCKS (0x1 | 0x10 | 0x10000)

static s32 clipKeep(const f32 *a, const f32 *b, const f32 *c, const struct codmaprow *row)
{
	const f32 cx = (a[0] + b[0] + c[0]) / 3, cy = (a[1] + b[1] + c[1]) / 3;
	const f32 top = MAX(MAX(a[2], b[2]), c[2]);

	return cx >= row->box[0] && cx <= row->box[1] && cy >= row->box[2] && cy <= row->box[3] && top >= CODSCAN_GROUND;
}

// One face of a brush: the corners on plane (n, d), in order round their middle,
// wound to face along n
static s32 clipFace(const f32 (*v)[3], s32 numv, const f32 n[3], f32 d, const f32 *centre, s32 mat, const struct codmaprow *row)
{
	f32 on[64][3], ang[64], mid[3] = { 0, 0, 0 }, u[3], w[3], len;
	s32 count = 0, i, j, tris = 0;

	for (i = 0; i < numv && count < 64; i++) {
		if (fabsf(v[i][0] * n[0] + v[i][1] * n[1] + v[i][2] * n[2] - d) < 0.5f) {
			memcpy(on[count++], v[i], sizeof(on[0]));
		}
	}

	if (count < 3) {
		return 0;
	}

	for (i = 0; i < count; i++) {
		for (j = 0; j < 3; j++) {
			mid[j] += on[i][j] / count;
		}
	}

	// a basis in the plane: u any line in it, w = n x u
	u[0] = on[0][0] - mid[0];
	u[1] = on[0][1] - mid[1];
	u[2] = on[0][2] - mid[2];
	len = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);

	if (len < 1e-4f) {
		return 0;
	}

	for (j = 0; j < 3; j++) {
		u[j] /= len;
	}

	w[0] = n[1] * u[2] - n[2] * u[1];
	w[1] = n[2] * u[0] - n[0] * u[2];
	w[2] = n[0] * u[1] - n[1] * u[0];

	for (i = 0; i < count; i++) {
		const f32 px = on[i][0] - mid[0], py = on[i][1] - mid[1], pz = on[i][2] - mid[2];

		ang[i] = atan2f(px * w[0] + py * w[1] + pz * w[2], px * u[0] + py * u[1] + pz * u[2]);
	}

	// round its middle, anticlockwise about n (insertion sort, few corners)
	for (i = 1; i < count; i++) {
		for (j = i; j > 0 && ang[j] < ang[j - 1]; j--) {
			f32 t = ang[j];
			f32 p[3];

			ang[j] = ang[j - 1];
			ang[j - 1] = t;
			memcpy(p, on[j], sizeof(p));
			memcpy(on[j], on[j - 1], sizeof(p));
			memcpy(on[j - 1], p, sizeof(p));
		}
	}

	for (i = 1; i + 1 < count; i++) {
		if (clipKeep(on[0], on[i], on[i + 1], row)) {
			scanTri(on[0], on[i], on[i + 1], centre, mat, 0);
			tris++;
		}
	}

	return tris;
}

/**
 * The brush models standing in Survival: a script_brushmodel's (entities'
 * "model" "*N") at its origin, unless a zombie_door trigger opens it (the bar's,
 * the bank's and the barber's buyable doors, the electric door: open, as
 * Perfect Dark's doors are not here) or it is the lab's hatch or the mature
 * content's blood. Its script_parameters name the modes it is gone from (Grief's
 * meat_remove and the like), none of them Survival. The bank's vault doors and
 * three blockers by the bar and the bank are what this places on Town
 */
#define CODCLIP_MODELS 1024

static s32 readPlacedModels(const char *path, f32 (*at)[3], u8 *placed)
{
	FILE *f = fopen(path, "rb");
	char line[512], classname[64] = "", model[32] = "", origin[128] = "", target[128] = "", targetname[128] = "";
	char opened[64][128];
	s32 numopened = 0, n = 0, i;
	struct { s32 model; f32 o[3]; char name[128]; } cand[256];
	s32 numcand = 0;

	if (!f) {
		return 0;
	}

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '{') {
			classname[0] = model[0] = origin[0] = target[0] = targetname[0] = 0;
		} else if (line[0] == '}') {
			if (strcmp(targetname, "zombie_door") == 0 && target[0] && numopened < 64) {
				snprintf(opened[numopened++], sizeof(opened[0]), "%s", target);
			}

			if (strcmp(classname, "script_brushmodel") == 0 && model[0] == '*' && numcand < 256
					&& strcmp(targetname, "lab_secret_hatch") != 0 && strcmp(targetname, "mature_blood") != 0) {
				cand[numcand].model = atoi(model + 1);
				cand[numcand].o[0] = cand[numcand].o[1] = cand[numcand].o[2] = 0;
				sscanf(origin, "%f %f %f", &cand[numcand].o[0], &cand[numcand].o[1], &cand[numcand].o[2]);
				snprintf(cand[numcand].name, sizeof(cand[0].name), "%s", targetname);
				numcand++;
			}
		} else {
			char key[64], value[256];

			if (sscanf(line, " \"%63[^\"]\" \"%255[^\"]\"", key, value) == 2) {
				if (strcmp(key, "classname") == 0) {
					snprintf(classname, sizeof(classname), "%s", value);
				} else if (strcmp(key, "model") == 0) {
					snprintf(model, sizeof(model), "%s", value);
				} else if (strcmp(key, "origin") == 0) {
					snprintf(origin, sizeof(origin), "%s", value);
				} else if (strcmp(key, "target") == 0) {
					snprintf(target, sizeof(target), "%s", value);
				} else if (strcmp(key, "targetname") == 0) {
					snprintf(targetname, sizeof(targetname), "%s", value);
				}
			}
		}
	}

	fclose(f);

	for (i = 0; i < numcand; i++) {
		s32 j, door = 0;

		for (j = 0; j < numopened && cand[i].name[0]; j++) {
			door |= strcmp(opened[j], cand[i].name) == 0;
		}

		if (!door && cand[i].model > 0 && cand[i].model < CODCLIP_MODELS) {
			memcpy(at[cand[i].model], cand[i].o, sizeof(at[0]));
			placed[cand[i].model] = 1;
			n++;
		}
	}

	return n;
}

static s32 readClipMap(const char *path, const char *entspath, const struct codmaprow *row, const f32 *centre)
{
	FILE *f = fopen(path, "rb");
	static f32 at[CODCLIP_MODELS][3];
	static u8 placed[CODCLIP_MODELS];
	s32 numplaced = 0, placedbrushes = 0;
	static char line[1 << 16];
	s32 *matcontents = NULL;
	s32 nummats = 0;
	s32 brushes = 0, brushtris = 0, worldtris = 0, modeltris = 0, localbrushes = 0;
	const s32 mat = matFind("codmap_clip");

	if (!f) {
		return 0;
	}

	memset(placed, 0, sizeof(placed));
	numplaced = readPlacedModels(entspath, at, placed);

	while (fgets(line, sizeof(line), f)) {
		char *s = line + 2;
		s32 used, model = 0;

		// a brush model's (B, T): in its own space, placed at its entity or left out
		if ((line[0] == 'B' || line[0] == 'T') && line[1] == ' ') {
			if (sscanf(s, "%d%n", &model, &used) != 1 || model <= 0 || model >= CODCLIP_MODELS || !placed[model]) {
				continue;
			}

			s += used;
			line[0] = line[0] == 'B' ? 'b' : 't';
		}

		if (line[0] == 'm' && line[1] == ' ') {
			s32 index, sflags, cflags;
			char name[128];

			if (sscanf(s, "%d %127s %d %d", &index, name, &sflags, &cflags) == 4 && index >= 0 && index < 65536) {
				if (index >= nummats) {
					matcontents = realloc(matcontents, sizeof(s32) * (index + 1));
					memset(matcontents + nummats, 0, sizeof(s32) * (index + 1 - nummats));
					nummats = index + 1;
				}

				matcontents[index] = cflags;
			}
		} else if (line[0] == 'b' && line[1] == ' ') {
			s32 contents, numv, numsides, i;
			f32 mins[3], maxs[3];
			f32 (*v)[3];

			if (sscanf(s, "%d %f %f %f %f %f %f %d%n", &contents, &mins[0], &mins[1], &mins[2],
						&maxs[0], &maxs[1], &maxs[2], &numv, &used) != 8 || !(contents & CODCLIP_BLOCKS)) {
				continue;
			}

			s += used;

			if (model) {
				for (i = 0; i < 3; i++) {
					mins[i] += at[model][i];
					maxs[i] += at[model][i];
				}
			}

			// a brush model's (a door's, the bus's) brushes are in its own space,
			// round its middle, and its entity places it: at the map's origin they
			// were walls and floors through the middle of Town. The exporter
			// marks those it can tell (B lines); the rest are told by their
			// shape, centred on an axis as nothing of Town's is: tall round
			// height 0 (Town's street is at -55), or wide round x or y 0
			{
				const s32 symx = fabsf(mins[0] + maxs[0]) < 2, symy = fabsf(mins[1] + maxs[1]) < 2;
				const s32 symz = fabsf(mins[2] + maxs[2]) < 2;

				if (!model && ((symz && maxs[2] - mins[2] > 150) || symx + symy + symz >= 2
						|| (symx && maxs[0] - mins[0] > 1000) || (symy && maxs[1] - mins[1] > 1000))) {
					localbrushes++;
					continue;
				}
			}

			// none of it in the box: past it
			if (maxs[0] < row->box[0] || mins[0] > row->box[1] || maxs[1] < row->box[2] || mins[1] > row->box[3]
					|| maxs[2] < CODSCAN_GROUND || numv < 4 || numv > 256) {
				continue;
			}

			v = malloc(sizeof(*v) * numv);

			for (i = 0; i < numv; i++) {
				sscanf(s, "%f %f %f%n", &v[i][0], &v[i][1], &v[i][2], &used);
				s += used;

				if (model) {
					v[i][0] += at[model][0];
					v[i][1] += at[model][1];
					v[i][2] += at[model][2];
				}
			}

			placedbrushes += model != 0;

			// the six axial faces
			for (i = 0; i < 6; i++) {
				f32 n[3] = { 0, 0, 0 };

				n[i % 3] = i < 3 ? -1.0f : 1.0f;
				brushtris += clipFace(v, numv, n, i < 3 ? -mins[i % 3] : maxs[i % 3], centre, mat, row);
			}

			// and its sides' planes
			if (sscanf(s, "%d%n", &numsides, &used) == 1) {
				s += used;

				for (i = 0; i < numsides; i++) {
					f32 n[3], d;

					if (sscanf(s, "%f %f %f %f%n", &n[0], &n[1], &n[2], &d, &used) != 4) {
						break;
					}

					s += used;

					if (model) {
						d += n[0] * at[model][0] + n[1] * at[model][1] + n[2] * at[model][2];
					}

					brushtris += clipFace(v, numv, n, d, centre, mat, row);
				}
			}

			free(v);
			brushes++;
		} else if ((line[0] == 't' || line[0] == 's') && line[1] == ' ') {
			s32 tag;
			f32 p[3][3];

			if (sscanf(s, "%d %f %f %f %f %f %f %f %f %f", &tag, &p[0][0], &p[0][1], &p[0][2],
						&p[1][0], &p[1][1], &p[1][2], &p[2][0], &p[2][1], &p[2][2]) != 10) {
				continue;
			}

			if (model) {
				s32 k;

				for (k = 0; k < 9; k++) {
					p[k / 3][k % 3] += at[model][k % 3];
				}
			}

			// a triangle's contents are its material's, a model's its own
			if (line[0] == 't' ? (tag < 0 || tag >= nummats || !(matcontents[tag] & CODCLIP_BLOCKS))
					: !(tag & CODCLIP_BLOCKS)) {
				continue;
			}

			if (!clipKeep(p[0], p[1], p[2], row)) {
				continue;
			}

			// either face: a floor faces up (a ceiling taken for one is over
			// the player, and the ground is what is under them)
			scanTri(p[0], p[1], p[2], centre, mat, 1);

			if (line[0] == 't') {
				worldtris++;
			} else {
				modeltris++;
			}
		}
	}

	fclose(f);
	free(matcontents);
	g_ScanCollision = 1;
	sysLogPrintf(LOG_NOTE, "codmap: %s: the map's own collision, %d brushes (%d triangles), %d world and %d model triangles; %d brushes a brush model's; %d brush models placed (%d brushes)",
			path, brushes, brushtris, worldtris, modeltris, localbrushes, numplaced, placedbrushes);

	return 1;
}

static void cbS32List(cbuf *b, const s32 *v, s32 n)
{
	s32 i;

	for (i = 0; i < n; i++) {
		cbU32(b, (u32)v[i]);
	}

	cbU32(b, 0xffffffff);
}

// The pads (geconvert.c's writePads()): the spawn points, then a pad for each
// path node, its waypoint, and the groups
static cbuf writePads(const struct codspawn *spawns, s32 numspawns)
{
	cbuf head = {0}, body = {0}, out = {0}, wprec = {0}, nbblob = {0}, rec = {0}, wlists = {0}, nlists = {0}, z;
	const s32 numpads = numspawns + g_NumNodes + g_NumExtraPads;
	const size_t start = (0x14 + 2 * numpads + 3) & ~3u;
	size_t wpstart, wgstart, coverstart;
	u32 npos, wpos;
	s32 i, g;

	for (i = 0; i < numpads; i++) {
		f32 pos[3];
		f32 yaw = 0;
		s32 room;

		if (i < numspawns) {
			memcpy(pos, spawns[i].pos, sizeof(pos));
			pos[1] = floorUnder(pos) + 1;
			yaw = spawns[i].yaw;
		} else if (i < numspawns + g_NumNodes) {
			memcpy(pos, g_Nodes[i - numspawns].pos, sizeof(pos));
		} else {
			memcpy(pos, g_ExtraPads[i - numspawns - g_NumNodes].pos, sizeof(pos));
			yaw = g_ExtraPads[i - numspawns - g_NumNodes].yaw;
		}

		room = 1 + cellOf(pos[0], pos[2]);

		cbU16(&head, (u32)(start + body.n));
		cbU32(&body, ((u32)room & 0x3ff) << 4);
		cbF32(&body, pos[0]);
		cbF32(&body, pos[1]);
		cbF32(&body, pos[2]);
		cbF32(&body, 0);
		cbF32(&body, 1);
		cbF32(&body, 0);
		// Call of Duty's yaw turns from +x towards +y, which is our -z
		cbF32(&body, cosf(yaw));
		cbF32(&body, 0);
		cbF32(&body, -sinf(yaw));
		cbPad(&body, 4);
	}

	cbPad(&head, 4);
	wpstart = 0x14 + head.n + body.n;
	npos = (u32)(wpstart + 16 * g_NumNodes + 16);

	for (i = 0; i < g_NumNodes; i++) {
		const size_t before = nbblob.n;

		cbU32(&wprec, (u32)(numspawns + i));
		cbU32(&wprec, npos);
		cbU32(&wprec, (u32)g_Nodes[i].group);
		cbU32(&wprec, 0);
		cbS32List(&nbblob, g_Nodes[i].nb, g_Nodes[i].numnb);
		npos += (u32)(nbblob.n - before);
	}

	cbU32(&wprec, 0xffffffff);
	cbZeros(&wprec, 12);

	wgstart = wpstart + wprec.n + nbblob.n;

	{
		// each group's waypoints, and the groups next to it
		s32 *members = malloc(sizeof(s32) * (g_NumNodes + 1));
		s32 *near = malloc(sizeof(s32) * (g_NumGroups + 1));
		u32 *nposes = malloc(sizeof(u32) * (g_NumGroups + 1));
		u32 *wposes = malloc(sizeof(u32) * (g_NumGroups + 1));
		cbuf nl = {0};

		for (g = 0; g < g_NumGroups; g++) {
			s32 n = 0, m = 0, k, j;

			for (i = 0; i < g_NumNodes; i++) {
				if (g_Nodes[i].group != g) {
					continue;
				}

				members[n++] = i;

				for (j = 0; j < g_Nodes[i].numnb; j++) {
					const s32 og = g_Nodes[g_Nodes[i].nb[j]].group;

					if (og == g) {
						continue;
					}

					for (k = 0; k < m && near[k] != og; k++) {
					}

					if (k == m) {
						near[m++] = og;
					}
				}
			}

			wposes[g] = (u32)wlists.n;
			cbS32List(&wlists, members, n);
			nposes[g] = (u32)nl.n;
			cbS32List(&nl, near, m);
		}

		wpos = (u32)(wgstart + 12 * g_NumGroups + 12);
		npos = wpos + (u32)wlists.n;

		for (g = 0; g < g_NumGroups; g++) {
			cbU32(&rec, npos + nposes[g]);
			cbU32(&rec, wpos + wposes[g]);
			cbU32(&rec, 0);
		}

		cbZeros(&rec, 12);
		cbPut(&nlists, nl.v, nl.n);
		free(nl.v);
		free(members);
		free(near);
		free(nposes);
		free(wposes);
	}

	coverstart = wgstart + rec.n + wlists.n + nlists.n;

	cbU32(&out, (u32)numpads);
	cbU32(&out, 0);
	cbU32(&out, (u32)wpstart);
	cbU32(&out, (u32)wgstart);
	cbU32(&out, (u32)coverstart);
	cbPut(&out, head.v, head.n);
	cbPut(&out, body.v, body.n);
	cbPut(&out, wprec.v, wprec.n);
	cbPut(&out, nbblob.v, nbblob.n);
	cbPut(&out, rec.v, rec.n);
	cbPut(&out, wlists.v, wlists.n);
	cbPut(&out, nlists.v, nlists.n);
	cbPad(&out, 16);
	z = rzip(out.v, out.n);
	free(head.v);
	free(body.v);
	free(wprec.v);
	free(nbblob.v);
	free(rec.v);
	free(wlists.v);
	free(nlists.v);
	free(out.v);

	return z;
}
// The multiplayer setup (geconvert.c's writeMpSetup()): the spawns, no props
static cbuf writeMpSetup(s32 numspawns)
{
	static const u8 ai1000[] = { 0x01, 0x85, 0x01, 0x45, 0x01, 0x46, 0x00, 0x05, 0xfd, 0x00, 0x00, 0x00, 0x04 };
	static const u8 ai1001[] = { 0x01, 0xb2, 0x16, 0x00, 0x05, 0xfd, 0x00, 0x00, 0x00, 0x04 };
	cbuf intro = {0}, props = {0}, out = {0}, z;
	size_t introat, propsat, pathsat, aiat, aicodeat;
	s32 i;

	for (i = 0; i < numspawns; i++) {
		cbU32(&intro, 0);
		cbU32(&intro, (u32)i);
		cbU32(&intro, 0);
	}

	cbU32(&intro, 0x0c);
	cbU32(&props, 0x34);

	introat = 0x20;
	propsat = introat + intro.n;
	pathsat = propsat + props.n;
	aiat = pathsat + 8;
	aicodeat = aiat + 24;

	cbU32(&out, 0);
	cbU32(&out, 0);
	cbU32(&out, 0);
	cbU32(&out, (u32)introat);
	cbU32(&out, (u32)propsat);
	cbU32(&out, (u32)pathsat);
	cbU32(&out, (u32)aiat);
	cbU32(&out, 0);
	cbPut(&out, intro.v, intro.n);
	cbPut(&out, props.v, props.n);
	cbZeros(&out, 8);
	cbU32(&out, (u32)aicodeat);
	cbU32(&out, 0x1000);
	cbU32(&out, (u32)(aicodeat + sizeof(ai1000)));
	cbU32(&out, 0x1001);
	cbZeros(&out, 8);
	cbPut(&out, ai1000, sizeof(ai1000));
	cbPut(&out, ai1001, sizeof(ai1001));
	cbPad(&out, 16);
	z = rzip(out.v, out.n);
	free(intro.v);
	free(props.v);
	free(out.v);

	return z;
}

/* ------------------------------------------------------------------------ */

static s32 writeOut(const char *dir, const char *rel, const cbuf *b)
{
	char path[1024];
	char *slash;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, rel);

	// its folders first
	for (slash = path + strlen(dir) + 1; (slash = strchr(slash, '/')) != NULL; slash++) {
		*slash = 0;
		fsCreateDir(path);
		*slash = '/';
	}

	f = fopen(path, "wb");

	if (!f) {
		sysLogPrintf(LOG_ERROR, "codmap: cannot write %s", path);
		return 0;
	}

	fwrite(b->v, 1, b->n, f);
	fclose(f);

	return 1;
}

// The map's entities: _entities.txt from the newer C2M, _mapEnts.txt the older
static void codmapEntsPath(char *path, size_t size, const char *indir, const char *map)
{
	snprintf(path, size, "%s/%s/%s_%s.txt", indir, map, map, g_NewC2m ? "entities" : "mapEnts");
}

// indir's last folder: the game (C2M's exported_maps/black_ops_2)
static const char *codmapGameOf(const char *indir)
{
	static char game[128];
	size_t n = strlen(indir);
	const char *s;

	while (n > 0 && (indir[n - 1] == '/' || indir[n - 1] == '\\')) {
		n--;
	}

	for (s = indir + n; s > indir && s[-1] != '/' && s[-1] != '\\'; s--);

	snprintf(game, sizeof(game), "%.*s", (int)(indir + n - s), s);

	return game;
}

static s32 codmapConvertRow(const struct codmaprow *row, const char *indir, const char *outdir, cbuf *maps)
{
	char path[1024], rel[256];
	const f32 centre[2] = { (row->box[0] + row->box[1]) / 2, (row->box[2] + row->box[3]) / 2 };
	struct codspawn spawns[64];
	struct croom *rooms;
	struct cportal *portals;
	s32 numrooms, numportals = 0, numspawns;
	s32 t, r, ix, iz, floors, walls;
	cbuf bg, tiles, pads, setup;

	g_NumTris = 0;
	g_NumMats = 0;

	// which C2M wrote it (g_NewC2m)
	{
		FILE *probe;

		snprintf(path, sizeof(path), "%s/%s/%s_static_models.json", indir, row->map, row->map);
		probe = fopen(path, "rb");
		g_NewC2m = probe != NULL;

		if (probe) {
			fclose(probe);
		}
	}

	snprintf(path, sizeof(path), "%s/textures/codmap.txt", outdir);
	texTableRead(path);
	snprintf(path, sizeof(path), "%s/%s/%s.obj", indir, row->map, row->map);

	if (!readObj(path, row, centre)) {
		return 0;
	}

	// the props: the newer C2M's own models (exported_models/<game>/, beside
	// exported_maps/<game>/, unless --codmap-props says), else where Greyhound's
	// export of them is given
	if (g_NewC2m) {
		char models[1024];

		if (sysArgGetString("--codmap-props")) {
			snprintf(models, sizeof(models), "%s", sysArgGetString("--codmap-props"));
		} else {
			snprintf(models, sizeof(models), "%s/../../exported_models/%s", indir, codmapGameOf(indir));
		}

		snprintf(path, sizeof(path), "%s/%s/%s_static_models.json", indir, row->map, row->map);
		readProps(path, models, row, centre);

		// and the wrecks Survival's script stands in the roads
		codmapEntsPath(path, sizeof(path), indir, row->map);
		readModeObjects(path, models, row, centre);
	} else if (sysArgGetString("--codmap-props")) {
		snprintf(path, sizeof(path), "%s/%s/%s_xmodels.json", indir, row->map, row->map);
		readProps(path, sysArgGetString("--codmap-props"), row, centre);
	}

	// The game's own collision where bo2scan has read it (--codmap-scan, else
	// Plutonium's scriptdata), else slopes over the stairs
	{
		char scan[1024];

		g_ScanCollision = 0;

		if (sysArgGetString("--codmap-scan")) {
			snprintf(scan, sizeof(scan), "%s", sysArgGetString("--codmap-scan"));
		} else {
			// only when asked for: built from the scan, Town's collision let the
			// player through nearly everything (F3 2026-10-29), so the drawn
			// world, the ramps and the walls stay the default
			scan[0] = 0;
		}

		// the map's own collision, exported (readClipMap()): --codmap-clip, else
		// where export_town_collision.bat puts it beside OpenAssetTools
		{
			char clip[1024];

			if (sysArgGetString("--codmap-clip")) {
				snprintf(clip, sizeof(clip), "%s", sysArgGetString("--codmap-clip"));
			} else {
				snprintf(clip, sizeof(clip), "%s/Desktop/Git/OpenAssetTools/clipdump/%s/maps/mp/%s.d3dbsp.clip.txt",
						getenv("USERPROFILE") ? getenv("USERPROFILE") : ".", row->map, row->map);
			}

			char ents[1024];

			codmapEntsPath(ents, sizeof(ents), indir, row->map);

			if (!readClipMap(clip, ents, row, centre) && !readScan(scan, row, centre)) {
				addRamps(row);
			}
		}
	}

	codmapEntsPath(path, sizeof(path), indir, row->map);
	numspawns = readSpawns(path, row, centre, spawns, ARRAYCOUNT(spawns));

	if (numspawns == 0) {
		sysLogPrintf(LOG_ERROR, "codmap: %s has no spawns named %s", row->map, row->spawns);
		return 0;
	}

	// the grid over the box
	g_GridX0 = (row->box[0] - centre[0]) * CODMAP_SCALE;
	g_GridZ0 = -(row->box[3] - centre[1]) * CODMAP_SCALE;
	g_GridX = (s32)ceilf((row->box[1] - row->box[0]) * CODMAP_SCALE / CODMAP_CELL);
	g_GridZ = (s32)ceilf((row->box[3] - row->box[2]) * CODMAP_SCALE / CODMAP_CELL);
	numrooms = g_GridX * g_GridZ;
	rooms = calloc(numrooms, sizeof(*rooms));
	g_MinY = 1e30f;
	g_MaxY = -1e30f;

	for (t = 0; t < g_NumTris; t++) {
		for (s32 k = 0; k < 3; k++) {
			g_MinY = g_Tris[t].p[k][1] < g_MinY ? g_Tris[t].p[k][1] : g_MinY;
			g_MaxY = g_Tris[t].p[k][1] > g_MaxY ? g_Tris[t].p[k][1] : g_MaxY;
		}
	}

	g_MinY -= 100;
	g_MaxY += 100;

	for (ix = 0; ix < g_GridX; ix++) {
		for (iz = 0; iz < g_GridZ; iz++) {
			struct croom *rm = &rooms[ix * g_GridZ + iz];

			rm->x0 = g_GridX0 + ix * CODMAP_CELL;
			rm->z0 = g_GridZ0 + iz * CODMAP_CELL;
			rm->centre[0] = rm->x0 + CODMAP_CELL / 2;
			rm->centre[1] = floorf((g_MinY + g_MaxY) / 2);
			rm->centre[2] = rm->z0 + CODMAP_CELL / 2;
		}
	}

	for (t = 0; t < g_NumTris; t++) {
		const struct ctri *tri = &g_Tris[t];

		if (g_Mats[tri->mat].unseen || tri->hide) {
			continue;
		}

		roomAddTri(&rooms[cellOf((tri->p[0][0] + tri->p[1][0] + tri->p[2][0]) / 3,
					(tri->p[0][2] + tri->p[1][2] + tri->p[2][2]) / 3)], t);
	}

	// a portal between every two neighbours, the level's height
	portals = calloc(2 * numrooms + 1, sizeof(*portals));

	for (ix = 0; ix < g_GridX; ix++) {
		for (iz = 0; iz < g_GridZ; iz++) {
			const s32 here = ix * g_GridZ + iz;
			const f32 x0 = g_GridX0 + ix * CODMAP_CELL, z0 = g_GridZ0 + iz * CODMAP_CELL;

			if (ix + 1 < g_GridX) {
				struct cportal *p = &portals[numportals++];
				const f32 x = x0 + CODMAP_CELL;
				const f32 pts[4][3] = { { x, g_MinY, z0 }, { x, g_MaxY, z0 }, { x, g_MaxY, z0 + CODMAP_CELL }, { x, g_MinY, z0 + CODMAP_CELL } };

				p->room1 = here + 1;
				p->room2 = here + g_GridZ + 1;
				memcpy(p->pts, pts, sizeof(pts));
			}

			if (iz + 1 < g_GridZ) {
				struct cportal *p = &portals[numportals++];
				const f32 z = z0 + CODMAP_CELL;
				const f32 pts[4][3] = { { x0, g_MinY, z }, { x0, g_MaxY, z }, { x0 + CODMAP_CELL, g_MaxY, z }, { x0 + CODMAP_CELL, g_MinY, z } };

				p->room1 = here + 1;
				p->room2 = here + 1 + 1;
				memcpy(p->pts, pts, sizeof(pts));
			}
		}
	}

	// the light under roofs, before the rooms are written with it
	shadePass();
	// A room with more triangles than its vertices can hold gives the rest to
	// pieces after the grid's rooms, drawn whenever it is (bg.c's
	// g_BgRoomPieces), each about the same centre with no portal and no tiles
	{
		const s32 numcells = numrooms;
		s32 r;

		for (r = 0; r < numcells; r++) {
			while (rooms[r].numtris > CODMAP_PIECE_TRIS) {
				struct croom *piece;
				const s32 keep = rooms[r].numtris - CODMAP_PIECE_TRIS;
				const s32 take = MIN(CODMAP_PIECE_TRIS, keep);

				rooms = realloc(rooms, sizeof(*rooms) * (numrooms + 1));
				piece = &rooms[numrooms++];
				memset(piece, 0, sizeof(*piece));
				piece->x0 = rooms[r].x0;
				piece->z0 = rooms[r].z0;
				memcpy(piece->centre, rooms[r].centre, sizeof(piece->centre));
				piece->drawnwith = r + 1;
				piece->tris = malloc(sizeof(s32) * take);
				memcpy(piece->tris, rooms[r].tris + rooms[r].numtris - take, sizeof(s32) * take);
				piece->numtris = piece->maxtris = take;
				rooms[r].numtris -= take;
			}
		}

		if (numrooms > numcells) {
			sysLogPrintf(LOG_NOTE, "codmap: %d rooms cut into %d more pieces", numcells, numrooms - numcells);
		}
	}

	bg = writeBg(rooms, numrooms, portals, numportals);
	tiles = writeTiles(rooms, numrooms, &floors, &walls);
	codmapEntsPath(path, sizeof(path), indir, row->map);
	readNodes(path, row, centre);
	readLayout(path, row, centre, numspawns + g_NumNodes);
	blockGridFree();
	pads = writePads(spawns, numspawns);

	if (g_Layout.n) {
		snprintf(rel, sizeof(rel), "zombies/%s.txt", row->name);
		writeOut(outdir, rel, &g_Layout);
	}
	setup = writeMpSetup(numspawns);

	snprintf(rel, sizeof(rel), "files/bgdata/bg_%s.seg", row->key);
	writeOut(outdir, rel, &bg);
	snprintf(rel, sizeof(rel), "files/bgdata/bg_%s_tilesZ", row->key);
	writeOut(outdir, rel, &tiles);
	snprintf(rel, sizeof(rel), "files/bgdata/bg_%s_padsZ", row->key);
	writeOut(outdir, rel, &pads);
	snprintf(rel, sizeof(rel), "files/Ump_setup%sZ", row->key);
	writeOut(outdir, rel, &setup);

	{
		char line[512];

		snprintf(line, sizeof(line), "%s  map \"%s\" bg \"bgdata/bg_%s.seg\" tiles \"bgdata/bg_%s_tilesZ\" pads \"bgdata/bg_%s_padsZ\" mpsetup \"Ump_setup%sZ\" fog \"%s\"",
				maps->n ? "\n" : "", row->name, row->key, row->key, row->key, row->key, row->fog);
		cbPut(maps, line, strlen(line));
	}

	sysLogPrintf(LOG_NOTE, "codmap: %s: %d rooms (%dx%d), %d portals, %d floor and %d wall tiles, %d spawns; bg %u bytes, tiles %u",
			row->name, numrooms, g_GridX, g_GridZ, numportals, floors, walls, numspawns, (u32)bg.n, (u32)tiles.n);

	for (r = 0; r < numrooms; r++) {
		free(rooms[r].tris);
		free(rooms[r].data.v);
	}

	free(rooms);
	free(portals);
	free(bg.v);
	free(tiles.v);
	free(pads.v);
	free(setup.v);

	return 1;
}

#include "config.h"

void modMapsEnableByName(const char *name);

#define CODMAP_MODDIR "Black Ops II Maps"

static s32 g_CodMapMapsOffered;

PD_CONSTRUCTOR static void codmapConfigInit(void)
{
	configRegisterInt("Mod.BlackOps2MapsOffered", &g_CodMapMapsOffered, 0, 1);
}

/**
 * Black Ops II's maps, once they are converted, on in the Stage Loader the
 * first time they are found (as GE Plus turns on its arenas, gexplusrom.c):
 * left to the Stage Loader's own checkbox they were never in the arena list.
 * Asked once, so a player who turns them off keeps them off. Before the mods
 * are mounted (main.c).
 */
void codmapEnableMapsAtStartup(void)
{
	char path[1024];

	if (g_CodMapMapsOffered) {
		return;
	}

	snprintf(path, sizeof(path), "%s", fsFullPath("$E/mods/" CODMAP_MODDIR "/modconfig.txt"));

	if (fsFileSize(path) <= 0) {
		return;
	}

	g_CodMapMapsOffered = 1;
	modMapsEnableByName(CODMAP_MODDIR);
	configSave(CONFIG_PATH);
	sysLogPrintf(LOG_NOTE, "codmap: Black Ops II's maps turned on in the Stage Loader");
}

const char *modloaderGetStageModDir(s32 stagenum);
s32 mainGetStageNum(void);

/**
 * texpack.c asks: whether the running stage is one of Black Ops II's, whose
 * mod's own pack (textures/<num>.png) is its textures and not a choice, so it
 * is drawn whatever Mod.LoadTextures says - the placeholders under it are a
 * flat colour each
 */
// bg.c asks: whether a stage is one of Black Ops II's, whose rooms may have
// pieces drawn with them as a converted GoldenEye level's do
s32 codmapStageIs(s32 stagenum)
{
	const char *dir = modloaderGetStageModDir(stagenum);

	return dir && strstr(dir, CODMAP_MODDIR) != NULL;
}

s32 codmapStageWantsPack(void)
{
	const char *dir = modloaderGetStageModDir(mainGetStageNum());

	return dir && strstr(dir, CODMAP_MODDIR) != NULL;
}

// --codmap-convert <dir>: every row whose map C2M exported into dir, then exit
void codmapConvertFromCommandLine(void)
{
	const char *indir = sysArgGetString("--codmap-convert");
	char outdir[1024];
	cbuf maps = {0}, config = {0};
	s32 done = 0;
	s32 i;

	if (!indir) {
		return;
	}

	snprintf(outdir, sizeof(outdir), "%s", fsFullPath("$E/mods/" CODMAP_MODDIR));
	fsCreateDir(fsFullPath("$E/mods"));
	fsCreateDir(outdir);

	for (i = 0; i < (s32)ARRAYCOUNT(g_CodMapRows); i++) {
		done += codmapConvertRow(&g_CodMapRows[i], indir, outdir, &maps);
	}

	if (done) {
		const char *head = "# Black Ops II's maps, from C2M's export (port/src/codmap.c)\nmaps {\n";

		cbPut(&config, head, strlen(head));
		cbPut(&config, maps.v, maps.n);
		cbPut(&config, "\n}\n", 3);
		writeOut(outdir, "modconfig.txt", &config);
	}

	sysLogPrintf(LOG_NOTE, "codmap: %d of %d maps converted into %s", done, (s32)ARRAYCOUNT(g_CodMapRows), outdir);
	free(maps.v);
	free(config.v);
	exit(done ? 0 : 1);
}

#endif
