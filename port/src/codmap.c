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
#define CODMAP_CELL  1500.0f  // a room's side
#define CODMAP_REACH 60.0f    // a tile goes in every room it comes this near
#define CODMAP_FLOOR_NY 0.6f  // a triangle facing up this much is a floor

#define FLOOR_FLAGS (0x0001 | 0x0002 | 0x0008 | 0x0010)
#define WALL_FLAGS  (0x0004 | 0x0008 | 0x0010)

struct codmaprow {
	const char *key;    // the files' name: bg_<key>.seg
	const char *name;   // the arena's
	const char *map;    // C2M's folder and file stem
	f32 box[4];         // x0 x1 y0 y1, the map's inches
	const char *spawns; // the targetname of its player spawns
};

static const struct codmaprow g_CodMapRows[] = {
	{ "bo2town", "Town (BO2)", "zm_transit", { -2168, 4785, -4578, 2416 }, "town_standard_player_spawns" },
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
	f32 n[3];
	u32 colour;    // the vertices', lit
	s32 mat;
	u8 solid;
};

struct cmat {
	char name[96];
	u32 rgb;
	u8 solid;
};

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
	static const char *const nosolid[] = { "decal", "foliage", "fol_", "leaf", "leaves", "grass", "vine", "fx_", "sky" };
	static const u32 palette[] = {
		0x8a7a66, 0x6f6a60, 0x7d6b55, 0x5f5a52, 0x8f8576, 0x6b5b4a, 0x777a74, 0x857060,
		0x6a6f6a, 0x9a8c78, 0x5c5248, 0x807568,
	};
	s32 i;
	u32 h;

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

	for (i = 0; i < (s32)ARRAYCOUNT(nosolid); i++) {
		if (strstr(name, nosolid[i])) {
			g_Mats[g_NumMats].solid = 0;
		}
	}

	return g_NumMats++;
}

static void triAdd(const f32 a[3], const f32 b[3], const f32 c[3], s32 mat)
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

	// a light from above and a little to the side, either face alike
	lit = 0.55f + 0.45f * fabsf(t->n[1] * 0.85f + t->n[0] * 0.3f + t->n[2] * 0.2f);
	t->colour = (u32)(((rgb >> 16) & 0xff) * lit) << 24 | (u32)(((rgb >> 8) & 0xff) * lit) << 16
		| (u32)((rgb & 0xff) * lit) << 8 | 0xff;
	t->mat = mat;
	t->solid = g_Mats[mat].solid;
	g_NumTris++;
}

/* ------------------------------------------------------------------------ */

static void codToOurs(const f32 cod[3], const f32 *centre, f32 out[3])
{
	out[0] = (cod[0] - centre[0]) * CODMAP_SCALE;
	out[1] = cod[2] * CODMAP_SCALE;
	out[2] = -(cod[1] - centre[1]) * CODMAP_SCALE;
}

// The OBJ's triangles whose middle is in the row's box, into g_Tris
static s32 readObj(const char *path, const struct codmaprow *row, const f32 *centre)
{
	FILE *f = fopen(path, "rb");
	char line[1024];
	f32 *verts = NULL;
	s32 numverts = 0, maxverts = 0;
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
			numverts++;
		} else if (strncmp(line, "usemtl ", 7) == 0) {
			char *e = line + strlen(line);

			while (e > line + 7 && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) {
				*--e = 0;
			}

			mat = matFind(line + 7);
		} else if (line[0] == 'f' && line[1] == ' ') {
			s32 idx[16];
			s32 n = 0;
			char *s = line + 2;
			f32 cx = 0, cy = 0;
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
				n++;

				while (*s && *s != ' ') {
					s++;
				}
			}

			if (n < 3 || mat < 0) {
				continue;
			}

			for (k = 0; k < n; k++) {
				if (idx[k] < 0 || idx[k] >= numverts) {
					n = 0;
					break;
				}

				cx += verts[idx[k] * 3];
				cy += verts[idx[k] * 3 + 1];
			}

			if (n < 3) {
				continue;
			}

			cx /= n;
			cy /= n;

			if (cx < row->box[0] || cx > row->box[1] || cy < row->box[2] || cy > row->box[3]) {
				continue;
			}

			for (k = 1; k < n - 1; k++) {
				f32 a[3], b[3], c[3];

				codToOurs(&verts[idx[0] * 3], centre, a);
				codToOurs(&verts[idx[k] * 3], centre, b);
				codToOurs(&verts[idx[k + 1] * 3], centre, c);
				triAdd(a, b, c, mat);
			}
		}
	}

	fclose(f);
	free(verts);
	sysLogPrintf(LOG_NOTE, "codmap: %s: %d vertices, %d triangles in the box, %d materials", path, numverts, g_NumTris, g_NumMats);

	return g_NumTris > 0;
}

/* ------------------------------------------------------------------------ */

struct codspawn {
	f32 pos[3];
	f32 yaw;
};

