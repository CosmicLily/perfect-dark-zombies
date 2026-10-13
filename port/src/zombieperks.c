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

#define ZOMBIEPERK_COUNT 6

static const char *const perkFiles[ZOMBIEPERK_COUNT] = {
	"$E/zombies/perks/juggernog.png",
	"$E/zombies/perks/quickrevive.png",
	"$E/zombies/perks/speedcola.png",
	"$E/zombies/perks/doubletap.png",
	"$E/zombies/perks/staminup.png",
	"$E/zombies/perks/mulekick.png",
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

static struct menuimage perkImages[ZOMBIEPERK_COUNT] = {
	{ NULL, 0, zombiePerkLoad0, "zombies/perks/juggernog.png" },
	{ NULL, 0, zombiePerkLoad1, "zombies/perks/quickrevive.png" },
	{ NULL, 0, zombiePerkLoad2, "zombies/perks/speedcola.png" },
	{ NULL, 0, zombiePerkLoad3, "zombies/perks/doubletap.png" },
	{ NULL, 0, zombiePerkLoad4, "zombies/perks/staminup.png" },
	{ NULL, 0, zombiePerkLoad5, "zombies/perks/mulekick.png" },
};

// Whether each picture is there, asked once: -1 not yet
static s32 perkPresent[ZOMBIEPERK_COUNT] = { -1, -1, -1, -1, -1, -1 };

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
