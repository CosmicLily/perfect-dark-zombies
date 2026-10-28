/**
 * A mod map's pictures loaded before its match, the way Black Ops III loads a
 * workshop map's: the moment the map is chosen (the Combat Simulator's arena
 * list, or --boot-map), its mod's every texture is queued for decoding
 * (texpackPreloadStage()), and a box at the bottom right of the screen shows
 * how far it has come. If the match is started before it is through, the
 * stage load waits for the rest (mappreloadFinish()), so the map never opens
 * on the game's flat placeholders swapping to the real pictures one at a time
 * as the player looks round.
 *
 * Black Ops II's Town is 291 pictures, about 200 MB decoded.
 */

#include <stdio.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "game/game_1531a0.h"
#include "lib/vi.h"
#include "gbiex.h"
#include "texpack.h"
#include "modloader.h"
#include "system.h"
#include "mappreload.h"
#include <SDL.h>

void gfx_texpack_poll(void); // gfx_pc.cpp: lands the decoded pictures

// How long "Ready" stays up once everything is in, in frames
#define MAPPRELOAD_READY_FRAMES 90
// How long a stage load waits for the rest at most, in milliseconds
#define MAPPRELOAD_WAIT_MS 30000

static s32 g_MapPreloadStage = -1;  // the stage last looked at
static s32 g_MapPreloadActive;      // a preload is in hand
static s32 g_MapPreloadReadyFrames; // "Ready" counting down
static char g_MapPreloadName[32];

/** Called each frame: a newly chosen mod map starts its preload. */
static void mappreloadTick(void)
{
	const s32 stagenum = g_MpSetup.stagenum;

	if (stagenum == g_MapPreloadStage) {
		return;
	}

	g_MapPreloadStage = stagenum;
	g_MapPreloadActive = 0;
	g_MapPreloadReadyFrames = 0;
	texpackPreloadStop();

	if (modloaderGetStageModDir(stagenum) == NULL) {
		return;
	}

	snprintf(g_MapPreloadName, sizeof(g_MapPreloadName), "%s",
			modloaderGetStageMapName(stagenum) ? modloaderGetStageMapName(stagenum) : "Custom Map");

	g_MapPreloadActive = texpackPreloadStage(stagenum);
}

void mappreloadFinish(s32 stagenum)
{
	const u32 start = SDL_GetTicks();
	s32 done;
	s32 total;

	// chosen some other way than the arena list
	if (stagenum != g_MapPreloadStage) {
		g_MapPreloadStage = stagenum;
		g_MapPreloadActive = modloaderGetStageModDir(stagenum) != NULL && texpackPreloadStage(stagenum);
	}

	if (!g_MapPreloadActive) {
		return;
	}

	while (texpackPreloadProgress(&done, &total) && done < total
			&& SDL_GetTicks() - start < MAPPRELOAD_WAIT_MS) {
		gfx_texpack_poll();
		SDL_PumpEvents();
		SDL_Delay(2);
	}

	texpackPreloadProgress(&done, &total);
	sysLogPrintf(LOG_NOTE, "mappreload: stage 0x%02x: %d of %d picture(s) in before the match, %u ms waited",
			stagenum, done, total, SDL_GetTicks() - start);

	g_MapPreloadActive = 0;
	g_MapPreloadReadyFrames = 0;
	texpackPreloadStop();
}

Gfx *mappreloadRender(Gfx *gdl)
{
	s32 done = 0;
	s32 total = 0;
	s32 x;
	s32 y;
	s32 x1;
	s32 y1;
	s32 x2;
	s32 y2;
	s32 barx2;
	char line[64];

	mappreloadTick();

	if (!g_CharsHandelGothicSm || !g_FontHandelGothicSm) {
		return gdl;
	}

	// --mappreload-show: the box half way, held, to look at it in a match
	if (sysArgCheck("--mappreload-show")) {
		done = 1;
		total = 2;
		snprintf(g_MapPreloadName, sizeof(g_MapPreloadName), "Town (BO2)");
	} else if (!g_MapPreloadActive) {
		return gdl;
	} else if (!texpackPreloadProgress(&done, &total) || total <= 0) {
		g_MapPreloadActive = 0;
		return gdl;
	}

	if (done >= total) {
		if (g_MapPreloadReadyFrames == 0) {
			g_MapPreloadReadyFrames = MAPPRELOAD_READY_FRAMES;
			sysLogPrintf(LOG_NOTE, "mappreload: %s: all %d picture(s) loaded", g_MapPreloadName, total);
		} else if (--g_MapPreloadReadyFrames <= 0) {
			g_MapPreloadActive = 0;
			texpackPreloadStop();
			return gdl;
		}
	}

	// bottom right: the map, a bar, and the count
	x2 = viGetWidth() - 20;
	x1 = x2 - 130;
	y2 = viGetHeight() - 20;
	y1 = y2 - 42;
	barx2 = x1 + 6 + (s32)((f32)(x2 - x1 - 12) * done / total);

	gSPSetExtraGeometryModeEXT(gdl++, g_HudAlignModeR);
	gdl = text0f153628(gdl);

	gdl = textSetPrimColour(gdl, 0x000000b0);
	gDPFillRectangleScaled(gdl++, x1, y1, x2, y2);

	gdl = textSetPrimColour(gdl, 0x404040ff);
	gDPFillRectangleScaled(gdl++, x1 + 6, y2 - 10, x2 - 6, y2 - 6);

	gdl = textSetPrimColour(gdl, done >= total ? 0x40d040ff : 0xd0a020ff);
	gDPFillRectangleScaled(gdl++, x1 + 6, y2 - 10, barx2, y2 - 6);

	gdl = text0f153838(gdl);

	x = x1 + 6;
	y = y1 + 5;
	gdl = textRender(gdl, &x, &y, g_MapPreloadName, g_CharsHandelGothicSm, g_FontHandelGothicSm,
			0xffffffe0, 0x000000a0, viGetWidth(), viGetHeight(), 0, 0);

	if (done >= total) {
		snprintf(line, sizeof(line), "Ready");
	} else {
		snprintf(line, sizeof(line), "Loading %d%%", done * 100 / total);
	}

	x = x1 + 6;
	y = y1 + 17;
	gdl = textRender(gdl, &x, &y, line, g_CharsHandelGothicSm, g_FontHandelGothicSm,
			0xc0c0c0e0, 0x000000a0, viGetWidth(), viGetHeight(), 0, 0);

	gdl = text0f153780(gdl);
	gSPClearExtraGeometryModeEXT(gdl++, g_HudAlignModeR);

	return gdl;
}
