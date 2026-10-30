/**
 * Zombies' perk icons as pictures (scenarios/zombies.inc draws them).
 *
 * A perk's icon is drawn from a PNG beside the executable when there is one -
 * zombies/perks/<perk>.png, a square badge on a transparent field - and from
 * the HUD's own drawn crest when there is not. The pictures are the player's
 * own and are never part of the game or its repository: Call of Duty's perk
 * art is Activision's.
 *
 * They reach the screen the way a community pack's cover does (menuimage.c):
 * a stand-in tile bound in the display list, the picture swapped in for it by
 * the renderer, decoded on the render thread the first time it is drawn.
 *
 * The order is zombies.inc's ZOMBIEPERK_* numbers.
 */

#include <ultra64.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "fs.h"
#include "pngread.h"
#include "menuimage.h"
#include "system.h"
#include "config.h"

#define ZOMBIEPERK_COUNT 9

static const char *const perkFiles[ZOMBIEPERK_COUNT] = {
	"$E/zombies/perks/juggernog.png",
	"$E/zombies/perks/quickrevive.png",
	"$E/zombies/perks/speedcola.png",
	"$E/zombies/perks/doubletap.png",
	"$E/zombies/perks/staminup.png",
	"$E/zombies/perks/mulekick.png",
	"$E/zombies/perks/deadshot.png",
	"$E/zombies/perks/phdflopper.png",
	"$E/zombies/perks/electriccherry.png",
};

static u8 *zombiePerkLoad(s32 perk, s32 *outWidth, s32 *outHeight)
{
	return pngRead(fsFullPath(perkFiles[perk]), outWidth, outHeight);
}

// menuimage's load callback takes no argument: one each
static u8 *zombiePerkLoad0(s32 *w, s32 *h) { return zombiePerkLoad(0, w, h); }
static u8 *zombiePerkLoad1(s32 *w, s32 *h) { return zombiePerkLoad(1, w, h); }
static u8 *zombiePerkLoad2(s32 *w, s32 *h) { return zombiePerkLoad(2, w, h); }
static u8 *zombiePerkLoad3(s32 *w, s32 *h) { return zombiePerkLoad(3, w, h); }
static u8 *zombiePerkLoad4(s32 *w, s32 *h) { return zombiePerkLoad(4, w, h); }
static u8 *zombiePerkLoad5(s32 *w, s32 *h) { return zombiePerkLoad(5, w, h); }
static u8 *zombiePerkLoad6(s32 *w, s32 *h) { return zombiePerkLoad(6, w, h); }
static u8 *zombiePerkLoad7(s32 *w, s32 *h) { return zombiePerkLoad(7, w, h); }
static u8 *zombiePerkLoad8(s32 *w, s32 *h) { return zombiePerkLoad(8, w, h); }

static struct menuimage perkImages[ZOMBIEPERK_COUNT] = {
	{ NULL, 0, zombiePerkLoad0, "zombies/perks/juggernog.png" },
	{ NULL, 0, zombiePerkLoad1, "zombies/perks/quickrevive.png" },
	{ NULL, 0, zombiePerkLoad2, "zombies/perks/speedcola.png" },
	{ NULL, 0, zombiePerkLoad3, "zombies/perks/doubletap.png" },
	{ NULL, 0, zombiePerkLoad4, "zombies/perks/staminup.png" },
	{ NULL, 0, zombiePerkLoad5, "zombies/perks/mulekick.png" },
	{ NULL, 0, zombiePerkLoad6, "zombies/perks/deadshot.png" },
	{ NULL, 0, zombiePerkLoad7, "zombies/perks/phdflopper.png" },
	{ NULL, 0, zombiePerkLoad8, "zombies/perks/electriccherry.png" },
};

// Whether each picture is there, asked once: -1 not yet
static s32 perkPresent[ZOMBIEPERK_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };

PD_CONSTRUCTOR static void zombiePerksInit(void)
{
	s32 i;

	for (i = 0; i < ZOMBIEPERK_COUNT; i++) {
		menuImageRegister(&perkImages[i]);
	}
}

/**
 * The picture for a perk, or NULL when the player has none for it (and the
 * drawn crest is used). The file is looked for once a run.
 */
struct menuimage *zombiePerkImage(s32 perk)
{
	if (perk < 0 || perk >= ZOMBIEPERK_COUNT) {
		return NULL;
	}

	if (perkPresent[perk] < 0) {
		perkPresent[perk] = fsFileSize(perkFiles[perk]) > 0;

		sysLogPrintf(LOG_NOTE, "zombies: perk icon %s %s", perkFiles[perk],
				perkPresent[perk] ? "found" : "not there, the drawn one is used");
	}

	return perkPresent[perk] ? &perkImages[perk] : NULL;
}

/**
 * Zombies' own settings, in pd.ini's [Game] and on the Zombies Options page of
 * the Combat Simulator's setup (scenarios/zombies.inc's menu): how many points
 * a match starts with (Call of Duty's 500), the round it starts on, how many
 * perks a player may have at once, and the power-ups, the round music and the
 * zombies' groans each on or off.
 */
s32 g_ZombieCfgStartPoints = 20000;
s32 g_ZombieCfgStartRound = 1;
s32 g_ZombieCfgPowerups = 1;
s32 g_ZombieCfgRoundMusic = 1;
s32 g_ZombieCfgGroans = 1;
s32 g_ZombieCfgPerkLimit = 4; // perks a player may have at once, Black Ops II's four; 9 and over is all nine
s32 g_ZombieCfgAdsSens[2] = { 60, 60 }; // the look down the sights, percent of the hip's: the mouse's, the stick's (Controls)

PD_CONSTRUCTOR static void zombieConfigInit(void)
{
	configRegisterInt("Game.ZombiesPerkLimit", &g_ZombieCfgPerkLimit, 1, 99);
	configRegisterInt("Game.ZombiesAdsSensitivityMouse", &g_ZombieCfgAdsSens[0], 10, 100);
	configRegisterInt("Game.ZombiesAdsSensitivityController", &g_ZombieCfgAdsSens[1], 10, 100);
	configRegisterInt("Game.ZombiesStartPoints", &g_ZombieCfgStartPoints, 0, 1000000);
	configRegisterInt("Game.ZombiesStartRound", &g_ZombieCfgStartRound, 1, 100);
	configRegisterInt("Game.ZombiesPowerups", &g_ZombieCfgPowerups, 0, 1);
	configRegisterInt("Game.ZombiesRoundMusic", &g_ZombieCfgRoundMusic, 0, 1);
	configRegisterInt("Game.ZombiesGroans", &g_ZombieCfgGroans, 0, 1);
}
