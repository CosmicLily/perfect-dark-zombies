// Prototype Zombies mode, hooked into Combat for now.

void scenarioCreateHudmsg(s32 playernum, char *message);
void botSpawn(struct chrdata *chr, u8 respawning);
void mainEndStage(void);
void botApplyAttack(struct chrdata *chr, struct prop *prop);
void botSetTarget(struct chrdata *chr, s32 propnum);
bool chrIsDead(struct chrdata *chr);

void lvSetMpTimeLimit60(u32 limit);
void lvSetMpScoreLimit(u32 limit);
void lvSetMpTeamScoreLimit(u32 limit);
void mpCalculateTeamIsOnlyAi(void);
extern u32 g_HudAlignModeL;
extern u32 g_HudAlignModeR;

static s32 g_ZombieRound = 0;
static s32 g_ZombieNextWaveFrame = -1;
static s32 g_ZombieGraceUntil = 0;
static bool g_ZombieGameOver = false;
static bool g_ZombieDeathMsgShown = false;
static bool g_ZombieSaved = false;
static u32 g_ZombieSavedOptions = 0;
static u8 g_ZombieSavedTeams[MAX_MPCHRS];
static s32 g_ZombieSavedBotType[MAX_BOTS];
static s32 g_ZombieSavedBotDiff[MAX_BOTS];
static f32 g_ZombieMaxHealth = 1.0f;
static f32 g_ZombieLastHealth = 1.0f;
static s32 g_ZombieRegenDelay60 = 0;

static void zombiesShowSurvivedMsg(void)
{
	char msg[48];
	s32 prevplayernum = g_Vars.currentplayernum;

	sprintf(msg, "YOU SURVIVED %d ROUND%s\n", g_ZombieRound, g_ZombieRound == 1 ? "" : "S");

	setCurrentPlayerNum(0);
	hudmsgCreateWithFlags(msg, HUDMSGTYPE_MPSCENARIO, 0);
	setCurrentPlayerNum(prevplayernum);
}

// Puts back the settings we changed at match start.
static void zombiesRestoreSettings(void)
{
	s32 i;

	if (!g_ZombieSaved) {
		return;
	}

	g_MpSetup.options &= ~MPOPTION_TEAMSENABLED;
	g_MpSetup.options |= (g_ZombieSavedOptions & MPOPTION_TEAMSENABLED);

	for (i = 0; i < g_MpNumChrs; i++) {
		g_MpAllChrConfigPtrs[i]->team = g_ZombieSavedTeams[i];
	}

	for (i = 0; i < g_BotCount; i++) {
		if (g_MpBotChrPtrs[i] && g_MpBotChrPtrs[i]->aibot) {
			g_MpBotChrPtrs[i]->aibot->config->type = g_ZombieSavedBotType[i];
			g_MpBotChrPtrs[i]->aibot->config->difficulty = g_ZombieSavedBotDiff[i];
		}
	}

	g_ZombieSaved = false;
}

static u8 zombiesDifficultyForRound(s32 round)
{
	if (round <= 2) {
		return BOTDIFF_EASY;
	}

	if (round <= 4) {
		return BOTDIFF_NORMAL;
	}

	if (round <= 6) {
		return BOTDIFF_HARD;
	}

	if (round <= 8) {
		return BOTDIFF_PERFECT;
	}

	return BOTDIFF_DARK;
}

// Health regeneration: after a few seconds without being hurt, health refills.
static void zombiesTickRegen(void)
{
	struct player *player = g_Vars.players[0];

	if (player->isdead) {
		return;
	}

	if (player->bondhealth < g_ZombieLastHealth) {
		// just got hurt: wait 6 seconds before healing starts
		g_ZombieRegenDelay60 = 360;
	} else if (g_ZombieRegenDelay60 > 0) {
		g_ZombieRegenDelay60 -= g_Vars.lvupdate60;
	} else if (player->bondhealth < g_ZombieMaxHealth) {
		// heal back to full over about 5 seconds
		player->bondhealth += g_ZombieMaxHealth * 0.0035f * g_Vars.lvupdate60;

		if (player->bondhealth > g_ZombieMaxHealth) {
			player->bondhealth = g_ZombieMaxHealth;
		}
	}

	g_ZombieLastHealth = player->bondhealth;
}