// The row's player spawns from the map's entities (C2M's _mapEnts.txt)
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
	s32 i = 0;
	s32 c;

	// the state: no texture, the vertex colour as it is, both faces
	{
		Gfx g[4];

		gSPTexture(&g[0], 0, 0, 0, G_TX_RENDERTILE, G_OFF);
		gDPSetCombineMode(&g[1], G_CC_SHADE, G_CC_SHADE);
		gSPClearGeometryMode(&g[2], G_LIGHTING | G_TEXTURE_GEN | G_CULL_BOTH);
		gSPSetGeometryMode(&g[3], G_SHADE | G_SHADING_SMOOTH);

		for (c = 0; c < 4; c++) {
			gfxWords(&words, &g[c]);
		}
	}

	while (i < r->numtris) {
		f32 batch[16][3];
		u32 bcol[16];
		u8 idx[64][3];
		s32 nb = 0, nt = 0;
		const s32 start = numvtx;
		s32 k;

		for (; i < r->numtris && nt < 64; i++) {
			const struct ctri *t = &g_Tris[r->tris[i]];
			s32 got[3];
			s32 added = nb;
			s32 ok = 1;

			for (k = 0; k < 3 && ok; k++) {
				s32 j = 0;

				while (j < added && !(batch[j][0] == t->p[k][0] && batch[j][1] == t->p[k][1]
							&& batch[j][2] == t->p[k][2] && bcol[j] == t->colour)) {
					j++;
				}

				if (j == added) {
					if (added == 16) {
						ok = 0;
						break;
					}

					memcpy(batch[added], t->p[k], sizeof(batch[added]));
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
			cbU16(&vtx, 0);
			cbU16(&vtx, 0);
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

	vtxat = (0x18 + 20 + 7) & ~7u;
	colat = vtxat + vtx.n;
	gdlat = (colat + col.n + 7) & ~7u;

	cbZeros(&r->data, gdlat);
	set32(r->data.v, 0, baseptr + (u32)vtxat);
	set32(r->data.v, 4, baseptr + (u32)colat);
	set32(r->data.v, 8, baseptr + 0x18);
	set32(r->data.v, 12, 0);
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
static cbuf writeTiles(struct croom *rooms, s32 numrooms, s32 *outfloors, s32 *outwalls)
{
	cbuf *bodies = calloc(numrooms + 1, sizeof(cbuf));
	cbuf out = {0}, z;
	u32 pos;
	s32 floors = 0, walls = 0;
	s32 t, r;

	for (t = 0; t < g_NumTris; t++) {
		const struct ctri *tri = &g_Tris[t];
		f32 lo[3], hi[3];
		s32 ix0, ix1, iz0, iz1, ix, iz, c, k;
		s16 p[3][3];
		u32 flags;

		if (!tri->solid) {
			continue;
		}

		flags = tri->n[1] >= CODMAP_FLOOR_NY ? FLOOR_FLAGS : WALL_FLAGS;

		for (c = 0; c < 3; c++) {
			lo[c] = hi[c] = tri->p[0][c];

			for (k = 1; k < 3; k++) {
				lo[c] = tri->p[k][c] < lo[c] ? tri->p[k][c] : lo[c];
				hi[c] = tri->p[k][c] > hi[c] ? tri->p[k][c] : hi[c];
			}

			for (k = 0; k < 3; k++) {
				p[k][c] = s16of(tri->p[k][c]);
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

				cbU8(b, 0);
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

				for (k = 0; k < 3; k++) {
					for (c = 0; c < 3; c++) {
						cbU16(b, (u16)p[k][c]);
					}
				}

				if (flags == FLOOR_FLAGS) {
					floors++;
				} else {
					walls++;
				}
			}
		}
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

// The pads (geconvert.c's writePads()): the spawn points and nothing else
static cbuf writePads(const struct codspawn *spawns, s32 numspawns)
{
	cbuf head = {0}, body = {0}, out = {0}, z;
	const size_t start = (0x14 + 2 * numspawns + 3) & ~3u;
	size_t wpstart, wgstart, coverstart;
	s32 i;

	for (i = 0; i < numspawns; i++) {
		f32 pos[3];
		const s32 room = 1 + cellOf(spawns[i].pos[0], spawns[i].pos[2]);

		memcpy(pos, spawns[i].pos, sizeof(pos));
		pos[1] = floorUnder(pos) + 1;

		cbU16(&head, (u32)(start + body.n));
		cbU32(&body, ((u32)room & 0x3ff) << 4);
		cbF32(&body, pos[0]);
		cbF32(&body, pos[1]);
		cbF32(&body, pos[2]);
		cbF32(&body, 0);
		cbF32(&body, 1);
		cbF32(&body, 0);
		// Call of Duty's yaw turns from +x towards +y, which is our -z
		cbF32(&body, cosf(spawns[i].yaw));
		cbF32(&body, 0);
		cbF32(&body, -sinf(spawns[i].yaw));
		cbPad(&body, 4);
	}

	cbPad(&head, 4);
	wpstart = 0x14 + head.n + body.n;
	wgstart = wpstart + 16;
	coverstart = wgstart + 12;

	cbU32(&out, (u32)numspawns);
	cbU32(&out, 0);
	cbU32(&out, (u32)wpstart);
	cbU32(&out, (u32)wgstart);
	cbU32(&out, (u32)coverstart);
	cbPut(&out, head.v, head.n);
	cbPut(&out, body.v, body.n);
	// no waypoints, no groups
	cbU32(&out, 0xffffffff);
	cbZeros(&out, 12);
	cbZeros(&out, 12);
	cbPad(&out, 16);
	z = rzip(out.v, out.n);
	free(head.v);
	free(body.v);
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
	snprintf(path, sizeof(path), "%s/%s/%s.obj", indir, row->map, row->map);

	if (!readObj(path, row, centre)) {
		return 0;
	}

	snprintf(path, sizeof(path), "%s/%s/%s_mapEnts.txt", indir, row->map, row->map);
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

	bg = writeBg(rooms, numrooms, portals, numportals);
	tiles = writeTiles(rooms, numrooms, &floors, &walls);
	pads = writePads(spawns, numspawns);
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

		snprintf(line, sizeof(line), "%s  map \"%s\" bg \"bgdata/bg_%s.seg\" tiles \"bgdata/bg_%s_tilesZ\" pads \"bgdata/bg_%s_padsZ\" mpsetup \"Ump_setup%sZ\"",
				maps->n ? "\n" : "", row->name, row->key, row->key, row->key, row->key);
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
