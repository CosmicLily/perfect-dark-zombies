#ifndef _IN_MAPPRELOAD_H
#define _IN_MAPPRELOAD_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

/**
 * A mod map's pictures loaded before its match (mappreload.c). Render every
 * frame (it starts a preload when the chosen map changes, and draws the box at
 * the bottom right); Finish at the stage load waits for what is left.
 */
Gfx *mappreloadRender(Gfx *gdl);
void mappreloadFinish(s32 stagenum);

#endif