void zombiesTick(void)
{
	s32 i;
	s32 alive = 0;

	// no time or score limits: the match only ends when you lose
	lvSetMpTimeLimit60(0);
	lvSetMpScoreLimit(0);
	lvSetMpTeamScoreLimit(0);

	if (g_Vars.lvframenum < 60) {
		// new match: reset state and set up teams (you vs. all bots)
		g_ZombieRound = 0;
		g_ZombieNextWaveFrame = -1;
		g_ZombieGraceUntil = 0;
		g_ZombieGameOver = false;
		g_ZombieDeathMsgShown = false;
		g_ZombieMaxHealth = g_Vars.players[0]->bondhealth;
		g_ZombieLastHealth = g_ZombieMaxHealth;
		g_ZombieRegenDelay60 = 0;
		
		// remember what the player had set up, so we can put it back afterwards
		if (!g_ZombieSaved) {
			g_ZombieSavedOptions = g_MpSetup.options;

			for (i = 0; i < g_MpNumChrs; i++) {
				g_ZombieSavedTeams[i] = g_MpAllChrConfigPtrs[i]->team;
			}

			for (i = 0; i < g_BotCount; i++) {
				if (g_MpBotChrPtrs[i] && g_MpBotChrPtrs[i]->aibot) {
					g_ZombieSavedBotType[i] = g_MpBotChrPtrs[i]->aibot->config->type;
					g_ZombieSavedBotDiff[i] = g_MpBotChrPtrs[i]->aibot->config->difficulty;
				}
			}

			g_ZombieSaved = true;
		}

		g_MpSetup.options |= MPOPTION_TEAMSENABLED;

		for (i = 0; i < g_MpNumChrs; i++) {
			struct mpchrconfig *cfg = g_MpAllChrConfigPtrs[i];

			cfg->team = (i < PLAYERCOUNT()) ? 0 : 1;
			g_MpAllChrPtrs[i]->team = 1 << cfg->team;
		}

		// zombies fight with fists only, starting at round 1 difficulty
		for (i = 0; i < g_BotCount; i++) {
			if (g_MpBotChrPtrs[i] && g_MpBotChrPtrs[i]->aibot) {
				g_MpBotChrPtrs[i]->aibot->config->type = BOTTYPE_FIST;
				g_MpBotChrPtrs[i]->aibot->config->difficulty = zombiesDifficultyForRound(1);
			}
		}

		mpCalculateTeamIsOnlyAi();
		return;
	}
	
	// the player has died: end the match once the death animation is done
	if (!g_ZombieGameOver && g_ZombieRound > 0 && g_Vars.players[0]->isdead) {
		if (g_Vars.players[0]->redbloodfinished
				&& g_Vars.players[0]->deathanimfinished
				&& g_Vars.players[0]->colourfadetimemax60 < 0) {
			g_ZombieGameOver = true;
			zombiesRestoreSettings();
			mainEndStage();
			return;
		}
	}

	zombiesTickRegen();

	if (g_ZombieRound == 0) {
		if (g_Vars.lvframenum >= 120) {
			g_ZombieRound = 1;
			g_ZombieGraceUntil = g_Vars.lvframenum + 120;
		}
		return;
	}

	// make every living zombie come after the player
	for (i = 0; i < g_BotCount; i++) {
		struct chrdata *zchr = g_MpBotChrPtrs[i];

		if (zchr && zchr->aibot && !chrIsDead(zchr)) {
			if (zchr->aibot->command != AIBOTCMD_ATTACK) {
				botApplyAttack(zchr, g_Vars.players[0]->prop);
			}

			// always know where the player is, even without line of sight
			if (!g_Vars.players[0]->isdead) {
				botSetTarget(zchr, g_Vars.players[0]->prop - g_Vars.props);
			}
		}
	}
	
	if (g_BotCount == 0 || g_Vars.lvframenum < g_ZombieGraceUntil) {
		return;
	}
	for (i = 0; i < g_BotCount; i++) {
		if (g_MpBotChrPtrs[i] && !chrIsDead(g_MpBotChrPtrs[i])) {
			alive++;
		}
	}

	if (alive > 0) {
		g_ZombieNextWaveFrame = -1;
		return;
	}

	// everyone is dead: wait, then start the next round
	if (g_ZombieNextWaveFrame < 0) {
		g_ZombieNextWaveFrame = g_Vars.lvframenum + 300;
	} else if (g_Vars.lvframenum >= g_ZombieNextWaveFrame) {
		s32 numtospawn;

		g_ZombieRound++;
		g_ZombieNextWaveFrame = -1;
		g_ZombieGraceUntil = g_Vars.lvframenum + 120;

		numtospawn = g_ZombieRound + 1;

		if (numtospawn > g_BotCount) {
			numtospawn = g_BotCount;
		}

			for (i = 0; i < numtospawn; i++) {
				struct chrdata *zchr = g_MpBotChrPtrs[i];

				if (zchr && zchr->aibot) {
					zchr->aibot->config->difficulty = zombiesDifficultyForRound(g_ZombieRound);
					botSpawn(zchr, true);
				}
			}
		}
	}

// Called when a dead bot's fade-out finishes. Return true to let it respawn.
bool zombiesCanRespawn(struct chrdata *chr)
{
	if (g_MpSetup.scenario != MPSCENARIO_ZOMBIES) {
		return true; // other game modes behave normally
	}

	return false; // zombies stay dead until a wave brings them back
}

// True while a Zombies match is running.
bool zombiesIsActive(void)
{
	return g_Vars.normmplayerisrunning && g_MpSetup.scenario == MPSCENARIO_ZOMBIES;
}

// Points: 100 per kill, plus 500 for every round survived past round 1.
// Capped because the ranking code only has 16 bits for the score.
static s32 zombiesGetPoints(struct mpchrconfig *mpchr, s32 chrnum)
{
	s32 i;
	s32 kills = 0;
	s32 points;

	for (i = 0; i < MAX_MPCHRS; i++) {
		if (i != chrnum) {
			kills += mpchr->killcounts[i];
		}
	}

	points = kills * 100;

	if (g_ZombieRound > 1) {
		points += (g_ZombieRound - 1) * 500;
	}

	if (points > 32767) {
		points = 32767;
	}

	return points;
}

// Used by the end screen and scoreboard: your "score" is your points.
void zombiesCalculatePlayerScore(struct mpchrconfig *mpchr, s32 chrnum, s32 *score, s32 *deaths)
{
	if (chrnum < PLAYERCOUNT()) {
		*score = zombiesGetPoints(mpchr, chrnum);
	} else {
		*score = 0;
	}

	*deaths = mpchr->numdeaths;
}

// Draws text with its left edge at x and its bottom edge at bottomy.
static Gfx *zombiesDrawTextBL(Gfx *gdl, char *text, s32 x, s32 bottomy, u32 colour, struct fontchar *chars, struct font *font)
{
	s32 textwidth;
	s32 textheight;
	s32 y;

	textMeasure(&textheight, &textwidth, text, chars, font, 0);

	y = bottomy - textheight;
	textwidth += x;
	textheight += y;

#if VERSION >= VERSION_NTSC_1_0
	gdl = text0f153990(gdl, x, y, textwidth, textheight);
	gdl = textRender(gdl, &x, &y, text, chars, font, colour, 0xa0, viGetWidth(), viGetHeight(), 0, 0);
#else
	gdl = text0f153858(gdl, &x, &y, &textwidth, &textheight);
	gdl = textRender(gdl, &x, &y, text, chars, font, colour, 0x88, viGetWidth(), viGetHeight(), 0, 0);
#endif

	return gdl;
}

// Draws text with its right edge at rightx and its bottom edge at bottomy.
static Gfx *zombiesDrawTextBR(Gfx *gdl, char *text, s32 rightx, s32 bottomy, u32 colour, struct fontchar *chars, struct font *font)
{
	s32 textwidth;
	s32 textheight;

	textMeasure(&textheight, &textwidth, text, chars, font, 0);

	return zombiesDrawTextBL(gdl, text, rightx - textwidth, bottomy, colour, chars, font);
}

// Compact ammo readout, e.g. "35/200", or "8|8/80" when dual wielding.
// Called from the game's own ammo HUD code in bondgun.c.
Gfx *zombiesDrawAmmo(Gfx *gdl, s32 clipl, s32 clipr, s32 reserve, s32 bottom, s32 scalex)
{
	char text[32];
	s32 rightx = (viGetViewLeft() + viGetViewWidth()) / scalex - 24;

	if (clipl >= 0 && clipr >= 0 && reserve >= 0) {
		sprintf(text, "%d|%d/%d", clipl, clipr, reserve);
	} else if (clipl >= 0 && clipr >= 0) {
		sprintf(text, "%d|%d", clipl, clipr);
	} else if (clipr >= 0 && reserve >= 0) {
		sprintf(text, "%d/%d", clipr, reserve);
	} else if (clipr >= 0) {
		sprintf(text, "%d", clipr);
	} else if (reserve >= 0) {
		sprintf(text, "%d", reserve);
	} else {
		return gdl;
	}

	return zombiesDrawTextBR(gdl, text, rightx, bottom, 0xffffffa0, g_CharsHandelGothicXs, g_FontHandelGothicXs);
}

// Draws a line of words (letters, not just digits) centred at the given spot.
static Gfx *zombiesDrawWords(Gfx *gdl, char *text, s32 centrex, s32 y, u32 colour)
{
	s32 textwidth;
	s32 textheight;
	s32 x;

	textMeasure(&textheight, &textwidth, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);

	x = centrex - textwidth / 2;
	textwidth += x;
	textheight += y;

#if VERSION >= VERSION_NTSC_1_0
	gdl = text0f153990(gdl, x, y, textwidth, textheight);
	gdl = textRender(gdl, &x, &y, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, colour, 0xa0, viGetWidth(), viGetHeight(), 0, 0);
#else
	gdl = text0f153858(gdl, &x, &y, &textwidth, &textheight);
	gdl = textRender(gdl, &x, &y, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, colour, 0x88, viGetWidth(), viGetHeight(), 0, 0);
#endif

	return gdl;
}

// Round number (red, bottom-left) and points (yellow, bottom-right, above the ammo).
Gfx *zombiesRenderHud(Gfx *gdl)
{
	s32 left;
	s32 right;
	s32 bottom;
	s32 centrex;
	char text[64];

	if (g_ZombieRound < 1) {
		return gdl;
	}

	left = viGetViewLeft() + 24;
	right = viGetViewLeft() + viGetViewWidth() - 24;
	bottom = viGetViewTop() + viGetViewHeight() - 40;
	centrex = viGetViewLeft() + viGetViewWidth() / 2;

	// round number: bottom-left, anchored to the left edge
#ifndef PLATFORM_N64
	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeL);
#endif

	gdl = text0f153628(gdl);

	sprintf(text, "%d", g_ZombieRound);
	gdl = zombiesDrawTextBL(gdl, text, left, bottom, 0xc00000a0, g_CharsHandelGothicMd, g_FontHandelGothicMd);

	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

	// points: right side, anchored to the right edge
#ifndef PLATFORM_N64
	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeR);
#endif

	gdl = text0f153628(gdl);

	sprintf(text, "%d", zombiesGetPoints(MPCHR(g_Vars.currentplayernum), g_Vars.currentplayernum));
	gdl = zombiesDrawTextBR(gdl, text, right, bottom, 0xffff00a0, g_CharsHandelGothicSm, g_FontHandelGothicSm);

	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

	// after dying, show how far you got in the middle of the screen
	if (g_Vars.currentplayer->isdead) {
#ifndef PLATFORM_N64
		gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
#endif

		gdl = text0f153628(gdl);

		sprintf(text, "YOU SURVIVED %d ROUND%s", g_ZombieRound, g_ZombieRound == 1 ? "" : "S");
		gdl = zombiesDrawWords(gdl, text, centrex, viGetViewTop() + 80, 0xffffffa0);

		gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
		gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif
	}

	return gdl;
}