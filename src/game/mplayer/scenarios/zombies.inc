// Prototype Zombies mode, hooked into Combat for now.

void scenarioCreateHudmsg(s32 playernum, char *message);
void botSpawn(struct chrdata *chr, u8 respawning);
void mainEndStage(void);
void botApplyAttack(struct chrdata *chr, struct prop *prop);
void botSetTarget(struct chrdata *chr, s32 propnum);
void chrDamageByMisc(struct chrdata *chr, f32 damage, struct coord *vector, struct gset *gset, struct prop *prop);
bool chrIsDead(struct chrdata *chr);

void lvSetMpTimeLimit60(u32 limit);
void lvSetMpScoreLimit(u32 limit);
void lvSetMpTeamScoreLimit(u32 limit);
void mpCalculateTeamIsOnlyAi(void);
extern u32 g_HudAlignModeL;
extern u32 g_HudAlignModeR;

/**
 * The buttons that use things - the box, the machine, a revive - as the
 * game's own use reads them (bondmove.c): B in the N64 control modes; in the
 * PC mode B, which only the keyboard has (E), and A, which is the
 * controller's. Reading B alone, a controller in the PC mode could not use
 * anything here.
 *
 * Unless Zombies' own Interact button (BUTTON_INTERACT, F by default, set on
 * the Zombies Options page) is bound on the device the player is using: then
 * that alone. The use button is the game's activate too, which with nothing
 * of the level's to operate jumps (and in the PC mode E is Accept as well),
 * so every buy on it jumped.
 */
// The player's Interact binding on the device they are using, NULL for none.
// Online only this machine's own player has binds here, on its first
// controller; a client's is said by its commands (zombiesUseMask()).
static const u32 *zombiesInteractBind(s32 playernum)
{
	const s32 mpindex = g_Vars.playerstats[playernum].mpindex;
	const s32 cont = g_NetMode == NETMODE_NONE ? optionsGetContpadNum1(mpindex) : netIsLocalSlot(playernum) ? 0 : -1;
	const bool keyboard = NET_LOCAL_UI(playernum) && inputLastPressWasKeyboard();
	const u32 *binds = inputKeyGetLiveBinds(cont, CK_0200);
	s32 b;

	if (binds == NULL) {
		return NULL;
	}

	for (b = 0; b < INPUT_MAX_BINDS; b++) {
		const bool pad = binds[b] >= VK_JOY_BEGIN;

		if (binds[b] && pad != keyboard) {
			return &binds[b];
		}
	}

	return NULL;
}

static u32 zombiesUseMask(s32 playernum)
{
	s32 mpindex = g_Vars.playerstats[playernum].mpindex;
	// a client's player on the host: whether its machine has Interact bound,
	// as its commands say (NETCMD_INTERACT, protocol 33)
	const s32 remote = g_NetMode == NETMODE_SERVER ? netPlayersInteractBound(playernum) : -1;

	if (remote > 0 || (remote < 0 && zombiesInteractBind(playernum))) {
		return BUTTON_INTERACT;
	}

	if (optionsGetControlMode(mpindex) == CONTROLMODE_PC) {
		return B_BUTTON | BUTTON_CANCEL_USE | BUTTON_ACCEPT_USE | BUTTON_INTERACT;
	}

	return B_BUTTON | BUTTON_INTERACT;
}

// What the prompts call that button: the key on the keyboard, the button on
// a controller
static const char *zombiesUseName(s32 playernum)
{
	static char name[32];
	s32 mpindex = g_Vars.playerstats[playernum].mpindex;
	const u32 *bind = zombiesInteractBind(playernum);

	if (bind) {
		char *p;

		snprintf(name, sizeof(name), "%s", inputGetKeyName(*bind));

		for (p = name; *p; p++) {
			if (*p == '_') {
				*p = ' ';
			}
		}

		return name;
	}

	if (NET_LOCAL_UI(playernum) && inputLastPressWasKeyboard()) {
		return "E";
	}

	return optionsGetControlMode(mpindex) == CONTROLMODE_PC ? "A" : "B";
}

// netplayers.c, each command a client sends: this machine's player has
// Interact bound where it plays (NETCMD_INTERACT); an s32, as bool is not the
// same size on both sides
s32 zombiesInteractBoundHere(void)
{
	return g_Vars.normmplayerisrunning && g_MpSetup.scenario == MPSCENARIO_ZOMBIES
		&& g_NetLocalSlot >= 0 && g_NetLocalSlot < PLAYERCOUNT() && zombiesInteractBind(g_NetLocalSlot) != NULL;
}

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
static f32 g_ZombieMaxHealth[MAX_PLAYERS];
static f32 g_ZombieLastHealth[MAX_PLAYERS];
static s32 g_ZombieRegenDelay60[MAX_PLAYERS];

/**
 * Downed, as Call of Duty's Zombies has it with more than one player: a hit
 * that would kill puts the player down instead (zombiesPlayerGoesDown(), from
 * playerDieByShooter()). Down, they squat (bondwalk.c's autocrouch), keep
 * their gun, cannot be hurt, are left alone by the zombies and lose their
 * perks. A teammate holding B (E) beside them for ZOMBIE_REVIVE60 stands them
 * up; otherwise they bleed out after ZOMBIE_BLEEDOUT60 and are dead until the
 * next round starts (zombiesRespawnRule()). The match ends when nobody is
 * left standing. Solo, a death is the end, as before.
 */
#define ZOMBIE_BLEEDOUT60     2700   // 45 seconds
#define ZOMBIE_REVIVE60       240    // 4 seconds of holding B
#define ZOMBIE_REVIVE_RANGE   110.0f
#define ZOMBIE_ALLDOWN_END60  180    // the match ends this long after the last one standing goes down

static bool g_ZombieDowned[MAX_PLAYERS];
static s32 g_ZombieBleed60[MAX_PLAYERS];      // what a downed player has left
static s32 g_ZombieRevive60[MAX_PLAYERS];     // how far a downed player's revive has got
static s32 g_ZombieReviver[MAX_PLAYERS];      // who is reviving a downed player, -1
static s32 g_ZombieNearDowned[MAX_PLAYERS];   // the downed player a standing one can revive, -1
static bool g_ZombieSavedInvincible[MAX_PLAYERS];
static bool g_ZombieRespawnDue[MAX_PLAYERS];  // dead, and the round has turned
static s32 g_ZombieAllDownFrame = -1;
static bool g_ZombieSpectating[MAX_PLAYERS];  // dead and watching a teammate (zombiesDeadCameraTick())
static s32 g_ZombieWatch[MAX_PLAYERS];        // whom, -1
static bool g_ZombieSpecFirst[MAX_PLAYERS];   // through their eyes, not over their shoulder
static bool g_ZombieSpecHave[MAX_PLAYERS];    // the follow camera's easing has somewhere to start
static struct coord g_ZombieSpecLook[MAX_PLAYERS];
static f32 g_ZombieSpecDist[MAX_PLAYERS];
static s32 g_ZombieRoundTotal = 0;
static s32 g_ZombieSpawned = 0;
static s32 g_ZombieNextSpawnFrame = 0;

// Points, as Call of Duty's Zombies gives them: 10 for every hit that does
// not kill, and the kill by how it was done. A match starts with the Zombies
// Options page's Starting Points (Call of Duty's own are 500).

// Zombies Options (port/src/zombieperks.c keeps them in pd.ini)
extern s32 g_ZombieCfgStartPoints;
extern s32 g_ZombieCfgStartRound;
extern s32 g_ZombieCfgPowerups;
extern s32 g_ZombieCfgRoundMusic;
extern s32 g_ZombieCfgGroans;
#define ZOMBIE_POINTS_HIT      10
#define ZOMBIE_POINTS_KILL     60
#define ZOMBIE_POINTS_HEADSHOT 100
#define ZOMBIE_POINTS_MELEE    130
#define ZOMBIE_POINTS_BLAST    50

static s32 g_ZombiePoints[MAX_PLAYERS];

// Online, a seat nobody plays: a host that takes joins in progress has every
// seat a player from the start, the open ones dead and hidden. Not a player
// for the HUD's rows, the rules for playing alone, or the next round's
// respawn (the host's from netPlayerOutOfPlay(), a client's from the block)
static bool g_ZombieOutOfPlay[MAX_PLAYERS];

// The players in play: alone, Quick Revive and going down are Call of Duty's
// solo rules
static s32 zombiesPlayersInPlay(void)
{
	s32 n = 0;
	s32 i;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		n += g_ZombieOutOfPlay[i] ? 0 : 1;
	}

	return n;
}

// When the current round began, for the counter fading in
static s32 g_ZombieRoundStartFrame = 0;

// The top of each player's ammo and gun name stack, so the points sit on it
static s32 g_ZombieStackTop[MAX_PLAYERS];

// The "+10"s that float off the points readout
#define ZOMBIE_MAX_POPUPS     12
#define ZOMBIE_POPUP_LIFE60   50

struct zombiepopup {
	s16 playernum;
	s16 value;
	s16 age60; // -1 when the slot is free
	s16 yoffset;
};

static struct zombiepopup g_ZombiePopups[ZOMBIE_MAX_POPUPS];

/**
 * Online (protocol 33): what the host's Zombies makes that a client's would
 * have made, kept for the scenario block (zombiesNetCapture()) - the last
 * ZOMBIE_NET_RING of each, numbered, so a client takes the ones it has not
 * had even across a lost snapshot or two.
 */
#define ZOMBIE_NET_RING 16 // a power of two: the u16 numbers wrap onto the same slots

struct zombienetpopup {
	s8 playernum;
	s16 value;
};

struct zombienetsound {
	u16 sound;
	u8 pitch;    // in 64ths
	s8 who;      // the player it is for, -1 everyone
	u8 chr;      // an mpchr slot it comes from (a groan), 0xff none
	u8 mine;     // that player's machine made it already (its knife)
};

static struct zombienetpopup g_ZombieNetPopups[ZOMBIE_NET_RING];
static u16 g_ZombieNetPopupSeq = 0;
static struct zombienetsound g_ZombieNetSounds[ZOMBIE_NET_RING];
static u16 g_ZombieNetSoundSeq = 0;
static s32 g_ZombieSoundFor = -1;      // whose the sounds being made are, -1 everyone's
static bool g_ZombieSoundMine = false; // ... and that player's machine made them too
static u32 g_ZombieNetRand = 0x5eed;   // a client's own numbers, never the match's
static s32 g_ZombieNetRemaining = 0;   // a client's "Zombies: N", the host's count

static void zombiesResetPoints(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ZombiePoints[i] = g_ZombieCfgStartPoints;
		g_ZombieStackTop[i] = -1;
	}

	for (i = 0; i < ZOMBIE_MAX_POPUPS; i++) {
		g_ZombiePopups[i].age60 = -1;
	}
}

static void zombiesAddPopup(s32 playernum, s32 value)
{
	s32 i;
	s32 oldest = 0;

	for (i = 0; i < ZOMBIE_MAX_POPUPS; i++) {
		if (g_ZombiePopups[i].age60 < 0) {
			oldest = i;
			break;
		}

		if (g_ZombiePopups[i].age60 > g_ZombiePopups[oldest].age60) {
			oldest = i;
		}
	}

	g_ZombiePopups[oldest].playernum = playernum;
	g_ZombiePopups[oldest].value = value;
	g_ZombiePopups[oldest].age60 = 0;

	if (NET_CLIENT) {
		// the host's random numbers are the match's: a client draws its own
		g_ZombieNetRand = g_ZombieNetRand * 1103515245 + 12345;
		g_ZombiePopups[oldest].yoffset = (s32)((g_ZombieNetRand >> 16) % 13) - 6;
	} else {
		g_ZombiePopups[oldest].yoffset = (s32)(rngRandom() % 13) - 6;
	}

	// for the clients, who see their own
	if (g_NetMode == NETMODE_SERVER) {
		struct zombienetpopup *p = &g_ZombieNetPopups[g_ZombieNetPopupSeq % ZOMBIE_NET_RING];

		p->playernum = (s8)playernum;
		p->value = (s16)(value < -32768 ? -32768 : value > 32767 ? 32767 : value);
		g_ZombieNetPopupSeq++;
	}
}

static void zombiesTickPopups(void)
{
	s32 i;

	for (i = 0; i < ZOMBIE_MAX_POPUPS; i++) {
		if (g_ZombiePopups[i].age60 >= 0) {
			g_ZombiePopups[i].age60 += g_Vars.lvupdate60;

			if (g_ZombiePopups[i].age60 >= ZOMBIE_POPUP_LIFE60) {
				g_ZombiePopups[i].age60 = -1;
			}
		}
	}
}

static void zombiesMaybeDrop(struct chrdata *victim);

// The power-ups that last (zombiesTickDrops()): 60ths left, 0 when not on
#define ZOMBIE_POWERUP_TIME60 1800 // 30 seconds, as Call of Duty's
static s32 g_ZombieInstaKill60 = 0;
static s32 g_ZombieDoublePoints60 = 0;

bool zombiesIsActive(void);

// chrDamage() asks: while Insta-Kill is on, a player's hit kills any zombie
bool zombiesInstaKillOn(void)
{
	return zombiesIsActive() && g_ZombieInstaKill60 > 0;
}

// Called from chrDamage() for every hit that lands on a living zombie.
void zombiesAwardHit(struct prop *aprop, s32 hitpart, bool ismelee, bool explosion, bool killed, struct chrdata *victim)
{
	s32 playernum;
	s32 value;

	// a client's points are the host's (the scenario block)
	if (!aprop || aprop->type != PROPTYPE_PLAYER || g_ZombieGameOver || NET_CLIENT) {
		return;
	}

	playernum = playermgrGetPlayerNumByProp(aprop);

	if (playernum < 0 || playernum >= MAX_PLAYERS) {
		return;
	}

	if (!killed) {
		value = ZOMBIE_POINTS_HIT;
	} else if (explosion) {
		value = ZOMBIE_POINTS_BLAST;
	} else if (ismelee) {
		value = ZOMBIE_POINTS_MELEE;
	} else if (hitpart == HITPART_HEAD) {
		value = ZOMBIE_POINTS_HEADSHOT;
	} else {
		value = ZOMBIE_POINTS_KILL;
	}

	// Double Points
	if (g_ZombieDoublePoints60 > 0) {
		value *= 2;
	}

	g_ZombiePoints[playernum] += value;
	zombiesAddPopup(playernum, value);

	// a kill may leave a power-up where the zombie fell
	if (killed && victim) {
		zombiesMaybeDrop(victim);
	}
}

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

// Health regeneration at Call of Duty's pace: 2.4 seconds after the last hit
// it starts, and refills from nothing in about a second and a quarter
#define ZOMBIE_REGEN_DELAY60 144
#define ZOMBIE_REGEN_FULL60  75

// Health regeneration: after a few seconds without being hurt, health refills.
static void zombiesTickRegen(s32 playernum)
{
	struct player *player = g_Vars.players[playernum];

	if (player->isdead || g_ZombieDowned[playernum]) {
		g_ZombieLastHealth[playernum] = player->bondhealth;
		return;
	}

	if (player->bondhealth < g_ZombieLastHealth[playernum]) {
		// just got hurt: the wait before healing starts begins again
		g_ZombieRegenDelay60[playernum] = ZOMBIE_REGEN_DELAY60;
	} else if (g_ZombieRegenDelay60[playernum] > 0) {
		g_ZombieRegenDelay60[playernum] -= g_Vars.lvupdate60;
	} else if (player->bondhealth < g_ZombieMaxHealth[playernum]) {
		player->bondhealth += g_ZombieMaxHealth[playernum] * g_Vars.lvupdate60 / ZOMBIE_REGEN_FULL60;

		if (player->bondhealth > g_ZombieMaxHealth[playernum]) {
			player->bondhealth = g_ZombieMaxHealth[playernum];
		}
	}

	g_ZombieLastHealth[playernum] = player->bondhealth;
}

#define ZOMBIE_MAX_ALIVE 24
#define ZOMBIE_ROUND_BREAK 300 // frames between clearing a round and the next one starting
#define ZOMBIE_AI_PER_PLAYER 6 // World at War's zombie_ai_per_player

// How many zombies a round sends, as World at War's _zombiemode.gsc works it
// out for Shi No Numa and the rest: 24, plus more for each player and as the
// rounds go on, with the first five rounds cut down. Solo that is 6, 8, 13,
// 18, 24, 27, 28, 28, 29, 33, 34, 36... No more than 24 are out at once
// (ZOMBIE_MAX_ALIVE); the rest come in as those die.
static s32 zombiesTotalForRound(s32 round)
{
	f64 multiplier = round / 5.0;
	s32 numplayers = PLAYERCOUNT();
	s32 max = 24;

	if (multiplier < 1) {
		multiplier = 1;
	}

	if (round >= 10) {
		multiplier *= round * 0.15;
	}

	if (numplayers <= 1) {
		max += (s32)(0.5 * ZOMBIE_AI_PER_PLAYER * multiplier);
	} else {
		max += (s32)((numplayers - 1) * ZOMBIE_AI_PER_PLAYER * multiplier);
	}

	switch (round) {
	case 1: max = (s32)(max * 0.25); break;
	case 2: max = (s32)(max * 0.3); break;
	case 3: max = (s32)(max * 0.5); break;
	case 4: max = (s32)(max * 0.7); break;
	case 5: max = (s32)(max * 0.9); break;
	}

	return max;
}

static s32 zombiesCountAlive(void)
{
	s32 i;
	s32 alive = 0;

	for (i = 0; i < g_BotCount; i++) {
		if (g_MpBotChrPtrs[i] && !chrIsDead(g_MpBotChrPtrs[i])) {
			alive++;
		}
	}

	return alive;
}

// Frames between zombie spawns: a slow trickle early on, as Call of Duty's
// first rounds are, then quicker: 4 seconds in round 1, 10% quicker each
// round (about 1.5s by round 10), never under half a second.
#define ZOMBIE_SPAWN_DELAY_FIRST 240
#define ZOMBIE_SPAWN_DELAY_MIN   30

static s32 zombiesSpawnDelay(s32 round)
{
	f32 delay = ZOMBIE_SPAWN_DELAY_FIRST;
	s32 i;

	for (i = 1; i < round; i++) {
		delay *= 0.9f;
	}

	if (delay < ZOMBIE_SPAWN_DELAY_MIN) {
		delay = ZOMBIE_SPAWN_DELAY_MIN;
	}

	return (s32)delay;
}

/**
 * A zombie's health, as World at War and Black Ops give it: 150 in round 1,
 * 100 more a round up to round 9 (950), then a tenth more each round (1045 in
 * round 10, about 2700 by 20). Each zombie keeps what its round gave it when
 * it came in.
 *
 * In Perfect Dark's units a round 1 zombie has a stock simulant's health
 * (ZOMBIE_PD_HEALTH), so the guns feel as they always have there and it is
 * the rounds that make it harder. A player's melee hit is Call of Duty's
 * knife, 150: one in round 1, two in round 2 (zombiesMeleeDamage()).
 */
#define ZOMBIE_PD_HEALTH  8.0f  // a simulant's maxdamage, its script's own
#define ZOMBIE_COD_HEALTH 150   // what that is in Call of Duty's
#define ZOMBIE_COD_KNIFE  150   // a knife's (or a fist's) hit, in Call of Duty's

static f32 g_ZombieHealth[MAX_BOTS]; // each zombie's maxdamage, 0 for none yet

static s32 zombiesCodHealthForRound(s32 round)
{
	s32 health = ZOMBIE_COD_HEALTH;
	s32 i;

	for (i = 2; i <= round; i++) {
		if (i <= 9) {
			health += 100;
		} else {
			health += health / 10;
		}

		// far past where anyone gets to, and short of overflowing
		if (health > 10000000) {
			break;
		}
	}

	return health;
}

static f32 zombiesHealthForRound(s32 round)
{
	return ZOMBIE_PD_HEALTH * zombiesCodHealthForRound(round) / ZOMBIE_COD_HEALTH;
}

// chrDamage() asks: a player's melee hit on a zombie, whatever the angle
f32 zombiesMeleeDamage(void)
{
	return ZOMBIE_PD_HEALTH * ZOMBIE_COD_KNIFE / ZOMBIE_COD_HEALTH;
}

/**
 * How fast the zombies move, as a multiple of a simulant's run: walkers in the
 * first rounds, as Call of Duty's are, quickening to a full run by round 5.
 * botGetStatScale() asks in place of the speed slider.
 */
f32 zombiesSpeedScale(void)
{
	switch (g_ZombieRound) {
	case 0:
	case 1: return 0.55f;
	case 2: return 0.65f;
	case 3: return 0.8f;
	case 4: return 0.9f;
	}

	return 1.0f;
}

// Sends one finished-dying zombie back in. Returns true if one went.
static bool zombiesSpawnOne(void)
{
	s32 i;

	for (i = 0; i < g_BotCount; i++) {
		struct chrdata *zchr = g_MpBotChrPtrs[i];

		if (zchr && zchr->aibot && zchr->actiontype == ACT_DEAD
				&& zchr->act_dead.fadetimer60 >= TICKS(90)) {
			zchr->aibot->config->difficulty = zombiesDifficultyForRound(g_ZombieRound);
			botSpawn(zchr, true);
			g_ZombieHealth[i] = zombiesHealthForRound(g_ZombieRound);
			zchr->maxdamage = g_ZombieHealth[i];
			g_ZombieSpawned++;
			return true;
		}
	}

	return false;
}

// Called by aiMpInitSimulants() straight after botSpawnAll() has stood every
// simulant up for the first time. The horde starts down and trickles in
// (zombiesSpawnOne()), so they are all put down here.
//
// It has to be after that first spawn, not before it: botSpawn(chr, false)
// stands a chr up without clearing chr->damage, and a simulant whose damage
// is already past its maxdamage skips all of chrDamage()'s damage - killed
// on frame 1, stood up on frame 2, it could never be killed again. A respawn
// (botSpawn(chr, true), zombiesSpawnOne()) clears it.
void zombiesPutDownHorde(void)
{
	s32 i;

	for (i = 0; i < g_BotCount; i++) {
		struct chrdata *zchr = g_MpBotChrPtrs[i];

		if (zchr && zchr->aibot && !chrIsDead(zchr)) {
			struct coord zdir = {0, 0, 0};

			chrDamageByMisc(zchr, 1000, &zdir, NULL, NULL);
		}
	}
}

u32 bodyGetRace(s32 bodynum);

/**
 * A zombie's head that goes with its body. mpCreateBotFromProfile() deals any
 * head in the list onto the profile's one body, and on some pairs the head
 * sits sunk into the collar or askew - a head on a body it was not made for
 * (chrs-and-memory.md, "Heads fitted to bodies by measurement"). A body with
 * a head of its own wears that one; a body that takes any head (a guard's,
 * headnum 1000) keeps a head of its own sex, as stock deals them. A Perfect
 * Head is left alone.
 */
static void zombiesPairHead(s32 botnum)
{
	struct mpchrconfig *base = &g_BotConfigsArray[botnum].base;
	s32 mpbodynum = base->mpbodynum;
	s32 ownhead;
	s32 headnum;
	s32 mpheadnum;

	if (mpbodynum < 0 || mpbodynum >= g_MpListCounts.bodies || base->mpheadnum >= mpGetNumHeads2()) {
		return;
	}

	ownhead = g_MpBodies[mpbodynum].headnum;
	headnum = g_MpHeads[base->mpheadnum].headnum;

	if (ownhead == 1000) {
		if (g_HeadsAndBodies[headnum].ismale == g_HeadsAndBodies[g_MpBodies[mpbodynum].bodynum].ismale) {
			return;
		}
	} else if (headnum == ownhead) {
		return;
	}

	mpheadnum = mpGetMpheadnumByMpbodynum(mpbodynum);

	// it answers 0 for a head the list does not carry: keep what there was
	if (mpheadnum < 0 || mpheadnum >= g_MpListCounts.heads
			|| (ownhead != 1000 && g_MpHeads[mpheadnum].headnum != ownhead)) {
		return;
	}

	base->mpheadnum = mpheadnum;
}

// A new zombie's look: any human body the player has unlocked, so the horde
// is not one body 24 times over, wearing the head that goes with it
static void zombiesChooseLook(s32 botnum)
{
	s32 tries;

	for (tries = 0; tries < 40; tries++) {
		s32 mpbodynum = rngRandom() % g_MpListCounts.bodies;

		if (challengeIsFeatureUnlocked(g_MpBodies[mpbodynum].requirefeature)
				&& bodyGetRace(g_MpBodies[mpbodynum].bodynum) == RACE_HUMAN) {
			g_BotConfigsArray[botnum].base.mpbodynum = mpbodynum;
			break;
		}
	}

	zombiesPairHead(botnum);
}

// Called by mpStartMatch(): tops the simulants up to the horde's pool, so a
// Zombies match needs no simulants set up first. They are what the waves are
// made of; zombiesTick() turns them into fist-fighting zombies.
void zombiesAddSimulants(void)
{
	s32 i;
	s32 count = 0;
	s32 profile = mpFindBotProfile(BOTTYPE_GENERAL, BOTDIFF_EASY);

	if (profile < 0) {
		profile = 0;
	}

	for (i = 0; i < mpGetSimSlotCap(); i++) {
		if (mpIsSimSlotOn(i)) {
			count++;
		}
	}

	while (count < ZOMBIE_MAX_ALIVE && mpHasUnusedBotSlots()) {
		s32 botnum = mpGetSlotForNewBot();

		if (botnum < 0) {
			break;
		}

		mpCreateBotFromProfile(botnum, profile);
		zombiesChooseLook(botnum);
		count++;
	}

	// and the ones a setup already had, made before this check was: a head
	// that does not go with its body is swapped for one that does
	for (i = 0; i < mpGetSimSlotCap(); i++) {
		if (mpIsSimSlotOn(i)) {
			zombiesPairHead(i);
		}
	}

	mpGenerateBotNames();
}

/**
 * Juggernog.
 *
 * A machine stands at one of the arena's spawn pads (the one furthest from
 * where the player started, taken off the list so nobody spawns inside it).
 * Standing at it with the points and holding B buys the perk: the gun goes
 * down, a bottle comes up and is drunk (drawn on the HUD, zombiesRenderDrink()),
 * the gun comes back up, and from then on the player takes 1/2.5 of the
 * damage - Call of Duty's 250 health where there was 100.
 *
 * The machine is a placeholder: ZOMBIE_JUGG_MODEL is a Perfect Dark prop until
 * there is a model of its own.
 */
#define ZOMBIE_JUGG_MODEL      MODEL_G5_MAINFRAME
// The model's extrascale, 256 being its own size: the mainframe is 2334 units
// tall as it is (1000 square), and a player about 200, so this makes it a
// machine 210 tall and 90 across
#define ZOMBIE_JUGG_SCALE      23
#define ZOMBIE_JUGG_COST       2500
#define ZOMBIE_JUGG_HEALTH     2.5f   // healthscale: chrDamage() divides by it
#define ZOMBIE_JUGG_RANGE      170.0f // how near the machine buying works
#define ZOMBIE_JUGG_HOLD60     15     // how long B is held to buy

// The drink, in 60ths of a second, one stage after another
#define DRINK_GUNDOWN    18 // the gun goes down
#define DRINK_BOTTLEUP   16 // the bottle comes up
#define DRINK_GULP       98 // it is drunk: with the two either side, Black Ops II's drink (65 frames at 30)
#define DRINK_BOTTLEDOWN 16 // it goes away
#define DRINK_GUNUP      18 // the gun comes back
#define DRINK_TOTAL      (DRINK_GUNDOWN + DRINK_BOTTLEUP + DRINK_GULP + DRINK_BOTTLEDOWN + DRINK_GUNUP)
#define DRINK_GULPSTART  (DRINK_GUNDOWN + DRINK_BOTTLEUP)
#define DRINK_GULPEND    (DRINK_GULPSTART + DRINK_GULP)

#define ZOMBIE_JUGG_FRONT      70.0f  // how far in front of a marked stand the machine goes

/**
 * Each arena's machine, written down with --zombies-mark (zombiesMarkSpot()):
 * where a player stands to buy, the way they look at the machine, and the room
 * they stand in. An arena with no line here gets a spawn pad instead
 * (zombiesPlaceJuggernogAtSpawn()). Keyed by stage number, so a mod arena's
 * line is only good for the mods it was marked with.
 */
struct zombiespot {
	s32 stagenum;
	f32 x;
	f32 y;
	f32 z;
	f32 lookx;
	f32 lookz;
	s32 room;
};

static const struct zombiespot g_ZombieJuggSpots[] = {
	// paste zombie-spots.txt's lines here
	{ 0 },
};

static const struct zombiespot *zombiesFindJuggSpot(s32 stagenum);

// The machine's object. A few in turn, because a moved one (zombiesMarkSpot())
// is still being taken away while the next goes up.
static struct defaultobj g_ZombieJuggObjs[4];
static s32 g_ZombieJuggNext = 0;
static struct defaultobj *g_ZombieJugg = NULL;
static bool g_ZombieMarking = false;

static bool g_ZombieJuggPlaced = false;
static bool g_ZombieHasJugg[MAX_PLAYERS];
static s32 g_ZombieDrink60[MAX_PLAYERS];    // -1 when not drinking
static s32 g_ZombieUseHeld60[MAX_PLAYERS];
static bool g_ZombieUseNeedsRelease[MAX_PLAYERS];
static bool g_ZombieNearJugg[MAX_PLAYERS];
static s32 g_ZombieLastFrame = 0x7fffffff;

// Online, the host keeps it for the clients (zombiesNetCapture()); chr is the
// mpchr slot a sound comes from, -1 for none
static void zombiesNetKeepSound(s32 sound, f32 pitch, s32 chr)
{
	struct zombienetsound *s;
	s32 p = (s32)(pitch * 64 + 0.5f);

	if (g_NetMode != NETMODE_SERVER) {
		return;
	}

	s = &g_ZombieNetSounds[g_ZombieNetSoundSeq % ZOMBIE_NET_RING];
	s->sound = (u16)sound;
	s->pitch = (u8)(p < 1 ? 1 : p > 255 ? 255 : p);
	s->who = (s8)g_ZombieSoundFor;
	s->chr = chr >= 0 && chr < 0xff ? (u8)chr : 0xff;
	s->mine = g_ZombieSoundMine;
	g_ZombieNetSoundSeq++;
}

static void zombiesPlaySoundHere(s32 sound, f32 pitch)
{
	struct sndstate *handle = sndStart(var80095200, sound, NULL, -1, -1, -1, -1, -1);

	if (handle && pitch != 1.0f) {
		audioPostEvent(handle, AL_SNDP_PITCH_EVT, *(s32 *)&pitch);
	}
}

// Heard by everyone on this machine; online, a sound of one player's (its
// buy, its drink) only on that player's machine
static void zombiesPlaySound(s32 sound, f32 pitch)
{
	zombiesNetKeepSound(sound, pitch, -1);

	if (g_NetMode != NETMODE_NONE && g_ZombieSoundFor >= 0 && !netIsLocalSlot(g_ZombieSoundFor)) {
		return;
	}

	zombiesPlaySoundHere(sound, pitch);
}

static void zombiesResetJuggernog(void)
{
	s32 i;

	g_ZombieJuggPlaced = false;
	g_ZombieJugg = NULL;
	g_ZombieMarking = sysArgCheck("--zombies-mark");

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ZombieHasJugg[i] = false;
		g_ZombieDrink60[i] = -1;
		g_ZombieUseHeld60[i] = 0;
		g_ZombieUseNeedsRelease[i] = false;
		g_ZombieNearJugg[i] = false;
	}
}

static s32 zombiesMachineExtrascale(s32 id);

// The next object of the pool, filled in as the machine
static struct defaultobj *zombiesNewJuggObj(void)
{
	struct defaultobj *obj = &g_ZombieJuggObjs[g_ZombieJuggNext];

	g_ZombieJuggNext = (g_ZombieJuggNext + 1) % ARRAYCOUNT(g_ZombieJuggObjs);

	bzero(obj, sizeof(*obj));
	obj->extrascale = zombiesMachineExtrascale(0); // ZOMBIEPERK_JUGGERNOG, defined below
	obj->type = OBJTYPE_BASIC;
	obj->modelnum = ZOMBIE_JUGG_MODEL;
	// OBJFLAG_00000100: a solid block round the model's bounds (objInit(),
	// func0f069850()), as the level's own solid props have from their setup
	obj->flags = OBJFLAG_INVINCIBLE | OBJFLAG_00000100;
	obj->flags2 = OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_IMMUNETOEXPLOSIONS;
	obj->realrot[0][0] = obj->realrot[1][1] = obj->realrot[2][2] = 1;
	obj->maxdamage = 1000;
	obj->shadecol[0] = obj->shadecol[1] = obj->shadecol[2] = 0xff;
	obj->nextcol[0] = obj->nextcol[1] = obj->nextcol[2] = 0xff;
	obj->floorcol = 0x0fff;

	g_ZombieJugg = obj;

	return obj;
}

// The spawn pad an arena's machine stands on when it has no spot of its own,
// -1 for none
static s32 g_ZombieJuggPad = -1;

// The Mystery Box's pad (zombiesReserveJuggPad() takes it too), -1 for none,
// and the box itself once it is up (zombiesPlaceBox())
static s32 g_ZombieBoxPad = -1;
static struct defaultobj g_ZombieBoxObj;
static struct defaultobj *g_ZombieBox = NULL;

/**
 * Called from playerReset() once the arena's spawn list is built and before
 * anyone is put on it: picks the machine's pad and takes it off the list, so
 * nobody spawns inside the machine. The pick is the arena's alone - the spawn
 * pad furthest from the middle of all of them, in plan - so the machine is in
 * the same place every match, wherever the player starts. playerReset() runs
 * for each player and rebuilds the list each time; the same pad comes off.
 */
/**
 * The Mystery Box's pad, off the list as the machine's is: the one nearest the
 * middle of them all, where the players are, rather than out on the edge of
 * the arena (the furthest from the machine put it on a ledge at the end of
 * the map) - and not right beside the machine, while there is a pad that is
 * not. Only with three left after it, so the zombies still have somewhere to
 * come in. zombiesPlaceBox() then backs it onto the nearest wall.
 */
#define ZOMBIE_BOX_JUGG_APART 800.0f

static void zombiesReserveBoxPad(void)
{
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	struct coord mid = {0, 0, 0};
	struct coord jugg = {0, 0, 0};
	bool havejugg = false;
	f32 bestscore = 0;
	s32 best = -1;
	s32 i;

	g_ZombieBoxPad = -1;

	if (n < 4) {
		return;
	}

	if (g_ZombieJuggPad >= 0) {
		struct pad pad;

		padUnpack(g_ZombieJuggPad, PADFIELD_POS, &pad);
		jugg = pad.pos;
		havejugg = true;
	}

	for (i = 0; i < n; i++) {
		struct pad pad;

		padUnpack(g_SpawnPoints[i], PADFIELD_POS, &pad);
		mid.x += pad.pos.x / n;
		mid.y += pad.pos.y / n;
		mid.z += pad.pos.z / n;
	}

	for (i = 0; i < n; i++) {
		struct pad pad;
		f32 dx;
		f32 dy;
		f32 dz;
		f32 score;

		padUnpack(g_SpawnPoints[i], PADFIELD_POS, &pad);

		dx = pad.pos.x - mid.x;
		dy = pad.pos.y - mid.y;
		dz = pad.pos.z - mid.z;
		score = dx * dx + dy * dy * 4 + dz * dz;

		// beside the machine only if there is nowhere else
		if (havejugg) {
			f32 jx = pad.pos.x - jugg.x;
			f32 jz = pad.pos.z - jugg.z;

			if (jx * jx + jz * jz < ZOMBIE_BOX_JUGG_APART * ZOMBIE_BOX_JUGG_APART) {
				score += 1e12f;
			}
		}

		// ties to the lower pad number, so the pick never depends on the order
		if (best < 0 || score < bestscore
				|| (score == bestscore && g_SpawnPoints[i] < g_SpawnPoints[best])) {
			bestscore = score;
			best = i;
		}
	}

	if (best < 0) {
		return;
	}

	g_ZombieBoxPad = g_SpawnPoints[best];

	g_SpawnPoints[best] = g_SpawnPoints[n - 1];
	g_NumSpawnPoints = n - 1;
}

void zombiesReserveJuggPad(void)
{
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	struct coord mid = {0, 0, 0};
	f32 bestdist = -1;
	s32 best = -1;
	s32 i;

	g_ZombieJuggPad = -1;

	// with fewer, losing one would crowd the zombies coming in; an arena with
	// a spot of its own needs no pad
	if (n < 3 || zombiesFindJuggSpot(g_Vars.stagenum)) {
		zombiesReserveBoxPad();
		return;
	}

	for (i = 0; i < n; i++) {
		struct pad pad;

		padUnpack(g_SpawnPoints[i], PADFIELD_POS, &pad);
		mid.x += pad.pos.x / n;
		mid.z += pad.pos.z / n;
	}

	for (i = 0; i < n; i++) {
		struct pad pad;
		f32 dx;
		f32 dz;

		padUnpack(g_SpawnPoints[i], PADFIELD_POS, &pad);

		dx = pad.pos.x - mid.x;
		dz = pad.pos.z - mid.z;

		// ties to the lower pad number, so the pick never depends on the order
		if (dx * dx + dz * dz > bestdist
				|| (dx * dx + dz * dz == bestdist && g_SpawnPoints[i] < g_SpawnPoints[best])) {
			bestdist = dx * dx + dz * dz;
			best = i;
		}
	}

	if (best >= 0) {
		g_ZombieJuggPad = g_SpawnPoints[best];

		g_SpawnPoints[best] = g_SpawnPoints[n - 1];
		g_NumSpawnPoints = n - 1;
	}

	zombiesReserveBoxPad();
}

// The fallback: the pad zombiesReserveJuggPad() kept for it
static void zombiesPlaceJuggernogAtSpawn(void)
{
	struct defaultobj *obj;

	if (g_ZombieJuggPad < 0) {
		return;
	}

	obj = zombiesNewJuggObj();
	obj->pad = g_ZombieJuggPad;

	setupCreateObject(obj, 999);

	obj->hidden2 &= ~OBJH2FLAG_CANREGEN;
}

// Puts the machine in front of where a player would stand to buy, facing them:
// spot is that stand, as zombiesMarkSpot() wrote it down.
static void zombiesPlaceJuggernogAtSpot(const struct zombiespot *spot)
{
	struct defaultobj *obj;
	struct prop *prop;
	struct coord stand;
	struct coord pos;
	RoomNum standrooms[2];
	RoomNum rooms[8];
	Mtxf mtx;
	f32 len = sqrtf(spot->lookx * spot->lookx + spot->lookz * spot->lookz);
	f32 lookx;
	f32 lookz;

	if (len < 0.001f || spot->room <= 0 || spot->room >= g_Vars.roomcount) {
		return;
	}

	lookx = spot->lookx / len;
	lookz = spot->lookz / len;

	stand.x = spot->x;
	stand.y = spot->y;
	stand.z = spot->z;
	standrooms[0] = spot->room;
	standrooms[1] = -1;

	pos.x = stand.x + lookx * ZOMBIE_JUGG_FRONT;
	pos.y = stand.y;
	pos.z = stand.z + lookz * ZOMBIE_JUGG_FRONT;

	// the rooms the machine is in, walked to from the stand's
	func0f065e74(&stand, standrooms, &pos, rooms);

	if (rooms[0] <= 0) {
		return;
	}

	obj = zombiesNewJuggObj();
	obj->pad = -1;

	setupLoadModeldef(obj->modelnum);
	prop = objInitWithAutoModel(obj);

	if (prop == NULL || obj->model == NULL) {
		g_ZombieJugg = NULL;
		return;
	}

	modelSetScale(obj->model, obj->model->scale * (obj->extrascale * (1.0f / 256.0f)));

	// facing back the way the player looked, as setupCreateObject() faces a
	// pad's object along the pad's look
	mtx00016d58(&mtx, 0, 0, 0, lookx, 0, lookz, 0, 1, 0);
	mtx00015f04(obj->model->scale, &mtx);

	// set down on the floor under pos, as an object at a pad is
	func0f06a730(obj, &pos, &mtx, rooms, &pos);

	propActivate(prop);
	propEnable(prop);

	obj->hidden2 &= ~OBJH2FLAG_CANREGEN;
}

static const struct zombiespot *zombiesFindJuggSpot(s32 stagenum)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_ZombieJuggSpots); i++) {
		if (g_ZombieJuggSpots[i].stagenum == stagenum && g_ZombieJuggSpots[i].room > 0) {
			return &g_ZombieJuggSpots[i];
		}
	}

	return NULL;
}

// The arena's own spot if it has one, else a spawn pad
static void zombiesPlaceJuggernog(void)
{
	const struct zombiespot *spot = zombiesFindJuggSpot(g_Vars.stagenum);

	if (spot) {
		zombiesPlaceJuggernogAtSpot(spot);
	}

	if (!g_ZombieJugg || !g_ZombieJugg->prop) {
		zombiesPlaceJuggernogAtSpawn();
	}

	if (g_ZombieJugg && g_ZombieJugg->prop) {
		sysLogPrintf(LOG_NOTE, "zombies: Juggernog on stage 0x%02x at %.0f %.0f %.0f (%s)", g_Vars.stagenum,
				g_ZombieJugg->prop->pos.x, g_ZombieJugg->prop->pos.y, g_ZombieJugg->prop->pos.z,
				spot ? "its spot" : "a spawn pad");
	} else {
		sysLogPrintf(LOG_WARNING, "zombies: no Juggernog on stage 0x%02x (%d spawn pads)", g_Vars.stagenum, g_NumSpawnPoints);
	}
}

/**
 * The other perks, beside Juggernog: Call of Duty's Quick Revive, Speed Cola,
 * Double Tap and Stamin-Up, at its prices. Each has a machine of its own -
 * Juggernog's placeholder lit in the perk's colour - backed onto the wall
 * nearest a spawn pad, spread over the arena the way the wall buys are, and
 * far enough off the pad that a zombie can still come in on it (the pad stays
 * on the list). Held B (E) at one buys it and drinks it as Juggernog is drunk
 * (the bottle in the perk's colour); going down loses every perk.
 *
 * - Quick Revive (500 alone, 1500 with others): alone, going down is not the
 *   end - the player is back up by themselves after ZOMBIE_QR_SELF60, and it
 *   can be bought ZOMBIE_QR_SOLO_BUYS times a match, as Call of Duty's solo
 *   one can. With others, a player who has it revives in half the time.
 * - Speed Cola (3000): reloads twice as fast (zombiesHandRate(), bondgun.c).
 * - Double Tap (2000): fires a third faster, and every bullet counts twice,
 *   as Black Ops 2's Double Tap II (zombiesHandRate(), zombiesBulletScale()).
 * - Stamin-Up (2000): moves 15% faster (zombiesMoveScale(), bondwalk.c).
 * - Mule Kick (4000): a third gun (zombiesGunLimit()). Going down takes the
 *   third gun with it; drinking Mule Kick again gives that gun back.
 */
#define ZOMBIEPERK_JUGGERNOG   0
#define ZOMBIEPERK_QUICKREVIVE 1
#define ZOMBIEPERK_SPEEDCOLA   2
#define ZOMBIEPERK_DOUBLETAP   3
#define ZOMBIEPERK_STAMINUP    4
#define ZOMBIEPERK_MULEKICK    5
#define ZOMBIEPERK_COUNT       6   // zombieperks.c's pictures are in this order

#define ZOMBIE_PERK_WALLREACH   400.0f // how far from its pad a machine's wall may be (200 left Skedar three short)
#define ZOMBIE_PERK_PADCLEAR    140.0f // and how far at least, so the pad stays clear
#define ZOMBIE_PERK_HALFDEPTH   45.0f  // the machine's, at ZOMBIE_JUGG_SCALE
#define ZOMBIE_QR_SOLO_COST     500
#define ZOMBIE_QR_SOLO_BUYS     3
#define ZOMBIE_QR_SELF60        480    // 8 seconds down, then up again
#define ZOMBIE_SPEEDCOLA_RATE   2.0f
#define ZOMBIE_DOUBLETAP_RATE   1.33f
#define ZOMBIE_DOUBLETAP_DAMAGE 2.0f
#define ZOMBIE_STAMINUP_SPEED   1.15f
#define ZOMBIE_DOWNED_SPEED     0.35f  // a downed player's crawl, of their squatting pace

struct zombieperkdef {
	char *name;
	s32 cost;
	u8 colour[3];   // the machine lit, the drink in the bottle, the drawn badge
	u32 radar;
};

static struct zombieperkdef g_ZombiePerks[ZOMBIEPERK_COUNT] = {
	{ "Juggernog",    ZOMBIE_JUGG_COST, { 0xd0, 0x20, 0x20 }, 0xff800000 },
	{ "Quick Revive", 1500,             { 0x30, 0x90, 0xff }, 0x4090ff00 },
	{ "Speed Cola",   3000,             { 0x30, 0xd0, 0x40 }, 0x30d04000 },
	{ "Double Tap",   2000,             { 0xff, 0x90, 0x20 }, 0xff902000 },
	{ "Stamin-Up",    2000,             { 0xf0, 0xd0, 0x30 }, 0xf0d03000 },
	{ "Mule Kick",    4000,             { 0x70, 0x98, 0x48 }, 0x70984800 },
};

// Juggernog's is g_ZombieHasJugg; the rest here
static bool g_ZombieHasPerk[MAX_PLAYERS][ZOMBIEPERK_COUNT];
static s32 g_ZombieDrinkPerk[MAX_PLAYERS];     // the perk being drunk
static struct defaultobj g_ZombiePerkObjs[ZOMBIEPERK_COUNT];
static bool g_ZombiePerkPlaced[ZOMBIEPERK_COUNT];
static s32 g_ZombieNearPerk[MAX_PLAYERS];      // the machine (not Juggernog's) a player is at, -1 for none
static s32 g_ZombiePerkHeld60[MAX_PLAYERS];
static bool g_ZombiePerkNeedsRelease[MAX_PLAYERS];
static s32 g_ZombieQrSoloBuys = 0;
static s32 g_ZombieSelfRevive60[MAX_PLAYERS];  // Quick Revive alone: 60ths until up again, 0 for none

// The perks in the order they were drunk, as the HUD's row shows them: Call of
// Duty's line up as bought and never move. Going down clears it.
static s32 g_ZombiePerkOrder[MAX_PLAYERS][ZOMBIEPERK_COUNT];
static s32 g_ZombiePerkOrderCount[MAX_PLAYERS];

// Pack-a-Punch's machine (zombiesPlacePap()); a machine id past the perks', for
// the corner code, which places every machine alike
#define ZOMBIE_PAP_ID ZOMBIEPERK_COUNT
static struct defaultobj g_ZombiePapObj;
static bool g_ZombiePapPlaced;

// A machine by id, Pack-a-Punch's after the perks', the box after that: the
// ones any machine keeps clear of
#define ZOMBIE_MACHINE_IDS (ZOMBIEPERK_COUNT + 2)

static bool zombiesFindWall(struct coord *pos, RoomNum *rooms, f32 reach, f32 *dist, struct coord *dir);
static void zombiesLoseMuleGun(s32 playernum);
static void zombiesReturnMuleGun(s32 playernum);
static bool zombiesPlayerIsUp(s32 playernum);
static bool zombiesHoldingUse(s32 playernum);
bool zombiesIsActive(void);

bool zombiesPlayerHasPerk(s32 playernum, s32 perk)
{
	if (playernum < 0 || playernum >= MAX_PLAYERS || perk < 0 || perk >= ZOMBIEPERK_COUNT) {
		return false;
	}

	return perk == ZOMBIEPERK_JUGGERNOG ? g_ZombieHasJugg[playernum] : g_ZombieHasPerk[playernum][perk];
}

static void zombiesResetPerks(void)
{
	s32 i;
	s32 k;

	// a new stage: the last one's props went with it
	bzero(g_ZombiePerkObjs, sizeof(g_ZombiePerkObjs));
	g_ZombieQrSoloBuys = 0;

	for (k = 0; k < ZOMBIEPERK_COUNT; k++) {
		g_ZombiePerkPlaced[k] = false;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		for (k = 0; k < ZOMBIEPERK_COUNT; k++) {
			g_ZombieHasPerk[i][k] = false;
		}

		g_ZombieDrinkPerk[i] = ZOMBIEPERK_JUGGERNOG;
		g_ZombieNearPerk[i] = -1;
		g_ZombiePerkHeld60[i] = 0;
		g_ZombiePerkNeedsRelease[i] = false;
		g_ZombieSelfRevive60[i] = 0;
		g_ZombiePerkOrderCount[i] = 0;
	}
}

// What a perk costs this match: Quick Revive is cheap alone
static s32 zombiesPerkCost(s32 perk)
{
	if (perk == ZOMBIEPERK_QUICKREVIVE && zombiesPlayersInPlay() < 2) {
		return ZOMBIE_QR_SOLO_COST;
	}

	return g_ZombiePerks[perk].cost;
}

// Each player's perks off; Juggernog's health with it (going down)
static void zombiesLosePerks(s32 playernum)
{
	s32 k;

	for (k = 0; k < ZOMBIEPERK_COUNT; k++) {
		g_ZombieHasPerk[playernum][k] = false;
	}

	g_ZombiePerkOrderCount[playernum] = 0;

	if (g_ZombieHasJugg[playernum]) {
		g_ZombieHasJugg[playernum] = false;
		g_Vars.players[playernum]->healthscale = 1;
	}
}

/**
 * Walls as they are, not as 16 rays round a point see them. A wall found by
 * the nearest of 16 directions is only known to 22.5 degrees, and what was put
 * against it - a wall buy's chalk and gun, a machine's back - stood that far
 * off it, slanted. Here a wall is measured: rays either side of the one that
 * met it, each to where it stops to half a unit, give two points on it, and
 * the line through them is the wall.
 */

// How far along (dx, dz) from eye the first wall is, to half a unit; -1 for
// none within reach
static f32 zombiesRayToWall(struct coord *eye, RoomNum *rooms, f32 dx, f32 dz, f32 reach)
{
	struct coord to;
	f32 lo;
	f32 hi;
	f32 d;

	to.y = eye->y;

	for (d = 15; d <= reach; d += 15) {
		to.x = eye->x + dx * d;
		to.z = eye->z + dz * d;

		if (!cdTestLos04(eye, rooms, &to, CDTYPE_BG)) {
			break;
		}
	}

	if (d > reach) {
		return -1;
	}

	lo = d - 15;
	hi = d;

	while (hi - lo > 0.5f) {
		f32 mid = (lo + hi) * 0.5f;

		to.x = eye->x + dx * mid;
		to.z = eye->z + dz * mid;

		if (cdTestLos04(eye, rooms, &to, CDTYPE_BG)) {
			lo = mid;
		} else {
			hi = mid;
		}
	}

	return hi;
}

/**
 * The wall a ray at ang (direction sin, cos) from eye meets, as a line: its
 * normal, from the room into the wall, and how far it is from eye along it.
 * False when the rays either side meet something else - a corner, a door's
 * frame - and the three points are not on one line.
 */
static bool zombiesWallLine(struct coord *eye, RoomNum *rooms, f32 ang, f32 reach, struct coord *normal, f32 *dist)
{
	static const f32 spreads[] = { 0.25f, 0.12f, 0.06f };
	s32 s;

	for (s = 0; s < ARRAYCOUNT(spreads); s++) {
		f32 a0 = ang - spreads[s];
		f32 a1 = ang + spreads[s];
		f32 d0 = zombiesRayToWall(eye, rooms, sinf(a0), cosf(a0), reach * 1.5f);
		f32 d1 = zombiesRayToWall(eye, rooms, sinf(a1), cosf(a1), reach * 1.5f);
		f32 dm = zombiesRayToWall(eye, rooms, sinf(ang), cosf(ang), reach * 1.5f);
		f32 p0x, p0z, p1x, p1z, pmx, pmz;
		f32 tx, tz, len, nx, nz;

		if (d0 < 0 || d1 < 0 || dm < 0) {
			continue;
		}

		p0x = eye->x + sinf(a0) * d0;
		p0z = eye->z + cosf(a0) * d0;
		p1x = eye->x + sinf(a1) * d1;
		p1z = eye->z + cosf(a1) * d1;
		pmx = eye->x + sinf(ang) * dm;
		pmz = eye->z + cosf(ang) * dm;

		tx = p1x - p0x;
		tz = p1z - p0z;
		len = sqrtf(tx * tx + tz * tz);

		if (len < 1) {
			continue;
		}

		nx = tz / len;
		nz = -tx / len;

		if (nx * sinf(ang) + nz * cosf(ang) < 0) {
			nx = -nx;
			nz = -nz;
		}

		// the middle point on the same line, or it is not one wall
		if (fabsf((pmx - p0x) * nx + (pmz - p0z) * nz) > 2) {
			continue;
		}

		normal->x = nx;
		normal->y = 0;
		normal->z = nz;
		*dist = (p0x - eye->x) * nx + (p0z - eye->z) * nz;

		return true;
	}

	return false;
}

/**
 * The inside corners round pos within reach, nearest first: two walls meeting
 * near square, the corner itself where their lines cross, and each one's
 * normal (into the wall). A corner has to be in the room seen from pos, so a
 * doorway's two jambs are not taken for one.
 */
struct zombiecorner {
	struct coord pos;
	struct coord na;
	struct coord nb;
	f32 dist;
};

static s32 zombiesFindCorners(struct coord *pos, RoomNum *rooms, f32 reach, struct zombiecorner *out, s32 max)
{
	s32 count = 0;
	struct coord normals[16];
	f32 consts[16];
	struct coord eye = *pos;
	s32 numlines = 0;
	s32 k;
	s32 j;

	eye.y += 30;

	for (k = 0; k < 16; k++) {
		f32 ang = k * (M_BADTAU / 16);
		f32 dist;
		struct coord n;
		bool same = false;

		if (!zombiesWallLine(&eye, rooms, ang, reach, &n, &dist) || dist > reach) {
			continue;
		}

		// one line a wall
		for (j = 0; j < numlines; j++) {
			if (normals[j].x * n.x + normals[j].z * n.z > 0.98f
					&& fabsf(consts[j] - (eye.x * n.x + eye.z * n.z + dist)) < 4) {
				same = true;
			}
		}

		if (!same) {
			normals[numlines] = n;
			consts[numlines] = eye.x * n.x + eye.z * n.z + dist;
			numlines++;
		}
	}

	for (k = 0; k < numlines; k++) {
		for (j = k + 1; j < numlines; j++) {
			struct coord *a = &normals[k];
			struct coord *b = &normals[j];
			f32 det = a->x * b->z - a->z * b->x;
			struct coord c;
			struct coord inside;
			f32 dx;
			f32 dz;
			f32 d;

			// near square
			if (fabsf(a->x * b->x + a->z * b->z) > 0.26f || fabsf(det) < 0.01f) {
				continue;
			}

			c.x = (consts[k] * b->z - a->z * consts[j]) / det;
			c.z = (a->x * consts[j] - consts[k] * b->x) / det;
			c.y = eye.y;

			dx = c.x - eye.x;
			dz = c.z - eye.z;
			d = sqrtf(dx * dx + dz * dz);

			if (d > reach) {
				continue;
			}

			// a little out of it, into the room, has to be seen
			inside.x = c.x - (a->x + b->x) * 25;
			inside.y = eye.y;
			inside.z = c.z - (a->z + b->z) * 25;

			if (!cdTestLos04(&eye, rooms, &inside, CDTYPE_BG)) {
				continue;
			}

			// one corner once: the rays either side of it measure the same two
			// walls a hair apart
			{
				s32 m;
				bool dup = false;

				for (m = 0; m < count; m++) {
					f32 ex = out[m].pos.x - c.x;
					f32 ez = out[m].pos.z - c.z;

					if (ex * ex + ez * ez < 25 * 25) {
						dup = true;
					}
				}

				if (dup) {
					continue;
				}
			}

			// in by distance, the furthest dropped when full
			{
				s32 at = count < max ? count : max - 1;

				if (count >= max && d >= out[max - 1].dist) {
					continue;
				}

				while (at > 0 && out[at - 1].dist > d) {
					out[at] = out[at - 1];
					at--;
				}

				out[at].pos = c;
				out[at].pos.y = pos->y;
				out[at].na = *a;
				out[at].nb = *b;
				out[at].dist = d;

				if (count < max) {
					count++;
				}
			}
		}
	}

	return count;
}

// The nearest wall round pos that is at least mindist away and no more than
// reach, in 16 directions: a pad's nearest wall is often right beside it, and
// a machine there would stand on the pad
static bool zombiesFindPerkWall(struct coord *pos, RoomNum *rooms, f32 mindist, f32 reach, f32 *dist, struct coord *dir)
{
	struct coord eye = *pos;
	f32 best = reach + 1;
	s32 k;

	eye.y += 30;

	for (k = 0; k < 16; k++) {
		f32 ang = k * (M_BADTAU / 16);
		f32 d;

		for (d = 15; d <= reach; d += 15) {
			struct coord to;

			to.x = eye.x + sinf(ang) * d;
			to.y = eye.y;
			to.z = eye.z + cosf(ang) * d;

			if (!cdTestLos04(&eye, rooms, &to, CDTYPE_BG)) {
				if (d >= mindist && d < best) {
					best = d;
					dir->x = sinf(ang);
					dir->y = 0;
					dir->z = cosf(ang);
				}

				break;
			}
		}
	}

	*dist = best;

	return best <= reach;
}

// Machine id's against the wall beside pad, its front to the room
static bool zombiesPlaceMachineObj(s32 id, struct defaultobj *obj, struct pad *pad, f32 walldist, struct coord *wall)
{
	struct prop *prop;
	struct coord pos;
	RoomNum padrooms[2];
	RoomNum rooms[8];
	Mtxf mtx;
	const s32 extrascale = zombiesMachineExtrascale(id);
	f32 move = walldist - ZOMBIE_PERK_HALFDEPTH * extrascale / ZOMBIE_JUGG_SCALE - 8;

	padrooms[0] = pad->room;
	padrooms[1] = -1;

	pos.x = pad->pos.x + wall->x * move;
	pos.y = pad->pos.y;
	pos.z = pad->pos.z + wall->z * move;

	func0f065e74(&pad->pos, padrooms, &pos, rooms);

	if (rooms[0] <= 0) {
		return false;
	}

	bzero(obj, sizeof(*obj));
	obj->extrascale = extrascale;
	obj->type = OBJTYPE_BASIC;
	obj->modelnum = ZOMBIE_JUGG_MODEL;
	// OBJFLAG_00000100: a solid block round the model's bounds (objInit(),
	// func0f069850()), as the level's own solid props have from their setup
	obj->flags = OBJFLAG_INVINCIBLE | OBJFLAG_00000100;
	obj->flags2 = OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_IMMUNETOEXPLOSIONS;
	obj->realrot[0][0] = obj->realrot[1][1] = obj->realrot[2][2] = 1;
	obj->maxdamage = 1000;
	obj->floorcol = 0x0fff;
	obj->pad = -1;

	setupLoadModeldef(obj->modelnum);
	prop = objInitWithAutoModel(obj);

	if (prop == NULL || obj->model == NULL) {
		return false;
	}

	modelSetScale(obj->model, obj->model->scale * (obj->extrascale * (1.0f / 256.0f)));

	// as Juggernog's at a marked spot: facing back along the way to the wall
	mtx00016d58(&mtx, 0, 0, 0, wall->x, 0, wall->z, 0, 1, 0);
	mtx00015f04(obj->model->scale, &mtx);
	func0f06a730(obj, &pos, &mtx, rooms, &pos);

	propActivate(prop);
	propEnable(prop);

	obj->hidden2 &= ~OBJH2FLAG_CANREGEN;

	return true;
}

static bool zombiesPlacePerkMachine(s32 perk, struct pad *pad, f32 walldist, struct coord *wall)
{
	if (!zombiesPlaceMachineObj(perk, &g_ZombiePerkObjs[perk], pad, walldist, wall)) {
		return false;
	}

	g_ZombiePerkPlaced[perk] = true;

	return true;
}

// A machine by id: the perks', Pack-a-Punch's, then the box; NULL for none up
static struct defaultobj *zombiesMachineObjAt(s32 id)
{
	if (id == ZOMBIEPERK_JUGGERNOG) {
		return g_ZombieJugg;
	}

	if (id < ZOMBIEPERK_COUNT) {
		return g_ZombiePerkPlaced[id] ? &g_ZombiePerkObjs[id] : NULL;
	}

	if (id == ZOMBIE_PAP_ID) {
		return g_ZombiePapPlaced ? &g_ZombiePapObj : NULL;
	}

	return g_ZombieBox;
}

/**
 * A machine drawn from a model of its own in place of the mainframe that
 * stands in for it: zombies/models/<name>/<name>.obj beside the executable,
 * as tools/zombies/semodel2obj.py writes one from a Greyhound export (Call of
 * Duty's own machines, which stay on the player's disk and out of the tree).
 * A machine with no file there keeps the mainframe.
 *
 * The OBJ is Call of Duty's: centimetres, z up, the front of the machine
 * towards -y. It is turned to the game's y up (its -y the game's +z) and
 * built once into a display list of our own: a batch of up to sixteen
 * vertices at a time, the game's G_COL table beside each, every material's
 * picture bound as a stand-in tile the renderer swaps the PNG in against
 * (modelpackBindMaterial(), as a model pack's is). The light is baked into
 * the vertices from their normals, and the room's laid over it as the
 * primitive colour as it is drawn. A sign's material is drawn lit whatever
 * the room, as the machine's is with the power on. Its "..._off" material is
 * drawn too: Quick Revive's sign has no face but its power-off one (Black Ops
 * lights it with an effect laid over it), and left out the sign was a bare
 * rim. A "power_off" one is a cover over a part, and is left out.
 *
 * The mainframe is still the machine as far as everything else goes - its
 * box is what is solid and what the placement measures - and is only not
 * drawn: objRender() asks zombiesRenderMachine() first.
 */
#define ZOMBIE_MESH_SCALE   0.9f // Call of Duty's centimetres to the game's units
#define ZOMBIE_MESH_FIXED   4.0f // vertices are stored at four to the centimetre
#define ZOMBIE_MESH_BATCH   16   // vertices a G_VTX loads
#define ZOMBIE_MESH_MAXTRIS 64   // triangles a batch gathers

struct zombiemesh {
	bool tried;
	Gfx *gdl;
	Vtx *vtx;
	Col *col;
	f32 ymin;         // its foot, and the middle of its plan, in the game's axes
	f32 midx;
	f32 zmin;         // its back
	f32 width;        // across, and front to back, in its own centimetres
	f32 depth;
	f32 height;
	// a "_moving" material's vertices (Pack-a-Punch's drums), each run's
	// first and count in vtx, and their t as built, for zombiesMeshSpin()
	s32 numruns;
	s32 runfirst[4];
	s32 runcount[4];
	s16 *runt;
};

static struct zombiemesh g_ZombieMeshes[ZOMBIE_PAP_ID + 1];

static const char *g_ZombieMeshNames[ZOMBIE_PAP_ID + 1] = {
	"juggernog", "quickrevive", "speedcola", "doubletap", "staminup", "mulekick", "packapunch",
};

// A white tile for a material whose picture will not bind
static u16 g_ZombieMeshWhite[XBLATEX_TILE * XBLATEX_TILE] __attribute__((aligned(64)));

static void zombiesMeshVertex(const struct objvertex *ov, Vtx *v, Col *c, s32 slot)
{
	// z up and front -y, to y up and front +z, a turn and no mirror. The
	// mainframe's front is its +z: the placement's mtx00016d58() turns its
	// -z to the wall, the look it is given being into the corner
	f32 x = ov->pos[0];
	f32 y = ov->pos[2];
	f32 z = -ov->pos[1];
	f32 nx = ov->nrm[0];
	f32 ny = ov->nrm[2];
	f32 nz = -ov->nrm[1];
	// a light from above, in front and to the left
	f32 lit = nx * -0.35f + ny * 0.75f + nz * 0.56f;
	f32 shade = 0.6f + 0.4f * (lit > 0 ? lit : 0);
	u8 b = (u8)(shade * 255.0f);

	v->x = (s16)(x * ZOMBIE_MESH_FIXED);
	v->y = (s16)(y * ZOMBIE_MESH_FIXED);
	v->z = (s16)(z * ZOMBIE_MESH_FIXED);
	v->flags = 0;
	v->colour = (u8)(slot * 4);
	// in 10.5 texels of the 32 texel stand-in tile: a UV of one is 1024
	v->s = (s16)(ov->uv[0] * (XBLATEX_TILE * 32));
	v->t = (s16)(ov->uv[1] * (XBLATEX_TILE * 32));

	c->r = c->g = c->b = b;
	c->a = 0xff;
}

static void zombiesMeshBuildFrom(struct zombiemesh *zm, const char *rel, const char *label);

// A material lit with the power on: its name ends "_on" (Black Ops II's
// "mtl_zombie_vending_jugg_on", "..._vent_power_on", "..._logo_on")
static bool zombiesMaterialIsOn(const char *name)
{
	size_t len = strlen(name);

	return len >= 3 && strcmp(name + len - 3, "_on") == 0;
}

static void zombiesMeshBuild(s32 id)
{
	char rel[128];

	snprintf(rel, sizeof(rel), "$E/zombies/models/%s/%s.obj", g_ZombieMeshNames[id], g_ZombieMeshNames[id]);
	zombiesMeshBuildFrom(&g_ZombieMeshes[id], rel, g_ZombieMeshNames[id]);
}

static void zombiesMeshBuildFrom(struct zombiemesh *zm, const char *rel, const char *label)
{
	char path[FS_MAXPATH + 1];
	struct objmesh *m;
	u32 maxgfx;
	u32 numgfx = 0;
	u32 numvtx = 0;
	f32 lo[3] = { 1e9f, 1e9f, 1e9f };
	f32 hi[3] = { -1e9f, -1e9f, -1e9f };
	u32 d;
	u32 i;

	zm->tried = true;

	snprintf(path, sizeof(path), "%s", fsFullPath(rel));

	if (fsFileSize(path) <= 0) {
		return;
	}

	m = objmeshRead(path);

	if (!m || m->numtris == 0) {
		objmeshFree(m);
		return;
	}

	// at worst a batch a triangle: its three vertices, and the batch's head,
	// one triangle command and a material's state before each
	zm->vtx = malloc(sizeof(Vtx) * m->numtris * 3);
	zm->col = malloc(sizeof(Col) * m->numtris * 3);
	maxgfx = m->numtris * 3 + m->numdraws * 16 + 8;
	zm->gdl = malloc(sizeof(Gfx) * maxgfx);

	if (!zm->vtx || !zm->col || !zm->gdl) {
		free(zm->vtx);
		free(zm->col);
		free(zm->gdl);
		zm->vtx = NULL;
		zm->col = NULL;
		zm->gdl = NULL;
		objmeshFree(m);
		return;
	}

	for (d = 0; d < m->numdraws; d++) {
		const struct objdraw *draw = &m->draws[d];
		const struct objmaterial *mat = draw->material >= 0 && draw->material < (s32)m->nummaterials
			? &m->materials[draw->material] : NULL;
		const void *tile = NULL;
		s32 alpha = 0;
		s32 soft = 0;
		s32 slots[ZOMBIE_MESH_BATCH];
		u8 tris[ZOMBIE_MESH_MAXTRIS * 3];
		s32 numslots = 0;
		s32 numtris = 0;
		u32 t;
		Gfx *gdl;
		const u32 drawfirstvtx = numvtx;

		// a cover over a part with the power off: these machines are on
		if (mat && strstr(mat->name, "power_off")) {
			continue;
		}

		if (mat && mat->kind == OBJMAT_IMAGE) {
			tile = modelpackBindMaterial(mat, &alpha, &soft);
		}

		if (!tile) {
			if (g_ZombieMeshWhite[0] != 0xffff) {
				memset(g_ZombieMeshWhite, 0xff, sizeof(g_ZombieMeshWhite));
			}

			tile = g_ZombieMeshWhite;
		}

		gdl = &zm->gdl[numgfx];

		gDPPipeSync(gdl++);

		// A light with the power on - Black Ops II's "_on" materials, its
		// unlit_replace technique at fifteen to twenty-five times - glows:
		// the picture brightened by its own baked shade on top of itself,
		// whatever the room
		if (mat && zombiesMaterialIsOn(mat->name)) {
			gDPSetCombineLERP(gdl++, TEXEL0, 0, SHADE, TEXEL0, 0, 0, 0, 1,
					0, 0, 0, COMBINED, 0, 0, 0, 1);
		} else
		// A sign is lit, as the machine's is with the power on: not darkened
		// by the room (the second cycle's primitive colour, set round the
		// list, is the room's light)
		if (mat && strstr(mat->name, "sign")) {
			gDPSetCombineLERP(gdl++, TEXEL0, 0, SHADE, 0, 0, 0, 0, 1,
					0, 0, 0, COMBINED, 0, 0, 0, 1);
		} else {
			gDPSetCombineLERP(gdl++, TEXEL0, 0, SHADE, 0, 0, 0, 0, 1,
					COMBINED, 0, PRIMITIVE, 0, 0, 0, 0, 1);
		}

		gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
		gDPLoadTextureBlock(gdl++, tile, G_IM_FMT_RGBA, G_IM_SIZ_16b,
				XBLATEX_TILE, XBLATEX_TILE, 0,
				G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR,
				XBLATEX_TILE_MASK, XBLATEX_TILE_MASK, G_TX_NOLOD, G_TX_NOLOD);

		numgfx = (u32)(gdl - zm->gdl);

		// The triangles a batch at a time: as many as share sixteen vertices,
		// gathered, then the batch's one load and its triangles four to a
		// command
		for (t = draw->firsttri; t <= draw->firsttri + draw->numtris; t++) {
			const bool last = t == draw->firsttri + draw->numtris;
			s32 corner[3] = { -1, -1, -1 };
			s32 need = 0;
			s32 k;
			s32 j;

			if (!last) {
				for (k = 0; k < 3; k++) {
					for (j = 0; j < numslots; j++) {
						if (slots[j] == (s32)m->indices[t * 3 + k]) {
							corner[k] = j;
						}
					}

					if (corner[k] < 0) {
						// the same new vertex twice in one triangle is one slot
						for (j = 0; j < k; j++) {
							if (m->indices[t * 3 + j] == m->indices[t * 3 + k] && corner[j] < 0) {
								break;
							}
						}

						if (j == k) {
							need++;
						}
					}
				}
			}

			// the batch out: before a triangle it has no room for, and at the end
			if ((last || numslots + need > ZOMBIE_MESH_BATCH || numtris == ZOMBIE_MESH_MAXTRIS) && numtris > 0) {
				gdl = &zm->gdl[numgfx];

				for (j = 0; j < numslots; j++) {
					zombiesMeshVertex(&m->vertices[slots[j]], &zm->vtx[numvtx + j], &zm->col[numvtx + j], j);
				}

				gSPColor(gdl++, &zm->col[numvtx], numslots);
				gSPVertex(gdl++, &zm->vtx[numvtx], numslots, 0);

				for (j = 0; j < numtris; j += 4) {
					u8 *q = &tris[j * 3];
					s32 n = numtris - j;

					gSPTri4(gdl++, q[0], q[1], q[2],
							n > 1 ? q[3] : 0, n > 1 ? q[4] : 0, n > 1 ? q[5] : 0,
							n > 2 ? q[6] : 0, n > 2 ? q[7] : 0, n > 2 ? q[8] : 0,
							n > 3 ? q[9] : 0, n > 3 ? q[10] : 0, n > 3 ? q[11] : 0);
				}

				numgfx = (u32)(gdl - zm->gdl);
				numvtx += numslots;
				numslots = 0;
				numtris = 0;

				for (k = 0; k < 3; k++) {
					corner[k] = -1;
				}
			}

			if (last) {
				break;
			}

			for (k = 0; k < 3; k++) {
				const u32 vi = m->indices[t * 3 + k];
				const struct objvertex *ov = &m->vertices[vi];

				for (j = 0; j < numslots && corner[k] < 0; j++) {
					if (slots[j] == (s32)vi) {
						corner[k] = j;
					}
				}

				if (corner[k] < 0) {
					corner[k] = numslots;
					slots[numslots++] = (s32)vi;
				}

				// in the game's axes, as zombiesMeshVertex() turns them
				lo[0] = MIN(lo[0], ov->pos[0]);
				hi[0] = MAX(hi[0], ov->pos[0]);
				lo[1] = MIN(lo[1], ov->pos[2]);
				hi[1] = MAX(hi[1], ov->pos[2]);
				lo[2] = MIN(lo[2], -ov->pos[1]);
				hi[2] = MAX(hi[2], -ov->pos[1]);
			}

			// Wound over: Call of Duty's fronts are wound clockwise (Direct3D's),
			// the N64's the other way. As the file winds them it drew inside out,
			// the near side gone and the far side's inside seen
			tris[numtris * 3 + 0] = (u8)corner[0];
			tris[numtris * 3 + 1] = (u8)corner[2];
			tris[numtris * 3 + 2] = (u8)corner[1];
			numtris++;
		}

		// the draw's vertices, a run of their own, when they turn
		if (mat && strstr(mat->name, "_moving") && numvtx > drawfirstvtx && zm->numruns < 4) {
			zm->runfirst[zm->numruns] = drawfirstvtx;
			zm->runcount[zm->numruns] = numvtx - drawfirstvtx;
			zm->numruns++;
		}
	}

	gSPEndDisplayList(&zm->gdl[numgfx++]);

	if (zm->numruns > 0) {
		s32 total = 0;
		s32 r;
		s32 k;

		for (r = 0; r < zm->numruns; r++) {
			total += zm->runcount[r];
		}

		zm->runt = malloc(sizeof(s16) * total);

		if (zm->runt) {
			total = 0;

			for (r = 0; r < zm->numruns; r++) {
				for (k = 0; k < zm->runcount[r]; k++) {
					zm->runt[total++] = zm->vtx[zm->runfirst[r] + k].t;
				}
			}
		} else {
			zm->numruns = 0;
		}
	}

	zm->ymin = lo[1];
	zm->midx = (lo[0] + hi[0]) / 2;
	zm->zmin = lo[2];
	zm->width = hi[0] - lo[0];
	zm->depth = hi[2] - lo[2];
	zm->height = hi[1] - lo[1];

	sysLogPrintf(LOG_NOTE, "zombies: %s's own model, %d triangles in %d vertices, %d commands%s",
			label, m->numtris, numvtx, numgfx, zm->numruns ? ", its drums turning" : "");

	objmeshFree(m);
}

/**
 * The extrascale of machine id's mainframe, which is what is solid and what
 * placement measures. Its own ZOMBIE_JUGG_SCALE (90 across) unless the
 * machine has a model of its own: then a square the mean of the model's width
 * and depth as it is drawn, so a wide machine (Black Ops II's Mule Kick is 170
 * centimetres across, Pack-a-Punch on its table 218) stands clear of the walls
 * and is not walked into, and a shallow one leaves little air in front of it.
 * The mainframe is 1000 units square at 256.
 */
static s32 zombiesMachineExtrascale(s32 id)
{
	struct zombiemesh *zm;
	f32 side;

	if (id < 0 || id > ZOMBIE_PAP_ID) {
		return ZOMBIE_JUGG_SCALE;
	}

	zm = &g_ZombieMeshes[id];

	if (!zm->tried) {
		zombiesMeshBuild(id);
	}

	if (!zm->gdl || zm->width <= 0) {
		return ZOMBIE_JUGG_SCALE;
	}

	side = (zm->width + zm->depth) * 0.5f * ZOMBIE_MESH_SCALE;

	return (s32)(side * 256.0f / 1000.0f + 0.5f);
}

/**
 * Pack-a-Punch's drums turning, as Black Ops II's do. Each drum's picture
 * wraps once round it down its t, so sliding t turns it: the vertices are
 * rewritten in place before the list is drawn, as bgunRender() slides the
 * laser's liquid. A turn every ZOMBIE_PAP_SPIN60, three times as fast while
 * a gun is in the machine. The phase is the game's own time, the same for
 * every view drawn in a frame.
 */
#define ZOMBIE_PAP_SPIN60 150

static bool zombiesPapInUse(void);

static void zombiesMeshSpin(struct zombiemesh *zm)
{
	static s32 lastframe60 = -1;
	static f32 phase = 0;
	s32 shift;
	s32 total = 0;
	s32 r;
	s32 k;

	if (zm->numruns == 0 || !zm->runt) {
		return;
	}

	if (lastframe60 != g_Vars.lvframe60) {
		s32 dt = g_Vars.lvframe60 - lastframe60;

		if (dt < 0 || dt > 10 || lastframe60 < 0) {
			dt = 0;
		}

		phase += (f32)dt / ZOMBIE_PAP_SPIN60 * (zombiesPapInUse() ? 3.0f : 1.0f);
		phase -= (s32)phase;
		lastframe60 = g_Vars.lvframe60;
	}

	// a t of one round is XBLATEX_TILE * 32 (zombiesMeshVertex())
	shift = (s32)(phase * (XBLATEX_TILE * 32));

	for (r = 0; r < zm->numruns; r++) {
		for (k = 0; k < zm->runcount[r]; k++) {
			zm->vtx[zm->runfirst[r] + k].t = zm->runt[total++] + shift;
		}
	}
}

static bool zombiesPapGunInCamo(struct defaultobj *obj);
static Gfx *zombiesCamoAt(Gfx *gdl);
static bool zombiesMachineMeshPoint(s32 id, struct defaultobj *obj, const f32 objpos[3],
		struct coord *out, struct coord *front);
static f32 zombiesGroundAt(struct coord *pos, RoomNum *rooms);

/**
 * A machine's light on what is round it, as Call of Duty's machines cast
 * theirs: a soft glow in the perk's colour round its lit sign, turned to the
 * camera, and a pool of it on the floor in front. Both are a round falloff
 * added onto what is there (G_ADDITIVE_EXT), tested against the depth and
 * not written to it, in the translucent pass.
 */
#define ZOMBIE_GLOW_TEX   32
#define ZOMBIE_GLOW_HALO  0x70 // the glow's strength round the sign
#define ZOMBIE_GLOW_FLOOR 0x68 // and on the floor

static u8 g_ZombieGlowTex[ZOMBIE_GLOW_TEX * ZOMBIE_GLOW_TEX] __attribute__((aligned(8)));
static bool g_ZombieGlowTexMade = false;

// the falloff: bright in the middle, nothing at the edge, smooth all the way
static void zombiesGlowTexMake(void)
{
	s32 x;
	s32 y;

	for (y = 0; y < ZOMBIE_GLOW_TEX; y++) {
		for (x = 0; x < ZOMBIE_GLOW_TEX; x++) {
			f32 dx = (x + 0.5f) / (ZOMBIE_GLOW_TEX / 2) - 1;
			f32 dy = (y + 0.5f) / (ZOMBIE_GLOW_TEX / 2) - 1;
			f32 d = sqrtf(dx * dx + dy * dy);
			f32 v = d >= 1 ? 0 : (1 - d) * (1 - d);

			g_ZombieGlowTex[y * ZOMBIE_GLOW_TEX + x] = (u8)(v * 255.0f);
		}
	}

	g_ZombieGlowTexMade = true;
}

// A machine's colour: the perk's, Pack-a-Punch's purple
static void zombiesMachineGlowColour(s32 id, u8 rgb[3])
{
	if (id >= 0 && id < ZOMBIEPERK_COUNT) {
		rgb[0] = g_ZombiePerks[id].colour[0];
		rgb[1] = g_ZombiePerks[id].colour[1];
		rgb[2] = g_ZombiePerks[id].colour[2];
	} else {
		rgb[0] = 0xb0;
		rgb[1] = 0x40;
		rgb[2] = 0xff;
	}
}

// One quad: corners a, b, c, d in the space mtx takes into the view
static Gfx *zombiesGlowQuad(Gfx *gdl, Mtxf *mtx, const s16 corners[4][3])
{
	Vtx *vtx = gfxAllocate(sizeof(Vtx) * 4);
	Col *col = gfxAllocate(sizeof(Col) * 4);
	static const s16 st[4][2] = { { 0, 0 }, { ZOMBIE_GLOW_TEX << 5, 0 },
		{ ZOMBIE_GLOW_TEX << 5, ZOMBIE_GLOW_TEX << 5 }, { 0, ZOMBIE_GLOW_TEX << 5 } };
	s32 i;

	if (!vtx || !col) {
		return gdl;
	}

	for (i = 0; i < 4; i++) {
		vtx[i].x = corners[i][0];
		vtx[i].y = corners[i][1];
		vtx[i].z = corners[i][2];
		vtx[i].flags = 0;
		vtx[i].colour = (u8)(i * 4);
		vtx[i].s = st[i][0];
		vtx[i].t = st[i][1];
		col[i].r = col[i].g = col[i].b = col[i].a = 0xff;
	}

	gSPMatrix(gdl++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW | G_MTX_FLOATS);
	gSPColor(gdl++, col, 4);
	gSPVertex(gdl++, vtx, 4, 0);
	gSPTri4(gdl++, 0, 1, 2, 0, 2, 3, 0, 0, 0, 0, 0, 0);

	return gdl;
}

static Gfx *zombiesRenderMachineGlow(Gfx *gdl, s32 id, struct defaultobj *obj)
{
	struct zombiemesh *zm = &g_ZombieMeshes[id];
	Mtxf *cam = camGetWorldToScreenMtxf();
	struct coord sign;
	struct coord floor;
	struct coord front;
	f32 objpos[3];
	f32 scale;
	f32 r;
	u8 rgb[3];
	s32 i;

	if (!zm->gdl || zm->width <= 0 || zm->height <= 0) {
		return gdl;
	}

	// the sign: near the top, at the front, across the middle
	objpos[0] = zm->midx;
	objpos[1] = -(zm->zmin + zm->depth + 6);
	objpos[2] = zm->ymin + zm->height * 0.86f;

	if (!zombiesMachineMeshPoint(id, obj, objpos, &sign, &front)) {
		return gdl;
	}

	// the floor in front: its foot, a little out from its front
	objpos[1] = -(zm->zmin + zm->depth + 30);
	objpos[2] = zm->ymin + 1.5f;

	if (!zombiesMachineMeshPoint(id, obj, objpos, &floor, NULL)) {
		return gdl;
	}

	if (!g_ZombieGlowTexMade) {
		zombiesGlowTexMake();
	}

	// The floor itself, which the model's foot need not be on: looked up
	// once for each place the machine stands
	{
		static struct coord at[ZOMBIE_PAP_ID + 1];
		static f32 ground[ZOMBIE_PAP_ID + 1];
		static bool known[ZOMBIE_PAP_ID + 1];

		if (!known[id] || at[id].x != obj->prop->pos.x || at[id].y != obj->prop->pos.y || at[id].z != obj->prop->pos.z) {
			struct coord probe = floor;

			probe.y = obj->prop->pos.y;
			ground[id] = zombiesGroundAt(&probe, obj->prop->rooms);
			at[id] = obj->prop->pos;
			known[id] = true;
		}

		// none found (off a ledge): the model's foot
		if (ground[id] > floor.y - 100 && ground[id] < obj->prop->pos.y + 50) {
			floor.y = ground[id] + 1.5f;
		}
	}

	zombiesMachineGlowColour(id, rgb);

	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetRenderMode(gdl++, G_RM_ZB_XLU_SURF, G_RM_ZB_XLU_SURF2);
	gDPSetCombineLERP(gdl++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0,
			0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gDPSetTexturePersp(gdl++, G_TP_PERSP);
	gDPSetTextureLOD(gdl++, G_TL_TILE);
	gDPSetTextureLUT(gdl++, G_TT_NONE);
	gDPSetTextureConvert(gdl++, G_TC_FILT);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
	gDPLoadTextureBlock(gdl++, g_ZombieGlowTex, G_IM_FMT_I, G_IM_SIZ_8b,
			ZOMBIE_GLOW_TEX, ZOMBIE_GLOW_TEX, 0,
			G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
			G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
	gSPClearGeometryMode(gdl++, G_LIGHTING | G_TEXTURE_GEN | G_FOG | G_CULL_BOTH);
	gSPSetGeometryMode(gdl++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
	gSPSetExtraGeometryModeEXT(gdl++, G_ADDITIVE_EXT);

	// The pool on the floor: a flat quad, across the machine's front and out
	// from it, in the world
	{
		Mtxf world;
		Mtxf *mtx = gfxAllocateMatrix();
		f32 across = zm->width * ZOMBIE_MESH_SCALE * 0.9f;
		f32 out = 90;
		const s16 corners[4][3] = {
			{ (s16)-across, 0, (s16)-out }, { (s16)across, 0, (s16)-out },
			{ (s16)across, 0, (s16)out }, { (s16)-across, 0, (s16)out },
		};

		mtx4LoadIdentity(&world);
		// x across the front, z out of it
		world.m[0][0] = front.z;
		world.m[0][2] = -front.x;
		world.m[2][0] = front.x;
		world.m[2][2] = front.z;
		world.m[3][0] = floor.x;
		world.m[3][1] = floor.y;
		world.m[3][2] = floor.z;

		mtx4MultMtx4(cam, &world, mtx);

		gDPSetPrimColor(gdl++, 0, 0, rgb[0], rgb[1], rgb[2], ZOMBIE_GLOW_FLOOR);
		gdl = zombiesGlowQuad(gdl, mtx, corners);
	}

	// The halo round the sign: a square turned to the camera, the sign's
	// place taken into the view and the square laid out there
	{
		Mtxf *mtx = gfxAllocateMatrix();
		f32 c[3];
		s16 h;

		scale = sqrtf(cam->m[0][0] * cam->m[0][0] + cam->m[0][1] * cam->m[0][1] + cam->m[0][2] * cam->m[0][2]);
		// by the width of a perk machine at most: Pack-a-Punch on its table is
		// two metres across, and its halo washed the walls behind purple
		r = (zm->width < 100 ? zm->width : 100) * ZOMBIE_MESH_SCALE * 0.9f;
		h = (s16)r;

		for (i = 0; i < 3; i++) {
			c[i] = cam->m[0][i] * sign.x + cam->m[1][i] * sign.y + cam->m[2][i] * sign.z + cam->m[3][i];
		}

		mtx4LoadIdentity(mtx);
		mtx->m[0][0] = mtx->m[1][1] = mtx->m[2][2] = scale;
		mtx->m[3][0] = c[0];
		mtx->m[3][1] = c[1];
		mtx->m[3][2] = c[2];

		{
			const s16 corners[4][3] = { { -h, h, 0 }, { h, h, 0 }, { h, -h, 0 }, { -h, -h, 0 } };

			gDPSetPrimColor(gdl++, 0, 0, rgb[0], rgb[1], rgb[2], ZOMBIE_GLOW_HALO);
			gdl = zombiesGlowQuad(gdl, mtx, corners);
		}
	}

	gDPPipeSync(gdl++);
	gSPClearExtraGeometryModeEXT(gdl++, G_ADDITIVE_EXT);
	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);

	return gdl;
}

/**
 * propobj.c's objRender(), for each object before it draws its model: true
 * when this is a machine with a model of its own, drawn here instead.
 */
bool zombiesRenderMachine(struct modelrenderdata *renderdata, struct defaultobj *obj)
{
	struct zombiemesh *zm = NULL;
	struct modelrodata_bbox *bbox;
	s32 id;
	f32 len[3];
	Mtxf world;
	Mtxf *mtx;
	struct coord base;
	s32 i;
	s32 j;

	if (!zombiesIsActive()) {
		return false;
	}

	// the gun out of Pack-a-Punch, upgraded, in its camo
	if (zombiesPapGunInCamo(obj)) {
		renderdata->gdl = zombiesCamoAt(renderdata->gdl);
		modelRender(renderdata, obj->model);
		gDPGrayscaleEXT(renderdata->gdl++, 0);

		return true;
	}

	for (id = 0; id <= ZOMBIE_PAP_ID; id++) {
		if (obj == zombiesMachineObjAt(id)) {
			break;
		}
	}

	if (id > ZOMBIE_PAP_ID) {
		return false;
	}

	zm = &g_ZombieMeshes[id];

	if (!zm->tried) {
		zombiesMeshBuild(id);
	}

	if (!zm->gdl) {
		return false;
	}

	// drawn in the opaque pass; the translucent one has its glow
	if (!(renderdata->flags & MODELRENDERFLAG_OPA)) {
		renderdata->gdl = zombiesRenderMachineGlow(renderdata->gdl, id, obj);
		return true;
	}

	zombiesMeshSpin(zm);

	// The mainframe's turn without its size, which realrot carries
	for (i = 0; i < 3; i++) {
		len[i] = sqrtf(obj->realrot[i][0] * obj->realrot[i][0] + obj->realrot[i][1] * obj->realrot[i][1]
				+ obj->realrot[i][2] * obj->realrot[i][2]);

		if (len[i] <= 0) {
			return false;
		}
	}

	// Stood where the mainframe stands: its foot on the mainframe's, its
	// back to the mainframe's back, the middles of their widths together
	bbox = objFindBboxRodata(obj);
	base = obj->prop->pos;

	if (bbox) {
		f32 lx = (objGetLocalXMin(bbox) + objGetLocalXMax(bbox)) / 2;
		f32 ly = objGetLocalYMin(bbox);
		f32 lz = objGetLocalZMin(bbox);

		for (j = 0; j < 3; j++) {
			(&base.x)[j] += obj->realrot[0][j] * lx + obj->realrot[1][j] * ly + obj->realrot[2][j] * lz;
		}
	}

	mtx4LoadIdentity(&world);

	for (i = 0; i < 3; i++) {
		f32 s = ZOMBIE_MESH_SCALE / ZOMBIE_MESH_FIXED / len[i];

		for (j = 0; j < 3; j++) {
			world.m[i][j] = obj->realrot[i][j] * s;
		}
	}

	// less its own foot, middle and back, at the mainframe's
	for (j = 0; j < 3; j++) {
		(&base.x)[j] -= (world.m[0][j] * zm->midx + world.m[1][j] * zm->ymin + world.m[2][j] * zm->zmin)
			* ZOMBIE_MESH_FIXED;
	}

	world.m[3][0] = base.x;
	world.m[3][1] = base.y;
	world.m[3][2] = base.z;

	mtx = gfxAllocateMatrix();
	mtx4MultMtx4(camGetWorldToScreenMtxf(), &world, mtx);

	gDPPipeSync(renderdata->gdl++);
	gDPSetCycleType(renderdata->gdl++, G_CYC_2CYCLE);
	gDPSetRenderMode(renderdata->gdl++, G_RM_PASS,
			renderdata->zbufferenabled ? G_RM_AA_ZB_OPA_SURF2 : G_RM_AA_OPA_SURF2);
	// the picture times the baked light, times the room's (the shade colour
	// objRender() hands the prop's own lists as their fog colour); each
	// material's combiner is in the list
	// Half of it: Call of Duty's machines carry lights of their own, and in
	// one of Skedar's corners the room's alone (0x3e5c7f) was a navy machine
	{
		u32 room = (renderdata->fogcolour & 0xffffff00) ? renderdata->fogcolour : 0xffffffff;

		gDPSetPrimColor(renderdata->gdl++, 0, 0,
				0x80 + (((room >> 24) & 0xff) >> 1),
				0x80 + (((room >> 16) & 0xff) >> 1),
				0x80 + (((room >> 8) & 0xff) >> 1), 0xff);
	}
	gDPSetTextureFilter(renderdata->gdl++, G_TF_BILERP);
	gDPSetTexturePersp(renderdata->gdl++, G_TP_PERSP);
	gDPSetTextureLOD(renderdata->gdl++, G_TL_TILE);
	// Every face both ways: Call of Duty draws a machine's thin parts (Buried
	// Pack-a-Punch's table planks) from either side, and culled they were
	// gone, the machine left floating on one leg. The depth buffer hides
	// the inside of the rest.
	gSPClearGeometryMode(renderdata->gdl++, G_LIGHTING | G_TEXTURE_GEN | G_FOG | G_CULL_BOTH);
	gSPSetGeometryMode(renderdata->gdl++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
	gSPMatrix(renderdata->gdl++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW | G_MTX_FLOATS);
	gSPDisplayList(renderdata->gdl++, zm->gdl);
	gDPPipeSync(renderdata->gdl++);
	gDPSetCycleType(renderdata->gdl++, G_CYC_1CYCLE);
	gSPClearGeometryMode(renderdata->gdl++, G_CULL_BOTH);

	return true;
}

/**
 * The perk drink in the player's own hands: Black Ops II's drink (its arms'
 * animation, its bottles) retargeted onto the first person hand model the
 * player's character has, painted as handtint.c paints it. tools/zombies/
 * vmbake.py bakes it a 60th at a time into the build's zombies/viewmodel/
 * (drink.zpd, and each bottle as two OBJs, the bottle and its cap); the art
 * stays on the player's disk. Without them the drink is the HUD's bottle
 * (zombiesRenderBottle()).
 *
 * drink.zpd's matrices are into Call of Duty's view space: inches, x forward,
 * y left, z up. The view model's is the game's camera space: x right, y up,
 * looking down -z. A hand joint's matrix takes the hand model's own vertices
 * (each joint's relative to its rest place, unturned); a bottle part's takes
 * its OBJ's, which zombiesMeshVertex() has turned and stored at a quarter.
 *
 * The animation runs the bottle's part of the drink: up, open, drunk,
 * thrown away, DRINK_BOTTLEUP + DRINK_GULP + DRINK_BOTTLEDOWN 60ths, which
 * DRINK_GULP is set to make Call of Duty's 65 frames at 30.
 */
#define ZOMBIE_DRINK_JOINTS 33
#define ZOMBIE_DRINK_LEFTHAND 17 // the hand file's left forearm, and the left hand's joints after it
#define ZOMBIE_DRINK_PARTS  2
#define ZOMBIE_DRINK_UNITS  2.54f // Call of Duty's inches to the view model's units

static struct {
	bool tried;
	s32 ticks;
	const f32 *mtx; // ticks x (joints + parts) x 12, rows of three by four
} g_ZombieDrinkPose;

static struct zombiemesh g_ZombieBottleMeshes[ZOMBIEPERK_COUNT][ZOMBIE_DRINK_PARTS];

// A bottle's stored vertex (zombiesMeshVertex(): x, z, -y at four to the
// unit) back to its OBJ's own axes and units (vmbake.py: tenths of an inch,
// the bottle up its z)
static const f32 g_ZombieBottleObjAxes[3][3] = {
	{ 1.0f / ZOMBIE_MESH_FIXED, 0, 0 },
	{ 0, 0, -1.0f / ZOMBIE_MESH_FIXED },
	{ 0, 1.0f / ZOMBIE_MESH_FIXED, 0 },
};

// A perk's bottle (part 0) or its cap (1), built the first time it is wanted;
// NULL when its files are not there
static struct zombiemesh *zombiesBottleMesh(s32 perk, s32 part)
{
	struct zombiemesh *zm;

	if (perk < 0 || perk >= ZOMBIEPERK_COUNT || part < 0 || part >= ZOMBIE_DRINK_PARTS) {
		return NULL;
	}

	zm = &g_ZombieBottleMeshes[perk][part];

	if (!zm->tried) {
		char rel[128];
		char label[64];

		snprintf(rel, sizeof(rel), "$E/zombies/viewmodel/bottle_%s_%s.obj",
				g_ZombieMeshNames[perk], part ? "top" : "body");
		snprintf(label, sizeof(label), "%s's bottle%s", g_ZombieMeshNames[perk], part ? " cap" : "");
		zombiesMeshBuildFrom(zm, rel, label);
	}

	return zm->gdl ? zm : NULL;
}

// The state a bottle is drawn in: its picture times its baked light times
// prim, both ways round, the matrix a float one into the view
static Gfx *zombiesBottleDraw(Gfx *gdl, struct zombiemesh *zm, Mtxf *mtx, u8 r, u8 g, u8 b)
{
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_2CYCLE);
	gDPSetRenderMode(gdl++, G_RM_PASS, G_RM_AA_ZB_OPA_SURF2);
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
	gSPDisplayList(gdl++, zm->gdl);
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);

	return gdl;
}

static bool zombiesDrinkPoseLoad(void)
{
	const char *rel = "$E/zombies/viewmodel/drink.zpd";
	u8 *data;
	u32 size = 0;
	u16 hdr[4];

	if (g_ZombieDrinkPose.tried) {
		return g_ZombieDrinkPose.mtx != NULL;
	}

	g_ZombieDrinkPose.tried = true;

	if (fsFileSize(fsFullPath(rel)) <= 0) {
		return false;
	}

	data = fsFileLoad(rel, &size);

	if (!data || size < 12 || memcmp(data, "ZPD1", 4) != 0) {
		return false;
	}

	memcpy(hdr, data + 4, sizeof(hdr));

	if (hdr[1] != ZOMBIE_DRINK_JOINTS || hdr[2] != ZOMBIE_DRINK_PARTS || hdr[0] == 0
			|| size < 12 + (u32)hdr[0] * (ZOMBIE_DRINK_JOINTS + ZOMBIE_DRINK_PARTS) * 12 * sizeof(f32)) {
		sysLogPrintf(LOG_WARNING, "zombies: %s is not vmbake.py's", rel);
		return false;
	}

	g_ZombieDrinkPose.ticks = hdr[0];
	g_ZombieDrinkPose.mtx = (const f32 *)(data + 12);

	sysLogPrintf(LOG_NOTE, "zombies: the perk drink in the player's own hands, %d 60ths", hdr[0]);

	return true;
}

/**
 * Into the view model's space: a 3x4 of drink.zpd's, after pre (a 3x3 taking
 * the drawn vertices to the 3x4's), and Call of Duty's axes turned to the
 * camera's.
 */
static void zombiesDrinkMtx(const f32 *rows, const f32 pre[3][3], Mtxf *out)
{
	const f32 u = ZOMBIE_DRINK_UNITS;
	f32 a[3][4];
	s32 r;
	s32 c;
	s32 k;

	for (r = 0; r < 3; r++) {
		for (c = 0; c < 3; c++) {
			a[r][c] = 0;

			for (k = 0; k < 3; k++) {
				a[r][c] += rows[r * 4 + k] * (pre ? pre[k][c] : (k == c ? 1.0f : 0.0f));
			}
		}

		a[r][3] = rows[r * 4 + 3];
	}

	// camera x is Call of Duty's -y, y its z, z its -x
	for (c = 0; c < 4; c++) {
		out->m[c][0] = -a[1][c] * u;
		out->m[c][1] = a[2][c] * u;
		out->m[c][2] = -a[0][c] * u;
		out->m[c][3] = c == 3 ? 1.0f : 0.0f;
	}
}

/**
 * The hands the drink is drawn on: the character's hand model, or, with no
 * gun in hand, the fists - bare hands are a weapon whose model is the hand
 * file itself (bgunTickMasterLoad()), and no hand model is loaded beside it.
 * With Pack-a-Punch holding the only gun, a perk drunk bare handed was the
 * HUD's bottle. GoldenEye's slappers are a model of their own, not the hands.
 */
static struct model *zombiesDrinkHandModel(struct player *player)
{
	struct hand *hand = &player->hands[HAND_RIGHT];

	if (player->gunctrl.handmodeldef != NULL && hand->handmodel.definition != NULL) {
		return &hand->handmodel;
	}

	if (player->gunctrl.weaponnum == WEAPON_UNARMED && !geslappersActive()
			&& hand->gunmodel.definition != NULL
			&& hand->gunmodel.definition->nummatrices >= ZOMBIE_DRINK_JOINTS) {
		return &hand->gunmodel;
	}

	return NULL;
}

/**
 * bgunRender()'s, after the guns: the drink in the hands of the player whose
 * view is being drawn. True when it was drawn, so the HUD's bottle is not.
 */
bool zombiesDrinkViewmodelActive(s32 playernum)
{
	s32 t;

	if (!zombiesIsActive() || playernum < 0 || playernum >= MAX_PLAYERS) {
		return false;
	}

	t = g_ZombieDrink60[playernum] - DRINK_GUNDOWN;

	if (g_ZombieDrink60[playernum] < 0 || t < 0 || t >= DRINK_BOTTLEUP + DRINK_GULP + DRINK_BOTTLEDOWN) {
		return false;
	}

	if (!zombiesDrinkPoseLoad()) {
		return false;
	}

	// only where bgunRender() draws (player.c): not in third person, through
	// the X-ray scanner, or behind GoldenEye's watch
	if (g_Vars.players[playernum]->thirdpersondist > 0
			|| g_Vars.players[playernum]->visionmode == VISIONMODE_XRAY
			|| geWatchHidesGun()) {
		return false;
	}

	return zombiesDrinkHandModel(g_Vars.players[playernum]) != NULL;
}

Gfx *zombiesRenderDrinkViewmodel(Gfx *gdl)
{
	const f32 (*objaxes)[3] = g_ZombieBottleObjAxes;
	const s32 playernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;
	struct model *handmodel;
	struct modelrenderdata renderdata = {NULL, true, 3};
	const f32 *frame;
	Mtxf *matrices;
	Mtxf *saved;
	s32 perk;
	s32 n;
	s32 t;
	s32 j;

	if (!zombiesDrinkViewmodelActive(playernum)) {
		return gdl;
	}

	t = g_ZombieDrink60[playernum] - DRINK_GUNDOWN;
	t = t >= g_ZombieDrinkPose.ticks ? g_ZombieDrinkPose.ticks - 1 : t;
	frame = g_ZombieDrinkPose.mtx + t * (ZOMBIE_DRINK_JOINTS + ZOMBIE_DRINK_PARTS) * 12;
	handmodel = zombiesDrinkHandModel(player);
	n = handmodel->definition->nummatrices;

	if (n < ZOMBIE_DRINK_JOINTS) {
		n = ZOMBIE_DRINK_JOINTS;
	}

	if (!gfxHasVtxSpace(n * sizeof(Mtxf))) {
		return gdl;
	}

	matrices = gfxAllocate(n * sizeof(Mtxf));

	for (j = 0; j < n; j++) {
		zombiesDrinkMtx(frame + (j < ZOMBIE_DRINK_JOINTS ? j : 0) * 12, NULL, &matrices[j]);

		// The drink is the right hand's. The hand model with a gun shows no
		// left hand, but the fists (bare hands) have both, and the left one
		// stood at the edge of the view; put behind the eye here, as good as
		// not there. Not folded to nothing: a matrix of no size has no
		// inverse, and the release's meshes, lit from one, lost both hands
		if (j >= ZOMBIE_DRINK_LEFTHAND && j < ZOMBIE_DRINK_JOINTS) {
			matrices[j].m[3][0] = 0;
			matrices[j].m[3][1] = 0;
			matrices[j].m[3][2] = 20000;
		}
	}

	// in bgunRender()'s depth and projection, which the guns have set
	gSPPerspNormalize(gdl++, mtx00016dcc(0, 300));

	renderdata.gdl = gdl;
	renderdata.unk30 = 4;
	renderdata.envcolour = player->gunshadecol[0] << 24 | player->gunshadecol[1] << 16
		| player->gunshadecol[2] << 8 | player->gunshadecol[3];
	renderdata.zbufferenabled = true;

	mtx00016760();

	// The hands: the character's own, painted as theirs (handtint.c)
	handtintTick();

	saved = handmodel->matrices;
	handmodel->matrices = matrices;
	modelUpdateRelations(handmodel);

	// the release's meshes keep a model's pose for the rest of the pass
	// (xblamesh.c), and the hands may have been drawn on the gun already
	xblaMeshPassBegin();
	modelRender(&renderdata, handmodel);
	xblaMeshPassBegin();

	handmodel->matrices = saved;
	gdl = renderdata.gdl;

	// The bottle and its cap, in the hands
	perk = g_ZombieDrinkPerk[playernum];

	if (perk >= 0 && perk < ZOMBIEPERK_COUNT) {
		for (j = 0; j < ZOMBIE_DRINK_PARTS; j++) {
			struct zombiemesh *zm = zombiesBottleMesh(perk, j);
			Mtxf *mtx;

			if (!zm) {
				continue;
			}

			mtx = gfxAllocateMatrix();
			zombiesDrinkMtx(frame + (ZOMBIE_DRINK_JOINTS + j) * 12, objaxes, mtx);

			// in full light: the gun's shade colour is nothing here, the
			// hands being drawn in their own light
			gdl = zombiesBottleDraw(gdl, zm, mtx, 0xff, 0xff, 0xff);
		}
	}

	// the lists read the hands' matrices in fixed point, as bgunRender()'s do
	mtxF2LBulk(matrices, n);
	mtx00016784();

	gSPPerspNormalize(gdl++, viGetPerspScale());

	return gdl;
}

/**
 * The drink on the body, seen in third person or by anybody else: Perfect
 * Dark has no drinking animation, so the right arm is bent to it after the
 * body's own animation has posed it (chr.c, after modelSetMatricesWithAnim()),
 * the gun put away and the bottle drawn in the hand.
 *
 * The arm is three joints: shoulder, elbow and the hand
 * (MODELPART_CHR_RIGHTHAND), each a matrix of the model's into the view,
 * found up the tree from the hand so any human body will do. The hand's place
 * is worked out in the chest's frame (its matrix's axes carry the model's
 * scale, so its own units are the model's), moved there from where the
 * animation had it by two-bone IK - the forearm reaching on to the middle of
 * the palm, where the bottle is held - and each joint below the one turned is
 * turned about it. The elbow bends down and out.
 *
 * The timing is the first person drink's, in its 60ths from the bottle coming
 * up: raised to the chest, to the mouth and tipped up, back down, thrown away
 * as Black Ops II's is (its frame 57), and the arm back to the animation's.
 */
#define ZOMBIE_BODYDRINK_PALM   70.0f  // the wrist to the middle of the palm, model units
#define ZOMBIE_BODYDRINK_THROW  114    // the bottle gone, in the drink's 60ths
#define ZOMBIE_BODYDRINK_END    130

struct zombiebodyarm {
	s32 chest;
	s32 shoulder;
	s32 elbow[2];
	s32 hand;
	f32 side;       // the right arm's side of the chest, +1 or -1 along its x
	f32 perinch;    // model units to Call of Duty's inch, by the arm's length
};

// a node's nearest position node above it, NULL for none
static struct modelnode *zombiesPositionAbove(struct modelnode *node)
{
	for (node = node ? node->parent : NULL; node; node = node->parent) {
		if ((node->type & 0xff) == MODELNODETYPE_POSITION) {
			return node;
		}
	}

	return NULL;
}

static bool zombiesBodyArm(struct model *model, struct zombiebodyarm *arm)
{
	struct modelnode *hand;
	struct modelnode *elbow;
	struct modelnode *shoulder;
	struct modelnode *chest;
	struct modelrodata_position *e;
	struct modelrodata_position *h;
	s32 n;

	if (!model || !model->definition || !model->matrices || model->definition->skel != &g_SkelChr) {
		return false;
	}

	hand = modelGetPart(model->definition, MODELPART_CHR_RIGHTHAND);

	if (!hand || (hand->type & 0xff) != MODELNODETYPE_POSITION) {
		return false;
	}

	elbow = zombiesPositionAbove(hand);
	shoulder = zombiesPositionAbove(elbow);
	chest = zombiesPositionAbove(shoulder);

	if (!chest) {
		return false;
	}

	n = model->definition->nummatrices;
	arm->chest = chest->rodata->position.mtxindex0;
	arm->shoulder = shoulder->rodata->position.mtxindex0;
	arm->elbow[0] = elbow->rodata->position.mtxindex0;
	arm->elbow[1] = elbow->rodata->position.mtxindex1;
	arm->hand = hand->rodata->position.mtxindex0;

	if (arm->chest < 0 || arm->chest >= n || arm->shoulder < 0 || arm->shoulder >= n
			|| arm->elbow[0] < 0 || arm->elbow[0] >= n || arm->hand < 0 || arm->hand >= n
			|| arm->elbow[1] >= n) {
		return false;
	}

	arm->side = shoulder->rodata->position.pos.x >= 0 ? 1.0f : -1.0f;

	// an upper arm and forearm are some 22 inches
	e = &elbow->rodata->position;
	h = &hand->rodata->position;
	arm->perinch = (sqrtf(e->pos.x * e->pos.x + e->pos.y * e->pos.y + e->pos.z * e->pos.z)
		+ sqrtf(h->pos.x * h->pos.x + h->pos.y * h->pos.y + h->pos.z * h->pos.z)) / 22.0f;

	return arm->perinch > 0;
}

// The drink's 60ths from the bottle coming up, for the player this body is;
// -1 when it is not drinking
static s32 zombiesBodyDrinkT(struct chrdata *chr, s32 *perk)
{
	s32 playernum;
	s32 t;

	if (!zombiesIsActive() || !chr || !chr->prop || chr->prop->type != PROPTYPE_PLAYER) {
		return -1;
	}

	playernum = playermgrGetPlayerNumByProp(chr->prop);

	if (playernum < 0 || playernum >= MAX_PLAYERS || g_ZombieDrink60[playernum] < 0) {
		return -1;
	}

	t = g_ZombieDrink60[playernum] - DRINK_GUNDOWN;

	if (t < 0 || t >= ZOMBIE_BODYDRINK_END) {
		return -1;
	}

	if (perk) {
		*perk = g_ZombieDrinkPerk[playernum];
	}

	return t;
}

static f32 zombiesEase(f32 a, f32 b, f32 t)
{
	f32 f = (t - a) / (b - a);

	f = f < 0 ? 0 : f > 1 ? 1 : f;

	return f * f * (3 - 2 * f);
}

/**
 * Where the palm is wanted at drink time t, in the chest's frame (x towards
 * the right arm times side, y up, z forward), how far the bottle is tipped
 * up towards the mouth (radians) and how much of the drink's pose there is
 * against the animation's (0 to 1).
 */
static void zombiesBodyDrinkPose(s32 t, const struct zombiebodyarm *arm, f32 grip[3], f32 *tip, f32 *weight)
{
	// held in front of the chest, below the shoulder
	const f32 hold[3] = { 70, 230, 290 };
	// the mouth, and the bottle's length from the palm to its neck
	const f32 mouth[3] = { 0, 450, 110 };
	const f32 neck = 6.5f * arm->perinch;
	const f32 maxtip = 1.95f;
	f32 up = zombiesEase(28, 44, t) * (1 - zombiesEase(98, 110, t));
	f32 a;
	s32 i;

	*tip = maxtip * up;
	a = *tip;

	for (i = 0; i < 3; i++) {
		// at the mouth the palm is the bottle's length back down its line
		const f32 axis[3] = { 0, cosf(a), -sinf(a) };
		f32 drink = mouth[i] - axis[i] * neck;

		grip[i] = hold[i] + (drink - hold[i]) * up;
	}

	grip[0] *= arm->side;

	*weight = zombiesEase(0, 28, t) * (1 - zombiesEase(ZOMBIE_BODYDRINK_THROW, ZOMBIE_BODYDRINK_END - 2, t));
}

static void zombiesVec3Norm(f32 v[3])
{
	f32 len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (len > 0) {
		v[0] /= len;
		v[1] /= len;
		v[2] /= len;
	}
}

// The rotation taking unit vector a to unit vector b, the shortest way
static void zombiesRotBetween(const f32 a[3], const f32 b[3], f32 r[3][3])
{
	f32 v[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
	f32 c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	f32 k;
	s32 i;
	s32 j;

	if (c < -0.9999f) {
		// half a turn: about any line across a
		f32 p[3] = { fabsf(a[0]) < 0.9f ? 1.0f : 0.0f, fabsf(a[0]) < 0.9f ? 0.0f : 1.0f, 0 };
		f32 q[3] = { a[1] * p[2] - a[2] * p[1], a[2] * p[0] - a[0] * p[2], a[0] * p[1] - a[1] * p[0] };

		zombiesVec3Norm(q);

		for (i = 0; i < 3; i++) {
			for (j = 0; j < 3; j++) {
				r[i][j] = 2 * q[i] * q[j] - (i == j ? 1.0f : 0.0f);
			}
		}

		return;
	}

	k = 1.0f / (1.0f + c);

	r[0][0] = v[0] * v[0] * k + c;    r[0][1] = v[0] * v[1] * k - v[2]; r[0][2] = v[0] * v[2] * k + v[1];
	r[1][0] = v[1] * v[0] * k + v[2]; r[1][1] = v[1] * v[1] * k + c;    r[1][2] = v[1] * v[2] * k - v[0];
	r[2][0] = v[2] * v[0] * k - v[1]; r[2][1] = v[2] * v[1] * k + v[0]; r[2][2] = v[2] * v[2] * k + c;
}

// A joint's matrix turned by r about pivot p, all in the view
static void zombiesMtxTurnAbout(Mtxf *m, f32 r[3][3], const f32 p[3])
{
	f32 col[3];
	s32 c;
	s32 i;

	for (c = 0; c < 3; c++) {
		for (i = 0; i < 3; i++) {
			col[i] = r[i][0] * m->m[c][0] + r[i][1] * m->m[c][1] + r[i][2] * m->m[c][2];
		}

		m->m[c][0] = col[0];
		m->m[c][1] = col[1];
		m->m[c][2] = col[2];
	}

	for (i = 0; i < 3; i++) {
		col[i] = m->m[3][i] - p[i];
	}

	for (i = 0; i < 3; i++) {
		m->m[3][i] = p[i] + r[i][0] * col[0] + r[i][1] * col[1] + r[i][2] * col[2];
	}
}

// A point in a joint's own units into the view
static void zombiesMtxPoint(const Mtxf *m, const f32 local[3], f32 out[3])
{
	s32 i;

	for (i = 0; i < 3; i++) {
		out[i] = m->m[0][i] * local[0] + m->m[1][i] * local[1] + m->m[2][i] * local[2] + m->m[3][i];
	}
}

void zombiesPoseDrinkArm(struct chrdata *chr, struct model *model)
{
	struct zombiebodyarm arm;
	Mtxf *mtx;
	f32 grip[3];
	f32 tip;
	f32 weight;
	f32 s[3];
	f32 e[3];
	f32 h[3];
	f32 palm[3];
	f32 target[3];
	f32 pole[3];
	f32 d[3];
	f32 r[3][3];
	f32 upper;
	f32 lower;
	f32 dist;
	f32 cosa;
	f32 sina;
	f32 dot;
	f32 newe[3];
	f32 from[3];
	f32 to[3];
	s32 t;
	s32 i;

	t = zombiesBodyDrinkT(chr, NULL);

	if (t < 0 || !zombiesBodyArm(model, &arm)) {
		return;
	}

	zombiesBodyDrinkPose(t, &arm, grip, &tip, &weight);

	if (weight <= 0) {
		return;
	}

	mtx = model->matrices;

	for (i = 0; i < 3; i++) {
		s[i] = mtx[arm.shoulder].m[3][i];
		e[i] = mtx[arm.elbow[0]].m[3][i];
		h[i] = mtx[arm.hand].m[3][i];
	}

	upper = sqrtf((e[0] - s[0]) * (e[0] - s[0]) + (e[1] - s[1]) * (e[1] - s[1]) + (e[2] - s[2]) * (e[2] - s[2]));
	lower = sqrtf((h[0] - e[0]) * (h[0] - e[0]) + (h[1] - e[1]) * (h[1] - e[1]) + (h[2] - e[2]) * (h[2] - e[2]));

	if (upper <= 0 || lower <= 0) {
		return;
	}

	// the palm, the forearm carried on past the wrist; the joints' own
	// units are the model's, so the palm's length is too, made the view's
	// by the chest's scale
	{
		f32 scale = sqrtf(mtx[arm.chest].m[1][0] * mtx[arm.chest].m[1][0]
				+ mtx[arm.chest].m[1][1] * mtx[arm.chest].m[1][1]
				+ mtx[arm.chest].m[1][2] * mtx[arm.chest].m[1][2]);
		f32 reach = lower + ZOMBIE_BODYDRINK_PALM * scale;

		for (i = 0; i < 3; i++) {
			palm[i] = e[i] + (h[i] - e[i]) * reach / lower;
		}

		lower = reach;
	}

	// where the drink wants the palm, from where the animation has it
	zombiesMtxPoint(&mtx[arm.chest], grip, target);

	for (i = 0; i < 3; i++) {
		target[i] = palm[i] + (target[i] - palm[i]) * weight;
		d[i] = target[i] - s[i];
	}

	dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);

	if (dist <= 0) {
		return;
	}

	if (dist > (upper + lower) * 0.999f) {
		dist = (upper + lower) * 0.999f;
	}

	if (dist < fabsf(upper - lower) * 1.001f + 0.001f) {
		dist = fabsf(upper - lower) * 1.001f + 0.001f;
	}

	zombiesVec3Norm(d);

	// The elbow: down and out from the chest, across the shoulder-to-palm line
	{
		const f32 local[3] = { 0.5f * arm.side, -1.0f, -0.3f };

		for (i = 0; i < 3; i++) {
			pole[i] = mtx[arm.chest].m[0][i] * local[0] + mtx[arm.chest].m[1][i] * local[1]
				+ mtx[arm.chest].m[2][i] * local[2];
		}
	}

	dot = pole[0] * d[0] + pole[1] * d[1] + pole[2] * d[2];

	for (i = 0; i < 3; i++) {
		pole[i] -= d[i] * dot;
	}

	zombiesVec3Norm(pole);

	cosa = (upper * upper + dist * dist - lower * lower) / (2 * upper * dist);
	cosa = cosa > 1 ? 1 : cosa < -1 ? -1 : cosa;
	sina = sqrtf(1 - cosa * cosa);

	for (i = 0; i < 3; i++) {
		newe[i] = s[i] + upper * (d[i] * cosa + pole[i] * sina);
	}

	// The upper arm turned onto its new line, the elbow and hand with it
	for (i = 0; i < 3; i++) {
		from[i] = e[i] - s[i];
		to[i] = newe[i] - s[i];
	}

	zombiesVec3Norm(from);
	zombiesVec3Norm(to);
	zombiesRotBetween(from, to, r);

	zombiesMtxTurnAbout(&mtx[arm.shoulder], r, s);
	zombiesMtxTurnAbout(&mtx[arm.elbow[0]], r, s);

	if (arm.elbow[1] >= 0) {
		zombiesMtxTurnAbout(&mtx[arm.elbow[1]], r, s);
	}

	zombiesMtxTurnAbout(&mtx[arm.hand], r, s);

	// then the forearm onto the palm's place
	for (i = 0; i < 3; i++) {
		h[i] = mtx[arm.hand].m[3][i];
		from[i] = h[i] - newe[i];
		to[i] = s[i] + d[i] * dist - newe[i];
	}

	zombiesVec3Norm(from);
	zombiesVec3Norm(to);
	zombiesRotBetween(from, to, r);

	zombiesMtxTurnAbout(&mtx[arm.elbow[0]], r, newe);
	zombiesMtxTurnAbout(&mtx[arm.hand], r, newe);
}

// The drink on the player's body, seen from their third person camera, so the
// HUD's bottle is not wanted
static bool zombiesBodyDrinkSeen(s32 playernum)
{
	struct player *player;
	struct zombiebodyarm arm;
	s32 perk = -1;

	if (playernum < 0 || playernum >= MAX_PLAYERS || !g_Vars.players[playernum]) {
		return false;
	}

	player = g_Vars.players[playernum];

	if (player->thirdpersondist <= 0 || !player->prop || !player->prop->chr
			|| zombiesBodyDrinkT(player->prop->chr, &perk) < 0) {
		return false;
	}

	return zombiesBottleMesh(perk, 0) != NULL && zombiesBodyArm(player->prop->chr->model, &arm);
}

// chr.c's chrRenderAttachedObject(): the gun put away while the bottle is out
bool zombiesBodyHidesHeld(struct chrdata *chr, struct prop *prop)
{
	s32 t = zombiesBodyDrinkT(chr, NULL);

	if (t < 0 || !prop) {
		return false;
	}

	return prop == chr->weapons_held[0] || prop == chr->weapons_held[1];
}

// chrRender()'s, after the body in its opaque pass: the bottle in the hand
Gfx *zombiesRenderBodyBottle(Gfx *gdl, struct chrdata *chr, struct model *model)
{
	struct zombiebodyarm arm;
	Mtxf *mtx;
	Mtxf *out;
	f32 grip[3];
	f32 tip;
	f32 weight;
	f32 e[3];
	f32 h[3];
	f32 palm[3];
	f32 lower;
	f32 scale;
	f32 rb[3][3];
	s32 perk = -1;
	s32 t;
	s32 part;
	s32 i;
	s32 j;
	s32 k;

	t = zombiesBodyDrinkT(chr, &perk);

	if (t < 0 || t >= ZOMBIE_BODYDRINK_THROW || !zombiesBodyArm(model, &arm)) {
		return gdl;
	}

	zombiesBodyDrinkPose(t, &arm, grip, &tip, &weight);
	mtx = model->matrices;

	for (i = 0; i < 3; i++) {
		e[i] = mtx[arm.elbow[0]].m[3][i];
		h[i] = mtx[arm.hand].m[3][i];
	}

	lower = sqrtf((h[0] - e[0]) * (h[0] - e[0]) + (h[1] - e[1]) * (h[1] - e[1]) + (h[2] - e[2]) * (h[2] - e[2]));
	scale = sqrtf(mtx[arm.chest].m[1][0] * mtx[arm.chest].m[1][0]
			+ mtx[arm.chest].m[1][1] * mtx[arm.chest].m[1][1]
			+ mtx[arm.chest].m[1][2] * mtx[arm.chest].m[1][2]);

	if (lower <= 0 || scale <= 0) {
		return gdl;
	}

	for (i = 0; i < 3; i++) {
		palm[i] = h[i] + (h[i] - e[i]) * ZOMBIE_BODYDRINK_PALM * scale / lower;
	}

	// The bottle's own axes in the chest's frame: up its z, tipped back
	// towards the mouth about the chest's x, its label to the side
	// (columns x, y and z, y being z across x so it is not mirrored)
	rb[0][0] = 1; rb[0][1] = 0;           rb[0][2] = 0;
	rb[1][0] = 0; rb[1][1] = -sinf(tip);  rb[1][2] = cosf(tip);
	rb[2][0] = 0; rb[2][1] = -cosf(tip);  rb[2][2] = -sinf(tip);

	for (part = 0; part < ZOMBIE_DRINK_PARTS; part++) {
		struct zombiemesh *zm = zombiesBottleMesh(perk, part);
		f32 lin[3][3];

		// the cap is off once it is opened (Black Ops II's frame 16)
		if (!zm || (part == 1 && t >= 32)) {
			continue;
		}

		// the stored vertex to its OBJ's tenths of an inch, to model units
		// in the chest's frame, to the view
		for (i = 0; i < 3; i++) {
			for (j = 0; j < 3; j++) {
				f32 v = 0;

				for (k = 0; k < 3; k++) {
					v += rb[i][k] * g_ZombieBottleObjAxes[k][j];
				}

				lin[i][j] = v * arm.perinch / 10.0f;
			}
		}

		out = gfxAllocateMatrix();

		for (j = 0; j < 3; j++) {
			for (i = 0; i < 3; i++) {
				out->m[j][i] = mtx[arm.chest].m[0][i] * lin[0][j] + mtx[arm.chest].m[1][i] * lin[1][j]
					+ mtx[arm.chest].m[2][i] * lin[2][j];
			}

			out->m[j][3] = 0;
		}

		out->m[3][0] = palm[0];
		out->m[3][1] = palm[1];
		out->m[3][2] = palm[2];
		out->m[3][3] = 1;

		gdl = zombiesBottleDraw(gdl, zm, out,
				0x80 + (chr->shadecol[0] >> 1), 0x80 + (chr->shadecol[1] >> 1), 0x80 + (chr->shadecol[2] >> 1));
	}

	return gdl;
}

/**
 * A point on machine id's own model (in its OBJ's centimetres, z up and the
 * front -y, as the file has it) where zombiesRenderMachine() draws it, into
 * the world; and the machine's front, flat. False when the machine has no
 * model of its own.
 */
static bool zombiesMachineMeshPoint(s32 id, struct defaultobj *obj, const f32 objpos[3],
		struct coord *out, struct coord *front)
{
	struct zombiemesh *zm;
	struct modelrodata_bbox *bbox;
	f32 local[3];
	f32 len[3];
	f32 m[3];
	s32 i;
	s32 j;

	if (id < 0 || id > ZOMBIE_PAP_ID || !obj || !obj->prop) {
		return false;
	}

	zm = &g_ZombieMeshes[id];

	if (!zm->tried) {
		zombiesMeshBuild(id);
	}

	if (!zm->gdl) {
		return false;
	}

	for (i = 0; i < 3; i++) {
		len[i] = sqrtf(obj->realrot[i][0] * obj->realrot[i][0] + obj->realrot[i][1] * obj->realrot[i][1]
				+ obj->realrot[i][2] * obj->realrot[i][2]);

		if (len[i] <= 0) {
			return false;
		}
	}

	// the OBJ's axes turned as zombiesMeshVertex() turns them, less the
	// model's own foot, middle and back
	local[0] = objpos[0] - zm->midx;
	local[1] = objpos[2] - zm->ymin;
	local[2] = -objpos[1] - zm->zmin;

	*out = obj->prop->pos;
	bbox = objFindBboxRodata(obj);

	if (bbox) {
		m[0] = (objGetLocalXMin(bbox) + objGetLocalXMax(bbox)) / 2;
		m[1] = objGetLocalYMin(bbox);
		m[2] = objGetLocalZMin(bbox);

		for (j = 0; j < 3; j++) {
			(&out->x)[j] += obj->realrot[0][j] * m[0] + obj->realrot[1][j] * m[1] + obj->realrot[2][j] * m[2];
		}
	}

	for (j = 0; j < 3; j++) {
		for (i = 0; i < 3; i++) {
			(&out->x)[j] += obj->realrot[i][j] / len[i] * ZOMBIE_MESH_SCALE * local[i];
		}
	}

	if (front) {
		f32 flat = sqrtf(obj->realrot[2][0] * obj->realrot[2][0] + obj->realrot[2][2] * obj->realrot[2][2]);

		front->x = flat > 0 ? obj->realrot[2][0] / flat : 0;
		front->y = 0;
		front->z = flat > 0 ? obj->realrot[2][2] / flat : 1;
	}

	return true;
}

static const char *zombiesMachineName(s32 id)
{
	return id < ZOMBIEPERK_COUNT ? g_ZombiePerks[id].name : "Pack-a-Punch";
}

/**
 * The four machines: each on the pad furthest from everything up so far
 * (Juggernog, the box, the machines before it), whose wall is near enough
 * to back onto and far enough to leave the pad clear. Ties to the lower pad
 * number, so the machines are in the same places every match.
 */
static void zombiesPlacePerks(void)
{
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	struct coord taken[ZOMBIEPERK_COUNT + 2];
	struct coord walldir[MAX_SPAWNPOINTS];
	f32 walldist[MAX_SPAWNPOINTS];
	bool usable[MAX_SPAWNPOINTS];
	s32 ntaken = 0;
	s32 perk;
	s32 i;

	if (g_ZombieJugg && g_ZombieJugg->prop) {
		taken[ntaken++] = g_ZombieJugg->prop->pos;
	}

	if (g_ZombieBox && g_ZombieBox->prop) {
		taken[ntaken++] = g_ZombieBox->prop->pos;
	}

	for (i = 0; i < n; i++) {
		struct pad pad;
		RoomNum rooms[2];

		padUnpack(g_SpawnPoints[i], PADFIELD_POS | PADFIELD_ROOM, &pad);
		rooms[0] = pad.room;
		rooms[1] = -1;

		usable[i] = pad.room > 0
			&& zombiesFindPerkWall(&pad.pos, rooms, ZOMBIE_PERK_PADCLEAR, ZOMBIE_PERK_WALLREACH, &walldist[i], &walldir[i]);
	}

	for (perk = ZOMBIEPERK_QUICKREVIVE; perk < ZOMBIEPERK_COUNT; perk++) {
		f32 bestscore = -1;
		s32 best = -1;
		struct pad pad;

		for (i = 0; i < n; i++) {
			f32 score = 1e20f;
			s32 t;

			if (!usable[i]) {
				continue;
			}

			padUnpack(g_SpawnPoints[i], PADFIELD_POS, &pad);

			for (t = 0; t < ntaken; t++) {
				f32 dx = pad.pos.x - taken[t].x;
				f32 dy = pad.pos.y - taken[t].y;
				f32 dz = pad.pos.z - taken[t].z;
				f32 d = dx * dx + dy * dy * 4 + dz * dz;

				if (d < score) {
					score = d;
				}
			}

			if (score > bestscore || (score == bestscore && g_SpawnPoints[i] < g_SpawnPoints[best])) {
				bestscore = score;
				best = i;
			}
		}

		if (best < 0) {
			sysLogPrintf(LOG_WARNING, "zombies: no wall near enough a pad for %s on stage 0x%02x",
					g_ZombiePerks[perk].name, g_Vars.stagenum);
			continue;
		}

		usable[best] = false;
		padUnpack(g_SpawnPoints[best], PADFIELD_POS | PADFIELD_ROOM, &pad);

		if (zombiesPlacePerkMachine(perk, &pad, walldist[best], &walldir[best])) {
			taken[ntaken++] = g_ZombiePerkObjs[perk].prop->pos;
			sysLogPrintf(LOG_NOTE, "zombies: %s on stage 0x%02x at %.0f %.0f %.0f", g_ZombiePerks[perk].name,
					g_Vars.stagenum, g_ZombiePerkObjs[perk].prop->pos.x, g_ZombiePerkObjs[perk].prop->pos.y,
					g_ZombiePerkObjs[perk].prop->pos.z);
		}
	}
}

// The machine for a perk, its prop, or NULL
static struct prop *zombiesPerkMachineProp(s32 perk)
{
	if (perk == ZOMBIEPERK_JUGGERNOG) {
		return g_ZombieJugg && g_ZombieJugg->model ? g_ZombieJugg->prop : NULL;
	}

	return g_ZombiePerkPlaced[perk] && g_ZombiePerkObjs[perk].model ? g_ZombiePerkObjs[perk].prop : NULL;
}

// Every machine lit in its perk's colour, whatever light it stands in
static void zombiesTintPerkMachines(void)
{
	s32 k;

	for (k = 0; k < ZOMBIEPERK_COUNT; k++) {
		struct defaultobj *obj = k == ZOMBIEPERK_JUGGERNOG ? g_ZombieJugg : &g_ZombiePerkObjs[k];

		if (obj && obj->prop && obj->model) {
			obj->shadecol[0] = obj->nextcol[0] = 0x60 + g_ZombiePerks[k].colour[0] * 0x9f / 0xff;
			obj->shadecol[1] = obj->nextcol[1] = 0x60 + g_ZombiePerks[k].colour[1] * 0x9f / 0xff;
			obj->shadecol[2] = obj->nextcol[2] = 0x60 + g_ZombiePerks[k].colour[2] * 0x9f / 0xff;
			obj->shadecol[3] = obj->nextcol[3] = 0xff;
		}
	}
}

/**
 * Every machine, wherever it was put up, into the nearest corner of its room,
 * as Call of Duty's stand: back to one wall and side to the other, or - for
 * Juggernog and Double Tap - turned to face out of the corner, its back to
 * both. A corner is taken only when the machine there covers no spawn pad and
 * stands clear of the other machines and the box; when none is, it stays
 * where it was. Machines and the box are solid well over head height
 * (zombiesRaiseSolid()): their block was only as high as the model, and a
 * player jumped onto it and slid off its top, which is no floor.
 */
#define ZOMBIE_CORNER_REACH  900.0f // how far from where it was a machine's corner may be
#define ZOMBIE_CORNER_GAP    4.0f   // off each wall
#define ZOMBIE_SOLID_HEIGHT  300.0f // the solid block's height, from its bottom

static void zombiesRaiseSolid(struct defaultobj *obj)
{
	if (obj && obj->geocyl && (obj->hidden2 & OBJH2FLAG_08)) {
		obj->geocyl->ymax = obj->geocyl->ymin + ZOMBIE_SOLID_HEIGHT;
	}
}

// Whether a machine of radius at pos covers no spawn pad and keeps clear of
// the other machines and the box
static bool zombiesMachineSpotFree(struct coord *pos, f32 radius, struct defaultobj *self)
{
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	s32 i;

	for (i = 0; i < n; i++) {
		struct pad pad;
		f32 dx;
		f32 dz;

		padUnpack(g_SpawnPoints[i], PADFIELD_POS, &pad);
		dx = pad.pos.x - pos->x;
		dz = pad.pos.z - pos->z;

		// a player's width clear of it: the spawn nudges anyone out of a solid
		if (dx * dx + dz * dz < (radius + 30) * (radius + 30) && fabsf(pad.pos.y - pos->y) < 200) {
			return false;
		}
	}

	for (i = 0; i < ZOMBIE_MACHINE_IDS; i++) {
		struct defaultobj *other = zombiesMachineObjAt(i);
		f32 dx;
		f32 dz;

		if (other == NULL || other == self || other->prop == NULL) {
			continue;
		}

		dx = other->prop->pos.x - pos->x;
		dz = other->prop->pos.z - pos->z;

		if (dx * dx + dz * dz < (radius + 110) * (radius + 110) && fabsf(other->prop->pos.y - pos->y) < 200) {
			return false;
		}
	}

	return true;
}

// The floor under pos, from rooms
static f32 zombiesGroundAt(struct coord *pos, RoomNum *rooms)
{
	u16 floorcol;
	u8 floortype;
	u16 floorflags;
	RoomNum floorroom;
	s32 inlift;
	struct prop *lift;
	struct coord above = *pos;

	above.y += 40;

	return cdFindGroundInfoAtCyl(&above, 20, rooms, &floorcol, &floortype, &floorflags, &floorroom, &inlift, &lift);
}

// The machine into a free corner of those round origin, on origin's floor -
// only the one at want, when want is given; false for none
static bool zombiesCornerFrom(s32 perk, struct defaultobj *obj, struct coord *origin, RoomNum *originrooms,
		f32 halfw, f32 halfd, f32 radius, f32 lift, struct coord *want)
{
	struct zombiecorner corners[8];
	// every machine turned to face out of its corner, at 45 degrees
	bool angled = true;
	f32 floor = zombiesGroundAt(origin, originrooms);
	s32 n = zombiesFindCorners(origin, originrooms, ZOMBIE_CORNER_REACH, corners, ARRAYCOUNT(corners));
	struct coord oldpos = obj->prop->pos;
	RoomNum oldrooms[8];
	f32 oldrot[3][3];
	s32 i;

	roomsCopy(obj->prop->rooms, oldrooms);
	memcpy(oldrot, obj->realrot, sizeof(oldrot));

	for (i = 0; i < n; i++) {
		struct zombiecorner *c = &corners[i];
		struct coord pos;
		struct coord look;
		RoomNum rooms[8];
		Mtxf mtx;

		if (want) {
			f32 wx = c->pos.x - want->x;
			f32 wz = c->pos.z - want->z;

			if (wx * wx + wz * wz > 40 * 40) {
				continue;
			}
		}

		if (angled) {
			// its back's two corners on the two walls
			f32 in = ZOMBIE_CORNER_GAP + (halfd + halfw) * 0.70710678f;
			f32 len;

			look.x = c->na.x + c->nb.x;
			look.z = c->na.z + c->nb.z;
			len = sqrtf(look.x * look.x + look.z * look.z);
			look.x /= len;
			look.z /= len;

			pos.x = c->pos.x - (c->na.x + c->nb.x) * in;
			pos.z = c->pos.z - (c->na.z + c->nb.z) * in;
		} else {
			// back to wall a, side to wall b
			look = c->na;

			pos.x = c->pos.x - c->na.x * (halfd + ZOMBIE_CORNER_GAP) - c->nb.x * (halfw + ZOMBIE_CORNER_GAP);
			pos.z = c->pos.z - c->na.z * (halfd + ZOMBIE_CORNER_GAP) - c->nb.z * (halfw + ZOMBIE_CORNER_GAP);
		}

		pos.y = origin->y;

		if (!zombiesMachineSpotFree(&pos, radius, obj)) {
			continue;
		}

		func0f065e74(origin, originrooms, &pos, rooms);

		if (rooms[0] <= 0) {
			continue;
		}

		// on the floor it was looked for from, not up on a ledge or a step
		// the corner is on (Quick Revive was put up on one)
		if (fabsf(zombiesGroundAt(&pos, rooms) - floor) > 30) {
			continue;
		}

		mtx00016d58(&mtx, 0, 0, 0, look.x, 0, look.z, 0, 1, 0);
		mtx00015f04(obj->model->scale, &mtx);
		func0f06a730(obj, &pos, &mtx, rooms, &pos);

		// set down where it is put, it can still land on a platform over the
		// floor the corner was looked for on: back where it was then
		if (fabsf(obj->prop->pos.y - (floor + lift)) > 30) {
			Mtxf back;

			mtx3ToMtx4(oldrot, &back);
			func0f06a580(obj, &oldpos, &back, oldrooms);
			continue;
		}

		sysLogPrintf(LOG_NOTE, "zombies: %s into a corner at %.0f %.0f %.0f%s", zombiesMachineName(perk),
				obj->prop->pos.x, obj->prop->pos.y, obj->prop->pos.z, angled ? ", angled" : "");

		return true;
	}

	return false;
}

/**
 * Spots picked by hand for an arena. A corner, by a point in it: the corner
 * nearest it is the machine's, at 45 degrees. Or a wall, by a point on it: the
 * machine stands square to it there, its back to the wall. Skedar's Stamin-Up
 * across its room from where it went by itself, into the corner by the dark
 * block; its Double Tap in the middle of the recess under the overhang with
 * the blue light, across from the Shotgun's wall.
 */
struct zombiecornerpick {
	s32 stagenum;
	s32 perk;
	struct coord from;   // somewhere in the room, looked from (faced, for a floor spot)
	struct coord corner; // and the corner, the point on the wall, or the floor spot
	s32 kind;            // ZPICK_*
};

#define ZPICK_CORNER 0
#define ZPICK_WALL   1
#define ZPICK_FLOOR  2 // standing free on the floor at corner, its front to from

static const struct zombiecornerpick g_ZombieCornerPicks[] = {
	// Skedar's Stamin-Up in the corner the player stood in (1365, 0), the
	// pyramid room's narrow end, the room opening out to +x; the corner
	// itself, two walls at 45 degrees, measured 42 behind them
	{ 0x32, ZOMBIEPERK_STAMINUP,  { 1700, 160, 0 },      { 1323, 109, 0 },       ZPICK_CORNER }, // Skedar
	{ 0x32, ZOMBIEPERK_DOUBLETAP, { -2200, -255, 2200 }, { -1644, -255, 1656 },  ZPICK_WALL   }, // Skedar
	// Skedar's Pack-a-Punch on the tiled platform at the sand room's back,
	// facing out over the sand to the room's ceiling column (the spot stood on
	// and read off with zombiesTickWhere())
	{ 0x32, ZOMBIE_PAP_ID,         { -4435, -300, -4395 }, { -5114, 60, -5047 },   ZPICK_FLOOR  },
};

void bgFindRoomsByPos(struct coord *pos, RoomNum *inrooms, RoomNum *aboverooms, s32 max, RoomNum *bestroom);

// The machine square to the wall at spot, seen from origin: its back to the
// wall as measured there, facing out; on origin's floor
static bool zombiesWallSpot(s32 perk, struct defaultobj *obj, struct coord *origin, RoomNum *originrooms,
		const struct coord *spot, f32 halfd, f32 radius, f32 lift)
{
	f32 floor = zombiesGroundAt(origin, originrooms);
	f32 dx = spot->x - origin->x;
	f32 dz = spot->z - origin->z;
	struct coord eye = *origin;
	struct coord n;
	struct coord pos;
	RoomNum rooms[8];
	Mtxf mtx;
	f32 dist;
	f32 along;

	eye.y += 30;

	if (!zombiesWallLine(&eye, originrooms, atan2f(dx, dz), sqrtf(dx * dx + dz * dz) + 200, &n, &dist)) {
		return false;
	}

	// the spot onto the wall as it is, then out from it by the machine's depth
	along = (spot->x - eye.x) * -n.z + (spot->z - eye.z) * n.x;
	pos.x = eye.x + n.x * (dist - halfd - ZOMBIE_CORNER_GAP) - n.z * along;
	pos.z = eye.z + n.z * (dist - halfd - ZOMBIE_CORNER_GAP) + n.x * along;
	pos.y = origin->y;

	if (!zombiesMachineSpotFree(&pos, radius, obj)) {
		sysLogPrintf(LOG_NOTE, "zombies: %s's picked wall spot is not free", zombiesMachineName(perk));
		return false;
	}

	func0f065e74(origin, originrooms, &pos, rooms);

	if (rooms[0] <= 0 || fabsf(zombiesGroundAt(&pos, rooms) - floor) > 30) {
		return false;
	}

	mtx00016d58(&mtx, 0, 0, 0, n.x, 0, n.z, 0, 1, 0);
	mtx00015f04(obj->model->scale, &mtx);
	func0f06a730(obj, &pos, &mtx, rooms, &pos);

	sysLogPrintf(LOG_NOTE, "zombies: %s on its picked wall at %.0f %.0f %.0f", zombiesMachineName(perk),
			obj->prop->pos.x, obj->prop->pos.y, obj->prop->pos.z);

	return fabsf(obj->prop->pos.y - (floor + lift)) <= 30;
}

// The machine standing free on the floor at spot, its front to face
static bool zombiesFloorSpot(s32 perk, struct defaultobj *obj, const struct coord *spot, const struct coord *face, f32 lift)
{
	struct coord pos = *spot;
	RoomNum rooms[8];
	RoomNum above[8];
	Mtxf mtx;
	f32 floor;
	f32 lx = spot->x - face->x;
	f32 lz = spot->z - face->z;
	f32 len = sqrtf(lx * lx + lz * lz);

	bgFindRoomsByPos(&pos, rooms, above, 7, NULL);

	if (rooms[0] <= 0 || len < 1) {
		return false;
	}

	floor = zombiesGroundAt(&pos, rooms);
	pos.y = floor + lift;

	// its back away from face: the look is the way to its back
	mtx00016d58(&mtx, 0, 0, 0, lx / len, 0, lz / len, 0, 1, 0);
	mtx00015f04(obj->model->scale, &mtx);
	func0f06a730(obj, &pos, &mtx, rooms, &pos);

	sysLogPrintf(LOG_NOTE, "zombies: %s on its picked floor spot at %.0f %.0f %.0f", zombiesMachineName(perk),
			obj->prop->pos.x, obj->prop->pos.y, obj->prop->pos.z);

	return true;
}

static bool zombiesPickedCorner(s32 perk, struct defaultobj *obj, f32 halfw, f32 halfd, f32 radius, f32 lift)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_ZombieCornerPicks); i++) {
		const struct zombiecornerpick *pick = &g_ZombieCornerPicks[i];
		struct coord at;
		RoomNum rooms[8];
		RoomNum above[8];

		if (pick->stagenum != g_Vars.stagenum || pick->perk != perk) {
			continue;
		}

		at = pick->from;
		bgFindRoomsByPos(&at, rooms, above, 7, NULL);

		if (pick->kind == ZPICK_FLOOR) {
			if (zombiesFloorSpot(perk, obj, &pick->corner, &pick->from, lift)) {
				return true;
			}

			continue;
		}

		if (pick->kind == ZPICK_WALL) {
			if (rooms[0] > 0 && zombiesWallSpot(perk, obj, &at, rooms, &pick->corner, halfd, radius, lift)) {
				return true;
			}

			continue;
		}

		if (rooms[0] > 0 && zombiesCornerFrom(perk, obj, &at, rooms, halfw, halfd, radius, lift, (struct coord *)&pick->corner)) {
			return true;
		}
	}

	return false;
}

/**
 * The corner of the arena with the most floor in front of it (how far out
 * from it, square to it, the room goes), well apart from the other machines
 * and the box: a machine there is seen across a room, not met round a bend
 */
#define ZOMBIE_OPEN_APART 900.0f // how far from every other machine and the box

struct zombiecornercand {
	struct zombiecorner corner;
	struct coord origin;
	RoomNum rooms[2];
	f32 open;
};

static bool zombiesOpenestCorner(s32 perk, struct defaultobj *obj, f32 halfw, f32 halfd, f32 radius, f32 lift)
{
	static struct zombiecornercand cands[128];
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	s32 numcands = 0;
	s32 i;
	s32 j;

	for (i = 0; i < n && numcands < ARRAYCOUNT(cands); i++) {
		struct zombiecorner corners[8];
		struct pad pad;
		RoomNum rooms[2];
		s32 m;

		padUnpack(g_SpawnPoints[i], PADFIELD_POS | PADFIELD_ROOM, &pad);

		if (pad.room <= 0) {
			continue;
		}

		rooms[0] = pad.room;
		rooms[1] = -1;
		m = zombiesFindCorners(&pad.pos, rooms, ZOMBIE_CORNER_REACH, corners, ARRAYCOUNT(corners));

		for (j = 0; j < m && numcands < ARRAYCOUNT(cands); j++) {
			struct zombiecorner *c = &corners[j];
			struct coord eye;
			f32 ox = -(c->na.x + c->nb.x) * 0.70710678f;
			f32 oz = -(c->na.z + c->nb.z) * 0.70710678f;
			bool apart = true;
			s32 k;

			for (k = 0; k < ZOMBIE_MACHINE_IDS; k++) {
				struct defaultobj *other = zombiesMachineObjAt(k);

				if (other && other != obj && other->prop) {
					f32 dx = other->prop->pos.x - c->pos.x;
					f32 dz = other->prop->pos.z - c->pos.z;

					if (dx * dx + dz * dz < ZOMBIE_OPEN_APART * ZOMBIE_OPEN_APART) {
						apart = false;
					}
				}
			}

			if (!apart) {
				continue;
			}

			eye.x = c->pos.x + ox * 40;
			eye.y = pad.pos.y + 30;
			eye.z = c->pos.z + oz * 40;

			cands[numcands].corner = *c;
			cands[numcands].origin = pad.pos;
			cands[numcands].rooms[0] = rooms[0];
			cands[numcands].rooms[1] = -1;
			cands[numcands].open = zombiesRayToWall(&eye, rooms, ox, oz, 2000);

			if (cands[numcands].open < 0) {
				cands[numcands].open = 2000;
			}

			numcands++;
		}
	}

	// the openest first
	for (i = 1; i < numcands; i++) {
		struct zombiecornercand tmp = cands[i];

		for (j = i; j > 0 && cands[j - 1].open < tmp.open; j--) {
			cands[j] = cands[j - 1];
		}

		cands[j] = tmp;
	}

	for (i = 0; i < numcands; i++) {
		if (zombiesCornerFrom(perk, obj, &cands[i].origin, cands[i].rooms, halfw, halfd, radius, lift, &cands[i].corner.pos)) {
			sysLogPrintf(LOG_NOTE, "zombies: %s in the openest free corner, %.0f of room in front", zombiesMachineName(perk), cands[i].open);
			return true;
		}
	}

	return false;
}

/**
 * The corners round where the machine was put up first; none free there, the
 * corners round the spawn pads nearest it, so it keeps near its own part of
 * the arena
 */
#define ZOMBIE_CORNER_ROAM 1e9f    // how far from where it was a machine may go for a corner: anywhere, nearest first

static void zombiesCornerMachine(s32 perk, struct defaultobj *obj)
{
	struct modelrodata_bbox *bbox;
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	bool tried[MAX_SPAWNPOINTS];
	struct coord home;
	f32 halfw;
	f32 halfd;
	f32 radius;
	f32 lift;
	s32 i;

	if (obj == NULL || obj->prop == NULL || obj->model == NULL) {
		return;
	}

	bbox = objFindBboxRodata(obj);

	if (bbox == NULL) {
		zombiesRaiseSolid(obj);
		return;
	}

	halfw = (bbox->xmax - bbox->xmin) * 0.5f * obj->model->scale;
	halfd = (bbox->zmax - bbox->zmin) * 0.5f * obj->model->scale;
	radius = sqrtf(halfw * halfw + halfd * halfd);
	home = obj->prop->pos;

	// how high over its floor the machine's middle stands
	lift = obj->prop->pos.y - zombiesGroundAt(&obj->prop->pos, obj->prop->rooms);

	// a corner picked for it on this arena
	if (zombiesPickedCorner(perk, obj, halfw, halfd, radius, lift)) {
		zombiesRaiseSolid(obj);
		return;
	}

	// Double Tap where it is best seen: the corner with the most room in front
	if ((perk == ZOMBIEPERK_DOUBLETAP || perk == ZOMBIE_PAP_ID) && zombiesOpenestCorner(perk, obj, halfw, halfd, radius, lift)) {
		zombiesRaiseSolid(obj);
		return;
	}

	if (zombiesCornerFrom(perk, obj, &obj->prop->pos, obj->prop->rooms, halfw, halfd, radius, lift, NULL)) {
		zombiesRaiseSolid(obj);
		return;
	}

	for (i = 0; i < n; i++) {
		tried[i] = false;
	}

	for (;;) {
		f32 bestdist = ZOMBIE_CORNER_ROAM * ZOMBIE_CORNER_ROAM;
		s32 best = -1;
		struct pad pad;
		RoomNum rooms[2];

		for (i = 0; i < n; i++) {
			f32 dx;
			f32 dz;

			if (tried[i]) {
				continue;
			}

			padUnpack(g_SpawnPoints[i], PADFIELD_POS, &pad);
			dx = pad.pos.x - home.x;
			dz = pad.pos.z - home.z;

			if (dx * dx + dz * dz < bestdist) {
				bestdist = dx * dx + dz * dz;
				best = i;
			}
		}

		if (best < 0) {
			break;
		}

		tried[best] = true;
		padUnpack(g_SpawnPoints[best], PADFIELD_POS | PADFIELD_ROOM, &pad);

		if (pad.room <= 0) {
			continue;
		}

		rooms[0] = pad.room;
		rooms[1] = -1;

		if (zombiesCornerFrom(perk, obj, &pad.pos, rooms, halfw, halfd, radius, lift, NULL)) {
			zombiesRaiseSolid(obj);
			return;
		}
	}

	sysLogPrintf(LOG_NOTE, "zombies: no free corner for %s, left on its wall", zombiesMachineName(perk));
	zombiesRaiseSolid(obj);
}

static bool zombiesPlacePap(void);

static void zombiesCornerMachines(void)
{
	s32 k;

	// Double Tap last: it looks over the whole arena (zombiesOpenestCorner()),
	// and first it took a corner another machine was about to take
	for (k = 0; k < ZOMBIEPERK_COUNT; k++) {
		if (k == ZOMBIEPERK_JUGGERNOG) {
			zombiesCornerMachine(k, g_ZombieJugg);
		} else if (g_ZombiePerkPlaced[k] && k != ZOMBIEPERK_DOUBLETAP) {
			zombiesCornerMachine(k, &g_ZombiePerkObjs[k]);
		}
	}

	if (g_ZombiePerkPlaced[ZOMBIEPERK_DOUBLETAP]) {
		zombiesCornerMachine(ZOMBIEPERK_DOUBLETAP, &g_ZombiePerkObjs[ZOMBIEPERK_DOUBLETAP]);
	}

	// Pack-a-Punch last of all, where it is best seen, apart from the rest
	if (zombiesPlacePap()) {
		zombiesCornerMachine(ZOMBIE_PAP_ID, &g_ZombiePapObj);
	}

	zombiesRaiseSolid(g_ZombieBox);
}

/**
 * The player at one of the four machines (Juggernog's is zombiesTickJuggernog()'s):
 * which one, and B held there buys it and starts the drink
 */
static void zombiesTickPerkMachines(s32 playernum)
{
	struct player *player = g_Vars.players[playernum];
	f32 bestdist = ZOMBIE_JUGG_RANGE * ZOMBIE_JUGG_RANGE;
	s32 near = -1;
	s32 perk;
	s32 cost;
	bool held;

	g_ZombieNearPerk[playernum] = -1;

	if (g_ZombieMarking || !zombiesPlayerIsUp(playernum) || g_ZombieDrink60[playernum] >= 0
			|| g_ZombieNearDowned[playernum] >= 0) {
		g_ZombiePerkHeld60[playernum] = 0;
		return;
	}

	for (perk = ZOMBIEPERK_QUICKREVIVE; perk < ZOMBIEPERK_COUNT; perk++) {
		struct prop *machine = zombiesPerkMachineProp(perk);
		f32 dx;
		f32 dy;
		f32 dz;

		if (machine == NULL || g_ZombieHasPerk[playernum][perk]) {
			continue;
		}

		// alone, Quick Revive runs out
		if (perk == ZOMBIEPERK_QUICKREVIVE && zombiesPlayersInPlay() < 2 && g_ZombieQrSoloBuys >= ZOMBIE_QR_SOLO_BUYS) {
			continue;
		}

		dx = player->prop->pos.x - machine->pos.x;
		dy = player->prop->pos.y - machine->pos.y;
		dz = player->prop->pos.z - machine->pos.z;

		if (dx * dx + dz * dz < bestdist && dy < 250 && dy > -250) {
			bestdist = dx * dx + dz * dz;
			near = perk;
		}
	}

	g_ZombieNearPerk[playernum] = near;
	held = near >= 0 && zombiesHoldingUse(playernum);

	if (!held) {
		g_ZombiePerkHeld60[playernum] = 0;
		g_ZombiePerkNeedsRelease[playernum] = false;
		return;
	}

	if (g_ZombiePerkNeedsRelease[playernum]) {
		return;
	}

	g_ZombiePerkHeld60[playernum] += g_Vars.lvupdate60;

	if (g_ZombiePerkHeld60[playernum] < ZOMBIE_JUGG_HOLD60) {
		return;
	}

	g_ZombiePerkNeedsRelease[playernum] = true;
	g_ZombiePerkHeld60[playernum] = 0;
	cost = zombiesPerkCost(near);

	if (g_ZombiePoints[playernum] < cost) {
		zombiesPlaySound(SFX_MENU_ERROR, 0.4f);
		return;
	}

	g_ZombiePoints[playernum] -= cost;
	zombiesAddPopup(playernum, -cost);
	zombiesPlaySound(SFX_MENU_SELECT, 1.0f);

	if (near == ZOMBIEPERK_QUICKREVIVE && zombiesPlayersInPlay() < 2) {
		g_ZombieQrSoloBuys++;
	}

	g_ZombieDrinkPerk[playernum] = near;
	g_ZombieDrink60[playernum] = 0;
}

static void zombiesGivePerkEx(s32 playernum, s32 perk, bool quiet);

// A drink finished: the perk is the player's
static void zombiesGivePerk(s32 playernum, s32 perk)
{
	zombiesGivePerkEx(playernum, perk, false);
}

// ... or had back with no drink (a match carried on under a new host): no
// sound, and Mule Kick's gun is the player's own record's
static void zombiesGivePerkQuietly(s32 playernum, s32 perk)
{
	zombiesGivePerkEx(playernum, perk, true);
}

static void zombiesGivePerkEx(s32 playernum, s32 perk, bool quiet)
{
	// on the end of the row: any lost since (not by going down, which clears
	// the row) come off first, so one bought again goes on the end too
	if (perk >= 0 && perk < ZOMBIEPERK_COUNT) {
		s32 *order = g_ZombiePerkOrder[playernum];
		s32 n = 0;
		s32 i;

		for (i = 0; i < g_ZombiePerkOrderCount[playernum]; i++) {
			if (order[i] != perk && zombiesPlayerHasPerk(playernum, order[i])) {
				order[n++] = order[i];
			}
		}

		order[n++] = perk;
		g_ZombiePerkOrderCount[playernum] = n;
	}

	if (perk == ZOMBIEPERK_JUGGERNOG) {
		g_Vars.players[playernum]->healthscale = ZOMBIE_JUGG_HEALTH;
		g_ZombieHasJugg[playernum] = true;
	} else if (perk > 0 && perk < ZOMBIEPERK_COUNT) {
		g_ZombieHasPerk[playernum][perk] = true;

		if (perk == ZOMBIEPERK_MULEKICK && !quiet) {
			zombiesReturnMuleGun(playernum);
		}
	}

	if (!quiet) {
		zombiesPlaySound(SFX_PICKUP_SHIELD, 1.0f);
	}
}

/**
 * bondgun.c asks, for the current player's hand: how much faster its clock
 * runs in the state it is in - Speed Cola's reload, Double Tap's fire
 */
f32 zombiesHandRate(s32 handstate)
{
	s32 playernum = g_Vars.currentplayernum;

	if (!zombiesIsActive()) {
		return 1;
	}

	if (handstate == HANDSTATE_RELOAD && zombiesPlayerHasPerk(playernum, ZOMBIEPERK_SPEEDCOLA)) {
		return ZOMBIE_SPEEDCOLA_RATE;
	}

	if (handstate == HANDSTATE_ATTACK && zombiesPlayerHasPerk(playernum, ZOMBIEPERK_DOUBLETAP)) {
		return ZOMBIE_DOUBLETAP_RATE;
	}

	return 1;
}

// bondwalk.c asks: Stamin-Up's quicker legs for the current player
f32 zombiesMoveScale(void)
{
	s32 playernum = g_Vars.currentplayernum;

	// down, a crawl: the squat alone was still nearly a walk
	if (zombiesIsActive() && playernum >= 0 && playernum < MAX_PLAYERS && g_ZombieDowned[playernum]) {
		return ZOMBIE_DOWNED_SPEED;
	}

	if (zombiesIsActive() && zombiesPlayerHasPerk(g_Vars.currentplayernum, ZOMBIEPERK_STAMINUP)) {
		return ZOMBIE_STAMINUP_SPEED;
	}

	return 1;
}

static bool zombiesGunIsPapped(s32 playernum, s32 weaponnum);

// chrDamage() asks, for a player's bullet on a zombie: Double Tap's two for
// one, and twice again from a Pack-a-Punched gun (the right hand's)
f32 zombiesBulletScale(struct prop *aprop)
{
	f32 scale = 1;

	if (aprop && aprop->type == PROPTYPE_PLAYER) {
		s32 playernum = playermgrGetPlayerNumByProp(aprop);

		if (zombiesPlayerHasPerk(playernum, ZOMBIEPERK_DOUBLETAP)) {
			scale *= ZOMBIE_DOUBLETAP_DAMAGE;
		}

		if (playernum >= 0 && zombiesGunIsPapped(playernum, g_Vars.players[playernum]->gunctrl.weaponnum)) {
			scale *= 2.0f;
		}
	}

	return scale;
}

static Gfx *zombiesRadarDrops(Gfx *gdl);

// The machine on the radar, in Juggernog's orange. radarDrawDot() keeps a dot
// past the radar's range on its rim, so it always points the way.
static Gfx *zombiesRadarWallBuys(Gfx *gdl);

Gfx *zombiesRadarExtra(Gfx *gdl)
{
	gdl = zombiesRadarDrops(gdl);

	struct coord dist;
	s32 k;

	// every perk machine, in its perk's colour
	for (k = 0; k < ZOMBIEPERK_COUNT; k++) {
		struct prop *machine = zombiesPerkMachineProp(k);

		if (machine) {
			dist.x = machine->pos.x - g_Vars.currentplayer->prop->pos.x;
			dist.y = machine->pos.y - g_Vars.currentplayer->prop->pos.y;
			dist.z = machine->pos.z - g_Vars.currentplayer->prop->pos.z;

			gdl = radarDrawDot(gdl, machine, &dist, g_ZombiePerks[k].radar, 0, 1);
		}
	}

	// and the Mystery Box, in a pale blue
	if (g_ZombieBox && g_ZombieBox->prop && g_ZombieBox->model) {
		struct prop *box = g_ZombieBox->prop;

		dist.x = box->pos.x - g_Vars.currentplayer->prop->pos.x;
		dist.y = box->pos.y - g_Vars.currentplayer->prop->pos.y;
		dist.z = box->pos.z - g_Vars.currentplayer->prop->pos.z;

		gdl = radarDrawDot(gdl, box, &dist, 0x60c0ff00, 0, 1);
	}

	// and Pack-a-Punch, in purple
	if (g_ZombiePapPlaced && g_ZombiePapObj.prop) {
		struct prop *pap = g_ZombiePapObj.prop;

		dist.x = pap->pos.x - g_Vars.currentplayer->prop->pos.x;
		dist.y = pap->pos.y - g_Vars.currentplayer->prop->pos.y;
		dist.z = pap->pos.z - g_Vars.currentplayer->prop->pos.z;

		gdl = radarDrawDot(gdl, pap, &dist, 0xb040ff00, 0, 1);
	}

	// and the wall buys, in green
	gdl = zombiesRadarWallBuys(gdl);

	return gdl;
}

/**
 * --zombies-mark: for writing down each arena's spot. Stand where a player
 * would stand to buy, look at where the machine goes (a wall, usually) and
 * press B (E on the keyboard). The machine moves there at once, and the spot
 * is added to zombie-spots.txt beside the executable as a line for
 * g_ZombieJuggSpots. Buying is off while marking.
 */
static void zombiesMarkSpot(s32 playernum)
{
	struct player *player = g_Vars.players[playernum];
	struct zombiespot spot;
	char line[160];
	FILE *f;

	spot.stagenum = g_Vars.stagenum;
	spot.x = player->prop->pos.x;
	spot.y = player->prop->pos.y;
	spot.z = player->prop->pos.z;
	spot.lookx = player->cam_look.x;
	spot.lookz = player->cam_look.z;
	spot.room = player->prop->rooms[0];

	// the arena's name for the line's comment, without its newline
	{
		char name[48] = "?";
		s32 i;

		for (i = 0; i < (s32)ARRAYCOUNT(g_MpArenas); i++) {
			if (g_MpArenas[i].stagenum == spot.stagenum) {
				char *src = mpGetArenaName(i);
				s32 j = 0;

				while (src && src[j] && src[j] != '\n' && j < (s32)sizeof(name) - 1) {
					name[j] = src[j];
					j++;
				}

				name[j] = '\0';
				break;
			}
		}

		snprintf(line, sizeof(line), "\t{ 0x%02x, %.0f, %.0f, %.0f, %.3f, %.3f, %d }, // %s\n",
				spot.stagenum, spot.x, spot.y, spot.z, spot.lookx, spot.lookz, spot.room, name);
	}

	f = fopen("zombie-spots.txt", "ab");

	if (f) {
		fputs(line, f);
		fclose(f);
	}

	// the old machine goes, the new one shows where this spot puts it
	if (g_ZombieJugg && g_ZombieJugg->prop) {
		g_ZombieJugg->hidden |= OBJHFLAG_DELETING;
	}

	g_ZombieJugg = NULL;
	zombiesPlaceJuggernogAtSpot(&spot);

	{
		const s32 prevplayernum = g_Vars.currentplayernum;

		setCurrentPlayerNum(playernum);
		hudmsgCreate(g_ZombieJugg ? "Juggernog spot written down\n" : "No room for the machine there\n", HUDMSGTYPE_DEFAULT);
		setCurrentPlayerNum(prevplayernum);
	}

	zombiesPlaySound(SFX_MENU_SELECT, 1.0f);
}

/**
 * The knife, as Call of Duty's: a button of its own (BUTTON_MELEE, G on the
 * keyboard) swung whatever gun is in hand. Perfect Dark draws one gun model at
 * a time, so the gun in hand is put away for the Combat Knife and taken out
 * again after, both as Quick Weapon Swap swaps (bgunTickIncChangeGun()) - no
 * draw, no dip - whatever that option says (zombiesKnifeQuickSwap()). The
 * knife is never put in the inventory, so it is not in the weapon cycle.
 *
 *   ZKNIFE_IN    asked for the knife; waits for the hand to be idle with it
 *   ZKNIFE_SWING the trigger held for it until the slash starts; the blow
 *                lands where the slash's own does (prop.c, HANDATTACKTYPE_MELEE
 *                asks zombiesKnifeStrike()), and the hand idle again ends it
 *   ZKNIFE_BACK  asked for the gun(s) that were in hand; waits for them
 *
 * The blow is the nearest zombie in front within reach, through chrDamage() as
 * a punch, where a player's melee hit on a zombie is the knife's 150
 * (zombiesMeleeDamage()) - one kills in round 1 - and a kill by it is worth the
 * melee's 130. Its reach is Call of Duty's lunge, not the Combat Knife's own.
 */
#define ZKNIFE_NONE  0
#define ZKNIFE_IN    1
#define ZKNIFE_SWING 2
#define ZKNIFE_BACK  3

#define ZOMBIE_KNIFE_RANGE   210.0f // how far it reaches: Call of Duty's lunge, not a fist's arm (140 missed a zombie about to swing)
#define ZOMBIE_KNIFE_COS     0.7f   // how wide: about 45 degrees either side of the look
#define ZOMBIE_KNIFE_STAGE60 90     // the most any stage waits before it gives up and moves on
#define ZOMBIE_KNIFE_ANIMSPEED 1.5f // the slash's pace, against the Combat Knife's own (2.0 felt too quick)
#define ZOMBIE_KNIFE_SHIFTX  -7.0f  // toward the middle of the view
#define ZOMBIE_KNIFE_SHIFTY  5.0f   // up
#define ZOMBIE_KNIFE_SHIFTZ  5.0f   // nearer the eye, so larger
#define ZOMBIE_KNIFE_ENTER60 7      // the knife comes up from below over this long
#define ZOMBIE_KNIFE_ENTER   0.5f   // from this far down (of the drink's whole way)
#define ZOMBIE_KNIFE_HITFRAME 20.0f // the blow, at this frame of the slash: mid-swipe
#define ZOMBIE_KNIFE_CUTFRAME 29.0f // the rest cut, from this frame: the blade gone off the view
#define ZOMBIE_KNIFE_RAISE60 14     // the gun given back comes up from below over this long
#define ZOMBIE_KNIFE_RAISE   0.7f   // from this far down: just out of sight (all the way left the view empty for a moment)
#define ZOMBIE_KNIFE_COOLDOWN60 30  // after the gun is given back, before the next swing: no spamming it

static s32 g_ZombieKnife60[MAX_PLAYERS];       // 0 when not swinging, else 1 + 60ths into the stage
static s32 g_ZombieKnifeEnter60[MAX_PLAYERS];  // counts down as the knife comes up
static s32 g_ZombieKnifeRaise60[MAX_PLAYERS];  // counts down as the gun comes back up
static s32 g_ZombieKnifeSwing60[MAX_PLAYERS];  // since the slash started
static s32 g_ZombieKnifeCooldown60[MAX_PLAYERS]; // counts down to the next swing allowed
static s32 g_ZombieKnifeStage[MAX_PLAYERS];    // ZKNIFE_*
static bool g_ZombieKnifeAttacked[MAX_PLAYERS]; // the slash has started
static bool g_ZombieKnifeStruck[MAX_PLAYERS];   // and its blow has been dealt
static s32 g_ZombieKnifeBack[MAX_PLAYERS][2];   // the right hand's gun and the left's, to go back to

static bool zombiesCrossed(s32 prev, s32 now, s32 at);
static bool zombiesPlayerIsUp(s32 playernum);
bool zombiesIsActive(void);

static f32 zombiesPapLowerFrac(s32 playernum);

// How far down the current player's gun is held for a drink, 0 to 1.
// bondgun.c asks, to lower the arms and hold the trigger.
f32 zombiesGunLowerFrac(void)
{
	s32 playernum = g_Vars.currentplayernum;
	f32 drink = 0;
	s32 t;

	if (playernum < 0 || playernum >= MAX_PLAYERS) {
		return 0;
	}

	// Pack-a-Punch: the gun is in the machine
	if (zombiesPapLowerFrac(playernum) > 0) {
		return zombiesPapLowerFrac(playernum);
	}

	// GAME OVER: the gun goes down and stays down
	if (g_ZombieGameOver) {
		extern s32 g_ZombieGameOver60;
		f32 f = g_ZombieGameOver60 / 30.0f;

		return f >= 1 ? 1 : f * f * (3 - 2 * f);
	}

	t = g_ZombieDrink60[playernum];

	if (t >= 0) {
		if (t < DRINK_GUNDOWN) {
			drink = (f32)t / DRINK_GUNDOWN;
		} else if (t < DRINK_TOTAL - DRINK_GUNUP) {
			drink = 1;
		} else {
			drink = (f32)(DRINK_TOTAL - t) / DRINK_GUNUP;
		}

		// eased in and out
		drink = drink * drink * (3 - 2 * drink);
	}

	// The knife coming up into its slash, and the gun after it coming back
	// up, rather than either appearing at once where it is held. Eased out:
	// quick off the bottom, settling as it arrives.
	if (g_ZombieKnifeEnter60[playernum] > 0) {
		f32 f = (f32)g_ZombieKnifeEnter60[playernum] / ZOMBIE_KNIFE_ENTER60;
		f32 enter = f * f * ZOMBIE_KNIFE_ENTER;

		if (enter > drink) {
			drink = enter;
		}
	}

	if (g_ZombieKnifeRaise60[playernum] > 0) {
		f32 f = (f32)g_ZombieKnifeRaise60[playernum] / ZOMBIE_KNIFE_RAISE60;
		f32 raise = f * f * ZOMBIE_KNIFE_RAISE;

		if (raise > drink) {
			drink = raise;
		}
	}

	return drink;
}

static s32 zombiesKnifeStageOf(s32 playernum)
{
	if (!zombiesIsActive() || playernum < 0 || playernum >= MAX_PLAYERS) {
		return ZKNIFE_NONE;
	}

	return g_ZombieKnifeStage[playernum];
}

// modoptions.c: the current player's swaps to the knife and back are quick ones
bool zombiesKnifeQuickSwap(void)
{
	return zombiesKnifeStageOf(g_Vars.currentplayernum) != ZKNIFE_NONE;
}

// bondgun.c: the knife in the current player's hand is the zombies' one, on
// its slash whatever the player last chose for the Combat Knife elsewhere
bool zombiesKnifeInHand(void)
{
	return zombiesKnifeStageOf(g_Vars.currentplayernum) != ZKNIFE_NONE
		&& g_Vars.currentplayer->gunctrl.weaponnum == WEAPON_COMBATKNIFE;
}

// bondgun.c: how much faster than the Combat Knife's own the slash plays.
// Its own is 0.9 s out and back; Call of Duty's is about half that.
f32 zombiesKnifeAnimSpeed(void)
{
	return ZOMBIE_KNIFE_ANIMSPEED;
}

// bondgun.c: where the knife is held, from where the Combat Knife is (its
// posx 18.5, posy -26.5, posz -28: low in the right corner, and far)
void zombiesKnifeViewShift(struct coord *shift)
{
	shift->x = ZOMBIE_KNIFE_SHIFTX;
	shift->y = ZOMBIE_KNIFE_SHIFTY;
	shift->z = ZOMBIE_KNIFE_SHIFTZ;
}

// bondgun.c: the trigger held for the slash until it starts
bool zombiesKnifeWantsTrigger(void)
{
	s32 playernum = g_Vars.currentplayernum;

	return zombiesKnifeStageOf(playernum) == ZKNIFE_SWING && !g_ZombieKnifeAttacked[playernum];
}

// bondmove.c, on a press of BUTTON_MELEE: the current player swings
void zombiesQuickMelee(void)
{
	s32 playernum = g_Vars.currentplayernum;
	struct gunctrl *ctrl;

	// a client's replay of its commands walks again, and nothing else
	if (!zombiesIsActive() || playernum < 0 || playernum >= MAX_PLAYERS || g_NetReplaying) {
		return;
	}

	if (!zombiesPlayerIsUp(playernum) || g_ZombieDrink60[playernum] >= 0 || g_ZombieKnifeStage[playernum] != ZKNIFE_NONE
			|| g_ZombieKnifeCooldown60[playernum] > 0 || zombiesPapLowerFrac(playernum) > 0) {
		return;
	}

	ctrl = &g_Vars.currentplayer->gunctrl;

	// mid-switch, the gun to go back to is the one on its way
	g_ZombieKnifeBack[playernum][0] = ctrl->switchtoweaponnum >= 0 ? ctrl->switchtoweaponnum : ctrl->weaponnum;
	g_ZombieKnifeBack[playernum][1] = ctrl->switchtoweaponnum < 0 && ctrl->dualwielding ? bgunGetWeaponNum(HAND_LEFT) : WEAPON_NONE;

	if (g_ZombieKnifeBack[playernum][0] < WEAPON_UNARMED || g_ZombieKnifeBack[playernum][0] == WEAPON_COMBATKNIFE) {
		g_ZombieKnifeBack[playernum][0] = WEAPON_UNARMED;
	}

	g_ZombieKnifeStage[playernum] = ZKNIFE_IN;
	g_ZombieKnife60[playernum] = 1;
	g_ZombieKnifeAttacked[playernum] = false;
	g_ZombieKnifeStruck[playernum] = false;
	g_ZombieKnifeSwing60[playernum] = 0;
	g_ZombieKnifeEnter60[playernum] = ZOMBIE_KNIFE_ENTER60;
	g_ZombieKnifeRaise60[playernum] = 0;

	ctrl->dualwielding = false;
	ctrl->leftwant = WEAPON_NONE;
	bgunEquipWeapon(WEAPON_COMBATKNIFE);

	// online, a client swings its own knife at once as it predicts its walk
	// (zombiesClientTick()): its swish is its machine's
	g_ZombieSoundFor = playernum;
	g_ZombieSoundMine = true;
	zombiesPlaySound(SFX_THROW, 1.5f);
	g_ZombieSoundMine = false;
	g_ZombieSoundFor = -1;
}

// Another gun put in the hand meanwhile (the box, a wall buy) is kept: the
// knife goes no further and nothing is given back over it
static void zombiesKnifeCancel(s32 playernum)
{
	g_ZombieKnifeStage[playernum] = ZKNIFE_NONE;
	g_ZombieKnife60[playernum] = 0;
}

// The blade landing: the nearest zombie in reach in front, and none through a wall
static void zombiesKnifeHit(s32 playernum)
{
	struct player *player = g_Vars.players[playernum];
	struct prop *me = player->prop;
	struct chrdata *best = NULL;
	struct coord dir = {0, 0, 1};
	f32 bestdist = ZOMBIE_KNIFE_RANGE;
	f32 lookx = player->cam_look.x;
	f32 lookz = player->cam_look.z;
	f32 looklen = sqrtf(lookx * lookx + lookz * lookz);
	s32 i;

	// a client's knife is for show: the host's blow is the one that lands
	if (me == NULL || looklen < 0.001f || NET_CLIENT) {
		return;
	}

	for (i = 0; i < g_BotCount; i++) {
		struct chrdata *zchr = g_MpBotChrPtrs[i];
		f32 dx;
		f32 dy;
		f32 dz;
		f32 dist;

		if (zchr == NULL || zchr->prop == NULL || chrIsDead(zchr)) {
			continue;
		}

		dx = zchr->prop->pos.x - me->pos.x;
		dy = zchr->prop->pos.y - me->pos.y;
		dz = zchr->prop->pos.z - me->pos.z;
		dist = sqrtf(dx * dx + dz * dz);

		if (dist > bestdist || dy > 150 || dy < -150) {
			continue;
		}

		if (dist > 1 && (dx * lookx + dz * lookz) / (dist * looklen) < ZOMBIE_KNIFE_COS) {
			continue;
		}

		if (!cdTestLos04(&me->pos, me->rooms, &zchr->prop->pos, CDTYPE_BG | CDTYPE_DOORS)) {
			continue;
		}

		best = zchr;
		bestdist = dist;

		if (dist > 1) {
			dir.x = dx / dist;
			dir.z = dz / dist;
		}
	}

	if (best) {
		struct gset fist = {WEAPON_UNARMED, 0, 0, FUNC_PRIMARY};

		chrDamageByImpact(best, 1, &dir, &fist, me, HITPART_TORSO);
		zombiesPlaySound(SFX_05F6, 1.0f);
	}
}

// prop.c, as a melee blow of the current player's lands: true when it is the
// zombies' knife's, dealt here in place of the Combat Knife's own. The blow as
// the arm comes up (HANDATTACKTYPE_MELEENOUNCLOAK) asks too, and deals nothing.
bool zombiesKnifeStrike(bool landing)
{
	s32 playernum = g_Vars.currentplayernum;

	if (zombiesKnifeStageOf(playernum) != ZKNIFE_SWING) {
		return false;
	}

	if (landing && !g_ZombieKnifeStruck[playernum]) {
		g_ZombieKnifeStruck[playernum] = true;
		zombiesKnifeHit(playernum);
	}

	return true;
}

static void zombiesTickKnife(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	s32 stage = g_ZombieKnifeStage[playernum];
	struct player *player = g_Vars.players[playernum];
	struct gunctrl *ctrl = &player->gunctrl;
	struct hand *hand = &player->hands[HAND_RIGHT];
	bool settled;
	bool late;

	if (g_ZombieKnifeEnter60[playernum] > 0) {
		g_ZombieKnifeEnter60[playernum] -= g_Vars.lvupdate60;
	}

	if (g_ZombieKnifeRaise60[playernum] > 0) {
		g_ZombieKnifeRaise60[playernum] -= g_Vars.lvupdate60;
	}

	if (g_ZombieKnifeCooldown60[playernum] > 0 && stage == ZKNIFE_NONE) {
		g_ZombieKnifeCooldown60[playernum] -= g_Vars.lvupdate60;
	}

	if (stage == ZKNIFE_NONE) {
		return;
	}

	g_ZombieKnife60[playernum] += g_Vars.lvupdate60;
	late = g_ZombieKnife60[playernum] > ZOMBIE_KNIFE_STAGE60;

	if (!zombiesPlayerIsUp(playernum)) {
		// downed: what is in hand is the downed state's business now
		zombiesKnifeCancel(playernum);
		return;
	}

	setCurrentPlayerNum(playernum);

	settled = ctrl->switchtoweaponnum < 0 && hand->state == HANDSTATE_IDLE;

	if (stage == ZKNIFE_IN) {
		if (ctrl->switchtoweaponnum >= 0 && ctrl->switchtoweaponnum != WEAPON_COMBATKNIFE) {
			zombiesKnifeCancel(playernum);
		} else if (settled && ctrl->weaponnum == WEAPON_COMBATKNIFE) {
			g_ZombieKnifeStage[playernum] = ZKNIFE_SWING;
			g_ZombieKnife60[playernum] = 1;
		} else if (late) {
			g_ZombieKnifeStage[playernum] = ZKNIFE_BACK;
			g_ZombieKnife60[playernum] = 1;
			bgunEquipHands(g_ZombieKnifeBack[playernum][0], g_ZombieKnifeBack[playernum][1]);
		}
	} else if (stage == ZKNIFE_SWING) {
		if (ctrl->switchtoweaponnum >= 0 || ctrl->weaponnum != WEAPON_COMBATKNIFE) {
			zombiesKnifeCancel(playernum);
		} else {
			if (hand->state == HANDSTATE_ATTACK) {
				g_ZombieKnifeAttacked[playernum] = true;
				g_ZombieKnifeSwing60[playernum] += g_Vars.lvupdate60;
			}

			// The Combat Knife's slash is two moves: the blade wound back
			// across to the left (its frames 0-17), then the swipe down
			// across the view and off the bottom right (18-28), then the
			// hold again. The blow and the cut are read off its frames, so
			// they keep their place at any ZOMBIE_KNIFE_ANIMSPEED. Cut on a
			// count from the blow instead, it fell on the wind-up's end and
			// the swipe itself was never seen.
			if (g_ZombieKnifeAttacked[playernum] && hand->state == HANDSTATE_ATTACK) {
				f32 frame = modelGetCurAnimFrame(&hand->gunmodel);

				// the blow, mid-swipe
				if (frame >= ZOMBIE_KNIFE_HITFRAME && !g_ZombieKnifeStruck[playernum]) {
					g_ZombieKnifeStruck[playernum] = true;
					zombiesKnifeHit(playernum);
				}

				// the blade gone off the view: the rest, back up into the hold
				// only to be swapped away, is skipped. The hand drops out of
				// sight (the gun's raise starts down) and the attack ends with
				// the animation.
				if (frame >= ZOMBIE_KNIFE_CUTFRAME && g_ZombieKnifeRaise60[playernum] <= 0) {
					s32 numframes = modelGetNumAnimFrames(&hand->gunmodel);

					g_ZombieKnifeRaise60[playernum] = ZOMBIE_KNIFE_RAISE60;

					if (numframes > 1) {
						modelSetAnimFrame(&hand->gunmodel, numframes - 1);
					}
				}
			}

			if ((g_ZombieKnifeAttacked[playernum] && hand->state == HANDSTATE_IDLE) || late) {
				// a slash that never got its blow in still lands it
				if (!g_ZombieKnifeStruck[playernum]) {
					g_ZombieKnifeStruck[playernum] = true;
					zombiesKnifeHit(playernum);
				}

				g_ZombieKnifeStage[playernum] = ZKNIFE_BACK;
				g_ZombieKnife60[playernum] = 1;
				g_ZombieKnifeRaise60[playernum] = ZOMBIE_KNIFE_RAISE60;
				bgunEquipHands(g_ZombieKnifeBack[playernum][0], g_ZombieKnifeBack[playernum][1]);
			}
		}
	} else if (stage == ZKNIFE_BACK) {
		if ((settled && ctrl->weaponnum != WEAPON_COMBATKNIFE) || late) {
			zombiesKnifeCancel(playernum);

			// counted from the gun back in hand
			g_ZombieKnifeCooldown60[playernum] = ZOMBIE_KNIFE_COOLDOWN60;
		}
	}

	setCurrentPlayerNum(prevplayernum);
}

static bool zombiesCrossed(s32 prev, s32 now, s32 at)
{
	return prev < at && now >= at;
}

static void zombiesTickJuggernog(s32 playernum)
{
	struct player *player = g_Vars.players[playernum];
	struct prop *machine = g_ZombieJugg ? g_ZombieJugg->prop : NULL;
	bool held;

	g_ZombieNearJugg[playernum] = false;

	if (player->isdead || g_ZombieDowned[playernum]) {
		g_ZombieDrink60[playernum] = -1;
		return;
	}

	// B beside a downed teammate is a revive (zombiesTickDowned())
	if (g_ZombieNearDowned[playernum] >= 0 && g_ZombieDrink60[playernum] < 0) {
		g_ZombieUseHeld60[playernum] = 0;
		g_ZombieUseNeedsRelease[playernum] = true;
		return;
	}

	// --zombies-mark: each press of B writes a spot down instead of buying
	if (g_ZombieMarking) {
		const s32 prevplayernum = g_Vars.currentplayernum;

		setCurrentPlayerNum(playernum);
		held = joyGetButtons(optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex), zombiesUseMask(g_Vars.currentplayernum)) != 0;
		setCurrentPlayerNum(prevplayernum);

		if (held && !g_ZombieUseNeedsRelease[playernum]) {
			zombiesMarkSpot(playernum);
		}

		g_ZombieUseNeedsRelease[playernum] = held;
		return;
	}

	// drinking
	if (g_ZombieDrink60[playernum] >= 0) {
		s32 prev = g_ZombieDrink60[playernum];
		s32 now = prev + g_Vars.lvupdate60;

		// on the drink's notes (Black Ops II's frames 16, 29 and 57, two
		// 60ths each, from the bottle coming up): the cap off, gulps, and the
		// bottle thrown away
		if (zombiesCrossed(prev, now, DRINK_GUNDOWN + 32)
				|| zombiesCrossed(prev, now, DRINK_GUNDOWN + 58)
				|| zombiesCrossed(prev, now, DRINK_GUNDOWN + 76)
				|| zombiesCrossed(prev, now, DRINK_GUNDOWN + 94)) {
			zombiesPlaySound(SFX_PICKUP_AMMO, 0.45f);
		}

		if (zombiesCrossed(prev, now, DRINK_GUNDOWN + 114)) {
			zombiesPlaySound(SFX_BOTTLE_BREAK, 0.5f);
		}

		// the perk takes as the last of it goes down
		if (zombiesCrossed(prev, now, DRINK_GULPEND)) {
			zombiesGivePerk(playernum, g_ZombieDrinkPerk[playernum]);
		}

		g_ZombieDrink60[playernum] = now >= DRINK_TOTAL ? -1 : now;
		return;
	}

	if (machine == NULL || g_ZombieHasJugg[playernum]) {
		return;
	}

	{
		f32 dx = player->prop->pos.x - machine->pos.x;
		f32 dy = player->prop->pos.y - machine->pos.y;
		f32 dz = player->prop->pos.z - machine->pos.z;

		if (dx * dx + dz * dz > ZOMBIE_JUGG_RANGE * ZOMBIE_JUGG_RANGE || dy > 250 || dy < -250) {
			g_ZombieUseHeld60[playernum] = 0;
			g_ZombieUseNeedsRelease[playernum] = false;
			return;
		}
	}

	g_ZombieNearJugg[playernum] = true;

	{
		const s32 prevplayernum = g_Vars.currentplayernum;

		setCurrentPlayerNum(playernum);
		held = joyGetButtons(optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex), zombiesUseMask(g_Vars.currentplayernum)) != 0;
		setCurrentPlayerNum(prevplayernum);
	}

	if (!held) {
		g_ZombieUseHeld60[playernum] = 0;
		g_ZombieUseNeedsRelease[playernum] = false;
		return;
	}

	if (g_ZombieUseNeedsRelease[playernum]) {
		return;
	}

	g_ZombieUseHeld60[playernum] += g_Vars.lvupdate60;

	if (g_ZombieUseHeld60[playernum] < ZOMBIE_JUGG_HOLD60) {
		return;
	}

	g_ZombieUseNeedsRelease[playernum] = true;

	if (g_ZombiePoints[playernum] < ZOMBIE_JUGG_COST) {
		zombiesPlaySound(SFX_MENU_ERROR, 0.4f);
		return;
	}

	g_ZombiePoints[playernum] -= ZOMBIE_JUGG_COST;
	zombiesAddPopup(playernum, -ZOMBIE_JUGG_COST);
	zombiesPlaySound(SFX_MENU_SELECT, 1.0f);
	g_ZombieDrinkPerk[playernum] = ZOMBIEPERK_JUGGERNOG;
	g_ZombieDrink60[playernum] = 0;
}

bool zombiesIsActive(void);

static void zombiesResetDowned(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ZombieDowned[i] = false;
		g_ZombieBleed60[i] = 0;
		g_ZombieRevive60[i] = 0;
		g_ZombieReviver[i] = -1;
		g_ZombieNearDowned[i] = -1;
		g_ZombieRespawnDue[i] = false;
		g_ZombieSpectating[i] = false;
		g_ZombieWatch[i] = -1;
		g_ZombieSpecHave[i] = false;
	}

	g_ZombieAllDownFrame = -1;
}

// Standing: alive and not down
static bool zombiesPlayerIsUp(s32 playernum)
{
	struct player *player = g_Vars.players[playernum];

	return player && player->prop && !player->isdead && !g_ZombieDowned[playernum];
}

// Whether the player is holding B (E on the keyboard)
static bool zombiesHoldingUse(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	bool held;

	setCurrentPlayerNum(playernum);
	held = joyGetButtons(optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex), zombiesUseMask(g_Vars.currentplayernum)) != 0;
	setCurrentPlayerNum(prevplayernum);

	return held;
}

/**
 * Where am I: the knife button held two seconds turns on (and off) the
 * player's position at the top of the view, for picking a machine's spot on
 * an arena by standing on it (g_ZombieCornerPicks).
 */
#define ZOMBIE_WHERE_HOLD60 120

static bool g_ZombieWhere[MAX_PLAYERS];
static s32 g_ZombieWhereHeld60[MAX_PLAYERS];

static void zombiesTickWhere(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	bool held;

	setCurrentPlayerNum(playernum);
	held = joyGetButtons(optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex), BUTTON_MELEE) != 0;
	setCurrentPlayerNum(prevplayernum);

	if (!held) {
		g_ZombieWhereHeld60[playernum] = 0;
		return;
	}

	if (g_ZombieWhereHeld60[playernum] < 0) {
		return; // toggled already, waiting for the button to come up
	}

	g_ZombieWhereHeld60[playernum] += g_Vars.lvupdate60;

	if (g_ZombieWhereHeld60[playernum] >= ZOMBIE_WHERE_HOLD60) {
		g_ZombieWhere[playernum] = !g_ZombieWhere[playernum];
		g_ZombieWhereHeld60[playernum] = -1;
	}
}
static void zombiesTellOthers(s32 playernum, char *message)
{
	s32 i;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		if (i != playernum) {
			scenarioCreateHudmsg(i, message);
		}
	}
}

static void zombiesClearDowned(s32 playernum)
{
	struct player *player = g_Vars.players[playernum];

	g_ZombieDowned[playernum] = false;
	g_ZombieRevive60[playernum] = 0;
	g_ZombieReviver[playernum] = -1;
	g_ZombieSelfRevive60[playernum] = 0;
	player->invincible = g_ZombieSavedInvincible[playernum];
	player->autocrouchpos = CROUCHPOS_STAND;
}

/**
 * Called by playerDieByShooter() for the player about to die. True when the
 * death is not to happen: they go down instead, or are down already and a
 * hit is not what ends that. force is a death nothing survives (a fall).
 */
bool zombiesPlayerGoesDown(s32 playernum, bool force)
{
	struct player *player;
	char msg[48];
	bool solo;

	if (!zombiesIsActive() || g_ZombieGameOver || playernum < 0 || playernum >= PLAYERCOUNT()) {
		return false;
	}

	player = g_Vars.players[playernum];

	if (player->isdead) {
		return false;
	}

	// a client: whether the host has the player down, and nothing changed here
	if (NET_CLIENT) {
		return g_ZombieDowned[playernum] && !force;
	}

	if (g_ZombieDowned[playernum]) {
		if (force) {
			zombiesClearDowned(playernum);
			return false;
		}

		return true;
	}

	// alone, only Quick Revive lets a player go down rather than die
	solo = zombiesPlayersInPlay() < 2;

	if (force || (solo && !g_ZombieHasPerk[playernum][ZOMBIEPERK_QUICKREVIVE])
			|| g_ZombieRound < 1 || player->invincible) {
		return false;
	}

	g_ZombieDowned[playernum] = true;
	g_ZombieBleed60[playernum] = ZOMBIE_BLEEDOUT60;
	g_ZombieRevive60[playernum] = 0;
	g_ZombieReviver[playernum] = -1;
	g_ZombieSavedInvincible[playernum] = player->invincible;
	g_ZombieSelfRevive60[playernum] = solo ? ZOMBIE_QR_SELF60 : 0;

	player->invincible = true;
	player->bondhealth = g_ZombieMaxHealth[playernum] * 0.01f;
	player->crouchpos = CROUCHPOS_SQUAT;
	player->autocrouchpos = CROUCHPOS_SQUAT;

	// going down costs the perks, Quick Revive with them, and Mule Kick's
	// third gun (asked before the perk goes)
	g_ZombieDrink60[playernum] = -1;
	zombiesLoseMuleGun(playernum);
	zombiesLosePerks(playernum);

	zombiesPlaySound(SFX_MENU_ERROR, 0.6f);

	sprintf(msg, "PLAYER %d IS DOWN!\n", playernum + 1);
	zombiesTellOthers(playernum, msg);

	return true;
}

// For bondwalk.c: a downed player squats whatever the crouch button says
bool zombiesIsPlayerDowned(s32 playernum)
{
	return zombiesIsActive() && playernum >= 0 && playernum < MAX_PLAYERS && g_ZombieDowned[playernum];
}

/**
 * For playerTick()'s respawn: -1 when Zombies has no say, 0 to keep a dead
 * player down (until the next round), 1 to stand them up now.
 */
s32 zombiesRespawnRule(s32 playernum)
{
	if (!zombiesIsActive() || playernum < 0 || playernum >= MAX_PLAYERS) {
		return -1;
	}

	return g_ZombieRespawnDue[playernum] && !g_ZombieGameOver ? 1 : 0;
}

// A new round stands the dead back up
static void zombiesRespawnTheDead(void)
{
	s32 i;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		if (g_Vars.players[i]->isdead && !g_ZombieOutOfPlay[i]) {
			g_ZombieRespawnDue[i] = true;
		}
	}
}

/**
 * A bled-out player watches a teammate until the next round, as Call of
 * Duty's dead do: once their death has faded to black the picture comes back
 * behind the teammate's shoulder (the follow camera netspec.c gives an online
 * spectator, pulled in where a wall stands between), and fire moves on to the
 * next one. Per player, so every dead view in a splitscreen game has its own.
 */
#define ZOMBIE_SPEC_BACK 150.0f
#define ZOMBIE_SPEC_UP   45.0f

f32 chrGetInverseTheta(struct chrdata *chr);

// Someone a dead player can watch: anyone else still alive, standing first
static bool zombiesCanWatch(s32 me, s32 playernum, bool standingonly)
{
	struct player *player;

	if (playernum < 0 || playernum >= PLAYERCOUNT() || playernum == me || g_ZombieOutOfPlay[playernum]) {
		return false;
	}

	player = g_Vars.players[playernum];

	if (!player->prop || !player->prop->chr || player->isdead) {
		return false;
	}

	return !standingonly || !g_ZombieDowned[playernum];
}

static void zombiesWatchNext(s32 me)
{
	s32 n = PLAYERCOUNT();
	s32 pass;
	s32 k;

	for (pass = 0; pass < 2; pass++) {
		for (k = 1; k <= n; k++) {
			s32 p = ((g_ZombieWatch[me] < 0 ? me : g_ZombieWatch[me]) + k) % n;

			if (zombiesCanWatch(me, p, pass == 0)) {
				g_ZombieWatch[me] = p;
				return;
			}
		}
	}

	g_ZombieWatch[me] = -1;
}

// How far each 60th the follow camera's aim and distance close on where they
// want to be: quick enough to keep up with a turn, slow enough not to shake
#define ZOMBIE_SPEC_EASE 0.18f

// up square to look, the world's up as near as it can be
static void zombiesUpFromLook(struct coord *look, struct coord *up)
{
	struct coord right;
	f32 len;

	right.x = -look->z;
	right.z = look->x;
	len = sqrtf(right.x * right.x + right.z * right.z);

	if (len < 0.0001f) {
		up->x = 0;
		up->y = 0;
		up->z = 1;
		return;
	}

	right.x /= len;
	right.z /= len;
	up->x = -right.z * look->y;
	up->y = right.z * look->x - right.x * look->z;
	up->z = right.x * look->y;

	if (up->y < 0) {
		up->x = -up->x;
		up->y = -up->y;
		up->z = -up->z;
	}
}

// First person: through the watched player's own camera, as their view has it
static void zombiesFirstPersonCamera(s32 target)
{
	struct player *tp = g_Vars.players[target];
	struct coord pos = tp->cam_pos;
	struct coord up = tp->cam_up;
	struct coord look = tp->cam_look;
	RoomNum rooms[2];

	rooms[0] = tp->cam_room;
	rooms[1] = -1;

	if (rooms[0] < 0) {
		player0f0c1840(&pos, &up, &look, &tp->prop->pos, tp->prop->rooms);
	} else {
		player0f0c1840(&pos, &up, &look, &tp->cam_pos, rooms);
	}
}

/**
 * Third person: behind the watched player and a little above, looking the
 * way they aim (their camera's look, not their body's turn, which jumps
 * about with the walk). The aim and the distance are eased, so a quick turn
 * swings the camera round rather than snapping it; a wall coming between
 * pulls it in at once, and it eases back out when the wall has gone.
 */
static void zombiesFollowCamera(s32 me, s32 target)
{
	struct player *tp = g_Vars.players[target];
	struct prop *tprop = tp->prop;
	struct coord eye = tprop->pos;
	struct coord want = tp->cam_look;
	struct coord *aim = &g_ZombieSpecLook[me];
	struct coord back;
	struct coord cam;
	struct coord look;
	struct coord up;
	f32 ease = ZOMBIE_SPEC_EASE * g_Vars.lvupdate60freal;
	f32 dist = ZOMBIE_SPEC_BACK;
	f32 len;

	if (ease > 1) {
		ease = 1;
	}

	if (!g_ZombieSpecHave[me]) {
		*aim = want;
		g_ZombieSpecDist[me] = ZOMBIE_SPEC_BACK;
		g_ZombieSpecHave[me] = true;
	} else {
		aim->x += (want.x - aim->x) * ease;
		aim->y += (want.y - aim->y) * ease;
		aim->z += (want.z - aim->z) * ease;
	}

	len = sqrtf(aim->x * aim->x + aim->y * aim->y + aim->z * aim->z);

	if (len < 0.0001f) {
		aim->x = 0;
		aim->y = 0;
		aim->z = 1;
	} else {
		aim->x /= len;
		aim->y /= len;
		aim->z /= len;
	}

	// straight back from the way they aim, level
	back.x = -aim->x;
	back.y = 0;
	back.z = -aim->z;
	len = sqrtf(back.x * back.x + back.z * back.z);

	if (len < 0.0001f) {
		f32 inv = chrGetInverseTheta(tprop->chr);

		back.x = -sinf(inv);
		back.z = -cosf(inv);
	} else {
		back.x /= len;
		back.z /= len;
	}

	cam.x = eye.x + back.x * ZOMBIE_SPEC_BACK;
	cam.y = eye.y + ZOMBIE_SPEC_UP;
	cam.z = eye.z + back.z * ZOMBIE_SPEC_BACK;

	// a wall between: in, a step at a time, to where it is clear
	if (!cdTestLos04(&eye, tprop->rooms, &cam, CDTYPE_BG)) {
		f32 frac = 1;
		s32 i;

		for (i = 0; i < 8; i++) {
			struct coord t;

			frac *= 0.75f;
			t.x = eye.x + (cam.x - eye.x) * frac;
			t.y = eye.y + (cam.y - eye.y) * frac;
			t.z = eye.z + (cam.z - eye.z) * frac;

			if (cdTestLos04(&eye, tprop->rooms, &t, CDTYPE_BG)) {
				break;
			}
		}

		dist = ZOMBIE_SPEC_BACK * frac;
	}

	if (dist < g_ZombieSpecDist[me]) {
		g_ZombieSpecDist[me] = dist;
	} else {
		g_ZombieSpecDist[me] += (dist - g_ZombieSpecDist[me]) * ease;
	}

	dist = g_ZombieSpecDist[me];

	cam.x = eye.x + back.x * dist;
	cam.y = eye.y + ZOMBIE_SPEC_UP * dist / ZOMBIE_SPEC_BACK;
	cam.z = eye.z + back.z * dist;

	// over their head at where they aim
	look.x = eye.x + aim->x * 300 - cam.x;
	look.y = eye.y + aim->y * 300 - cam.y;
	look.z = eye.z + aim->z * 300 - cam.z;
	len = sqrtf(look.x * look.x + look.y * look.y + look.z * look.z);

	if (len < 1) {
		look = *aim;
	} else {
		look.x /= len;
		look.y /= len;
		look.z /= len;
	}

	zombiesUpFromLook(&look, &up);

	player0f0c1840(&cam, &up, &look, &tprop->pos, tprop->rooms);
}

// The player the current one watches through the eyes of, or -1: whose gun
// player.c draws in this view (bgunRenderWatched())
s32 zombiesFirstPersonWatched(void)
{
	s32 me = g_Vars.currentplayernum;

	if (!zombiesIsActive() || me < 0 || me >= MAX_PLAYERS
			|| !g_ZombieSpectating[me] || !g_ZombieSpecFirst[me]
			|| g_ZombieWatch[me] < 0 || g_ZombieWatch[me] >= PLAYERCOUNT()) {
		return -1;
	}

	return g_ZombieWatch[me];
}

// Whether anyone watches playernum through their eyes, so their gun's pose
// is worth keeping for them (bondgun.c)
bool zombiesIsWatchedFirstPerson(s32 playernum)
{
	s32 i;

	if (!zombiesIsActive()) {
		return false;
	}

	for (i = 0; i < PLAYERCOUNT(); i++) {
		if (i != playernum && g_ZombieSpectating[i] && g_ZombieSpecFirst[i] && g_ZombieWatch[i] == playernum) {
			return true;
		}
	}

	return false;
}

// For chrRender(): a spectator in first person does not see the body they
// are looking out of, nor the gun in its hands
bool zombiesSpectatorHidesProp(struct prop *prop)
{
	s32 me = g_Vars.currentplayernum;
	s32 target;

	if (!zombiesIsActive() || me < 0 || me >= MAX_PLAYERS
			|| !g_ZombieSpectating[me] || !g_ZombieSpecFirst[me]) {
		return false;
	}

	target = g_ZombieWatch[me];

	return target >= 0 && target < PLAYERCOUNT() && g_Vars.players[target]->prop == prop;
}

/**
 * playerTick()'s dead camera, for the current player: 1 when it built the
 * camera, 0 for the game's own death camera (before the death has gone to
 * black, solo, once the round has turned and the new life is coming, and
 * when there is nobody left alive to watch).
 */
s32 zombiesDeadCameraTick(void)
{
	s32 me = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;
	s32 prevwatch;

	if (!zombiesIsActive() || me < 0 || me >= MAX_PLAYERS || zombiesPlayersInPlay() < 2) {
		return 0;
	}

	if (!player->isdead || g_ZombieRespawnDue[me] || g_ZombieGameOver) {
		g_ZombieSpectating[me] = false;
		return 0;
	}

	if (!g_ZombieSpectating[me]) {
		if (!player->redbloodfinished || !player->deathanimfinished
				|| !playerIsFadeComplete() || player->colourscreenfrac < 1) {
			return 0;
		}

		g_ZombieWatch[me] = -1;
		zombiesWatchNext(me);

		if (g_ZombieWatch[me] < 0) {
			return 0;
		}

		g_ZombieSpectating[me] = true;
		g_ZombieSpecHave[me] = false;

		// the picture back from the death's black
		playerSetFadeColour(0, 0, 0, 1);
		playerSetFadeFrac(60, 0);
	}

	prevwatch = g_ZombieWatch[me];

	if (!mpIsPaused()) {
		s32 pad = optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex);

		// fire: the next teammate
		if (joyGetButtonsPressedThisFrame(pad, Z_TRIG)) {
			zombiesWatchNext(me);
		}

		// use (E / A / B): through their eyes, or over their shoulder
		if (joyGetButtonsPressedThisFrame(pad, zombiesUseMask(me))) {
			g_ZombieSpecFirst[me] = !g_ZombieSpecFirst[me];
			g_ZombieSpecHave[me] = false;
		}
	}

	// the one watched has died, or gone down while someone else stands
	if (!zombiesCanWatch(me, g_ZombieWatch[me], false)) {
		zombiesWatchNext(me);
	} else if (!zombiesCanWatch(me, g_ZombieWatch[me], true)) {
		s32 i;

		for (i = 0; i < PLAYERCOUNT(); i++) {
			if (zombiesCanWatch(me, i, true)) {
				g_ZombieWatch[me] = i;
				break;
			}
		}
	}

	if (g_ZombieWatch[me] < 0) {
		return 0;
	}

	// someone new: the follow camera starts behind them, not swinging over
	if (g_ZombieWatch[me] != prevwatch) {
		g_ZombieSpecHave[me] = false;
	}

	if (g_ZombieSpecFirst[me]) {
		zombiesFirstPersonCamera(g_ZombieWatch[me]);
	} else {
		zombiesFollowCamera(me, g_ZombieWatch[me]);
	}

	return 1;
}

// A downed player up again: revived, or by their own Quick Revive
static void zombiesStandUp(s32 playernum, char *msg)
{
	struct player *player = g_Vars.players[playernum];

	zombiesClearDowned(playernum);
	player->bondhealth = g_ZombieMaxHealth[playernum] * 0.5f;
	player->crouchpos = CROUCHPOS_STAND;
	g_ZombieLastHealth[playernum] = player->bondhealth;
	g_ZombieRegenDelay60[playernum] = 0;

	zombiesPlaySound(SFX_PICKUP_SHIELD, 1.2f);
	scenarioCreateHudmsg(playernum, msg);
}

static void zombiesTickDowned(void)
{
	s32 numplayers = PLAYERCOUNT();
	s32 i;
	s32 r;

	for (i = 0; i < numplayers; i++) {
		struct player *player = g_Vars.players[i];

		g_ZombieNearDowned[i] = -1;

		// back in after waiting out a round dead
		if (g_ZombieRespawnDue[i] && !player->isdead) {
			g_ZombieRespawnDue[i] = false;
			g_ZombieLastHealth[i] = player->bondhealth;
			g_ZombieRegenDelay60[i] = 0;
		}

		if (g_ZombieDowned[i] && (player->isdead || !player->prop)) {
			zombiesClearDowned(i);
		}
	}

	// who can reach whom
	for (r = 0; r < numplayers; r++) {
		f32 bestdist = ZOMBIE_REVIVE_RANGE * ZOMBIE_REVIVE_RANGE;

		if (!zombiesPlayerIsUp(r)) {
			continue;
		}

		for (i = 0; i < numplayers; i++) {
			struct prop *a = g_Vars.players[r]->prop;
			struct prop *b = g_Vars.players[i]->prop;
			f32 dx;
			f32 dy;
			f32 dz;

			if (i == r || !g_ZombieDowned[i] || !b) {
				continue;
			}

			dx = a->pos.x - b->pos.x;
			dy = a->pos.y - b->pos.y;
			dz = a->pos.z - b->pos.z;

			if (dy < 150 && dy > -150 && dx * dx + dz * dz < bestdist) {
				bestdist = dx * dx + dz * dz;
				g_ZombieNearDowned[r] = i;
			}
		}
	}

	for (i = 0; i < numplayers; i++) {
		struct player *player = g_Vars.players[i];
		s32 reviver = -1;

		if (!g_ZombieDowned[i]) {
			continue;
		}

		// stays down, and squatting
		player->autocrouchpos = CROUCHPOS_SQUAT;

		if (player->bondhealth < g_ZombieMaxHealth[i] * 0.01f) {
			player->bondhealth = g_ZombieMaxHealth[i] * 0.01f;
		}

		// the one already at it keeps it while they hold on; else whoever
		// beside them is holding B
		if (g_ZombieReviver[i] >= 0 && g_ZombieNearDowned[g_ZombieReviver[i]] == i
				&& zombiesHoldingUse(g_ZombieReviver[i])) {
			reviver = g_ZombieReviver[i];
		} else {
			for (r = 0; r < numplayers; r++) {
				if (g_ZombieNearDowned[r] == i && zombiesHoldingUse(r)) {
					reviver = r;
					break;
				}
			}
		}

		if (reviver != g_ZombieReviver[i]) {
			g_ZombieRevive60[i] = 0;
			g_ZombieReviver[i] = reviver;
		}

		// Quick Revive alone: back up by themselves, and no bleeding meanwhile
		if (g_ZombieSelfRevive60[i] > 0) {
			g_ZombieSelfRevive60[i] -= g_Vars.lvupdate60;

			if (g_ZombieSelfRevive60[i] <= 0) {
				zombiesStandUp(i, "BACK ON YOUR FEET\n");
			}

			continue;
		}

		if (reviver >= 0) {
			// the clock stops while someone is at it; twice as fast for a
			// reviver with Quick Revive
			g_ZombieRevive60[i] += g_Vars.lvupdate60
				* (zombiesPlayerHasPerk(reviver, ZOMBIEPERK_QUICKREVIVE) ? 2 : 1);

			if (g_ZombieRevive60[i] >= ZOMBIE_REVIVE60) {
				char msg[48];

				sprintf(msg, "REVIVED BY PLAYER %d\n", reviver + 1);
				zombiesStandUp(i, msg);
			}

			continue;
		}

		g_ZombieBleed60[i] -= g_Vars.lvupdate60;

		if (g_ZombieBleed60[i] <= 0) {
			// bled out: dead now, and back at the next round
			const s32 prevplayernum = g_Vars.currentplayernum;
			char msg[48];

			zombiesClearDowned(i);

			setCurrentPlayerNum(i);
			playerDie(true);
			setCurrentPlayerNum(prevplayernum);

			sprintf(msg, "PLAYER %d BLED OUT\n", i + 1);
			zombiesTellOthers(i, msg);
		}
	}
}

// The nearest player a zombie can go for: standing, so not down or dead
static struct prop *zombiesNearestTarget(struct chrdata *zchr)
{
	struct prop *best = NULL;
	f32 bestdist = 0;
	s32 i;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		struct prop *prop = g_Vars.players[i]->prop;
		f32 dx;
		f32 dy;
		f32 dz;
		f32 dist;

		if (!zombiesPlayerIsUp(i)) {
			continue;
		}

		dx = prop->pos.x - zchr->prop->pos.x;
		dy = prop->pos.y - zchr->prop->pos.y;
		dz = prop->pos.z - zchr->prop->pos.z;
		dist = dx * dx + dy * dy + dz * dz;

		if (best == NULL || dist < bestdist) {
			best = prop;
			bestdist = dist;
		}
	}

	return best;
}

/**
 * With nobody standing - only alone, down with Quick Revive, since anyone
 * else's going down ends it - each zombie goes and stands on a spawn pad of
 * its own well away from the downed, and comes back once someone is up.
 */
#define ZOMBIE_SCATTER_MIN 1200.0f // how far from every downed player a zombie goes

void botApplyHold(struct chrdata *chr, struct coord *pos, RoomNum *room, f32 angle);

static s32 g_ZombieScatterPad[MAX_BOTS]; // the spawn pad's index each went to, -1 for none

static void zombiesScatter(s32 botnum, struct chrdata *zchr)
{
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	s32 pick = g_ZombieScatterPad[botnum];
	f32 bestfar = -1;
	s32 tries;
	struct pad pad;
	RoomNum rooms[2];

	if (n <= 0) {
		return;
	}

	if (pick >= 0 && zchr->aibot->command == AIBOTCMD_HOLD) {
		return;
	}

	// a random pad far enough from every downed player; the furthest of the
	// tries when none is
	for (tries = 0; tries < 16; tries++) {
		s32 i = rngRandom() % n;
		f32 nearest = 1e20f;
		s32 p;

		padUnpack(g_SpawnPoints[i], PADFIELD_POS, &pad);

		for (p = 0; p < PLAYERCOUNT(); p++) {
			struct prop *prop = g_Vars.players[p]->prop;

			if (prop) {
				f32 dx = pad.pos.x - prop->pos.x;
				f32 dz = pad.pos.z - prop->pos.z;
				f32 d = sqrtf(dx * dx + dz * dz);

				if (d < nearest) {
					nearest = d;
				}
			}
		}

		if (nearest > bestfar) {
			bestfar = nearest;
			pick = i;
		}

		if (nearest >= ZOMBIE_SCATTER_MIN) {
			break;
		}
	}

	padUnpack(g_SpawnPoints[pick], PADFIELD_POS | PADFIELD_ROOM, &pad);
	rooms[0] = pad.room;
	rooms[1] = -1;

	g_ZombieScatterPad[botnum] = pick;
	botApplyHold(zchr, &pad.pos, rooms, 0);
}

// True once nobody is standing and the end has played out: the dead's
// deaths finished, and the downed given a moment to be seen going down
static bool zombiesEveryoneIsDown(void)
{
	s32 i;
	bool anydowned = false;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		struct player *player = g_Vars.players[i];

		// an open seat online is nobody's to go down
		if (g_ZombieOutOfPlay[i]) {
			continue;
		}

		// up, or coming back up on their own (Quick Revive alone)
		if (zombiesPlayerIsUp(i) || (g_ZombieDowned[i] && g_ZombieSelfRevive60[i] > 0)) {
			g_ZombieAllDownFrame = -1;
			return false;
		}

		if (g_ZombieDowned[i]) {
			anydowned = true;
		} else if (player->isdead && !(player->redbloodfinished
					&& player->deathanimfinished
					&& player->colourfadetimemax60 < 0)) {
			return false;
		}
	}

	if (!anydowned) {
		return true;
	}

	if (g_ZombieAllDownFrame < 0) {
		g_ZombieAllDownFrame = g_Vars.lvframenum;
	}

	return g_Vars.lvframenum - g_ZombieAllDownFrame >= ZOMBIE_ALLDOWN_END60;
}

/**
 * Ammo as Call of Duty gives it, counted in each gun's own clips rather than
 * Perfect Dark's capacity for the whole ammo type (800 pistol rounds): a gun
 * from the box, or one Max Ammo fills, carries ZOMBIE_AMMO_CLIPS spare clips
 * (the Falcon's 8 by 64, a CMP150's 32 by 256); the pistol a life starts with
 * one in it and ZOMBIE_AMMO_START_CLIPS spare (8 / 32, the M1911's). Never
 * more than Perfect Dark itself holds, and never less than there already is
 * (guns of one ammo type share it).
 */
#define ZOMBIE_AMMO_CLIPS        8
#define ZOMBIE_AMMO_START_CLIPS  4

s32 zombiesPapClipMult(s32 weaponnum);
#define ZOMBIE_PAP_CLIPS_FILL 10 // a Pack-a-Punched gun's spare clips (ZOMBIE_PAP_CLIPS)

// Every ammunition the current player's weaponnum takes topped up to clips
// clips spare, and one to load when loadclip. A Pack-a-Punched gun's clips
// are its bigger ones, and it is never given fewer than its upgrade's.
static void zombiesFillGun(s32 weaponnum, s32 clips, bool loadclip)
{
	struct weapon *weapon = weaponFindById(weaponnum);
	s32 mult = zombiesPapClipMult(weaponnum);
	s32 j;

	if (weapon == NULL) {
		return;
	}

	if (mult > 1 && clips < ZOMBIE_PAP_CLIPS_FILL) {
		clips = ZOMBIE_PAP_CLIPS_FILL;
	}

	for (j = 0; j < ARRAYCOUNT(weapon->ammos); j++) {
		struct inventory_ammo *ammo = weapon->ammos[j];
		s32 clip;
		s32 want;
		s32 cap;

		if (ammo == NULL || ammo->type <= 0 || (ammo->flags & AMMOFLAG_NORESERVE)) {
			continue;
		}

		clip = (ammo->clipsize > 0 ? ammo->clipsize : 1) * mult;
		want = clip * (clips + (loadclip ? 1 : 0));
		cap = bgunGetCapacityByAmmotype(ammo->type);

		if (want > cap) {
			want = cap;
		}

		if (g_Vars.currentplayer->ammoheldarr[ammo->type] < want) {
			bgunSetAmmoQuantity(ammo->type, want);
		}
	}
}

// For playerSpawnWeapons(): what the pistol a life starts with holds, its
// clip and ZOMBIE_AMMO_START_CLIPS more
s32 zombiesStartAmmo(s32 weaponnum)
{
	struct weapon *weapon = weaponFindById(weaponnum);
	struct inventory_ammo *ammo = weapon ? weapon->ammos[0] : NULL;
	s32 clip = ammo && ammo->clipsize > 0 ? ammo->clipsize : 8;

	return clip * (ZOMBIE_AMMO_START_CLIPS + 1);
}

/**
 * The Mystery Box, as Call of Duty's: 950 points held on B (E) beside it, and
 * the guns go round over it - quick at first, slowing - for five seconds,
 * then one stays, sinking back in for ten. Only the one who paid can take it,
 * with B again, full of ammo. It is a big ammo crate until it has a model of
 * its own, on a spawn pad of its own across the arena from Juggernog.
 *
 * The guns are every gun the Combat Simulator has (g_MpWeapons): not the
 * thrown ones, the mines, the devices or the shield. Each one's model is
 * loaded when the box goes up, while the stage pool has room for it; one
 * that will not load is left out rather than drawn as nothing.
 */
#define ZOMBIE_BOX_MODEL      MODEL_MULTI_AMMO_CRATE
#define ZOMBIE_BOX_LENGTH     160.0f  // how long the box is, in world units
#define ZOMBIE_BOX_COST       950
#define ZOMBIE_BOX_RANGE      140.0f  // how near the box buying and taking work
#define ZOMBIE_BOX_HOLD60     15      // how long B is held to buy or take
#define ZOMBIE_BOX_SPIN60     210     // the guns going round (3.5 seconds)
#define ZOMBIE_BOX_WALLREACH  160.0f  // how far from its pad the box looks for a wall to stand against
#define ZOMBIE_BOX_OFFER60    600     // the one it landed on, waiting to be taken
#define ZOMBIE_BOX_RISE       40.0f   // how far over the lid a gun rises
#define ZOMBIE_BOX_GUNSCALE   1.8f    // the guns bigger than lying on a floor, for the box's size
#define ZOMBIE_BOX_POOLFLOOR  0x40000 // stage pool left alone when loading the guns

#define ZOMBIEBOX_IDLE  0
#define ZOMBIEBOX_SPIN  1
#define ZOMBIEBOX_OFFER 2

static s32 g_ZombieBoxGuns[NUM_MPWEAPONS];   // g_MpWeapons indexes
static s32 g_ZombieBoxNumGuns = 0;
static s32 g_ZombieBoxState = ZOMBIEBOX_IDLE;
static s32 g_ZombieBoxTime60 = 0;
static s32 g_ZombieBoxNextSwap60 = 0;
static s32 g_ZombieBoxUser = -1;
static s32 g_ZombieBoxShown = -1;            // the g_MpWeapons index over the box, -1 for none
static f32 g_ZombieBoxYaw = 0;               // the way the box is long, for the gun over it
static f32 g_ZombieBoxTop = 0;               // the lid's height
static struct defaultobj g_ZombieBoxGunObjs[4];
static s32 g_ZombieBoxGunNext = 0;
static struct defaultobj *g_ZombieBoxGun = NULL;
static bool g_ZombieNearBox[MAX_PLAYERS];
static s32 g_ZombieBoxHeld60[MAX_PLAYERS];
static bool g_ZombieBoxNeedsRelease[MAX_PLAYERS];

static void zombiesResetBox(void)
{
	s32 i;

	g_ZombieBox = NULL;
	g_ZombieBoxGun = NULL;
	g_ZombieBoxNumGuns = 0;

	// a new stage: the last one's props went with it
	bzero(g_ZombieBoxGunObjs, sizeof(g_ZombieBoxGunObjs));
	g_ZombieBoxGunNext = 0;
	g_ZombieBoxState = ZOMBIEBOX_IDLE;
	g_ZombieBoxTime60 = 0;
	g_ZombieBoxUser = -1;
	g_ZombieBoxShown = -1;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ZombieNearBox[i] = false;
		g_ZombieBoxHeld60[i] = 0;
		g_ZombieBoxNeedsRelease[i] = false;
	}
}

// The guns the box can give, their models loaded while there is room
static void zombiesBoxLoadGuns(void)
{
	s32 i;

	g_ZombieBoxNumGuns = 0;

	for (i = 0; i < ARRAYCOUNT(g_MpWeapons); i++) {
		s32 weaponnum = g_MpWeapons[i].weaponnum;
		s32 model = g_MpWeapons[i].model;

		if (weaponnum <= WEAPON_UNARMED || weaponnum == WEAPON_DISABLED || g_MpWeapons[i].priammotype == 0
				|| !g_MpWeapons[i].hasweapon
				|| weaponnum == WEAPON_COMBATKNIFE || weaponnum == WEAPON_GRENADE || weaponnum == WEAPON_NBOMB
				|| weaponnum == WEAPON_TIMEDMINE || weaponnum == WEAPON_PROXIMITYMINE || weaponnum == WEAPON_REMOTEMINE
				// GoldenEye's guns are GE Plus's: on Perfect Dark's arenas they
				// came out of the box as a "Pistol" held huge across the view
				|| WEAPON_IS_GE(weaponnum)
				|| model <= 0 || g_ModelStates[model].fileid == 0) {
			continue;
		}

		if (g_ModelStates[model].modeldef == NULL) {
			if (mempGetStageFreeTotal() < ZOMBIE_BOX_POOLFLOOR) {
				continue;
			}

			setupLoadModeldef(model);

			if (g_ModelStates[model].modeldef == NULL) {
				continue;
			}
		}

		g_ZombieBoxGuns[g_ZombieBoxNumGuns++] = i;
	}
}

static void zombiesPlaceBox(void)
{
	struct defaultobj *obj = &g_ZombieBoxObj;
	struct prop *prop;
	struct coord min;
	struct coord max;

	if (g_ZombieBoxPad < 0) {
		sysLogPrintf(LOG_WARNING, "zombies: no Mystery Box on stage 0x%02x (too few spawn pads)", g_Vars.stagenum);
		return;
	}

	bzero(obj, sizeof(*obj));
	obj->extrascale = 256;
	obj->type = OBJTYPE_BASIC;
	obj->modelnum = ZOMBIE_BOX_MODEL;
	// solid, as the perk machines are (zombiesNewJuggObj())
	obj->flags = OBJFLAG_INVINCIBLE | OBJFLAG_CANNOT_ACTIVATE | OBJFLAG_00000100;
	obj->flags2 = OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_IMMUNETOEXPLOSIONS;
	obj->realrot[0][0] = obj->realrot[1][1] = obj->realrot[2][2] = 1;
	obj->maxdamage = 1000;
	obj->shadecol[0] = obj->shadecol[1] = obj->shadecol[2] = 0xff;
	obj->nextcol[0] = obj->nextcol[1] = obj->nextcol[2] = 0xff;
	obj->floorcol = 0x0fff;
	obj->pad = -1;

	setupLoadModeldef(obj->modelnum);

	if (g_ModelStates[obj->modelnum].modeldef == NULL) {
		sysLogPrintf(LOG_WARNING, "zombies: the Mystery Box's model would not load on stage 0x%02x", g_Vars.stagenum);
		return;
	}

	prop = objInitWithAutoModel(obj);

	if (prop == NULL || obj->model == NULL) {
		sysLogPrintf(LOG_WARNING, "zombies: the Mystery Box would not go up on stage 0x%02x", g_Vars.stagenum);
		return;
	}

	// as long as ZOMBIE_BOX_LENGTH, whatever the crate's own size
	{
		struct modelrodata_bbox *bbox = objFindBboxRodata(obj);

		if (bbox) {
			f32 len = bbox->xmax - bbox->xmin > bbox->zmax - bbox->zmin ? bbox->xmax - bbox->xmin : bbox->zmax - bbox->zmin;

			if (len * obj->model->scale > 0.01f) {
				modelSetScale(obj->model, obj->model->scale * ZOMBIE_BOX_LENGTH / (len * obj->model->scale));
			}
		}
	}

	// Backed onto the nearest wall, as Call of Duty's stands, its long side
	// along the wall and its front to the room; on the pad as the pad looks
	// when there is no wall near enough
	{
		struct modelrodata_bbox *bbox = objFindBboxRodata(obj);
		struct pad pad;
		struct coord pos;
		struct coord eye;
		struct coord wall = {0, 0, 0};
		struct coord look;
		RoomNum rooms[8];
		Mtxf mtx;
		f32 walldist = ZOMBIE_BOX_WALLREACH + 1;
		f32 halfdepth = 25;
		bool longx = true;
		f32 len;
		s32 k;

		padUnpack(g_ZombieBoxPad, PADFIELD_POS | PADFIELD_LOOK | PADFIELD_ROOM, &pad);

		pos = pad.pos;
		rooms[0] = pad.room;
		rooms[1] = -1;

		if (bbox) {
			f32 xlen = bbox->xmax - bbox->xmin;
			f32 zlen = bbox->zmax - bbox->zmin;

			longx = xlen >= zlen;
			halfdepth = (longx ? zlen : xlen) * 0.5f * obj->model->scale;
		}

		// the nearest wall round the pad, a little over the floor
		eye = pos;
		eye.y += 30;

		for (k = 0; k < 16; k++) {
			f32 ang = k * (M_BADTAU / 16);
			f32 d;

			for (d = 10; d <= ZOMBIE_BOX_WALLREACH; d += 10) {
				struct coord to;

				to.x = eye.x + sinf(ang) * d;
				to.y = eye.y;
				to.z = eye.z + cosf(ang) * d;

				if (!cdTestLos04(&eye, rooms, &to, CDTYPE_BG)) {
					if (d < walldist) {
						walldist = d;
						wall.x = sinf(ang);
						wall.z = cosf(ang);
					}

					break;
				}
			}
		}

		if (walldist <= ZOMBIE_BOX_WALLREACH) {
			f32 move = walldist - 10 - halfdepth;

			// up to the wall, leaving a hand's width
			if (move > 0) {
				struct coord to;
				RoomNum torooms[8];

				to.x = pos.x + wall.x * move;
				to.y = pos.y;
				to.z = pos.z + wall.z * move;

				func0f065e74(&pos, rooms, &to, torooms);

				if (torooms[0] > 0) {
					pos = to;
					memcpy(rooms, torooms, sizeof(rooms));
				}
			}

			// the short side faces the room: z away from the wall when the
			// crate is long in x, along the wall when it is long in z
			if (longx) {
				look.x = -wall.x;
				look.z = -wall.z;
			} else {
				look.x = wall.z;
				look.z = -wall.x;
			}
		} else {
			look.x = pad.look.x;
			look.z = pad.look.z;
		}

		len = sqrtf(look.x * look.x + look.z * look.z);

		if (len < 0.001f) {
			look.x = 0;
			look.z = 1;
			len = 1;
		}

		look.x /= len;
		look.z /= len;

		mtx00016d58(&mtx, 0, 0, 0, look.x, 0, look.z, 0, 1, 0);
		mtx00015f04(obj->model->scale, &mtx);

		func0f06a730(obj, &pos, &mtx, rooms, &pos);

		// which way the box is long, in the world, for the gun over it
		{
			f32 *axis = longx ? obj->realrot[0] : obj->realrot[2];
			f32 alen = sqrtf(axis[0] * axis[0] + axis[2] * axis[2]);

			g_ZombieBoxYaw = alen > 0.0001f ? atan2f(axis[0] / alen, axis[2] / alen) : 0;
		}

		sysLogPrintf(LOG_NOTE, "zombies: Mystery Box %s", walldist <= ZOMBIE_BOX_WALLREACH ? "backed onto a wall" : "on its pad, no wall near");
	}

	propActivate(prop);
	propEnable(prop);

	obj->hidden2 &= ~OBJH2FLAG_CANREGEN;
	g_ZombieBox = obj;

	g_ZombieBoxTop = obj->prop->pos.y + 20;

	if (setupGetObjBbox(obj, &obj->prop->pos, obj->realrot, &min, &max)) {
		g_ZombieBoxTop = max.y;
	}

	zombiesBoxLoadGuns();

	sysLogPrintf(LOG_NOTE, "zombies: Mystery Box on stage 0x%02x at %.0f %.0f %.0f, %d guns, stage pool %u free",
			g_Vars.stagenum, obj->prop->pos.x, obj->prop->pos.y, obj->prop->pos.z,
			g_ZombieBoxNumGuns, mempGetStageFreeTotal());
}

// The gun over the box goes
static void zombiesBoxHideGun(void)
{
	if (g_ZombieBoxGun && g_ZombieBoxGun->prop) {
		g_ZombieBoxGun->hidden |= OBJHFLAG_DELETING;
	}

	g_ZombieBoxGun = NULL;
	g_ZombieBoxShown = -1;
}

// Where the gun over the box is now: up out of the lid while the guns go
// round, sinking back in while it waits; level along the box, standing up as
// it would in a hand
static void zombiesBoxPlaceGun(void)
{
	struct defaultobj *obj = g_ZombieBoxGun;
	struct coord pos;
	Mtxf mtx;
	f32 rise;

	if (obj == NULL || obj->prop == NULL || obj->model == NULL || g_ZombieBox == NULL || g_ZombieBox->prop == NULL) {
		return;
	}

	if (g_ZombieBoxState == ZOMBIEBOX_SPIN) {
		f32 frac = g_ZombieBoxTime60 / 60.0f;

		rise = ZOMBIE_BOX_RISE * (frac > 1 ? 1 : frac);
	} else {
		rise = ZOMBIE_BOX_RISE * (1.0f - 0.85f * g_ZombieBoxTime60 / (f32)ZOMBIE_BOX_OFFER60);
	}

	pos.x = g_ZombieBox->prop->pos.x;
	pos.y = g_ZombieBoxTop + 8 + rise;
	pos.z = g_ZombieBox->prop->pos.z;

	// A gun's model (g_SkelChrGun) lies on its side with its barrel along x,
	// as it lies on a floor: a quarter turn about x stands it up, grip and
	// magazine down, and a quarter turn more about y puts its barrel along
	// the box, side on to whoever is in front of it
	{
		Mtxf yaw;
		Mtxf roll;

		mtx4LoadYRotation(g_ZombieBoxYaw + M_BADPI / 2, &yaw);
		mtx4LoadXRotation(-M_BADPI / 2, &roll);
		mtx4MultMtx4(&yaw, &roll, &mtx);
	}

	mtx00015f04(obj->model->scale, &mtx);

	func0f06a580(obj, &pos, &mtx, g_ZombieBox->prop->rooms);
}

// A gun's model carries its muzzle flash, which a chr holding it shows while
// firing; a model's rwdata starts with it showing. The star flash is under a
// toggle of its own (weaponSetGunfireVisible()). A gun shown on its own - over
// the box, on a wall - has both put out.
static void zombiesHideGunFlash(struct defaultobj *obj)
{
	struct modelnode *node = modelGetPart(obj->model->definition, MODELPART_CHRGUN_0002);

	if (node && (node->type & 0xff) == MODELNODETYPE_TOGGLE) {
		union modelrwdata *rwdata = modelGetNodeRwData(obj->model, node);

		if (rwdata) {
			rwdata->toggle.visible = false;
		}
	}

	node = obj->model->definition->rootnode;

	while (node) {
		if ((node->type & 0xff) == MODELNODETYPE_CHRGUNFIRE) {
			union modelrwdata *rwdata = modelGetNodeRwData(obj->model, node);

			if (rwdata) {
				rwdata->chrgunfire.visible = false;
			}
		}

		if (node->child) {
			node = node->child;
		} else {
			while (node) {
				if (node->next) {
					node = node->next;
					break;
				}

				node = node->parent;
			}
		}
	}
}

// The gun g_MpWeapons[index] over the box in place of the one there
static void zombiesBoxShowGun(s32 index)
{
	struct defaultobj *obj = &g_ZombieBoxGunObjs[g_ZombieBoxGunNext];
	struct prop *prop;

	// one still being taken away is not taken again yet
	if (obj->prop) {
		return;
	}

	zombiesBoxHideGun();

	g_ZombieBoxGunNext = (g_ZombieBoxGunNext + 1) % ARRAYCOUNT(g_ZombieBoxGunObjs);

	bzero(obj, sizeof(*obj));
	obj->extrascale = 256;
	obj->type = OBJTYPE_BASIC;
	obj->modelnum = g_MpWeapons[index].model;
	obj->flags = OBJFLAG_INVINCIBLE | OBJFLAG_CANNOT_ACTIVATE | OBJFLAG_UNCOLLECTABLE;
	obj->flags2 = OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_IMMUNETOEXPLOSIONS | OBJFLAG2_SHOOTTHROUGH
		| OBJFLAG2_THROWTHROUGH | OBJFLAG2_NOFALL;
	obj->flags3 = OBJFLAG3_WALKTHROUGH;
	obj->realrot[0][0] = obj->realrot[1][1] = obj->realrot[2][2] = 1;
	obj->maxdamage = 1000;
	obj->shadecol[0] = obj->shadecol[1] = obj->shadecol[2] = 0xff;
	obj->nextcol[0] = obj->nextcol[1] = obj->nextcol[2] = 0xff;
	obj->floorcol = 0x0fff;
	obj->pad = -1;

	if (g_ModelStates[obj->modelnum].modeldef == NULL) {
		return;
	}

	prop = objInitWithAutoModel(obj);

	if (prop == NULL || obj->model == NULL) {
		return;
	}

	modelSetScale(obj->model, obj->model->scale * ZOMBIE_BOX_GUNSCALE
			* (g_MpWeapons[index].extrascale * (1.0f / 256.0f)));

	zombiesHideGunFlash(obj);

	g_ZombieBoxGun = obj;
	g_ZombieBoxShown = index;

	zombiesBoxPlaceGun();

	propActivate(prop);
	propEnable(prop);

	obj->hidden2 &= ~OBJH2FLAG_CANREGEN;
}

// A gun for the box to land on: not the one showing, and if it can be helped
// not one the player already has
static s32 zombiesBoxPick(s32 playernum, bool final)
{
	s32 tries;

	if (g_ZombieBoxNumGuns <= 0) {
		return -1;
	}

	for (tries = 0; tries < 24; tries++) {
		s32 index = g_ZombieBoxGuns[rngRandom() % g_ZombieBoxNumGuns];

		if (g_ZombieBoxNumGuns > 1 && index == g_ZombieBoxShown) {
			continue;
		}

		if (final && playernum >= 0) {
			const s32 prevplayernum = g_Vars.currentplayernum;
			bool has;

			setCurrentPlayerNum(playernum);
			has = invHasSingleWeaponIncAllGuns(g_MpWeapons[index].weaponnum);
			setCurrentPlayerNum(prevplayernum);

			if (has) {
				continue;
			}
		}

		return index;
	}

	return g_ZombieBoxGuns[rngRandom() % g_ZombieBoxNumGuns];
}

/**
 * Two guns, as Call of Duty's: one bought off a wall or taken from the box
 * while two are held takes the place of the one in hand. Mule Kick makes it
 * three. Going down loses Mule Kick and the third gun with it, which is kept
 * aside and given back, full, when Mule Kick is drunk again.
 *
 * The guns are listed in the order they were had - the inventory's own order
 * is the weapons' numbers - so the third is the last one had. Fists, the
 * knife and anything thrown are not guns here. The list is brought up to date
 * from the inventory whenever it is asked (a gun picked up or lost some other
 * way is not missed): one gone comes off, one new goes on the end.
 */
#define ZOMBIE_GUNS_MAX 8

static s32 g_ZombieGuns[MAX_PLAYERS][ZOMBIE_GUNS_MAX];
static s32 g_ZombieGunCount[MAX_PLAYERS];
static s32 g_ZombieMuleGun[MAX_PLAYERS]; // the third gun lost going down, WEAPON_NONE for none

static bool zombiesIsSlotGun(s32 weaponnum)
{
	return weaponnum > WEAPON_UNARMED
		&& weaponnum != WEAPON_COMBATKNIFE
		&& weaponnum != WEAPON_GRENADE
		&& weaponnum != WEAPON_NBOMB
		&& weaponnum != WEAPON_TIMEDMINE
		&& weaponnum != WEAPON_PROXIMITYMINE
		&& weaponnum != WEAPON_REMOTEMINE;
}

static s32 zombiesGunLimit(s32 playernum)
{
	return zombiesPlayerHasPerk(playernum, ZOMBIEPERK_MULEKICK) ? 3 : 2;
}

static void zombiesResetGuns(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ZombieGunCount[i] = 0;
		g_ZombieMuleGun[i] = WEAPON_NONE;
	}
}

// The current player's, who is playernum
static void zombiesSyncGuns(s32 playernum)
{
	s32 *guns = g_ZombieGuns[playernum];
	s32 n = 0;
	s32 count = invGetCount();
	s32 i;
	s32 j;

	for (i = 0; i < g_ZombieGunCount[playernum]; i++) {
		if (invHasSingleWeaponIncAllGuns(guns[i])) {
			guns[n++] = guns[i];
		}
	}

	for (i = 0; i < count && n < ZOMBIE_GUNS_MAX; i++) {
		s32 weaponnum = invGetWeaponNumByIndex(i);
		bool listed = false;

		if (!zombiesIsSlotGun(weaponnum)) {
			continue;
		}

		for (j = 0; j < n; j++) {
			if (guns[j] == weaponnum) {
				listed = true;
			}
		}

		if (!listed) {
			guns[n++] = weaponnum;
		}
	}

	g_ZombieGunCount[playernum] = n;
}

// The gun the current player holds, or is about to: through a knife swing,
// the one it gives back
static s32 zombiesHeldGun(s32 playernum)
{
	struct gunctrl *ctrl = &g_Vars.currentplayer->gunctrl;

	if (g_ZombieKnifeStage[playernum] != ZKNIFE_NONE) {
		return g_ZombieKnifeBack[playernum][0];
	}

	return ctrl->switchtoweaponnum >= 0 ? ctrl->switchtoweaponnum : ctrl->weaponnum;
}

static void zombiesDropListedGun(s32 playernum, s32 weaponnum)
{
	s32 *guns = g_ZombieGuns[playernum];
	s32 n = 0;
	s32 i;

	invRemoveItemByNum(weaponnum);

	for (i = 0; i < g_ZombieGunCount[playernum]; i++) {
		if (guns[i] != weaponnum) {
			guns[n++] = guns[i];
		}
	}

	g_ZombieGunCount[playernum] = n;
}

// Room for one more gun for the current player: the one in hand gives way
// when the hands are full, or the last one had when the one in hand is no gun
static void zombiesMakeRoomForGun(s32 playernum)
{
	s32 held = zombiesHeldGun(playernum);

	zombiesSyncGuns(playernum);

	while (g_ZombieGunCount[playernum] >= zombiesGunLimit(playernum) && g_ZombieGunCount[playernum] > 0) {
		s32 victim = g_ZombieGuns[playernum][g_ZombieGunCount[playernum] - 1];
		s32 i;

		for (i = 0; i < g_ZombieGunCount[playernum]; i++) {
			if (g_ZombieGuns[playernum][i] == held) {
				victim = held;
			}
		}

		zombiesDropListedGun(playernum, victim);
		held = WEAPON_NONE;
	}
}

// Going down with Mule Kick: its third gun is put aside
static void zombiesLoseMuleGun(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	s32 lost;

	if (!g_ZombieHasPerk[playernum][ZOMBIEPERK_MULEKICK]) {
		return;
	}

	setCurrentPlayerNum(playernum);
	zombiesSyncGuns(playernum);

	if (g_ZombieGunCount[playernum] > 2) {
		bool inhand = zombiesHeldGun(playernum) == g_ZombieGuns[playernum][g_ZombieGunCount[playernum] - 1];

		lost = g_ZombieGuns[playernum][g_ZombieGunCount[playernum] - 1];
		g_ZombieMuleGun[playernum] = lost;
		zombiesDropListedGun(playernum, lost);

		// out of the hand too, for the first gun had
		if (inhand) {
			if (g_ZombieKnifeStage[playernum] != ZKNIFE_NONE) {
				g_ZombieKnifeBack[playernum][0] = g_ZombieGuns[playernum][0];
				g_ZombieKnifeBack[playernum][1] = WEAPON_NONE;
			} else {
				bgunEquipHands(g_ZombieGuns[playernum][0], WEAPON_NONE);
			}
		}

		sysLogPrintf(LOG_NOTE, "zombies: player %d went down and lost Mule Kick's %s", playernum + 1, bgunGetName(lost));
	}

	setCurrentPlayerNum(prevplayernum);
}

// Mule Kick drunk again: the third gun lost with it comes back, full
static void zombiesReturnMuleGun(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	s32 weaponnum = g_ZombieMuleGun[playernum];

	g_ZombieMuleGun[playernum] = WEAPON_NONE;

	if (weaponnum <= WEAPON_NONE) {
		return;
	}

	setCurrentPlayerNum(playernum);

	if (!invHasSingleWeaponIncAllGuns(weaponnum)) {
		zombiesMakeRoomForGun(playernum);
		invGiveSingleWeapon(weaponnum);
		zombiesFillGun(weaponnum, ZOMBIE_AMMO_CLIPS, true);
		zombiesSyncGuns(playernum);
		sysLogPrintf(LOG_NOTE, "zombies: player %d has Mule Kick's %s back", playernum + 1, bgunGetName(weaponnum));
	}

	setCurrentPlayerNum(prevplayernum);
}

/**
 * Pack-a-Punch, as Call of Duty's: a machine of its own (the perk machines'
 * model, lit purple, in the openest corner left: zombiesPlacePap()), where B
 * (E) held with a gun in hand and 5000 points puts the gun in for a few
 * seconds - it goes down off the view as a perk's drink takes it - and gives
 * it back upgraded:
 *
 * - a name of its own (zombiesPapName(); bondgun.c shows it),
 * - twice the damage (zombiesBulletScale()),
 * - a magazine twice as big (bondgun.c's clip sizes, zombiesPapClipMult()),
 * - and full, with ZOMBIE_PAP_CLIPS of the bigger magazine spare; Max Ammo and
 *   the wall buys fill it to that from then on (zombiesFillGun()).
 *
 * A gun is upgraded once. The upgrade is the player's gun's, and goes with
 * it: a gun traded away or lost and had again is a plain one. Its look is
 * an animated camo (zombiesCamoBegin()).
 *
 * As Call of Duty's, the gun is the machine's while it works: it goes out of
 * the hands (the next gun comes up, or none) and slides into the machine,
 * then slides back out of it upgraded and waits there, sinking back in, for
 * its owner to hold B (E) and take it. Not taken in ZOMBIE_PAP_OFFER60 it is
 * gone. One gun at a time is in the machine, and only its owner takes it.
 */
#define ZOMBIE_PAP_COST     5000
#define ZOMBIE_PAP_DOWN60   18   // the gun goes down out of the hands
#define ZOMBIE_PAP_WORK60   180  // in the machine
#define ZOMBIE_PAP_OFFER60  900  // out of it, upgraded, to be taken: Black Ops II's fifteen seconds
#define ZOMBIE_PAP_SLIDE60  30   // sliding in or out
#define ZOMBIE_PAP_OFFERAT  (ZOMBIE_PAP_DOWN60 + ZOMBIE_PAP_WORK60)
#define ZOMBIE_PAP_TOTAL60  (ZOMBIE_PAP_OFFERAT + ZOMBIE_PAP_OFFER60)
#define ZOMBIE_PAP_DAMAGE   2.0f
#define ZOMBIE_PAP_CLIPMULT 2
#define ZOMBIE_PAP_PITCH    1.5f // its shots' pitch
#define ZOMBIE_PAP_CLIPS    10
#define ZOMBIE_PAP_MAX      8
#define ZOMBIE_PAP_RADAR    0xb040ff00

static s32 g_ZombiePap60[MAX_PLAYERS];         // into the machine, -1 for not
static s32 g_ZombiePapGun[MAX_PLAYERS];        // the gun in it

// a gun in the machine, anybody's: its drums turn faster (zombiesMeshSpin())
static bool zombiesPapInUse(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (g_ZombiePap60[i] >= 0) {
			return true;
		}
	}

	return false;
}
static s32 g_ZombiePapped[MAX_PLAYERS][ZOMBIE_PAP_MAX];
static s32 g_ZombiePapCount[MAX_PLAYERS];
static bool g_ZombieNearPap[MAX_PLAYERS];
static s32 g_ZombiePapHeld60[MAX_PLAYERS];
static bool g_ZombiePapNeedsRelease[MAX_PLAYERS];

struct zombiepapname {
	s32 weaponnum;
	const char *name;
};

// Call of Duty names its upgrades; these are Perfect Dark's guns'. Each ends
// in a newline as the game's own names do: textMeasure() gives a line with
// none no height, and the HUD put the name down over the ammo.
static const struct zombiepapname g_ZombiePapNames[] = {
	{ WEAPON_FALCON2,          "Fallen Falcon\n" },
	{ WEAPON_FALCON2_SILENCER, "Silent Talon\n" },
	{ WEAPON_FALCON2_SCOPE,    "Falcon's Eye\n" },
	{ WEAPON_MAGSEC4,          "MagSec Overload\n" },
	{ WEAPON_MAULER,           "The Mangler\n" },
	{ WEAPON_PHOENIX,          "Ashen Phoenix\n" },
	{ WEAPON_DY357MAGNUM,      "Judgement Day\n" },
	{ WEAPON_DY357LX,          "Hand of Judgement\n" },
	{ WEAPON_CMP150,           "Cerberus 150\n" },
	{ WEAPON_CYCLONE,          "Maelstrom\n" },
	{ WEAPON_CALLISTO,         "Callisto Eclipse\n" },
	{ WEAPON_RCP120,           "RC-P1200\n" },
	{ WEAPON_LAPTOPGUN,        "Overclocked Laptop\n" },
	{ WEAPON_DRAGON,           "The Wyrm\n" },
	{ WEAPON_K7AVENGER,        "Avenger Prime\n" },
	{ WEAPON_AR34,             "AR-3400\n" },
	{ WEAPON_SUPERDRAGON,      "The Hydra\n" },
	{ WEAPON_SHOTGUN,          "Boomstick\n" },
	{ WEAPON_REAPER,           "The Grim Reaper\n" },
	{ WEAPON_SNIPERRIFLE,      "Long Silence\n" },
	{ WEAPON_FARSIGHT,         "Omniscience\n" },
	{ WEAPON_DEVASTATOR,       "Annihilator\n" },
	{ WEAPON_ROCKETLAUNCHER,   "Rocket Rain\n" },
	{ WEAPON_SLAYER,           "The Slaughterer\n" },
	{ WEAPON_CROSSBOW,         "Silent Death\n" },
	{ WEAPON_TRANQUILIZER,     "Lullaby\n" },
	{ WEAPON_LASER,            "Sun Spear\n" },
	{ WEAPON_PP9I,             "PP9-Infinity\n" },
	{ WEAPON_CC13,             "CC-1300\n" },
	{ WEAPON_KL01313,          "KL-Overdrive\n" },
	{ WEAPON_KF7SPECIAL,       "KF7 Ultimate\n" },
	{ WEAPON_ZZT,              "ZZT Zero\n" },
	{ WEAPON_DMC,              "DMC Devil\n" },
	{ WEAPON_AR53,             "AR-5300\n" },
	{ WEAPON_RCP45,            "RC-P4500\n" },
};

// The gun in the machine, shown as the box shows its (zombiesBoxShowGun())
static struct defaultobj g_ZombiePapGunObj;
static bool g_ZombiePapGunUp = false;

static void zombiesResetPap(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ZombiePap60[i] = -1;
		g_ZombiePapGun[i] = WEAPON_NONE;
		g_ZombiePapCount[i] = 0;
		g_ZombieNearPap[i] = false;
		g_ZombiePapHeld60[i] = 0;
		g_ZombiePapNeedsRelease[i] = false;
	}

	// a new match's stage has none of the last one's props
	bzero(&g_ZombiePapGunObj, sizeof(g_ZombiePapGunObj));
	g_ZombiePapGunUp = false;
}

static void zombiesPapHideGun(void)
{
	if (g_ZombiePapGunUp && g_ZombiePapGunObj.prop) {
		g_ZombiePapGunObj.hidden |= OBJHFLAG_DELETING;
	}

	g_ZombiePapGunUp = false;
}

/**
 * The gun in the machine where it is at t (g_ZombiePap60): sliding in out of
 * the front of the middle drum's mouth, hidden inside while it works, sliding
 * back out, and sinking slowly back in while it waits to be taken, so the
 * time left shows as the box's gun shows it. Side on, as the box holds its.
 */
static void zombiesPapPlaceGun(s32 t)
{
	// Buried's machine: its middle drum's mouth, a metre up, and the gun out
	// in front of it (the OBJ's centimetres, the front -y), clear of the tray
	static const f32 inside[3] = { 0, -5, 106 };
	static const f32 outside[3] = { 0, -52, 106 };
	struct defaultobj *obj = &g_ZombiePapGunObj;
	struct coord pos;
	struct coord front;
	f32 at[3];
	f32 out;
	Mtxf mtx;
	s32 i;

	if (!g_ZombiePapGunUp || !obj->prop || !obj->model || !g_ZombiePapPlaced) {
		return;
	}

	if (t < ZOMBIE_PAP_OFFERAT) {
		f32 f = (f32)(t - ZOMBIE_PAP_DOWN60) / ZOMBIE_PAP_SLIDE60;

		out = f >= 1 ? 0 : 1 - f;
	} else {
		s32 u = t - ZOMBIE_PAP_OFFERAT;

		if (u < ZOMBIE_PAP_SLIDE60) {
			out = (f32)u / ZOMBIE_PAP_SLIDE60;
		} else {
			out = 1 - 0.75f * (u - ZOMBIE_PAP_SLIDE60) / (f32)(ZOMBIE_PAP_OFFER60 - ZOMBIE_PAP_SLIDE60);
		}
	}

	out = out < 0 ? 0 : out > 1 ? 1 : out;

	for (i = 0; i < 3; i++) {
		at[i] = inside[i] + (outside[i] - inside[i]) * out;
	}

	if (!zombiesMachineMeshPoint(ZOMBIE_PAP_ID, &g_ZombiePapObj, at, &pos, &front)) {
		// the stock mainframe: in front of it, at waist height
		front.x = g_ZombiePapObj.realrot[2][0];
		front.y = 0;
		front.z = g_ZombiePapObj.realrot[2][2];

		{
			f32 len = sqrtf(front.x * front.x + front.z * front.z);

			front.x = len > 0 ? front.x / len : 0;
			front.z = len > 0 ? front.z / len : 1;
		}

		pos = g_ZombiePapObj.prop->pos;
		pos.x += front.x * 60 * out;
		pos.y += 100;
		pos.z += front.z * 60 * out;
	}

	// inside the machine there is nothing to see
	if (out <= 0.05f) {
		obj->flags2 |= OBJFLAG2_INVISIBLE;
	} else {
		obj->flags2 &= ~OBJFLAG2_INVISIBLE;
	}

	// stood up, its barrel across the machine's front (zombiesWallBuyShowGun()
	// turns its gun along the wall the same way)
	{
		Mtxf yaw;
		Mtxf roll;

		mtx4LoadYRotation(atan2f(-front.z, front.x) + M_BADPI / 2, &yaw);
		mtx4LoadXRotation(-M_BADPI / 2, &roll);
		mtx4MultMtx4(&yaw, &roll, &mtx);
	}

	mtx00015f04(obj->model->scale, &mtx);
	func0f06a580(obj, &pos, &mtx, g_ZombiePapObj.prop->rooms);
}

// The gun weaponnum into the machine, to be seen going in and coming out
static void zombiesPapShowGun(s32 weaponnum)
{
	struct defaultobj *obj = &g_ZombiePapGunObj;
	struct prop *prop;
	s32 index;

	zombiesPapHideGun();

	// one still being taken away is not taken again yet
	if (obj->prop || !g_ZombiePapPlaced || !g_ZombiePapObj.prop) {
		return;
	}

	for (index = 0; index < NUM_MPWEAPONS; index++) {
		if (g_MpWeapons[index].weaponnum == weaponnum && g_MpWeapons[index].model > 0) {
			break;
		}
	}

	if (index >= NUM_MPWEAPONS) {
		return;
	}

	bzero(obj, sizeof(*obj));
	obj->extrascale = 256;
	obj->type = OBJTYPE_BASIC;
	obj->modelnum = g_MpWeapons[index].model;
	obj->flags = OBJFLAG_INVINCIBLE | OBJFLAG_CANNOT_ACTIVATE | OBJFLAG_UNCOLLECTABLE;
	obj->flags2 = OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_IMMUNETOEXPLOSIONS | OBJFLAG2_SHOOTTHROUGH
		| OBJFLAG2_THROWTHROUGH | OBJFLAG2_NOFALL;
	obj->flags3 = OBJFLAG3_WALKTHROUGH;
	obj->realrot[0][0] = obj->realrot[1][1] = obj->realrot[2][2] = 1;
	obj->maxdamage = 1000;
	obj->shadecol[0] = obj->shadecol[1] = obj->shadecol[2] = 0xff;
	obj->nextcol[0] = obj->nextcol[1] = obj->nextcol[2] = 0xff;
	obj->floorcol = 0x0fff;
	obj->pad = -1;

	if (g_ModelStates[obj->modelnum].modeldef == NULL) {
		if (mempGetStageFreeTotal() < ZOMBIE_BOX_POOLFLOOR) {
			return;
		}

		setupLoadModeldef(obj->modelnum);

		if (g_ModelStates[obj->modelnum].modeldef == NULL) {
			return;
		}
	}

	prop = objInitWithAutoModel(obj);

	if (prop == NULL || obj->model == NULL) {
		return;
	}

	modelSetScale(obj->model, obj->model->scale * ZOMBIE_BOX_GUNSCALE
			* (g_MpWeapons[index].extrascale * (1.0f / 256.0f)));
	zombiesHideGunFlash(obj);

	g_ZombiePapGunUp = true;
	zombiesPapPlaceGun(ZOMBIE_PAP_DOWN60);

	propActivate(prop);
	propEnable(prop);

	obj->hidden2 &= ~OBJH2FLAG_CANREGEN;
}

static bool zombiesGunIsPapped(s32 playernum, s32 weaponnum)
{
	s32 i;

	if (playernum < 0 || playernum >= MAX_PLAYERS || weaponnum <= WEAPON_UNARMED) {
		return false;
	}

	for (i = 0; i < g_ZombiePapCount[playernum]; i++) {
		if (g_ZombiePapped[playernum][i] == weaponnum) {
			return true;
		}
	}

	return false;
}

// bondgun.c: the gun's name on the HUD, its upgrade's when it has one
const char *zombiesPapName(s32 weaponnum, const char *name)
{
	s32 i;

	if (!zombiesIsActive() || !zombiesGunIsPapped(g_Vars.currentplayernum, weaponnum)) {
		return name;
	}

	for (i = 0; i < ARRAYCOUNT(g_ZombiePapNames); i++) {
		if (g_ZombiePapNames[i].weaponnum == weaponnum) {
			return g_ZombiePapNames[i].name;
		}
	}

	return "Pack-a-Punched\n";
}

// bondgun.c: how many times its own the current player's weaponnum's
// magazine is
s32 zombiesPapClipMult(s32 weaponnum)
{
	if (zombiesIsActive() && zombiesGunIsPapped(g_Vars.currentplayernum, weaponnum)) {
		return ZOMBIE_PAP_CLIPMULT;
	}

	return 1;
}

// bondgun.c: the pitch the current player's weaponnum fires at, an
// upgraded gun's higher
f32 zombiesPapPitch(s32 weaponnum)
{
	if (zombiesIsActive() && zombiesGunIsPapped(g_Vars.currentplayernum, weaponnum)) {
		return ZOMBIE_PAP_PITCH;
	}

	return 1.0f;
}

/**
 * bondgun.c, round the first person gun's model: a Pack-a-Punched gun drawn in
 * its animated camo, Call of Duty's (Black Ops 3's moving ones). The renderer
 * draws it (SHADER_OPT_CAMO, port/fast3d/gfx_cc.cpp): G_SETGRAYSCALE_EXT 2,
 * and the grayscale colour carrying the camo's phase, a 16 bit turn in r and
 * g, and its strength in a. One loop every ZOMBIE_CAMO_LOOP60.
 */
#define ZOMBIE_CAMO_LOOP60 480 // eight seconds

// The camo on, at this moment's phase
static Gfx *zombiesCamoAt(Gfx *gdl)
{
	u32 phase = (u32)(g_Vars.lvframe60 % ZOMBIE_CAMO_LOOP60) * 65536 / ZOMBIE_CAMO_LOOP60;

	gDPSetGrayscaleColorEXT(gdl++, (phase >> 8) & 0xff, phase & 0xff, 0, 0xff);
	gDPGrayscaleEXT(gdl++, 2);

	return gdl;
}

Gfx *zombiesCamoBegin(Gfx *gdl, s32 weaponnum)
{
	if (!zombiesIsActive() || !zombiesGunIsPapped(g_Vars.currentplayernum, weaponnum)) {
		return gdl;
	}

	return zombiesCamoAt(gdl);
}

// The gun in Pack-a-Punch once it has come out upgraded: drawn in its camo
// (zombiesRenderMachine())
static bool zombiesPapGunInCamo(struct defaultobj *obj)
{
	s32 i;

	if (obj != &g_ZombiePapGunObj || !g_ZombiePapGunUp || !obj->model) {
		return false;
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (g_ZombiePap60[i] >= ZOMBIE_PAP_OFFERAT) {
			return true;
		}
	}

	return false;
}

Gfx *zombiesCamoEnd(Gfx *gdl, s32 weaponnum)
{
	if (!zombiesIsActive() || !zombiesGunIsPapped(g_Vars.currentplayernum, weaponnum)) {
		return gdl;
	}

	gDPGrayscaleEXT(gdl++, 0);

	return gdl;
}

// The machine up: against the wall of the pad furthest from every other
// machine and the box, for zombiesCornerMachine() to put in the openest
// corner left. Called once the others are placed.
static bool zombiesPlacePap(void)
{
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	f32 bestscore = -1;
	s32 best = -1;
	f32 bestwalldist = 0;
	struct coord bestwall;
	struct pad pad;
	s32 i;

	g_ZombiePapPlaced = false;
	bzero(&g_ZombiePapObj, sizeof(g_ZombiePapObj));

	for (i = 0; i < n; i++) {
		RoomNum rooms[2];
		f32 walldist;
		struct coord wall;
		f32 score = 1e20f;
		s32 k;

		padUnpack(g_SpawnPoints[i], PADFIELD_POS | PADFIELD_ROOM, &pad);

		if (pad.room <= 0) {
			continue;
		}

		rooms[0] = pad.room;
		rooms[1] = -1;

		if (!zombiesFindPerkWall(&pad.pos, rooms, ZOMBIE_PERK_PADCLEAR, ZOMBIE_PERK_WALLREACH, &walldist, &wall)) {
			continue;
		}

		for (k = 0; k < ZOMBIE_MACHINE_IDS; k++) {
			struct defaultobj *other = zombiesMachineObjAt(k);

			if (other && other->prop) {
				f32 dx = pad.pos.x - other->prop->pos.x;
				f32 dz = pad.pos.z - other->prop->pos.z;

				if (dx * dx + dz * dz < score) {
					score = dx * dx + dz * dz;
				}
			}
		}

		if (score > bestscore) {
			bestscore = score;
			best = i;
			bestwalldist = walldist;
			bestwall = wall;
		}
	}

	if (best < 0) {
		sysLogPrintf(LOG_WARNING, "zombies: no wall near enough a pad for Pack-a-Punch on stage 0x%02x", g_Vars.stagenum);
		return false;
	}

	padUnpack(g_SpawnPoints[best], PADFIELD_POS | PADFIELD_ROOM, &pad);

	if (!zombiesPlaceMachineObj(ZOMBIE_PAP_ID, &g_ZombiePapObj, &pad, bestwalldist, &bestwall)) {
		return false;
	}

	g_ZombiePapPlaced = true;

	return true;
}

// The machine lit purple, whatever light it stands in
static void zombiesTintPap(void)
{
	struct defaultobj *obj = &g_ZombiePapObj;

	if (g_ZombiePapPlaced && obj->prop && obj->model) {
		obj->shadecol[0] = obj->nextcol[0] = 0xb0;
		obj->shadecol[1] = obj->nextcol[1] = 0x70;
		obj->shadecol[2] = obj->nextcol[2] = 0xff;
		obj->shadecol[3] = obj->nextcol[3] = 0xff;
	}
}

// The gun given back: upgraded, its magazine the bigger one and full, and
// its spare clips with it
static void zombiesPapUpgrade(s32 playernum, s32 weaponnum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	struct player *player;
	s32 h;
	s32 j;

	if (zombiesGunIsPapped(playernum, weaponnum) || g_ZombiePapCount[playernum] >= ZOMBIE_PAP_MAX) {
		return;
	}

	setCurrentPlayerNum(playernum);
	player = g_Vars.currentplayer;

	if (!invHasSingleWeaponIncAllGuns(weaponnum)) {
		setCurrentPlayerNum(prevplayernum);
		return;
	}

	g_ZombiePapped[playernum][g_ZombiePapCount[playernum]++] = weaponnum;

	zombiesFillGun(weaponnum, ZOMBIE_PAP_CLIPS, false);

	// the hand holding it now has the bigger magazine, full
	for (h = 0; h < 2; h++) {
		struct hand *hand = &player->hands[h];

		if (!hand->inuse || hand->gset.weaponnum != weaponnum) {
			continue;
		}

		for (j = 0; j < 2; j++) {
			struct weapon *weapon = weaponFindById(weaponnum);

			if (weapon && weapon->ammos[j] && hand->ammotypes[j] >= 0 && weapon->ammos[j]->clipsize > 0) {
				hand->clipsizes[j] = weapon->ammos[j]->clipsize * ZOMBIE_PAP_CLIPMULT;
				hand->loadedammo[j] = hand->clipsizes[j];
			}
		}
	}

	// the new name up, as for a gun just drawn
	player->gunctrl.guntypetimer = 0;

	setCurrentPlayerNum(prevplayernum);

	sysLogPrintf(LOG_NOTE, "zombies: player %d Pack-a-Punched weapon %d", playernum + 1, weaponnum);
}

// The gun down out of the hands and into the machine: the next gun had comes
// up, or the bare hands when there is none
static void zombiesPapTakeGun(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	s32 weaponnum = g_ZombiePapGun[playernum];
	s32 next = WEAPON_UNARMED;
	s32 i;

	setCurrentPlayerNum(playernum);

	zombiesKnifeCancel(playernum);
	zombiesSyncGuns(playernum);
	zombiesDropListedGun(playernum, weaponnum);

	for (i = 0; i < g_ZombieGunCount[playernum]; i++) {
		if (g_ZombieGuns[playernum][i] != weaponnum) {
			next = g_ZombieGuns[playernum][i];
			break;
		}
	}

	bgunEquipWeapon(next);

	setCurrentPlayerNum(prevplayernum);

	zombiesPapShowGun(weaponnum);
}

// The gun taken back out of the machine by its owner: into the hands,
// upgraded and full; with the hands full again, the gun in them gives way
static void zombiesPapGiveGun(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	s32 weaponnum = g_ZombiePapGun[playernum];

	setCurrentPlayerNum(playernum);

	if (!invHasSingleWeaponIncAllGuns(weaponnum)) {
		zombiesMakeRoomForGun(playernum);
	}

	invGiveSingleWeapon(weaponnum);
	zombiesSyncGuns(playernum);

	setCurrentPlayerNum(prevplayernum);

	zombiesPapUpgrade(playernum, weaponnum);

	setCurrentPlayerNum(playernum);

	// its clip to load, of the bigger magazine
	zombiesFillGun(weaponnum, ZOMBIE_PAP_CLIPS, true);
	bgunEquipWeapon(weaponnum);
	zombiesKnifeCancel(playernum);

	setCurrentPlayerNum(prevplayernum);

	zombiesPapHideGun();
}

static void zombiesTickPap(s32 playernum)
{
	struct player *player = g_Vars.players[playernum];
	struct prop *machine = g_ZombiePapPlaced ? g_ZombiePapObj.prop : NULL;
	const s32 prevplayernum = g_Vars.currentplayernum;
	s32 held;
	s32 i;
	s32 n;

	// an upgrade goes with its gun: one no longer held comes off
	setCurrentPlayerNum(playernum);

	for (i = 0, n = 0; i < g_ZombiePapCount[playernum]; i++) {
		if (invHasSingleWeaponIncAllGuns(g_ZombiePapped[playernum][i])) {
			g_ZombiePapped[playernum][n++] = g_ZombiePapped[playernum][i];
		}
	}

	g_ZombiePapCount[playernum] = n;
	held = zombiesHeldGun(playernum);
	setCurrentPlayerNum(prevplayernum);

	g_ZombieNearPap[playernum] = false;

	// in the machine
	if (g_ZombiePap60[playernum] >= 0) {
		s32 prev = g_ZombiePap60[playernum];
		s32 now = prev + g_Vars.lvupdate60;

		// down before the gun had gone in: it stays in the hands
		if (prev < ZOMBIE_PAP_DOWN60 && !zombiesPlayerIsUp(playernum)) {
			g_ZombiePap60[playernum] = -1;
			return;
		}

		if (prev < ZOMBIE_PAP_DOWN60 && now >= ZOMBIE_PAP_DOWN60) {
			zombiesPapTakeGun(playernum);
		}

		// out of the machine, upgraded, to be taken
		if (prev < ZOMBIE_PAP_OFFERAT && now >= ZOMBIE_PAP_OFFERAT) {
			zombiesPlaySound(SFX_PICKUP_SHIELD, 0.8f);
		}

		// not taken in time: gone, as Call of Duty's is
		if (now >= ZOMBIE_PAP_TOTAL60) {
			g_ZombiePap60[playernum] = -1;
			zombiesPapHideGun();
			zombiesPlaySound(SFX_MENU_ERROR, 0.6f);
			sysLogPrintf(LOG_NOTE, "zombies: player %d left weapon %d in Pack-a-Punch", playernum + 1, g_ZombiePapGun[playernum]);
			return;
		}

		g_ZombiePap60[playernum] = now;
		zombiesPapPlaceGun(now);

		if (now < ZOMBIE_PAP_OFFERAT + ZOMBIE_PAP_SLIDE60 / 2 || !zombiesPlayerIsUp(playernum)
				|| g_ZombieDrink60[playernum] >= 0 || g_ZombieNearDowned[playernum] >= 0 || machine == NULL) {
			g_ZombiePapHeld60[playernum] = 0;
			return;
		}

		// near the machine, holding B (E) for it
		{
			f32 dx = player->prop->pos.x - machine->pos.x;
			f32 dy = player->prop->pos.y - machine->pos.y;
			f32 dz = player->prop->pos.z - machine->pos.z;

			if (dx * dx + dz * dz >= ZOMBIE_JUGG_RANGE * ZOMBIE_JUGG_RANGE || dy > 250 || dy < -250) {
				g_ZombiePapHeld60[playernum] = 0;
				g_ZombiePapNeedsRelease[playernum] = false;
				return;
			}
		}

		g_ZombieNearPap[playernum] = true;

		if (!zombiesHoldingUse(playernum)) {
			g_ZombiePapHeld60[playernum] = 0;
			g_ZombiePapNeedsRelease[playernum] = false;
			return;
		}

		if (g_ZombiePapNeedsRelease[playernum]) {
			return;
		}

		g_ZombiePapHeld60[playernum] += g_Vars.lvupdate60;

		if (g_ZombiePapHeld60[playernum] >= ZOMBIE_JUGG_HOLD60) {
			g_ZombiePapNeedsRelease[playernum] = true;
			g_ZombiePapHeld60[playernum] = 0;
			g_ZombiePap60[playernum] = -1;
			zombiesPapGiveGun(playernum);
			zombiesPlaySound(SFX_MENU_SELECT, 1.0f);
		}

		return;
	}

	if (machine == NULL || g_ZombieMarking || !zombiesPlayerIsUp(playernum) || g_ZombieDrink60[playernum] >= 0
			|| g_ZombieNearDowned[playernum] >= 0 || g_ZombieKnifeStage[playernum] != ZKNIFE_NONE) {
		g_ZombiePapHeld60[playernum] = 0;
		return;
	}

	{
		f32 dx = player->prop->pos.x - machine->pos.x;
		f32 dy = player->prop->pos.y - machine->pos.y;
		f32 dz = player->prop->pos.z - machine->pos.z;

		if (dx * dx + dz * dz >= ZOMBIE_JUGG_RANGE * ZOMBIE_JUGG_RANGE || dy > 250 || dy < -250) {
			g_ZombiePapHeld60[playernum] = 0;
			g_ZombiePapNeedsRelease[playernum] = false;
			return;
		}
	}

	g_ZombieNearPap[playernum] = true;

	if (!zombiesHoldingUse(playernum)) {
		g_ZombiePapHeld60[playernum] = 0;
		g_ZombiePapNeedsRelease[playernum] = false;
		return;
	}

	if (g_ZombiePapNeedsRelease[playernum]) {
		return;
	}

	g_ZombiePapHeld60[playernum] += g_Vars.lvupdate60;

	if (g_ZombiePapHeld60[playernum] < ZOMBIE_JUGG_HOLD60) {
		return;
	}

	g_ZombiePapNeedsRelease[playernum] = true;
	g_ZombiePapHeld60[playernum] = 0;

	if (!zombiesIsSlotGun(held) || zombiesGunIsPapped(playernum, held) || g_ZombiePoints[playernum] < ZOMBIE_PAP_COST
			|| zombiesPapInUse()) {
		zombiesPlaySound(SFX_MENU_ERROR, 0.4f);
		return;
	}

	g_ZombiePoints[playernum] -= ZOMBIE_PAP_COST;
	zombiesAddPopup(playernum, -ZOMBIE_PAP_COST);
	zombiesPlaySound(SFX_MENU_SELECT, 1.0f);
	zombiesPlaySound(SFX_MAULER_CHARGE, 0.6f);

	g_ZombiePapGun[playernum] = held;
	g_ZombiePap60[playernum] = 0;
}

// How far down the gun is held for Pack-a-Punch, 0 to 1 (zombiesGunLowerFrac())
static f32 zombiesPapLowerFrac(s32 playernum)
{
	s32 t = g_ZombiePap60[playernum];
	f32 f;

	// down until it goes into the machine; the next gun then comes up as any
	// gun drawn does
	if (t < 0 || t >= ZOMBIE_PAP_DOWN60) {
		return 0;
	}

	f = (f32)t / ZOMBIE_PAP_DOWN60;

	return f * f * (3 - 2 * f);
}

// The prompt at the machine: what it does with the gun in hand
static bool zombiesPapPrompt(s32 playernum, char *text)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	s32 held;

	if (!g_ZombieNearPap[playernum]) {
		return false;
	}

	// its own gun out of the machine, waiting: its upgraded name
	if (g_ZombiePap60[playernum] >= 0) {
		const char *name = NULL;
		s32 i;
		s32 n;

		for (i = 0; i < ARRAYCOUNT(g_ZombiePapNames); i++) {
			if (g_ZombiePapNames[i].weaponnum == g_ZombiePapGun[playernum]) {
				name = g_ZombiePapNames[i].name;
			}
		}

		if (name == NULL) {
			name = "Pack-a-Punched\n";
		}

		// without the name's own newline
		n = (s32)strlen(name);
		n = n > 0 && name[n - 1] == '\n' ? n - 1 : n;

		sprintf(text, "Hold %s for %.*s", zombiesUseName(playernum), n, name);

		return true;
	}

	if (zombiesPapInUse()) {
		strcpy(text, "Pack-a-Punch is in use");
		return true;
	}

	setCurrentPlayerNum(playernum);
	held = zombiesHeldGun(playernum);
	setCurrentPlayerNum(prevplayernum);

	if (!zombiesIsSlotGun(held)) {
		strcpy(text, "Pack-a-Punch takes a gun");
	} else if (zombiesGunIsPapped(playernum, held)) {
		strcpy(text, "This gun is already Pack-a-Punched");
	} else {
		sprintf(text, "Hold %s to Pack-a-Punch [Cost: %d]", zombiesUseName(playernum), ZOMBIE_PAP_COST);
	}

	return true;
}

// The gun from the box into the player's hands, full
static void zombiesBoxGive(s32 playernum, s32 index)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	struct mpweapon *mpweapon = &g_MpWeapons[index];

	setCurrentPlayerNum(playernum);

	// the hands full: the gun in them gives way, as Call of Duty's
	if (!invHasSingleWeaponIncAllGuns(mpweapon->weaponnum)) {
		zombiesMakeRoomForGun(playernum);
	}

	invGiveSingleWeapon(mpweapon->weaponnum);
	zombiesSyncGuns(playernum);

	// a clip to load and ZOMBIE_AMMO_CLIPS spare
	zombiesFillGun(mpweapon->weaponnum, ZOMBIE_AMMO_CLIPS, true);

	bgunEquipWeapon(mpweapon->weaponnum);

	// bought or taken mid-knife: the new gun is the one kept
	zombiesKnifeCancel(playernum);

	setCurrentPlayerNum(prevplayernum);
}

static void zombiesTickBox(void)
{
	s32 numplayers = PLAYERCOUNT();
	struct prop *box = g_ZombieBox ? g_ZombieBox->prop : NULL;
	s32 i;

	if (box == NULL || g_ZombieBox->model == NULL) {
		return;
	}

	// who is near enough, and holding B for it
	for (i = 0; i < numplayers; i++) {
		struct player *player = g_Vars.players[i];
		bool held = false;

		g_ZombieNearBox[i] = false;

		if (zombiesPlayerIsUp(i) && g_ZombieDrink60[i] < 0 && g_ZombieNearDowned[i] < 0) {
			f32 dx = player->prop->pos.x - box->pos.x;
			f32 dy = player->prop->pos.y - box->pos.y;
			f32 dz = player->prop->pos.z - box->pos.z;

			g_ZombieNearBox[i] = dx * dx + dz * dz < ZOMBIE_BOX_RANGE * ZOMBIE_BOX_RANGE && dy < 250 && dy > -250;
		}

		if (g_ZombieNearBox[i]) {
			held = zombiesHoldingUse(i);
		}


		if (!held) {
			g_ZombieBoxHeld60[i] = 0;
			g_ZombieBoxNeedsRelease[i] = false;
		} else if (!g_ZombieBoxNeedsRelease[i]) {
			g_ZombieBoxHeld60[i] += g_Vars.lvupdate60;
		}
	}

	switch (g_ZombieBoxState) {
	case ZOMBIEBOX_IDLE:
		for (i = 0; i < numplayers; i++) {
			if (g_ZombieBoxHeld60[i] < ZOMBIE_BOX_HOLD60) {
				continue;
			}

			g_ZombieBoxNeedsRelease[i] = true;
			g_ZombieBoxHeld60[i] = 0;
			g_ZombieSoundFor = i;

			if (g_ZombiePoints[i] < ZOMBIE_BOX_COST || g_ZombieBoxNumGuns <= 0) {
				zombiesPlaySound(SFX_MENU_ERROR, 0.4f);
				g_ZombieSoundFor = -1;
				continue;
			}

			g_ZombiePoints[i] -= ZOMBIE_BOX_COST;
			zombiesAddPopup(i, -ZOMBIE_BOX_COST);
			zombiesPlaySound(SFX_MENU_SELECT, 0.8f);
			g_ZombieSoundFor = -1;

			g_ZombieBoxState = ZOMBIEBOX_SPIN;
			g_ZombieBoxUser = i;
			g_ZombieBoxTime60 = 0;
			g_ZombieBoxNextSwap60 = 0;
			break;
		}
		break;
	case ZOMBIEBOX_SPIN:
		g_ZombieBoxTime60 += g_Vars.lvupdate60;

		if (g_ZombieBoxTime60 >= ZOMBIE_BOX_SPIN60) {
			// where it lands
			s32 index = zombiesBoxPick(g_ZombieBoxUser, true);

			if (index >= 0) {
				zombiesBoxShowGun(index);
			}

			zombiesPlaySound(SFX_PICKUP_SHIELD, 1.0f);
			g_ZombieBoxState = ZOMBIEBOX_OFFER;
			g_ZombieBoxTime60 = 0;
		} else if (g_ZombieBoxTime60 >= g_ZombieBoxNextSwap60) {
			// round they go, slowing: every 4 60ths at first, every 20 at the end
			s32 index = zombiesBoxPick(-1, false);

			if (index >= 0) {
				zombiesBoxShowGun(index);
			}

			zombiesPlaySound(SFX_PICKUP_AMMO, 1.6f);
			g_ZombieBoxNextSwap60 = g_ZombieBoxTime60 + 4 + 16 * g_ZombieBoxTime60 / ZOMBIE_BOX_SPIN60;
		}
		break;
	case ZOMBIEBOX_OFFER:
		g_ZombieBoxTime60 += g_Vars.lvupdate60;

		if (g_ZombieBoxUser >= 0 && g_ZombieBoxUser < numplayers
				&& g_ZombieBoxHeld60[g_ZombieBoxUser] >= ZOMBIE_BOX_HOLD60 && g_ZombieBoxShown >= 0) {
			g_ZombieBoxNeedsRelease[g_ZombieBoxUser] = true;
			g_ZombieBoxHeld60[g_ZombieBoxUser] = 0;

			zombiesBoxGive(g_ZombieBoxUser, g_ZombieBoxShown);
			zombiesPlaySound(SFX_PICKUP_AMMO, 1.0f);

			zombiesBoxHideGun();
			g_ZombieBoxState = ZOMBIEBOX_IDLE;
			g_ZombieBoxUser = -1;
		} else if (g_ZombieBoxTime60 >= ZOMBIE_BOX_OFFER60) {
			// not taken: back into the box
			zombiesBoxHideGun();
			g_ZombieBoxState = ZOMBIEBOX_IDLE;
			g_ZombieBoxUser = -1;
		}
		break;
	}

	zombiesBoxPlaceGun();
}

/**
 * Max Ammo, Call of Duty's power-up: a zombie killed by a player may leave one
 * where it fell (ZOMBIE_DROP_CHANCE in a hundred, no more than
 * ZOMBIE_DROPS_PER_ROUND a round). It is a small green ammo crate turning and
 * bobbing over the floor, on the radar in green; it lasts ZOMBIE_DROP_LIFE60,
 * blinking for the last ZOMBIE_DROP_BLINK60. Any player standing who walks
 * into it fills every gun everyone alive is carrying, and "MAX AMMO!" goes up
 * on every screen.
 */
#define ZOMBIE_DROP_CHANCE      4       // in a hundred, a kill
#define ZOMBIE_DROPS_PER_ROUND  4
#define ZOMBIE_DROP_LIFE60      1800    // 30 seconds
#define ZOMBIE_DROP_BLINK60     480     // blinking for the last 8
#define ZOMBIE_DROP_RANGE       90.0f   // how near a player picks it up
#define ZOMBIE_DROP_SIZE        80.0f   // how long the crate is (34 was easy to miss)
#define ZOMBIE_DROP_FLOAT       40.0f   // its bottom this far over the floor: waist high, as Call of Duty's float
#define ZOMBIE_DROP_BANNER60    150     // how long "MAX AMMO!" stays up

/**
 * And the rest of Call of Duty's power-ups, from the same drops: which one a
 * drop is goes round all four in a shuffled order before any comes again, as
 * Call of Duty deals them.
 *
 * - Insta-Kill: for ZOMBIE_POWERUP_TIME60 any player's hit kills any zombie
 *   (zombiesInstaKillOn(), asked by chrDamage()).
 * - Double Points: for as long, every point a hit or kill is worth is twice
 *   that (zombiesAwardHit()).
 * - Nuke: every zombie up dies, at once, and each player standing gets 400.
 *   The zombies it kills give no points of their own and drop nothing; the
 *   screen flashes white.
 *
 * Each has its own thing on the floor until there are models of their own:
 * Max Ammo a green crate, Insta-Kill a red knife, Double Points gold pills,
 * the Nuke an orange N-Bomb. The two that last count down at the bottom of
 * the screen, blinking for their last five seconds.
 */
#define ZOMBIEPOWERUP_MAXAMMO      0
#define ZOMBIEPOWERUP_INSTAKILL    1
#define ZOMBIEPOWERUP_DOUBLEPOINTS 2
#define ZOMBIEPOWERUP_NUKE         3
#define ZOMBIEPOWERUP_COUNT        4

#define ZOMBIE_NUKE_POINTS  400
#define ZOMBIE_NUKE_FLASH60 45     // the white of the flash, fading

struct zombiepowerup {
	s32 mpweaponnum;   // whose pickup model it is, -1 for none
	s32 model;         // or a model of its own, 0 for none (and the ammo crate when neither)
	u8 colour[3];      // what it is lit, whatever light it is in
	u32 radar;
	char *banner;
};

static struct zombiepowerup g_ZombiePowerups[ZOMBIEPOWERUP_COUNT] = {
	{ -1,                   0,                  { 0x70, 0xff, 0x70 }, 0x40ff4000, "MAX AMMO!"      },
	{ MPWEAPON_COMBATKNIFE, 0,                  { 0xff, 0x50, 0x50 }, 0xff404000, "INSTA-KILL!"    },
	// Hold the Briefcase's case: a case of cash, in gold (the Combat Boost's
	// pills said nothing about points)
	{ -1,                   MODEL_CHRBRIEFCASE, { 0xff, 0xe0, 0x40 }, 0xffe04000, "DOUBLE POINTS!" },
	{ MPWEAPON_NBOMB,       0,                  { 0xff, 0x90, 0x30 }, 0xff902000, "KABOOM!"        },
};

struct zombiedrop {
	struct defaultobj obj;
	bool active;
	s32 kind;          // ZOMBIEPOWERUP_*
	s32 age60;
	f32 floory;
	f32 lift;          // from the model's origin down to its bottom, scaled
	s32 tilt;          // a pickup stood on end: 0 not, 1 about x, 2 about z
	RoomNum rooms[8];
};

static struct zombiedrop g_ZombieDrops[4];
static u8 g_ZombieDropSerial[4];           // each slot's drops counted (online: a client knows a new one)
static s32 g_ZombieDropsThisRound = 0;
static s32 g_ZombieMaxAmmoBanner60 = -1;   // how long a power-up's name has been up, -1 when not
static s32 g_ZombieBannerKind = ZOMBIEPOWERUP_MAXAMMO;
static s32 g_ZombiePowerupBag[ZOMBIEPOWERUP_COUNT];
static s32 g_ZombiePowerupBagLeft = 0;
static s32 g_ZombieNukeFlash60 = -1;

static void zombiesResetDrops(void)
{
	// a new stage: the last one's props went with it
	bzero(g_ZombieDrops, sizeof(g_ZombieDrops));
	g_ZombieDropsThisRound = 0;
	g_ZombieMaxAmmoBanner60 = -1;
	g_ZombiePowerupBagLeft = 0;
	g_ZombieInstaKill60 = 0;
	g_ZombieDoublePoints60 = 0;
	g_ZombieNukeFlash60 = -1;
}

// The next power-up: all of them in a shuffled order, then shuffled again
static s32 zombiesNextPowerup(void)
{
	if (g_ZombiePowerupBagLeft <= 0) {
		s32 i;

		for (i = 0; i < ZOMBIEPOWERUP_COUNT; i++) {
			g_ZombiePowerupBag[i] = i;
		}

		for (i = ZOMBIEPOWERUP_COUNT - 1; i > 0; i--) {
			s32 j = rngRandom() % (i + 1);
			s32 tmp = g_ZombiePowerupBag[i];

			g_ZombiePowerupBag[i] = g_ZombiePowerupBag[j];
			g_ZombiePowerupBag[j] = tmp;
		}

		g_ZombiePowerupBagLeft = ZOMBIEPOWERUP_COUNT;
	}

	return g_ZombiePowerupBag[--g_ZombiePowerupBagLeft];
}

// The model a power-up shows: its pickup's, or the ammo crate
static s32 zombiesPowerupModel(s32 kind)
{
	s32 mpweaponnum = g_ZombiePowerups[kind].mpweaponnum;

	if (g_ZombiePowerups[kind].model > 0) {
		return g_ZombiePowerups[kind].model;
	}

	if (mpweaponnum >= 0 && mpweaponnum < ARRAYCOUNT(g_MpWeapons) && g_MpWeapons[mpweaponnum].model > 0) {
		return g_MpWeapons[mpweaponnum].model;
	}

	return MODEL_MULTI_AMMO_CRATE;
}

static void zombiesDropRemove(struct zombiedrop *drop)
{
	if (drop->obj.prop) {
		drop->obj.hidden |= OBJHFLAG_DELETING;
	}

	drop->active = false;
}

// The crate where it is this frame: turning, and bobbing over the floor
static void zombiesDropPlace(struct zombiedrop *drop)
{
	struct defaultobj *obj = &drop->obj;
	struct coord pos;
	Mtxf mtx;

	if (obj->prop == NULL || obj->model == NULL) {
		return;
	}

	// its bottom a hand over the floor, bobbing
	pos = obj->prop->pos;
	pos.y = drop->floory + drop->lift + ZOMBIE_DROP_FLOAT + sinf(drop->age60 * 0.07f) * 7;

	mtx4LoadYRotation(drop->age60 * 0.04f, &mtx);

	// a gun's pickup lies flat, a thin line from most of the room: on end,
	// turning, it shows its whole length whichever way it is seen
	if (drop->tilt) {
		Mtxf spin = mtx;
		Mtxf stand;

		// the N-Bomb's pickup lies the other way round: up the same way it came
		// out upside down
		f32 angle = drop->kind == ZOMBIEPOWERUP_NUKE ? -M_PI / 2 : M_PI / 2;

		if (drop->tilt == 1) {
			mtx4LoadXRotation(angle, &stand);
		} else {
			mtx4LoadZRotation(angle, &stand);
		}

		mtx4MultMtx4(&spin, &stand, &mtx);
	}
	mtx00015f04(obj->model->scale, &mtx);

	func0f06a580(obj, &pos, &mtx, drop->rooms);

	// its own colour, whatever light it is in
	obj->shadecol[0] = obj->nextcol[0] = g_ZombiePowerups[drop->kind].colour[0];
	obj->shadecol[1] = obj->nextcol[1] = g_ZombiePowerups[drop->kind].colour[1];
	obj->shadecol[2] = obj->nextcol[2] = g_ZombiePowerups[drop->kind].colour[2];
	obj->shadecol[3] = obj->nextcol[3] = 0xff;

	// blinking as it runs out
	if (ZOMBIE_DROP_LIFE60 - drop->age60 < ZOMBIE_DROP_BLINK60 && (drop->age60 / 12) % 2) {
		obj->flags2 |= OBJFLAG2_INVISIBLE;
	} else {
		obj->flags2 &= ~OBJFLAG2_INVISIBLE;
	}
}

static bool zombiesDropMake(struct zombiedrop *drop, s32 kind, struct coord *pos, f32 floory, RoomNum *rooms);

static void zombiesMaybeDrop(struct chrdata *victim)
{
	struct zombiedrop *drop = NULL;
	s32 i;

	if (victim->prop == NULL || !g_ZombieCfgPowerups || g_ZombieDropsThisRound >= ZOMBIE_DROPS_PER_ROUND
			|| (s32)(rngRandom() % 100) >= ZOMBIE_DROP_CHANCE) {
		return;
	}

	// a slot whose last crate has gone
	for (i = 0; i < ARRAYCOUNT(g_ZombieDrops); i++) {
		if (!g_ZombieDrops[i].active && g_ZombieDrops[i].obj.prop == NULL) {
			drop = &g_ZombieDrops[i];
			break;
		}
	}

	if (drop == NULL) {
		return;
	}

	if (zombiesDropMake(drop, zombiesNextPowerup(), &victim->prop->pos,
				victim->ground > -30000 ? victim->ground : victim->prop->pos.y - 60, victim->prop->rooms)) {
		g_ZombieDropSerial[drop - g_ZombieDrops]++; // a new one in the slot, for the clients
		g_ZombieDropsThisRound++;
		zombiesPlaySound(SFX_PICKUP_SHIELD, 1.5f);
	}
}

// A power-up of kind in drop's slot, floating over floory at pos; false when
// it could not be made. A client makes the host's this way (zombiesNetApply()).
static bool zombiesDropMake(struct zombiedrop *drop, s32 kind, struct coord *pos, f32 floory, RoomNum *rooms)
{
	struct defaultobj *obj;
	struct prop *prop;

	obj = &drop->obj;

	bzero(drop, sizeof(*drop));
	drop->kind = kind;
	obj->extrascale = 256;
	obj->type = OBJTYPE_BASIC;
	obj->modelnum = zombiesPowerupModel(drop->kind);
	obj->flags = OBJFLAG_INVINCIBLE | OBJFLAG_CANNOT_ACTIVATE | OBJFLAG_UNCOLLECTABLE;
	obj->flags2 = OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_IMMUNETOEXPLOSIONS | OBJFLAG2_SHOOTTHROUGH
		| OBJFLAG2_THROWTHROUGH | OBJFLAG2_NOFALL;
	obj->flags3 = OBJFLAG3_WALKTHROUGH;
	obj->realrot[0][0] = obj->realrot[1][1] = obj->realrot[2][2] = 1;
	obj->maxdamage = 1000;
	obj->floorcol = 0x0fff;
	obj->pad = -1;

	setupLoadModeldef(obj->modelnum);

	if (g_ModelStates[obj->modelnum].modeldef == NULL) {
		return false;
	}

	prop = objInitWithAutoModel(obj);

	if (prop == NULL || obj->model == NULL) {
		return false;
	}

	// small: ZOMBIE_DROP_SIZE long
	{
		struct modelrodata_bbox *bbox = objFindBboxRodata(obj);

		if (bbox) {
			f32 len = bbox->xmax - bbox->xmin > bbox->zmax - bbox->zmin ? bbox->xmax - bbox->xmin : bbox->zmax - bbox->zmin;

			if (len * obj->model->scale > 0.01f) {
				modelSetScale(obj->model, obj->model->scale * ZOMBIE_DROP_SIZE / (len * obj->model->scale));
			}

			drop->lift = -bbox->ymin * obj->model->scale;

			// a pickup on end: its long side up, half of it under its middle
			if (g_ZombiePowerups[drop->kind].mpweaponnum >= 0) {
				bool longx = bbox->xmax - bbox->xmin > bbox->zmax - bbox->zmin;

				drop->tilt = longx ? 2 : 1;
				drop->lift = ZOMBIE_DROP_SIZE / 2;
			}
		}
	}

	// a pickup's model carries its muzzle flash, showing
	if (g_ZombiePowerups[drop->kind].mpweaponnum >= 0) {
		zombiesHideGunFlash(obj);
	}

	drop->active = true;
	drop->age60 = 0;
	drop->floory = floory;
	memcpy(drop->rooms, rooms, sizeof(drop->rooms));

	prop->pos = *pos;
	zombiesDropPlace(drop);

	propActivate(prop);
	propEnable(prop);

	obj->hidden2 &= ~OBJH2FLAG_CANREGEN;

	return true;
}

// Every gun every living player carries, its spare clips full
static void zombiesMaxAmmo(void)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	s32 p;

	for (p = 0; p < PLAYERCOUNT(); p++) {
		s32 i;

		if (g_Vars.players[p]->isdead) {
			continue;
		}

		setCurrentPlayerNum(p);

		// as Call of Duty's: the spare clips, not the one in the gun
		for (i = 0; i < invGetCount(); i++) {
			zombiesFillGun(invGetWeaponNumByIndex(i), ZOMBIE_AMMO_CLIPS, false);
		}
	}

	setCurrentPlayerNum(prevplayernum);
}

// The power-ups on the radar, in green
static Gfx *zombiesRadarDrops(Gfx *gdl)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_ZombieDrops); i++) {
		struct prop *prop = g_ZombieDrops[i].obj.prop;
		struct coord dist;

		if (!g_ZombieDrops[i].active || prop == NULL) {
			continue;
		}

		dist.x = prop->pos.x - g_Vars.currentplayer->prop->pos.x;
		dist.y = prop->pos.y - g_Vars.currentplayer->prop->pos.y;
		dist.z = prop->pos.z - g_Vars.currentplayer->prop->pos.z;

		gdl = radarDrawDot(gdl, prop, &dist, g_ZombiePowerups[g_ZombieDrops[i].kind].radar, 0, 1);
	}

	return gdl;
}

// Every zombie up dies at once, and each player standing gets 400
static void zombiesNuke(void)
{
	struct coord none = {0, 0, 0};
	s32 i;

	for (i = 0; i < g_BotCount; i++) {
		struct chrdata *zchr = g_MpBotChrPtrs[i];

		// no attacker: no points for each, and no drops
		if (zchr && zchr->aibot && zchr->prop && !chrIsDead(zchr)) {
			chrDamageByMisc(zchr, 10000, &none, NULL, NULL);
		}
	}

	for (i = 0; i < PLAYERCOUNT(); i++) {
		if (zombiesPlayerIsUp(i)) {
			g_ZombiePoints[i] += ZOMBIE_NUKE_POINTS;
			zombiesAddPopup(i, ZOMBIE_NUKE_POINTS);
		}
	}

	g_ZombieNukeFlash60 = 0;
}

// A power-up picked up: what it does
static void zombiesPowerupTaken(s32 kind)
{
	switch (kind) {
	case ZOMBIEPOWERUP_MAXAMMO:
		zombiesMaxAmmo();
		zombiesPlaySound(SFX_PICKUP_AMMO, 0.8f);
		break;
	case ZOMBIEPOWERUP_INSTAKILL:
		g_ZombieInstaKill60 = ZOMBIE_POWERUP_TIME60;
		break;
	case ZOMBIEPOWERUP_DOUBLEPOINTS:
		g_ZombieDoublePoints60 = ZOMBIE_POWERUP_TIME60;
		break;
	case ZOMBIEPOWERUP_NUKE:
		zombiesNuke();
		zombiesPlaySound(SFX_EXPLOSION_8098, 1.0f);
		break;
	}

	zombiesPlaySound(SFX_PICKUP_SHIELD, 1.0f);
	g_ZombieBannerKind = kind;
	g_ZombieMaxAmmoBanner60 = 0;
}

// The Nuke's white flash over the whole view, fading
static Gfx *zombiesRenderNukeFlash(Gfx *gdl)
{
	s32 alpha;

	if (g_ZombieNukeFlash60 < 0 || g_ZombieNukeFlash60 >= ZOMBIE_NUKE_FLASH60) {
		return gdl;
	}

	alpha = 0xd0 * (ZOMBIE_NUKE_FLASH60 - g_ZombieNukeFlash60) / ZOMBIE_NUKE_FLASH60;

	gdl = text0f153628(gdl);
	gdl = text0f153a34(gdl, viGetViewLeft(), viGetViewTop(), viGetViewLeft() + viGetViewWidth(),
			viGetViewTop() + viGetViewHeight(), 0xffffff00 | alpha);
	gdl = text0f153780(gdl);

	return gdl;
}

static Gfx *zombiesDrawTextBL(Gfx *gdl, char *text, s32 x, s32 bottomy, u32 colour, struct fontchar *chars, struct font *font);

// Insta-Kill's and Double Points' time left, low in the middle of the view,
// blinking for the last five seconds
static Gfx *zombiesRenderPowerupTimers(Gfx *gdl)
{
	struct fontchar *chars = g_CharsHandelGothicSm;
	struct font *font = g_FontHandelGothicSm;
	char text[48];
	s32 line = 0;
	s32 k;

	for (k = 0; k < 2; k++) {
		s32 left = k == 0 ? g_ZombieInstaKill60 : g_ZombieDoublePoints60;
		u32 colour = k == 0 ? 0xff6060e0 : 0xffe040e0;
		s32 textwidth;
		s32 textheight;

		if (left <= 0) {
			continue;
		}

		if (left < 300 && (left / 15) % 2) {
			line++;
			continue;
		}

		sprintf(text, "%s %d", k == 0 ? "INSTA-KILL" : "DOUBLE POINTS", (left + 59) / 60);

#ifndef PLATFORM_N64
		gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
#endif

		gdl = text0f153628(gdl);
		textMeasure(&textheight, &textwidth, text, chars, font, 0);
		gdl = zombiesDrawTextBL(gdl, text, viGetViewLeft() + (viGetViewWidth() - textwidth) / 2,
				viGetViewTop() + viGetViewHeight() * 82 / 100 - line * 14, colour, chars, font);
		gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
		gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

		line++;
	}

	return gdl;
}

static Gfx *zombiesDrawTextBL(Gfx *gdl, char *text, s32 x, s32 bottomy, u32 colour, struct fontchar *chars, struct font *font);

// "MAX AMMO!" across the top of every view after one is picked up
static Gfx *zombiesRenderMaxAmmoBanner(Gfx *gdl)
{
	struct fontchar *chars = g_FontHandelGothicMd ? g_CharsHandelGothicMd : g_CharsHandelGothicSm;
	struct font *font = g_FontHandelGothicMd ? g_FontHandelGothicMd : g_FontHandelGothicSm;
	s32 t = g_ZombieMaxAmmoBanner60;
	s32 alpha = 0xf0;
	s32 textwidth;
	s32 textheight;
	char *text = g_ZombiePowerups[g_ZombieBannerKind].banner;

	if (t < 0) {
		return gdl;
	}

	// in quickly, out over the last 40
	if (t < 10) {
		alpha = 0xf0 * t / 10;
	} else if (t > ZOMBIE_DROP_BANNER60 - 40) {
		alpha = 0xf0 * (ZOMBIE_DROP_BANNER60 - t) / 40;
	}

	if (alpha <= 0) {
		return gdl;
	}

#ifndef PLATFORM_N64
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
#endif

	gdl = text0f153628(gdl);

	textMeasure(&textheight, &textwidth, text, chars, font, 0);
	gdl = zombiesDrawTextBL(gdl, text, viGetViewLeft() + (viGetViewWidth() - textwidth) / 2,
			viGetViewTop() + viGetViewHeight() * 30 / 100, 0xfff060 << 8 | alpha, chars, font);

	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

	return gdl;
}

static void zombiesTickDrops(void)
{
	s32 i;
	s32 p;

	if (g_ZombieMaxAmmoBanner60 >= 0) {
		g_ZombieMaxAmmoBanner60 += g_Vars.lvupdate60;

		if (g_ZombieMaxAmmoBanner60 > ZOMBIE_DROP_BANNER60) {
			g_ZombieMaxAmmoBanner60 = -1;
		}
	}

	// the power-ups that last, running down
	g_ZombieInstaKill60 = g_ZombieInstaKill60 > g_Vars.lvupdate60 ? g_ZombieInstaKill60 - g_Vars.lvupdate60 : 0;
	g_ZombieDoublePoints60 = g_ZombieDoublePoints60 > g_Vars.lvupdate60 ? g_ZombieDoublePoints60 - g_Vars.lvupdate60 : 0;

	if (g_ZombieNukeFlash60 >= 0) {
		g_ZombieNukeFlash60 += g_Vars.lvupdate60;

		if (g_ZombieNukeFlash60 >= ZOMBIE_NUKE_FLASH60) {
			g_ZombieNukeFlash60 = -1;
		}
	}

	for (i = 0; i < ARRAYCOUNT(g_ZombieDrops); i++) {
		struct zombiedrop *drop = &g_ZombieDrops[i];
		struct prop *prop = drop->obj.prop;

		if (!drop->active) {
			continue;
		}

		if (prop == NULL) {
			drop->active = false;
			continue;
		}

		drop->age60 += g_Vars.lvupdate60;

		if (drop->age60 >= ZOMBIE_DROP_LIFE60) {
			zombiesDropRemove(drop);
			continue;
		}

		// walked into by anyone standing
		for (p = 0; p < PLAYERCOUNT(); p++) {
			struct prop *pprop = g_Vars.players[p]->prop;
			f32 dx;
			f32 dy;
			f32 dz;

			if (!zombiesPlayerIsUp(p)) {
				continue;
			}

			dx = pprop->pos.x - prop->pos.x;
			dy = pprop->pos.y - prop->pos.y;
			dz = pprop->pos.z - prop->pos.z;

			if (dx * dx + dz * dz < ZOMBIE_DROP_RANGE * ZOMBIE_DROP_RANGE && dy < 200 && dy > -200) {
				const s32 kind = drop->kind;

				zombiesDropRemove(drop);
				zombiesPowerupTaken(kind);
				break;
			}
		}

		if (drop->active) {
			zombiesDropPlace(drop);
		}
	}
}

/**
 * Wall buys, as Call of Duty's: a gun on a wall at its own price, and its ammo
 * for half that once you have it. Held B (E) at the wall buys it, into the
 * hands with a clip in and ZOMBIE_AMMO_CLIPS spare, as the box gives one.
 *
 * Three a match, at Call of Duty's prices for the like guns: the Shotgun for
 * the Olympia's 500, the CMP150 for the MP5K's 1000, the Dragon for the
 * AK-74u's 1200. Each hangs at chest height on the wall nearest a spawn pad -
 * the pads furthest from Juggernog, the box and each other, so the guns are
 * spread over the arena - its barrel along the wall. A wall buy is on the
 * wall, not the floor, so its pad stays on the spawn list.
 */
#define ZOMBIE_WALLBUY_MAX     3
#define ZOMBIE_WALLBUY_RANGE   110.0f // how near the wall buying works
#define ZOMBIE_WALLBUY_REACH   220.0f // how far from a pad its wall may be
#define ZOMBIE_WALLBUY_HEIGHT  30.0f  // over the pad, which stands about 100 over the floor
#define ZOMBIE_WALLBUY_OFFWALL 7.0f   // out from the wall, for the gun's thickness

/**
 * Until somebody buys it, a wall buy is a chalk outline on the wall, as Call
 * of Duty's are; the first buy puts the gun up in it for good. There is no
 * drawing on a wall in Perfect Dark, so the outline is the HUD's: a gun's
 * points (along the wall, up it, in world units; the barrel to +x) are taken
 * to the screen each frame and joined in white, for a player who can see the
 * spot. Each gun has its own.
 */

// The Shotgun: a long barrel over a pump, a plain stock, no magazine
static const f32 g_ZombieChalkShotgun[][2] = {
	{ -40,  3 }, { -22,  6 }, {  -8,  7 }, {  40,  7 }, {  40,  4 }, {  24,  4 },
	{  24,  1 }, {   8,  1 }, {   8,  4 }, {   2,  4 }, {   0,  2 }, {  -6,  2 },
	{  -8, -5 }, { -13, -5 }, { -12,  1 }, { -20,  1 }, { -40, -6 }, { -40,  3 },
};

// The CMP150: short, no stock, a long straight magazine ahead of the grip
static const f32 g_ZombieChalkCmp150[][2] = {
	{ -18,  5 }, {   6,  5 }, {   6,  7 }, {  10,  7 }, {  10,  5 }, {  25,  4 },
	{  25,  1 }, {   8,  1 }, {   6, -1 }, {   2, -1 }, {   3, -13}, {  -3, -13},
	{  -4, -1 }, {  -8, -1 }, {  -9, -8 }, { -14, -8 }, { -13, -1 }, { -18, -1 },
	{ -18,  5 },
};

// The Dragon: a rifle, its sight on a raised rail, a curved magazine, a stock
static const f32 g_ZombieChalkDragon[][2] = {
	{ -35,  4 }, { -20,  5 }, {   6,  6 }, {   6,  8 }, {  14,  8 }, {  14,  6 },
	{  35,  5 }, {  35,  2 }, {  12,  2 }, {  10, -1 }, {   2, -1 }, {   3, -6 },
	{   1, -13}, {  -4, -13}, {  -3, -6 }, {  -4, -1 }, { -10, -1 }, { -11, -8 },
	{ -15, -8 }, { -14, -1 }, { -22, -1 }, { -35, -5 }, { -35,  4 },
};

struct zombiewallgun {
	s32 mpweaponnum;
	s32 cost;
	const f32 (*chalk)[2];
	s32 numchalk;
};

static const struct zombiewallgun g_ZombieWallGuns[ZOMBIE_WALLBUY_MAX] = {
	{ MPWEAPON_SHOTGUN, 500,  g_ZombieChalkShotgun, ARRAYCOUNT(g_ZombieChalkShotgun) },
	{ MPWEAPON_CMP150,  1000, g_ZombieChalkCmp150,  ARRAYCOUNT(g_ZombieChalkCmp150)  },
	{ MPWEAPON_DRAGON,  1200, g_ZombieChalkDragon,  ARRAYCOUNT(g_ZombieChalkDragon)  },
};

#define ZOMBIE_CHALK_SEEN 1500.0f // how far off the outline is drawn

struct zombiewallbuy {
	s32 mpweaponnum;
	s32 cost;
	const f32 (*chalk)[2];
	s32 numchalk;
	bool placed;
	bool shown;            // bought at least once: the gun is up in the outline
	struct coord pos;      // the gun's middle, ZOMBIE_WALLBUY_OFFWALL off the wall
	struct coord wall;     // the way to the wall from the room
	RoomNum rooms[8];
	struct defaultobj obj;
};

static struct zombiewallbuy g_ZombieWallBuys[ZOMBIE_WALLBUY_MAX];
static s32 g_ZombieNumWallBuys = 0;
static s32 g_ZombieNearWall[MAX_PLAYERS];    // the wall buy a player is at, -1 for none
static s32 g_ZombieWallHeld60[MAX_PLAYERS];
static bool g_ZombieWallNeedsRelease[MAX_PLAYERS];

static void zombiesResetWallBuys(void)
{
	s32 i;

	// a new stage: the last one's props went with it
	bzero(g_ZombieWallBuys, sizeof(g_ZombieWallBuys));
	g_ZombieNumWallBuys = 0;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ZombieNearWall[i] = -1;
		g_ZombieWallHeld60[i] = 0;
		g_ZombieWallNeedsRelease[i] = false;
	}
}

// The nearest wall round pos within reach, a little over the pad: its
// distance, and the way to it in dir. False for none.
static bool zombiesFindWall(struct coord *pos, RoomNum *rooms, f32 reach, f32 *dist, struct coord *dir)
{
	struct coord eye = *pos;
	f32 best = reach + 1;
	f32 bestang = 0;
	s32 k;

	eye.y += ZOMBIE_WALLBUY_HEIGHT;

	for (k = 0; k < 16; k++) {
		f32 ang = k * (M_BADTAU / 16);
		f32 d;

		for (d = 15; d <= reach; d += 15) {
			struct coord to;

			to.x = eye.x + sinf(ang) * d;
			to.y = eye.y;
			to.z = eye.z + cosf(ang) * d;

			if (!cdTestLos04(&eye, rooms, &to, CDTYPE_BG)) {
				if (d < best) {
					best = d;
					bestang = ang;
					dir->x = sinf(ang);
					dir->y = 0;
					dir->z = cosf(ang);
				}

				break;
			}
		}
	}

	// the wall as it is, square to it, not the nearest of the 16 ways: the
	// chalk and the gun on it were slanted by up to half a step (the CMP150's)
	if (best <= reach) {
		struct coord normal;
		f32 perp;

		if (zombiesWallLine(&eye, rooms, bestang, reach, &normal, &perp) && perp > 0) {
			*dir = normal;
			best = perp;
		}
	}

	*dist = best;

	return best <= reach;
}

// Where the wall buy goes, from its pad and the pad's wall; its gun model
// loaded now, while the stage pool has room, for the first buy to put up
static void zombiesPlaceWallBuy(struct zombiewallbuy *buy, struct pad *pad, f32 walldist, struct coord *wall)
{
	struct mpweapon *mpweapon = &g_MpWeapons[buy->mpweaponnum];
	RoomNum padrooms[2];
	f32 move = walldist - ZOMBIE_WALLBUY_OFFWALL;

	padrooms[0] = pad->room;
	padrooms[1] = -1;

	buy->pos.x = pad->pos.x + wall->x * move;
	buy->pos.y = pad->pos.y + ZOMBIE_WALLBUY_HEIGHT;
	buy->pos.z = pad->pos.z + wall->z * move;
	buy->wall = *wall;

	func0f065e74(&pad->pos, padrooms, &buy->pos, buy->rooms);

	if (buy->rooms[0] <= 0) {
		return;
	}

	if (mpweapon->model <= 0) {
		return;
	}

	if (g_ModelStates[mpweapon->model].modeldef == NULL) {
		if (mempGetStageFreeTotal() < ZOMBIE_BOX_POOLFLOOR) {
			return;
		}

		setupLoadModeldef(mpweapon->model);

		if (g_ModelStates[mpweapon->model].modeldef == NULL) {
			return;
		}
	}

	buy->placed = true;
}

// The gun up in its outline, the first time somebody buys it
static void zombiesWallBuyShowGun(struct zombiewallbuy *buy)
{
	struct defaultobj *obj = &buy->obj;
	struct mpweapon *mpweapon = &g_MpWeapons[buy->mpweaponnum];
	struct coord *wall = &buy->wall;
	struct prop *prop;
	Mtxf mtx;

	if (!buy->placed || buy->shown) {
		return;
	}

	buy->shown = true;

	bzero(obj, sizeof(*obj));
	obj->extrascale = 256;
	obj->type = OBJTYPE_BASIC;
	obj->modelnum = mpweapon->model;
	obj->flags = OBJFLAG_INVINCIBLE | OBJFLAG_CANNOT_ACTIVATE | OBJFLAG_UNCOLLECTABLE;
	obj->flags2 = OBJFLAG2_IMMUNETOGUNFIRE | OBJFLAG2_IMMUNETOEXPLOSIONS | OBJFLAG2_SHOOTTHROUGH
		| OBJFLAG2_THROWTHROUGH | OBJFLAG2_NOFALL;
	obj->flags3 = OBJFLAG3_WALKTHROUGH;
	obj->realrot[0][0] = obj->realrot[1][1] = obj->realrot[2][2] = 1;
	obj->maxdamage = 1000;
	obj->shadecol[0] = obj->shadecol[1] = obj->shadecol[2] = 0xff;
	obj->nextcol[0] = obj->nextcol[1] = obj->nextcol[2] = 0xff;
	obj->floorcol = 0x0fff;
	obj->pad = -1;

	if (g_ModelStates[obj->modelnum].modeldef == NULL) {
		if (mempGetStageFreeTotal() < ZOMBIE_BOX_POOLFLOOR) {
			return;
		}

		setupLoadModeldef(obj->modelnum);

		if (g_ModelStates[obj->modelnum].modeldef == NULL) {
			return;
		}
	}

	prop = objInitWithAutoModel(obj);

	if (prop == NULL || obj->model == NULL) {
		return;
	}

	modelSetScale(obj->model, obj->model->scale * ZOMBIE_BOX_GUNSCALE * (mpweapon->extrascale * (1.0f / 256.0f)));
	zombiesHideGunFlash(obj);

	// stood up and turned barrel along the wall, as the box turns its gun
	// along the box (zombiesBoxPlaceGun()), side on to the room
	{
		Mtxf yaw;
		Mtxf roll;

		mtx4LoadYRotation(atan2f(wall->z, -wall->x) + M_BADPI / 2, &yaw);
		mtx4LoadXRotation(-M_BADPI / 2, &roll);
		mtx4MultMtx4(&yaw, &roll, &mtx);
	}

	mtx00015f04(obj->model->scale, &mtx);
	func0f06a580(obj, &buy->pos, &mtx, buy->rooms);

	propActivate(prop);
	propEnable(prop);

	obj->hidden2 &= ~OBJH2FLAG_CANREGEN;
}

static void zombiesPlaceWallBuys(void)
{
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	struct coord taken[ZOMBIE_WALLBUY_MAX + 1 + ZOMBIE_MACHINE_IDS];
	struct coord walldir[MAX_SPAWNPOINTS];
	f32 walldist[MAX_SPAWNPOINTS];
	bool usable[MAX_SPAWNPOINTS];
	s32 ntaken = 0;
	s32 g;
	s32 i;

	// away from every perk machine (Juggernog's among them) and the box
	for (i = 0; i < ZOMBIEPERK_COUNT; i++) {
		struct prop *machine = zombiesPerkMachineProp(i);

		if (machine) {
			taken[ntaken++] = machine->pos;
		}
	}

	if (g_ZombieBox && g_ZombieBox->prop) {
		taken[ntaken++] = g_ZombieBox->prop->pos;
	}

	// every pad's nearest wall, once
	for (i = 0; i < n; i++) {
		struct pad pad;
		RoomNum rooms[2];

		padUnpack(g_SpawnPoints[i], PADFIELD_POS | PADFIELD_ROOM, &pad);
		rooms[0] = pad.room;
		rooms[1] = -1;

		usable[i] = pad.room > 0 && zombiesFindWall(&pad.pos, rooms, ZOMBIE_WALLBUY_REACH, &walldist[i], &walldir[i]);
	}

	for (g = 0; g < ZOMBIE_WALLBUY_MAX; g++) {
		const struct zombiewallgun *gun = &g_ZombieWallGuns[g];
		struct zombiewallbuy *buy = &g_ZombieWallBuys[g_ZombieNumWallBuys];
		f32 bestscore = -1;
		s32 best = -1;
		struct pad pad;

		if (gun->mpweaponnum >= ARRAYCOUNT(g_MpWeapons) || g_MpWeapons[gun->mpweaponnum].model <= 0) {
			continue;
		}

		// the pad furthest from everything placed so far; ties to the lower
		// pad number, so it is the same pads every match
		for (i = 0; i < n; i++) {
			f32 score = 1e20f;
			s32 t;

			if (!usable[i]) {
				continue;
			}

			padUnpack(g_SpawnPoints[i], PADFIELD_POS, &pad);

			for (t = 0; t < ntaken; t++) {
				f32 dx = pad.pos.x - taken[t].x;
				f32 dy = pad.pos.y - taken[t].y;
				f32 dz = pad.pos.z - taken[t].z;
				f32 d = dx * dx + dy * dy * 4 + dz * dz;

				if (d < score) {
					score = d;
				}
			}

			if (score > bestscore || (score == bestscore && g_SpawnPoints[i] < g_SpawnPoints[best])) {
				bestscore = score;
				best = i;
			}
		}

		if (best < 0) {
			break;
		}

		usable[best] = false;
		padUnpack(g_SpawnPoints[best], PADFIELD_POS | PADFIELD_ROOM, &pad);
		taken[ntaken++] = pad.pos;

		buy->mpweaponnum = gun->mpweaponnum;
		buy->cost = gun->cost;
		buy->chalk = gun->chalk;
		buy->numchalk = gun->numchalk;

		zombiesPlaceWallBuy(buy, &pad, walldist[best], &walldir[best]);

		if (buy->placed) {
			sysLogPrintf(LOG_NOTE, "zombies: wall buy %d (%d points) on stage 0x%02x at %.0f %.0f %.0f",
					buy->mpweaponnum, buy->cost, g_Vars.stagenum, buy->pos.x, buy->pos.y, buy->pos.z);
			g_ZombieNumWallBuys++;
		}
	}
}

// Whether the current player's weaponnum is full: a clip and ZOMBIE_AMMO_CLIPS
// spare of every ammunition it takes, or as much as Perfect Dark holds
static bool zombiesGunIsFull(s32 weaponnum)
{
	struct weapon *weapon = weaponFindById(weaponnum);
	s32 j;

	if (weapon == NULL) {
		return true;
	}

	for (j = 0; j < ARRAYCOUNT(weapon->ammos); j++) {
		struct inventory_ammo *ammo = weapon->ammos[j];
		s32 clip;
		s32 want;
		s32 cap;

		if (ammo == NULL || ammo->type <= 0 || (ammo->flags & AMMOFLAG_NORESERVE)) {
			continue;
		}

		clip = (ammo->clipsize > 0 ? ammo->clipsize : 1) * zombiesPapClipMult(weaponnum);
		want = clip * (zombiesPapClipMult(weaponnum) > 1 ? ZOMBIE_PAP_CLIPS_FILL : ZOMBIE_AMMO_CLIPS);
		cap = bgunGetCapacityByAmmotype(ammo->type);

		if (want > cap) {
			want = cap;
		}

		if (g_Vars.currentplayer->ammoheldarr[ammo->type] < want) {
			return false;
		}
	}

	return true;
}

// Whether the player has the wall buy's gun already: what it sells them then
// is its ammo, at half
static bool zombiesWallBuyOwned(s32 playernum, struct zombiewallbuy *buy)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	bool has;

	setCurrentPlayerNum(playernum);
	has = invHasSingleWeaponIncAllGuns(g_MpWeapons[buy->mpweaponnum].weaponnum);
	setCurrentPlayerNum(prevplayernum);

	return has;
}

// What its ammo costs the player: half the gun, or Call of Duty's 4500 for a
// gun of it they have Pack-a-Punched
#define ZOMBIE_PAP_AMMO_COST 4500

static s32 zombiesWallBuyAmmoCost(s32 playernum, struct zombiewallbuy *buy)
{
	if (zombiesGunIsPapped(playernum, g_MpWeapons[buy->mpweaponnum].weaponnum)) {
		return ZOMBIE_PAP_AMMO_COST;
	}

	return buy->cost / 2;
}

// The gun off wall buy w for the player, or its ammo if they have it already
static void zombiesWallBuyPurchase(s32 playernum, s32 w)
{
	struct zombiewallbuy *buy = &g_ZombieWallBuys[w];
	s32 weaponnum = g_MpWeapons[buy->mpweaponnum].weaponnum;
	bool owned = zombiesWallBuyOwned(playernum, buy);
	s32 cost = owned ? zombiesWallBuyAmmoCost(playernum, buy) : buy->cost;
	const s32 prevplayernum = g_Vars.currentplayernum;
	bool full = false;

	if (owned) {
		setCurrentPlayerNum(playernum);
		full = zombiesGunIsFull(weaponnum);
		setCurrentPlayerNum(prevplayernum);
	}

	if (full || g_ZombiePoints[playernum] < cost) {
		zombiesPlaySound(SFX_MENU_ERROR, 0.4f);
		return;
	}

	g_ZombiePoints[playernum] -= cost;
	zombiesAddPopup(playernum, -cost);
	zombiesPlaySound(SFX_MENU_SELECT, 1.0f);

	// the first buy puts the gun up in its outline, for everyone, for good
	zombiesWallBuyShowGun(buy);

	if (owned) {
		setCurrentPlayerNum(playernum);
		zombiesFillGun(weaponnum, ZOMBIE_AMMO_CLIPS, false);
		setCurrentPlayerNum(prevplayernum);
		zombiesPlaySound(SFX_PICKUP_AMMO, 1.0f);
	} else {
		zombiesBoxGive(playernum, buy->mpweaponnum);
		zombiesPlaySound(SFX_PICKUP_GUN, 1.0f);
	}
}

static void zombiesTickWallBuys(void)
{
	s32 i;
	s32 w;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		struct player *player = g_Vars.players[i];
		f32 bestdist = ZOMBIE_WALLBUY_RANGE * ZOMBIE_WALLBUY_RANGE;
		bool held;

		g_ZombieNearWall[i] = -1;

		if (!zombiesPlayerIsUp(i) || g_ZombieDrink60[i] >= 0 || g_ZombieNearDowned[i] >= 0) {
			g_ZombieWallHeld60[i] = 0;
			continue;
		}

		for (w = 0; w < g_ZombieNumWallBuys; w++) {
			struct coord *pos = &g_ZombieWallBuys[w].pos;
			f32 dx;
			f32 dy;
			f32 dz;

			if (!g_ZombieWallBuys[w].placed) {
				continue;
			}

			dx = player->prop->pos.x - pos->x;
			dy = player->prop->pos.y - pos->y;
			dz = player->prop->pos.z - pos->z;

			if (dx * dx + dz * dz < bestdist && dy < 200 && dy > -200) {
				bestdist = dx * dx + dz * dz;
				g_ZombieNearWall[i] = w;
			}
		}

		held = g_ZombieNearWall[i] >= 0 && zombiesHoldingUse(i);

		if (!held) {
			g_ZombieWallHeld60[i] = 0;
			g_ZombieWallNeedsRelease[i] = false;
			continue;
		}

		if (g_ZombieWallNeedsRelease[i]) {
			continue;
		}

		g_ZombieWallHeld60[i] += g_Vars.lvupdate60;

		if (g_ZombieWallHeld60[i] < ZOMBIE_BOX_HOLD60) {
			continue;
		}

		g_ZombieWallNeedsRelease[i] = true;
		g_ZombieWallHeld60[i] = 0;

		g_ZombieSoundFor = i;
		zombiesWallBuyPurchase(i, g_ZombieNearWall[i]);
		g_ZombieSoundFor = -1;
	}
}

#ifndef PLATFORM_N64
f32 videoGetAspect(void);
#endif

// How far in front of the camera (view space, whose z runs negative ahead) a
// chalk line is cut off. A point beside the camera projects ever further off
// the screen as it nears the camera's plane - standing against the wall at a
// slant, a line to one drew right across the screen - so a line is cut back
// to here first, as the renderer clips its own triangles at the near plane.
#define ZOMBIE_CHALK_NEAR (-10.0f)

// A projected x put where it lands under G_ASPECT_CENTER_EXT. As the sight
// puts its target box: the projection is the whole view, which the mode
// squeezes into its middle 4:3, so x is spread from the middle by as much
// first (sightGetAdjustedX())
static f32 zombiesHudSpreadX(f32 x)
{
#ifndef PLATFORM_N64
	return SCREEN_WIDTH_LO / 2 + (x - SCREEN_WIDTH_LO / 2) * (videoGetAspect() / SCREEN_ASPECT);
#else
	return x;
#endif
}

// The part of the line seg (x0, y0, x1, y1) inside the rectangle, in place;
// false for none (Liang-Barsky)
static bool zombiesClipLine(f32 seg[4], f32 left, f32 top, f32 right, f32 bottom)
{
	f32 dx = seg[2] - seg[0];
	f32 dy = seg[3] - seg[1];
	f32 p[4];
	f32 q[4];
	f32 t0 = 0;
	f32 t1 = 1;
	s32 k;

	p[0] = -dx; q[0] = seg[0] - left;
	p[1] = dx;  q[1] = right - seg[0];
	p[2] = -dy; q[2] = seg[1] - top;
	p[3] = dy;  q[3] = bottom - seg[1];

	for (k = 0; k < 4; k++) {
		if (p[k] == 0) {
			if (q[k] < 0) {
				return false;
			}
		} else {
			f32 r = q[k] / p[k];

			if (p[k] < 0) {
				if (r > t1) {
					return false;
				}

				if (r > t0) {
					t0 = r;
				}
			} else {
				if (r < t0) {
					return false;
				}

				if (r < t1) {
					t1 = r;
				}
			}
		}
	}

	seg[2] = seg[0] + dx * t1;
	seg[3] = seg[1] + dy * t1;
	seg[0] = seg[0] + dx * t0;
	seg[1] = seg[1] + dy * t0;

	return true;
}

// How thick a chalk line is, in the HUD's units (a unit is four or five
// pixels of a 1080 window)
#define ZOMBIE_CHALK_WIDTH 0.5f

// A filled box in the HUD's units to a quarter of one: gDPFillRectangleWideEXT()
// with the two fraction bits it shifts away, as gehud.c's hudFillBox() does.
// Whole units made a chalk line four pixels thick, and a far outline a blur
// of them. The wide fill also takes a negative x - spread for a wide screen
// the left edge is at one, which gDPFillRectangle()'s unsigned fields wrap
// round to the far right (a line clipped there drew across the screen).
static Gfx *zombiesFillQuarter(Gfx *gdl, f32 x1, f32 y1, f32 x2, f32 y2)
{
	Gfx *g0 = gdl++;
	Gfx *g1 = gdl++;

	g0->words.w0 = _SHIFTL(G_FILLRECT_WIDE_EXT, 24, 8) | _SHIFTL((s32)(x2 * 4.0f), 0, 24);
	g0->words.w1 = _SHIFTL((s32)(y2 * 4.0f), 0, 24);
	g1->words.w0 = _SHIFTL((s32)(x1 * 4.0f), 0, 24);
	g1->words.w1 = _SHIFTL((s32)(y1 * 4.0f), 0, 24);

	return gdl;
}

// A line from one point to another, ZOMBIE_CHALK_WIDTH thick, a quarter unit
// at a time; the prim colour set
static Gfx *zombiesHudLine(Gfx *gdl, f32 x0, f32 y0, f32 x1, f32 y1)
{
	const f32 half = ZOMBIE_CHALK_WIDTH * 0.5f;
	f32 dx = x1 - x0;
	f32 dy = y1 - y0;
	s32 steps = (s32)((fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy)) * 4);
	s32 i;

	// far off the screen: nothing worth the rectangles
	if (steps > 2400) {
		return gdl;
	}

	if (steps < 1) {
		steps = 1;
	}

	for (i = 0; i <= steps; i++) {
		f32 x = x0 + dx * i / steps;
		f32 y = y0 + dy * i / steps;

		gdl = zombiesFillQuarter(gdl, x - half, y - half, x + half, y + half);
	}

	return gdl;
}

// One chalk line, its ends in view space: cut at the near plane, projected,
// cut to the view, and drawn
static Gfx *zombiesChalkSegment(Gfx *gdl, struct coord *a, struct coord *b)
{
	struct player *player = g_Vars.currentplayer;
	struct coord p = *a;
	struct coord q = *b;
	f32 sp[2];
	f32 sq[2];
	f32 seg[4];

	if (p.z > ZOMBIE_CHALK_NEAR && q.z > ZOMBIE_CHALK_NEAR) {
		return gdl;
	}

	if (p.z > ZOMBIE_CHALK_NEAR) {
		f32 t = (ZOMBIE_CHALK_NEAR - p.z) / (q.z - p.z);

		p.x += (q.x - p.x) * t;
		p.y += (q.y - p.y) * t;
		p.z = ZOMBIE_CHALK_NEAR;
	} else if (q.z > ZOMBIE_CHALK_NEAR) {
		f32 t = (ZOMBIE_CHALK_NEAR - q.z) / (p.z - q.z);

		q.x += (p.x - q.x) * t;
		q.y += (p.y - q.y) * t;
		q.z = ZOMBIE_CHALK_NEAR;
	}

	cam0f0b4d68(&p, sp);
	cam0f0b4d68(&q, sq);

	seg[0] = sp[0];
	seg[1] = sp[1];
	seg[2] = sq[0];
	seg[3] = sq[1];

	if (!zombiesClipLine(seg, player->c_screenleft, player->c_screentop,
				player->c_screenleft + player->c_screenwidth - 1, player->c_screentop + player->c_screenheight - 1)) {
		return gdl;
	}

	return zombiesHudLine(gdl, zombiesHudSpreadX(seg[0]), seg[1], zombiesHudSpreadX(seg[2]), seg[3]);
}

// Every wall buy nobody has bought yet, as a chalk outline on its wall, for
// the player whose view is being drawn: from the room side, near enough, and
// with nothing between (the HUD draws over everything, the walls included)
static Gfx *zombiesRenderChalk(Gfx *gdl)
{
	struct player *player = g_Vars.currentplayer;
	bool started = false;
	s32 w;

	if (player == NULL || player->prop == NULL) {
		return gdl;
	}

	for (w = 0; w < g_ZombieNumWallBuys; w++) {
		struct zombiewallbuy *buy = &g_ZombieWallBuys[w];
		struct coord along;
		struct coord base;
		f32 dx;
		f32 dy;
		f32 dz;
		struct coord prev;
		s32 i;

		if (!buy->placed || buy->shown || buy->chalk == NULL) {
			continue;
		}

		dx = buy->pos.x - player->cam_pos.x;
		dy = buy->pos.y - player->cam_pos.y;
		dz = buy->pos.z - player->cam_pos.z;

		// near enough, and looked at from the room rather than through the wall
		if (dx * dx + dy * dy + dz * dz > ZOMBIE_CHALK_SEEN * ZOMBIE_CHALK_SEEN
				|| dx * buy->wall.x + dz * buy->wall.z < 0) {
			continue;
		}

		if (!cdTestLos04(&player->cam_pos, player->prop->rooms, &buy->pos, CDTYPE_BG | CDTYPE_DOORS)) {
			continue;
		}

		if (!started) {
#ifndef PLATFORM_N64
			gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
#endif
			gdl = text0f153628(gdl);
			gdl = textSetPrimColour(gdl, 0xf4f4f0c8);
			started = true;
		}

		// along the wall, and the outline a hair off its face
		along.x = buy->wall.z;
		along.y = 0;
		along.z = -buy->wall.x;

		base.x = buy->pos.x + buy->wall.x * (ZOMBIE_WALLBUY_OFFWALL - 1);
		base.y = buy->pos.y;
		base.z = buy->pos.z + buy->wall.z * (ZOMBIE_WALLBUY_OFFWALL - 1);

		for (i = 0; i < buy->numchalk; i++) {
			struct coord pt;
			struct coord view;
			f32 u = buy->chalk[i][0];
			f32 v = buy->chalk[i][1];

			pt.x = base.x + along.x * u;
			pt.y = base.y + v;
			pt.z = base.z + along.z * u;

			mtx4TransformVec(camGetWorldToScreenMtxf(), &pt, &view);

			if (i > 0) {
				gdl = zombiesChalkSegment(gdl, &prev, &view);
			}

			prev = view;
		}
	}

	if (started) {
		gdl = text0f153838(gdl);
		gdl = text0f153780(gdl);
#ifndef PLATFORM_N64
		gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
#endif
	}

	return gdl;
}

// The wall buys on the radar, green; past its range on the rim, pointing the way
static Gfx *zombiesRadarWallBuys(Gfx *gdl)
{
	s32 w;

	for (w = 0; w < g_ZombieNumWallBuys; w++) {
		struct coord *pos = &g_ZombieWallBuys[w].pos;
		struct coord dist;

		if (!g_ZombieWallBuys[w].placed) {
			continue;
		}

		dist.x = pos->x - g_Vars.currentplayer->prop->pos.x;
		dist.y = pos->y - g_Vars.currentplayer->prop->pos.y;
		dist.z = pos->z - g_Vars.currentplayer->prop->pos.z;

		// the gun's prop once it is up; the dot only asks whether it is the
		// player's own
		gdl = radarDrawDot(gdl, g_ZombieWallBuys[w].obj.prop, &dist, 0x40e04000, 0, 1);
	}

	return gdl;
}

// What a wall buy says to the player at it, false for nothing
static bool zombiesWallBuyPrompt(s32 playernum, char *text)
{
	struct zombiewallbuy *buy;
	char *name;
	s32 len;

	if (playernum < 0 || playernum >= MAX_PLAYERS || g_ZombieNearWall[playernum] < 0) {
		return false;
	}

	buy = &g_ZombieWallBuys[g_ZombieNearWall[playernum]];

	if (zombiesWallBuyOwned(playernum, buy)) {
		sprintf(text, "Hold %s for ammo [Cost: %d]", zombiesUseName(playernum), zombiesWallBuyAmmoCost(playernum, buy));
		return true;
	}

	name = bgunGetName(g_MpWeapons[buy->mpweaponnum].weaponnum);
	sprintf(text, "Hold %s for %s", zombiesUseName(playernum), name ? name : "the gun");
	len = strlen(text);

	// the game's names end in a newline
	while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == ' ')) {
		text[--len] = '\0';
	}

	sprintf(text + len, " [Cost: %d]", buy->cost);

	return true;
}

// What the box says to the player near it, false for nothing
static bool zombiesBoxPrompt(s32 playernum, char *text)
{
	if (playernum < 0 || playernum >= MAX_PLAYERS || !g_ZombieNearBox[playernum]) {
		return false;
	}

	if (g_ZombieBoxState == ZOMBIEBOX_IDLE) {
		sprintf(text, "Hold %s for Mystery Box [Cost: %d]", zombiesUseName(playernum), ZOMBIE_BOX_COST);
		return true;
	}

	if (g_ZombieBoxState == ZOMBIEBOX_OFFER && g_ZombieBoxUser == playernum && g_ZombieBoxShown >= 0) {
		char *name = bgunGetName(g_MpWeapons[g_ZombieBoxShown].weaponnum);
		s32 len;

		sprintf(text, "Hold %s for %s", zombiesUseName(playernum), name ? name : "the gun");
		len = strlen(text);

		// the game's names end in a newline
		while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == ' ')) {
			text[--len] = '\0';
		}

		return true;
	}

	return false;
}

/**
 * The atmosphere, as Call of Duty's: a sting of music as a round is cleared,
 * the level's music paused under it (musicStartSting()); the zombies
 * groaning as they come; and a game over that holds the screen - GAME OVER,
 * the rounds survived, the points and the arena's best - before the match's
 * own end screens.
 *
 * Perfect Dark has no music of its own for these, so they are borrowed: the
 * opening of Skedar Mystery for the end of a round (the death stings tried
 * there and at a round's start sounded like a death), and the failed
 * mission's end screen music for the game over (the solo mission's death
 * music was too much a death, and not the end of a game). The groans are
 * its pained men, its death gurgles and the Skedar's roars, pitched down.
 */
#define ZOMBIE_STING240        TICKS(1200) // a sting's length: the round break's five seconds
#define ZOMBIE_GAMEOVER60      480         // how long GAME OVER holds before the end screens
#define ZOMBIE_GAMEOVER_FADE60 60          // its fade in
#define ZOMBIE_GROAN_MIN60     70          // a groan from somewhere this often at the most,
#define ZOMBIE_GROAN_MAX60     200         // and at the least, while any zombie is near
#define ZOMBIE_GROAN_REACH     2500.0f     // how near a zombie is to be heard
#define ZOMBIE_RECORDS_FILE    "$S/zombies-records.txt"

s32 g_ZombieGameOver60;            // since the game ended (zombiesGunLowerFrac() reads it earlier)
static bool g_ZombieGameOverEnded; // mainEndStage() asked for
static s32 g_ZombieBestRound;      // the arena's best before this match, 0 for none
static bool g_ZombieNewBest;
static s32 g_ZombieNextGroan60;

/**
 * The best round each arena has seen, one line a stage: "<stagenum> <round>".
 * In the save folder, beside pd.ini.
 */
static s32 zombiesReadBest(s32 stagenum)
{
	u32 size = 0;
	char *data = fsFileLoad(ZOMBIE_RECORDS_FILE, &size);
	char *p;
	s32 best = 0;

	if (data == NULL) {
		return 0;
	}

	p = data;

	while (p < data + size) {
		s32 stage = -1;
		s32 round = 0;

		if (sscanf(p, "%d %d", &stage, &round) == 2 && stage == stagenum && round > best) {
			best = round;
		}

		while (p < data + size && *p != '\n') {
			p++;
		}

		p++;
	}

	sysMemFree(data);

	return best;
}

static void zombiesWriteBest(s32 stagenum, s32 round)
{
	u32 size = 0;
	char *data = fsFileLoad(ZOMBIE_RECORDS_FILE, &size);
	FILE *f;
	char *p;

	f = fsFileOpenWrite(ZOMBIE_RECORDS_FILE);

	if (f == NULL) {
		if (data) {
			sysMemFree(data);
		}

		return;
	}

	// every other arena's line kept
	if (data) {
		p = data;

		while (p < data + size) {
			char *end = p;
			s32 stage = -1;
			s32 old = 0;

			while (end < data + size && *end != '\n') {
				end++;
			}

			if (sscanf(p, "%d %d", &stage, &old) == 2 && stage != stagenum) {
				fprintf(f, "%d %d\n", stage, old);
			}

			p = end + 1;
		}

		sysMemFree(data);
	}

	fprintf(f, "%d %d\n", stagenum, round);
	fsFileFree(f);
}

// The music under a round being cleared, while the counter blinks. Not
// MUSIC_DEATH_BETA: that is the solo game over's tune. A round's start has
// none (MUSIC_DEATH_MP was there, and was not liked): the level's own comes
// back.
static void zombiesRoundSting(void)
{
	if (!g_ZombieCfgRoundMusic) {
		return;
	}

	musicStartSting(MUSIC_SKEDAR_MYSTERY, ZOMBIE_STING240);
}

// bondgun.c and radar.c: GAME OVER has the screen to itself, no ammo or radar
bool zombiesHidesHud(void)
{
	return zombiesIsActive() && g_ZombieGameOver;
}

static void zombiesStartGameOver(void)
{
	s32 i;

	g_ZombieGameOver = true;
	g_ZombieGameOver60 = 0;
	g_ZombieGameOverEnded = false;

	// nothing changes now: nobody dies twice or gets up
	for (i = 0; i < PLAYERCOUNT(); i++) {
		g_Vars.players[i]->invincible = true;
	}

	g_ZombieBestRound = zombiesReadBest(g_Vars.stagenum);
	g_ZombieNewBest = g_ZombieRound > g_ZombieBestRound;

	if (g_ZombieNewBest) {
		zombiesWriteBest(g_Vars.stagenum, g_ZombieRound);
	}

	sysLogPrintf(LOG_NOTE, "zombies: game over on stage 0x%02x at round %d (best before %d)",
			g_Vars.stagenum, g_ZombieRound, g_ZombieBestRound);

	musicStartSting(MUSIC_MISSION_FAILED, TICKS(240 * 20));
}

// The game over held, then the match's end screens
static void zombiesTickGameOver(void)
{
	g_ZombieGameOver60 += g_Vars.lvupdate60;

	if (g_ZombieGameOver60 >= ZOMBIE_GAMEOVER60 && !g_ZombieGameOverEnded) {
		g_ZombieGameOverEnded = true;
		zombiesRestoreSettings();
		mainEndStage();
	}
}

/**
 * Now and then a groan from a zombie near enough a player to be heard, from
 * where it is: more of them, the more zombies there are about.
 */
static void zombiesTickGroans(void)
{
	static const s16 groans[] = {
		SFX_ARGH_MALE_0086, SFX_ARGH_MALE_0088, SFX_ARGH_MALE_008B, SFX_ARGH_MALE_008E,
		SFX_ARGH_MALE_0091, SFX_ARGH_MALE_0094, SFX_ARGH_MALE_0097, SFX_ARGH_MALE_009A,
		SFX_GURGLE_05B1, SFX_GURGLE_05B2,
		SFX_SKEDAR_ROAR_0529, SFX_SKEDAR_ROAR_052D, SFX_SKEDAR_ROAR_0531,
	};
	struct chrdata *near[MAX_BOTS];
	s32 numnear = 0;
	s32 i;
	s32 p;

	if (!g_ZombieCfgGroans) {
		return;
	}

	g_ZombieNextGroan60 -= g_Vars.lvupdate60;

	if (g_ZombieNextGroan60 > 0) {
		return;
	}

	for (i = 0; i < g_BotCount && numnear < MAX_BOTS; i++) {
		struct chrdata *zchr = g_MpBotChrPtrs[i];

		if (zchr == NULL || zchr->prop == NULL || chrIsDead(zchr)) {
			continue;
		}

		for (p = 0; p < PLAYERCOUNT(); p++) {
			struct prop *pprop = g_Vars.players[p]->prop;

			if (pprop) {
				f32 dx = zchr->prop->pos.x - pprop->pos.x;
				f32 dy = zchr->prop->pos.y - pprop->pos.y;
				f32 dz = zchr->prop->pos.z - pprop->pos.z;

				if (dx * dx + dy * dy + dz * dz < ZOMBIE_GROAN_REACH * ZOMBIE_GROAN_REACH) {
					near[numnear++] = zchr;
					break;
				}
			}
		}
	}

	// the next one sooner the more of them there are
	g_ZombieNextGroan60 = ZOMBIE_GROAN_MIN60 + rngRandom() % (ZOMBIE_GROAN_MAX60 - ZOMBIE_GROAN_MIN60);

	if (numnear > 1) {
		g_ZombieNextGroan60 = g_ZombieNextGroan60 * 2 / (numnear + 1) + ZOMBIE_GROAN_MIN60 / 2;
	}

	if (numnear > 0) {
		struct chrdata *zchr = near[rngRandom() % numnear];
		s32 sound = groans[rngRandom() % ARRAYCOUNT(groans)];
		f32 pitch = 0.6f + RANDOMFRAC() * 0.2f;

		// the Skedar's roar is deep already
		if (sound >= SFX_SKEDAR_ROAR_0529 && sound <= SFX_SKEDAR_ROAR_053A) {
			pitch += 0.15f;
		}

		psCreate(NULL, zchr->prop, sound, -1, -1, 0, 0, PSTYPE_NONE, NULL, pitch, NULL, -1, -1, -1, -1);

		// online, from the same zombie on every machine (its config slot)
		zombiesNetKeepSound(sound, pitch, func0f18d074(mpPlayerGetIndex(zchr)));
	}
}

static void zombiesNetResumeTake(void);

void zombiesTick(void)
{
	s32 i;
	s32 alive;

	// no time or score limits: the match only ends when you lose
	lvSetMpTimeLimit60(0);
	lvSetMpScoreLimit(0);
	lvSetMpTeamScoreLimit(0);

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ZombieOutOfPlay[i] = i < PLAYERCOUNT() && netPlayerOutOfPlay(i);
	}

	zombiesTickPopups();

	// a new match: the frame count has gone back (the block below runs for
	// a second of frames, so it is no good for a once-only reset)
	if (g_Vars.lvframenum < g_ZombieLastFrame) {
		zombiesResetJuggernog();
		zombiesResetBox();
		zombiesResetDrops();
		zombiesResetWallBuys();
		zombiesResetPerks();
		zombiesResetPap();

		for (i = 0; i < MAX_BOTS; i++) {
			g_ZombieHealth[i] = 0;
			g_ZombieScatterPad[i] = -1;
		}

		for (i = 0; i < MAX_PLAYERS; i++) {
			g_ZombieKnife60[i] = 0;
			g_ZombieKnifeStage[i] = ZKNIFE_NONE;
			g_ZombieKnifeEnter60[i] = 0;
			g_ZombieKnifeRaise60[i] = 0;
			g_ZombieKnifeCooldown60[i] = 0;
		}

		zombiesResetGuns();

		for (i = 0; i < PLAYERCOUNT(); i++) {
			g_Vars.players[i]->healthscale = 1;
		}
	}

	g_ZombieLastFrame = g_Vars.lvframenum;

	// the players are where they spawned by now
	if (!g_ZombieJuggPlaced && g_Vars.lvframenum >= 3 && g_Vars.players[0]->prop) {
		g_ZombieJuggPlaced = true;
		zombiesPlaceJuggernog();
		zombiesPlaceBox();
		zombiesPlacePerks();
		zombiesCornerMachines();
		zombiesPlaceWallBuys();
	}

	if (g_Vars.lvframenum < 60) {
		zombiesResetPoints();

		// new match: reset state and set up teams (you vs. all bots)
		g_ZombieRound = 0;
		g_ZombieNextWaveFrame = -1;
		g_ZombieGraceUntil = 0;
		g_ZombieGameOver = false;
		g_ZombieGameOver60 = 0;
		g_ZombieGameOverEnded = false;
		g_ZombieNextGroan60 = ZOMBIE_GROAN_MAX60;
		g_ZombieDeathMsgShown = false;
		g_ZombieRoundTotal = 0;
		g_ZombieSpawned = 0;
		g_ZombieNextSpawnFrame = 0;
		for (i = 0; i < PLAYERCOUNT(); i++) {
			g_ZombieMaxHealth[i] = g_Vars.players[i]->bondhealth;
			g_ZombieLastHealth[i] = g_ZombieMaxHealth[i];
			g_ZombieRegenDelay60[i] = 0;
		}

		zombiesResetDowned();

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

	zombiesTickDowned();

	// nobody is left standing: GAME OVER holds the screen a while, then the
	// match's end screens (zombiesTickGameOver())
	if (!g_ZombieGameOver && g_ZombieRound > 0 && zombiesEveryoneIsDown()) {
		zombiesStartGameOver();
	}

	if (g_ZombieGameOver) {
		zombiesTickGameOver();
		return;
	}

	for (i = 0; i < PLAYERCOUNT(); i++) {
		// online, a buy's and a drink's sounds are the player's own
		g_ZombieSoundFor = i;
		zombiesTickRegen(i);
		zombiesTickJuggernog(i);
		zombiesTickPerkMachines(i);
		zombiesTickPap(i);
		zombiesTickWhere(i);
		zombiesTickKnife(i);
	}

	g_ZombieSoundFor = -1;

	zombiesTintPerkMachines();
	zombiesTintPap();

	zombiesTickBox();
	zombiesTickWallBuys();
	zombiesTickDrops();

	if (g_ZombieRound > 0) {
		zombiesTickGroans();
	}



	if (g_ZombieRound == 0) {
		if (g_Vars.lvframenum >= 120) {
			// the Zombies Options page's Starting Round
			g_ZombieRound = g_ZombieCfgStartRound > 1 ? g_ZombieCfgStartRound : 1;
			// a match carried on under a new host: its round, and what each had
			zombiesNetResumeTake();
			g_ZombieRoundStartFrame = g_Vars.lvframenum;
			g_ZombieGraceUntil = g_Vars.lvframenum + 120;
			g_ZombieNextSpawnFrame = g_Vars.lvframenum + 120;
			g_ZombieRoundTotal = zombiesTotalForRound(g_ZombieRound);
			// the zombies already out from the start of the match count as spawned
			g_ZombieSpawned = zombiesCountAlive();
		}
		return;
	}

	// make every living zombie come after the nearest player still standing
	for (i = 0; i < g_BotCount; i++) {
		struct chrdata *zchr = g_MpBotChrPtrs[i];

		if (zchr && zchr->aibot && zchr->prop && !chrIsDead(zchr)) {
			struct prop *target = zombiesNearestTarget(zchr);

			// the round's health, held: a simulant's own script sets its
			// maxdamage as it comes back in, after zombiesSpawnOne() has
			if (g_ZombieHealth[i] > 0 && zchr->maxdamage != g_ZombieHealth[i]) {
				zchr->maxdamage = g_ZombieHealth[i];
			}

			// nobody standing (down alone with Quick Revive): off elsewhere
			// rather than crowding the body, as Call of Duty's wander off
			if (target == NULL) {
				zombiesScatter(i, zchr);
				continue;
			}

			g_ZombieScatterPad[i] = -1;

			if (zchr->aibot->command != AIBOTCMD_ATTACK
					|| zchr->aibot->attackpropnum != target - g_Vars.props) {
				botApplyAttack(zchr, target);
			}

			// always know where the player is, even without line of sight
			botSetTarget(zchr, target - g_Vars.props);
		}
	}

	if (g_BotCount == 0 || g_Vars.lvframenum < g_ZombieGraceUntil) {
		return;
	}

	alive = zombiesCountAlive();

	// let the zombies in one at a time, never above the cap
	if (g_ZombieSpawned < g_ZombieRoundTotal && alive < ZOMBIE_MAX_ALIVE
			&& g_Vars.lvframenum >= g_ZombieNextSpawnFrame) {
		if (zombiesSpawnOne()) {
			alive++;
			g_ZombieNextSpawnFrame = g_Vars.lvframenum + zombiesSpawnDelay(g_ZombieRound);
		}
	}

	if (alive > 0 || g_ZombieSpawned < g_ZombieRoundTotal) {
		g_ZombieNextWaveFrame = -1;
		return;
	}

	// the round is cleared: wait a few seconds, then start the next one
	if (g_ZombieNextWaveFrame < 0) {
		g_ZombieNextWaveFrame = g_Vars.lvframenum + ZOMBIE_ROUND_BREAK;
		zombiesRoundSting();
	} else if (g_Vars.lvframenum >= g_ZombieNextWaveFrame) {
		g_ZombieRound++;
		zombiesRespawnTheDead();
		g_ZombieDropsThisRound = 0;
		g_ZombieRoundStartFrame = g_Vars.lvframenum;
		g_ZombieNextWaveFrame = -1;
		g_ZombieGraceUntil = g_Vars.lvframenum + 120;
		g_ZombieNextSpawnFrame = g_Vars.lvframenum + 120;
		g_ZombieRoundTotal = zombiesTotalForRound(g_ZombieRound);
		g_ZombieSpawned = 0;
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

// input.c: Zombies' own controls in force (inputLiveBinds()); an s32, as
// bool is not the same size on both sides
s32 zombiesLayoutWanted(void)
{
	return zombiesIsActive() ? 1 : 0;
}

// A player's points (zombiesAwardHit() adds them up), all of them: the
// rankings sort on sixteen bits of it (mpRankKeyScore()) and show the whole
static s32 zombiesGetPoints(struct mpchrconfig *mpchr, s32 chrnum)
{
	s32 points;

	if (chrnum < 0 || chrnum >= MAX_PLAYERS) {
		return 0;
	}

	points = g_ZombiePoints[chrnum];

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

// A line's height in a font. textMeasure() only counts a line it sees a "\n"
// end, so a bare "1060" measures zero high.
static s32 zombiesLineHeight(struct fontchar *chars, struct font *font)
{
	s32 textwidth;
	s32 textheight;

	textMeasure(&textheight, &textwidth, "0\n", chars, font, 0);

	return textheight;
}

// The bottom-right stack, laid out as Call of Duty: Black Ops does it. From the
// bottom: the function's name and square (where Black Ops has its grenades,
// left where bondgun.c puts it), the ammo, the gun's name a size smaller, and
// the points on top. bottom is bondgun.c's own.
// The stack's right edge, in from the view's: where bondgun.c ends the gun's
// name and the function's square for one player (24, the bar's 9 and 2)
#define ZOMBIE_HUD_RIGHT 35

static s32 zombiesHudAmmoBottom(s32 bottom)
{
	return bottom - zombiesLineHeight(g_CharsHandelGothicXs, g_FontHandelGothicXs) - 3;
}

// Also called by bondgun.c for where the gun's name goes.
s32 zombiesHudNameBottom(s32 bottom)
{
	s32 namebottom = zombiesHudAmmoBottom(bottom) - zombiesLineHeight(g_CharsHandelGothicSm, g_FontHandelGothicSm) - 1;

	// the points sit on top of the name, whether it is showing or has faded
	if (g_Vars.currentplayernum >= 0 && g_Vars.currentplayernum < MAX_PLAYERS) {
		g_ZombieStackTop[g_Vars.currentplayernum] = namebottom - zombiesLineHeight(g_CharsHandelGothicXs, g_FontHandelGothicXs);
	}

	return namebottom;
}

// Draws text with its left edge at x and its bottom edge at bottomy.
static Gfx *zombiesDrawTextBL(Gfx *gdl, char *text, s32 x, s32 bottomy, u32 colour, struct fontchar *chars, struct font *font)
{
	s32 textwidth;
	s32 textheight;
	s32 y;

	textMeasure(&textheight, &textwidth, text, chars, font, 0);
	textheight = zombiesLineHeight(chars, font);

	y = bottomy - textheight;

#if VERSION >= VERSION_NTSC_1_0
	// left, top, width, height
	gdl = text0f153990(gdl, x, y, textwidth, textheight);
	gdl = textRender(gdl, &x, &y, text, chars, font, colour, 0xa0, viGetWidth(), viGetHeight(), 0, 0);
#else
	// this one wants the right and bottom edges
	textwidth += x;
	textheight += y;
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
	char text[48];
	s32 rightx = (viGetViewLeft() + viGetViewWidth()) / scalex - ZOMBIE_HUD_RIGHT;
	s32 ammobottom;

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

	// over the function's name, under the gun's (zombiesHudAmmoBottom())
	ammobottom = zombiesHudAmmoBottom(bottom);
	zombiesHudNameBottom(bottom);

	return zombiesDrawTextBR(gdl, text, rightx, ammobottom, 0xffffffc0, g_CharsHandelGothicSm, g_FontHandelGothicSm);
}

// Draws a line of words (letters, not just digits) centred at the given spot.
static Gfx *zombiesDrawWords(Gfx *gdl, char *text, s32 centrex, s32 y, u32 colour)
{
	s32 textwidth;
	s32 textheight;
	s32 x;

	textMeasure(&textheight, &textwidth, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);

	textheight = zombiesLineHeight(g_CharsHandelGothicXs, g_FontHandelGothicXs);
	x = centrex - textwidth / 2;

#if VERSION >= VERSION_NTSC_1_0
	gdl = text0f153990(gdl, x, y, textwidth, textheight);
	gdl = textRender(gdl, &x, &y, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, colour, 0xa0, viGetWidth(), viGetHeight(), 0, 0);
#else
	textwidth += x;
	textheight += y;
	gdl = text0f153858(gdl, &x, &y, &textwidth, &textheight);
	gdl = textRender(gdl, &x, &y, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, colour, 0x88, viGetWidth(), viGetHeight(), 0, 0);
#endif

	return gdl;
}

// The round counter, as big as Call of Duty's: the large font at twice its size
#define ZOMBIE_ROUND_SCALE    1
#define ZOMBIE_ROUND_RED      0xb0
#define ZOMBIE_ROUND_ALPHA    0xe0
#define ZOMBIE_ROUND_FADEIN   60  // frames a new round's number takes to go from white to red
#define ZOMBIE_ROUND_FADEOUT  40  // frames the old number takes to fade before the new one
#define ZOMBIE_ROUND_FLASH    40  // frames in one red-white-red flash between rounds

// How white the counter is, 0 (red) to 255 (white).
static s32 zombiesRoundWhiteness(void)
{
	s32 since = g_Vars.lvframenum - g_ZombieRoundStartFrame;

	if (g_ZombieNextWaveFrame >= 0) {
		// round cleared: flash between red and white until the next one starts
		s32 phase = (g_Vars.lvframenum - (g_ZombieNextWaveFrame - ZOMBIE_ROUND_BREAK)) % ZOMBIE_ROUND_FLASH;

		if (phase < ZOMBIE_ROUND_FLASH / 2) {
			return phase * 255 / (ZOMBIE_ROUND_FLASH / 2);
		}

		return (ZOMBIE_ROUND_FLASH - phase) * 255 / (ZOMBIE_ROUND_FLASH / 2);
	}

	if (since >= 0 && since < ZOMBIE_ROUND_FADEIN) {
		// new round: comes in white and settles to red
		return 255 - since * 255 / ZOMBIE_ROUND_FADEIN;
	}

	return 0;
}

static s32 zombiesRoundAlpha(void)
{
	s32 since = g_Vars.lvframenum - g_ZombieRoundStartFrame;

	if (g_ZombieNextWaveFrame >= 0) {
		s32 left = g_ZombieNextWaveFrame - g_Vars.lvframenum;

		if (left < ZOMBIE_ROUND_FADEOUT) {
			return left <= 0 ? 0 : ZOMBIE_ROUND_ALPHA * left / ZOMBIE_ROUND_FADEOUT;
		}

		return ZOMBIE_ROUND_ALPHA;
	}

	if (since >= 0 && since < ZOMBIE_ROUND_FADEIN / 3) {
		return ZOMBIE_ROUND_ALPHA * since / (ZOMBIE_ROUND_FADEIN / 3);
	}

	return ZOMBIE_ROUND_ALPHA;
}

static u32 zombiesRoundColour(void)
{
	s32 white = zombiesRoundWhiteness();
	u32 r = ZOMBIE_ROUND_RED + (0xff - ZOMBIE_ROUND_RED) * white / 255;
	u32 gb = white;

	return (r << 24) | (gb << 16) | (gb << 8) | zombiesRoundAlpha();
}

static Gfx *zombiesFillRect(Gfx *gdl, f32 x1, f32 y1, f32 x2, f32 y2, u32 colour)
{
	if (x2 <= x1 || y2 <= y1) {
		return gdl;
	}

	return text0f153a34(gdl, (s32)x1, (s32)y1, (s32)x2, (s32)y2, colour);
}

// A perk icon's badge, as wide as it is at v (0 at the top, 1 at the bottom),
// from 0 to 1, in Black Ops' shape: a flat top with its corners only just
// taken off, straight sides, and the bottom coming in to a shallow point
static f32 zombiesCrestHalfWidth(f32 v)
{
	if (v < 0.06f) {
		return 0.9f + 0.1f * (v / 0.06f);
	}

	if (v < 0.66f) {
		return 1;
	}

	return 1 - (v - 0.66f) / 0.34f * 0.94f;
}

// The crest filled in one colour, a rectangle a row: the RDP fills only
// rectangles, and a row at a time is how a shape is cut from them. Only the
// rows from v0 to v1 of it (0 and 1 for the whole crest).
static Gfx *zombiesFillCrestPart(Gfx *gdl, f32 cx, f32 top, f32 halfwidth, f32 height, f32 v0, f32 v1, u32 colour)
{
	s32 rows = (s32)height;
	s32 r;

	if (rows <= 0) {
		return gdl;
	}

	gdl = textSetPrimColour(gdl, colour);

	for (r = (s32)(v0 * rows); r < (s32)(v1 * rows); r++) {
		f32 hw = halfwidth * zombiesCrestHalfWidth((r + 0.5f) / rows);
		s32 x1 = (s32)(cx - hw + 0.5f);
		s32 x2 = (s32)(cx + hw + 0.5f);

		if (x2 > x1) {
			gDPFillRectangle(gdl++, x1, (s32)top + r, x2, (s32)top + r + 1);
		}
	}

	return text0f153838(gdl);
}

static Gfx *zombiesFillCrest(Gfx *gdl, f32 cx, f32 top, f32 halfwidth, f32 height, u32 colour)
{
	return zombiesFillCrestPart(gdl, cx, top, halfwidth, height, 0, 1, colour);
}

#ifndef PLATFORM_N64
#include "menuimage.h"
struct menuimage *zombiePerkImage(s32 perk);
#endif

// How tall a perk icon is on a 240-line screen: Black Ops' are about a
// twentieth of the screen's height (55 pixels at 1080)
#define ZOMBIE_PERK_ICON_SIZE 12.5f

// A perk's colour as RGBA, made brighter or darker by shade (1 for itself)
static u32 zombiesPerkColour(s32 perk, f32 shade, u32 alpha)
{
	u32 rgb[3];
	s32 j;

	for (j = 0; j < 3; j++) {
		f32 v = g_ZombiePerks[perk].colour[j] * shade;

		rgb[j] = v > 0xff ? 0xff : (u32)v;
	}

	return rgb[0] << 24 | rgb[1] << 16 | rgb[2] << 8 | (alpha & 0xff);
}

// A perk's drawn icon (when there is no picture of it), its bottom point at
// bottom, as wide as it is tall: the perk's colour, lighter towards the top,
// in a thin pale rim, with a white shield in the middle
static Gfx *zombiesRenderJuggIcon(Gfx *gdl, f32 cx, f32 bottom, f32 s, s32 perk)
{
	f32 h = ZOMBIE_PERK_ICON_SIZE * s;
	f32 hw = h / 2;
	f32 rim = h * 0.07f;
	f32 top = bottom - h;

	// the rim, a shade darker underneath so it reads against a bright floor
	gdl = zombiesFillCrest(gdl, cx, top + rim * 0.6f, hw, h, 0x200000a0);
	gdl = zombiesFillCrest(gdl, cx, top, hw, h, 0xe8e0dcf0);

	// the face, and its lighter top
	gdl = zombiesFillCrest(gdl, cx, top + rim, hw - rim, h - rim * 2.4f, zombiesPerkColour(perk, 0.85f, 0xf0));
	gdl = zombiesFillCrestPart(gdl, cx, top + rim, hw - rim, h - rim * 2.4f, 0, 0.4f, zombiesPerkColour(perk, 1.0f, 0xf0));

	// the shield: white, the perk's colour in the middle, and a white bar down it
	gdl = zombiesFillCrest(gdl, cx, top + h * 0.2f, h * 0.24f, h * 0.56f, 0xf8f8f8ff);
	gdl = zombiesFillCrest(gdl, cx, top + h * 0.27f, h * 0.16f, h * 0.4f, zombiesPerkColour(perk, 0.75f, 0xff));
	gdl = zombiesFillRect(gdl, cx - h * 0.035f, top + h * 0.27f, cx + h * 0.035f, top + h * 0.6f, 0xf8f8f8ff);

	return gdl;
}

// A perk's bottle, drawn up out of the bottom of the screen while the gun
// is down: glass and drink in the perk's colour, a white label, a silver cap.
// It comes up, rises to the mouth as it drains, and goes back down. s is a
// 240-line screen's pixel.
static Gfx *zombiesRenderBottle(Gfx *gdl, s32 t, s32 perk)
{
	f32 s = viGetViewHeight() / 240.0f;
	f32 offbottom = viGetViewTop() + viGetViewHeight() + 90 * s;
	f32 held = viGetViewTop() + viGetViewHeight() - 6 * s;
	f32 cx = viGetViewLeft() + viGetViewWidth() / 2 + 34 * s;
	f32 by;
	f32 level = 1;
	f32 p;

	if (t < DRINK_GUNDOWN || t >= DRINK_GULPEND + DRINK_BOTTLEDOWN) {
		return gdl;
	}

	if (t < DRINK_GULPSTART) {
		p = (f32)(t - DRINK_GUNDOWN) / DRINK_BOTTLEUP;
		p = p * p * (3 - 2 * p);
		by = offbottom + (held - offbottom) * p;
	} else if (t < DRINK_GULPEND) {
		f32 q = (f32)(t - DRINK_GULPSTART) / DRINK_GULP;
		f32 lift = q * 4 > 1 ? 1 : q * 4;

		by = held - 22 * s * lift;
		cx -= 18 * s * lift;
		level = 1 - q;
	} else {
		p = (f32)(t - DRINK_GULPEND) / DRINK_BOTTLEDOWN;
		p = p * p;
		by = held - 22 * s + (offbottom - (held - 22 * s)) * p;
		cx -= 18 * s * (1 - p);
		level = 0;
	}

	// body: the empty part dark, the drink from the bottom up
	gdl = zombiesFillRect(gdl, cx - 14 * s, by - 54 * s, cx + 14 * s, by, zombiesPerkColour(perk, 0.3f, 0xc0));
	gdl = zombiesFillRect(gdl, cx - 12 * s, by - 2 * s - 50 * s * level, cx + 12 * s, by - 2 * s, zombiesPerkColour(perk, 0.95f, 0xe8));
	// label
	gdl = zombiesFillRect(gdl, cx - 14 * s, by - 36 * s, cx + 14 * s, by - 22 * s, 0xe8e8e8e8);
	gdl = zombiesFillRect(gdl, cx - 14 * s, by - 31 * s, cx + 14 * s, by - 27 * s, zombiesPerkColour(perk, 0.85f, 0xe8));
	// shoulder, neck, cap
	gdl = zombiesFillRect(gdl, cx - 9 * s, by - 60 * s, cx + 9 * s, by - 54 * s, zombiesPerkColour(perk, 0.65f, 0xe0));
	gdl = zombiesFillRect(gdl, cx - 5 * s, by - 74 * s, cx + 5 * s, by - 60 * s, zombiesPerkColour(perk, 0.65f, 0xe0));
	gdl = zombiesFillRect(gdl, cx - 6 * s, by - 79 * s, cx + 6 * s, by - 74 * s, 0xb4b4b4ff);

	return gdl;
}

// The machine's prompt, the drink and the perk's icon, for the player whose
// view is being drawn.
static Gfx *zombiesRenderJuggernog(Gfx *gdl)
{
	s32 playernum = g_Vars.currentplayernum;
	char text[64];

	if (playernum < 0 || playernum >= MAX_PLAYERS) {
		return gdl;
	}

#ifndef PLATFORM_N64
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
#endif

	gdl = text0f153628(gdl);

	if (g_ZombieMarking) {
		s32 textwidth;
		s32 textheight;

		strcpy(text, "Marking: look where Juggernog goes, press E / B");
		textMeasure(&textheight, &textwidth, text, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);
		gdl = zombiesDrawTextBL(gdl, text, viGetViewLeft() + (viGetViewWidth() - textwidth) / 2,
				viGetViewTop() + viGetViewHeight() * 62 / 100, 0x40ff40e0,
				g_CharsHandelGothicSm, g_FontHandelGothicSm);
	} else if (g_ZombieDrink60[playernum] >= 0) {
		// the HUD's bottle when there is no drink in the hands, in first
		// person or on the body in third
		if (!zombiesDrinkViewmodelActive(playernum) && !zombiesBodyDrinkSeen(playernum)) {
			gdl = zombiesRenderBottle(gdl, g_ZombieDrink60[playernum], g_ZombieDrinkPerk[playernum]);
		}
	} else if (g_ZombieNearPerk[playernum] >= 0) {
		s32 perk = g_ZombieNearPerk[playernum];
		s32 textwidth;
		s32 textheight;

		sprintf(text, "Hold %s for %s [Cost: %d]", zombiesUseName(playernum), g_ZombiePerks[perk].name, zombiesPerkCost(perk));
		textMeasure(&textheight, &textwidth, text, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);
		gdl = zombiesDrawTextBL(gdl, text, viGetViewLeft() + (viGetViewWidth() - textwidth) / 2,
				viGetViewTop() + viGetViewHeight() * 62 / 100, 0xffffffe0,
				g_CharsHandelGothicSm, g_FontHandelGothicSm);
	} else if (g_ZombieNearJugg[playernum]) {
		s32 textwidth;
		s32 textheight;

		// B is E on the keyboard by default (input.c)
		sprintf(text, "Hold %s for Juggernog [Cost: %d]", zombiesUseName(playernum), ZOMBIE_JUGG_COST);
		textMeasure(&textheight, &textwidth, text, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);
		gdl = zombiesDrawTextBL(gdl, text, viGetViewLeft() + (viGetViewWidth() - textwidth) / 2,
				viGetViewTop() + viGetViewHeight() * 62 / 100, 0xffffffe0,
				g_CharsHandelGothicSm, g_FontHandelGothicSm);
	} else if (zombiesPapPrompt(playernum, text) || zombiesBoxPrompt(playernum, text) || zombiesWallBuyPrompt(playernum, text)) {
		s32 textwidth;
		s32 textheight;

		textMeasure(&textheight, &textwidth, text, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);
		gdl = zombiesDrawTextBL(gdl, text, viGetViewLeft() + (viGetViewWidth() - textwidth) / 2,
				viGetViewTop() + viGetViewHeight() * 62 / 100, 0xffffffe0,
				g_CharsHandelGothicSm, g_FontHandelGothicSm);
	}

	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

	// the perks' icons in a row, bottom-left over the round number as Black
	// Ops has them
	{
		f32 s = viGetViewHeight() / 240.0f;
		s32 roundheight = g_FontHandelGothicLg ? zombiesLineHeight(g_CharsHandelGothicLg, g_FontHandelGothicLg) : 24;
		// flush with the round number's left edge, just over it
		f32 x = viGetViewLeft() + 6;
		f32 y2 = viGetViewTop() + viGetViewHeight() - 2 - roundheight * ZOMBIE_ROUND_SCALE - 2;
		bool started = false;
		s32 k;
		s32 o;

		for (o = 0; o < g_ZombiePerkOrderCount[playernum]; o++) {
			k = g_ZombiePerkOrder[playernum][o];

			if (!zombiesPlayerHasPerk(playernum, k)) {
				continue;
			}

			if (!started) {
#ifndef PLATFORM_N64
				gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeL);
#endif
				gdl = text0f153628(gdl);
				started = true;
			}

#ifndef PLATFORM_N64
			// the player's own picture of it when they have one (zombieperks.c)
			{
				struct menuimage *img = zombiePerkImage(k);

				if (img) {
					s32 size = (s32)(ZOMBIE_PERK_ICON_SIZE * s + 0.5f);

					gdl = menuImageDraw(gdl, img, (s32)x, (s32)y2 - size, (s32)x + size, (s32)y2, 0xff);
					gdl = text0f153628(gdl);
				} else {
					gdl = zombiesRenderJuggIcon(gdl, x + ZOMBIE_PERK_ICON_SIZE * s / 2, y2, s, k);
				}
			}
#else
			gdl = zombiesRenderJuggIcon(gdl, x + ZOMBIE_PERK_ICON_SIZE * s / 2, y2, s, k);
#endif

			x += ZOMBIE_PERK_ICON_SIZE * s + 2;
		}

		if (started) {
			gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
			gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif
		}
	}

	return gdl;
}

#ifndef PLATFORM_N64
// The player's own head beside the points, as Black Ops III has it: the chr's
// model as the level has it (no copy is loaded, and the menus' model borrows
// the gun's memory), posed a second time into matrices of its own under a
// camera in front of the face, and drawn through a projection whose frustum
// is the box. The world's matrices are put back after, so nothing else that
// reads the model this frame (its held gun, the next view) sees the portrait.

// How much taller the box is than its points' digits, and the camera's field
// of view (narrow, so the face is not distorted)
#define ZOMBIE_PORTRAIT_OVER 1.25f
#define ZOMBIE_PORTRAIT_FOVY 24.0f

extern Lights1 var80071468; // the menus' model light (menu.c)
extern u8 var80062a48[];    // a chr's wound tint (chr.c)

// A head in the body's own units, from the joint it hangs off (a chr's root,
// at the top of the torso): where its middle is and how tall it is. Not
// measured from the head: a loaded head's lists have been rewritten, and
// headfitMeasureHeadWhole() made one ten times its size.
#define ZOMBIE_PORTRAIT_HEAD_MID    130.0f
#define ZOMBIE_PORTRAIT_HEAD_HEIGHT 260.0f

/**
 * Where an x drawn under the HUD's alignment mode really lands, in the
 * screen's coordinates with no alignment: the vertex shader's and the
 * rectangles' (ofs + x) * native / aspect in clip space (gfx_pc.cpp,
 * gfx_update_aspect_mode()). A scissor set under the mode is moved by ofs
 * alone (gfx_adjust_viewport_or_scissor()), which put the portrait's left
 * half outside its own scissor on a wide screen, so it is set without one.
 */
static f32 zombiesHudAlignedX(f32 x, u32 mode)
{
	f32 width = viGetWidth();
	f32 native = (f32)videoGetNativeWidth() / videoGetNativeHeight();
	f32 aspect = videoGetAspect();
	f32 ndc = 2.0f * x / width - 1.0f;
	f32 ofs = 0.0f;

	if (mode == 0 || native <= 0.0f || aspect <= 0.0f) {
		return x;
	}

	if ((mode & G_ASPECT_CENTER_EXT) == G_ASPECT_LEFT_EXT) {
		ofs = 1.0f - aspect / native;
	} else if ((mode & G_ASPECT_CENTER_EXT) == G_ASPECT_RIGHT_EXT) {
		ofs = aspect / native - 1.0f;
	}

	if ((mode & G_ASPECT_CENTER_EXT) && (mode & G_ASPECT_WIDE_EXT) && aspect > 16.0f / 9.0f) {
		ofs *= (16.0f / 9.0f) / aspect;
	}

	ndc = (ofs + ndc) * native / aspect;

	return (ndc + 1.0f) * 0.5f * width;
}

/**
 * A player's head in a box boxh tall whose right edge is at right and bottom
 * at bottom, in the HUD's coordinates as the points are drawn (aligned to the
 * right edge), backed in backing. Returns the box's left edge, or right when
 * nothing was drawn.
 */
static s32 zombiesRenderPortrait(Gfx **gdlptr, s32 playernum, s32 right, s32 bottom, s32 boxh, u32 backing)
{
	Gfx *gdl = *gdlptr;
	struct prop *prop = (playernum >= 0 && playernum < PLAYERCOUNT()) ? g_Vars.players[playernum]->prop : NULL;
	struct chrdata *chr = prop ? prop->chr : NULL;
	struct model *model = chr ? chr->model : NULL;
	struct modelnode *spot;
	struct modelrenderdata renderdata = {NULL, true, 3};
	Mtxf *worldmtxs;
	Mtxf *viewmtxs;
	Mtxf *savedmtxs;
	Mtxf identity;
	Mtxf view;
	Mtxf proj;
	Mtx *projl;
	Vp *vp;
	Mtxf *neck;
	struct coord centre;
	struct coord facing;
	f32 unit;
	f32 framing;
	f32 dist;
	s32 n;
	s32 i;
	s32 width = viGetWidth();
	s32 height = viGetHeight();
	s32 boxw;
	s32 left;
	s32 top;
	f32 x1n;
	f32 x2n;
	f32 y1n;
	f32 y2n;
	f32 sx;
	f32 sy;
	f32 cx;
	f32 cy;
	u16 perspnorm;
	void (*jointfunc)(s32 mtxindex, Mtxf *mtx);

	if (playernum < 0 || playernum >= MAX_PLAYERS || model == NULL || model->definition == NULL
			|| model->definition->skel != &g_SkelChr) {
		return right;
	}

	spot = modelGetPart(model->definition, MODELPART_CHR_HEADSPOT);

	if (spot == NULL) {
		return right;
	}

	n = model->definition->nummatrices;

	if (!gfxHasVtxSpace(n * sizeof(Mtxf) * 2 + sizeof(Vp) + sizeof(Mtx) * 2)) {
		return right;
	}

	// The box: square on the screen (the HUD's pixels under the right
	// alignment are the native mode's)
	boxw = boxh * width / (height * ((f32)videoGetNativeWidth() / videoGetNativeHeight()));
	left = right - boxw;
	top = bottom - boxh;

	// Posed once in the world for where the head is and which way the body
	// faces, the joint hook off: chrHandleJointPositioned() aims the chr
	// being ticked (g_CurModelChr), not this one
	savedmtxs = model->matrices;
	jointfunc = g_ModelJointPositionedFunc;
	g_ModelJointPositionedFunc = NULL;

	mtx4LoadIdentity(&identity);
	worldmtxs = gfxAllocate(n * sizeof(Mtxf));
	renderdata.unk00 = &identity;
	renderdata.unk10 = worldmtxs;
	modelSetMatricesWithAnim(&renderdata, model);

	neck = modelFindNodeMtx(model, spot, 0);

	if (neck == NULL) {
		model->matrices = savedmtxs;
		g_ModelJointPositionedFunc = jointfunc;
		return right;
	}

	// a unit of the body's own space in the world, from its joint's y axis
	unit = sqrtf(neck->m[1][0] * neck->m[1][0] + neck->m[1][1] * neck->m[1][1] + neck->m[1][2] * neck->m[1][2]);

	centre.x = neck->m[3][0] + neck->m[1][0] * ZOMBIE_PORTRAIT_HEAD_MID;
	centre.y = neck->m[3][1] + neck->m[1][1] * ZOMBIE_PORTRAIT_HEAD_MID;
	centre.z = neck->m[3][2] + neck->m[1][2] * ZOMBIE_PORTRAIT_HEAD_MID;

	// the head and a little of the neck, the middle just under the face
	framing = ZOMBIE_PORTRAIT_HEAD_HEIGHT * unit * 1.3f;
	centre.y -= framing * 0.06f;

	// the way the chr faces, as the world and the spectator's camera have it
	{
		f32 inv = chrGetInverseTheta(chr);

		facing.x = sinf(inv);
		facing.y = 0;
		facing.z = cosf(inv);
	}

	dist = framing * 0.5f * cosf(ZOMBIE_PORTRAIT_FOVY * (M_PI / 360.0f)) / sinf(ZOMBIE_PORTRAIT_FOVY * (M_PI / 360.0f));

	// a touch to the side, as Black Ops' is: the face turned slightly
	{
		f32 turn = 0.35f;
		f32 ex = facing.x * cosf(turn) - facing.z * sinf(turn);
		f32 ez = facing.x * sinf(turn) + facing.z * cosf(turn);

		guLookAtF(view.m, centre.x + ex * dist, centre.y + framing * 0.08f, centre.z + ez * dist,
				centre.x, centre.y, centre.z, 0, 1, 0);
	}

	// Posed again through the camera, which is also what picks its levels
	// of detail
	viewmtxs = gfxAllocate(n * sizeof(Mtxf));
	renderdata.unk00 = &view;
	renderdata.unk10 = viewmtxs;
	modelSetMatricesWithAnim(&renderdata, model);

	g_ModelJointPositionedFunc = jointfunc;

	// A projection whose whole frustum is the box: a square one, scaled and
	// moved in clip space onto the box's part of a full-screen viewport, so
	// that under the HUD's right alignment it lands where the text does
	guPerspectiveF(proj.m, &perspnorm, ZOMBIE_PORTRAIT_FOVY, 1.0f, dist * 0.2f, dist * 4.0f, 1.0f);

	x1n = 2.0f * left / width - 1.0f;
	x2n = 2.0f * right / width - 1.0f;
	y1n = 1.0f - 2.0f * top / height;
	y2n = 1.0f - 2.0f * bottom / height;
	sx = (x2n - x1n) * 0.5f;
	sy = (y1n - y2n) * 0.5f;
	cx = (x1n + x2n) * 0.5f;
	cy = (y1n + y2n) * 0.5f;

	for (i = 0; i < 4; i++) {
		proj.m[i][0] = sx * proj.m[i][0] + cx * proj.m[i][3];
		proj.m[i][1] = sy * proj.m[i][1] + cy * proj.m[i][3];
	}

	projl = gfxAllocateMatrix();
	guMtxF2L(proj.m, projl);

	vp = gfxAllocate(sizeof(Vp));
	vp->vp.vscale[0] = width * 2;
	vp->vp.vscale[1] = height * 2;
	vp->vp.vscale[2] = 511;
	vp->vp.vscale[3] = 0;
	vp->vp.vtrans[0] = width * 2;
	vp->vp.vtrans[1] = height * 2;
	vp->vp.vtrans[2] = 511;
	vp->vp.vtrans[3] = 0;

	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeR);

	// a dark backing, as the head is drawn over whatever is behind the HUD
	gdl = text0f153628(gdl);
	gdl = zombiesFillRect(gdl, left, top, right, bottom, backing);
	gdl = text0f153780(gdl);

	// The gun has been drawn into the depth already, and the world before it;
	// nothing after the HUD reads them (TAA took its copy before the gun)
	gdl = viPrepareZbuf(gdl);

	gDPPipeSync(gdl++);
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
	gDPSetScissor(gdl++, G_SC_NON_INTERLACE,
			(s32)zombiesHudAlignedX(left, g_HudAlignModeR), top,
			(s32)(zombiesHudAlignedX(right, g_HudAlignModeR) + 0.999f), bottom);
	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeR);

	gSPDisplayList(gdl++, var80061380);
	gSPDisplayList(gdl++, var800613a0);

	gSPViewport(gdl++, OS_K0_TO_PHYSICAL(vp));
	gSPMatrix(gdl++, OS_K0_TO_PHYSICAL(projl), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, perspnorm);

	gSPSetLights1(gdl++, var80071468);

	if (camGetLookAt()) {
		gSPLookAt(gdl++, camGetLookAt());
	}

	gSPSetGeometryMode(gdl++, G_ZBUFFER);

	renderdata.flags = 3;
	renderdata.zbufferenabled = true;

	// Drawn the way chrRender() draws a chr - mode 7, the wound tint for the
	// environment, and the fog colour the chr is blended towards by the fog's
	// alpha (the room's darkness; 255 is a white silhouette) - but with none
	// of that blend, so the icon is as bright in a dark room as in a lit one,
	// and without the team highlight. The menus' mode 1 sets neither colour,
	// and a soft span (the hair's half-clear fringe, xblamesh.c) goes through
	// the fog: it took whatever the world had left, the red a highlighted
	// teammate is drawn in
	renderdata.unk30 = 7;
	renderdata.envcolour = var80062a48[0] << 24 | var80062a48[1] << 16 | var80062a48[2] << 8;
	renderdata.fogcolour = 0xffffff00;
	renderdata.gdl = gdl;

	// The release's meshes keep a body's posed copy, and the matrix it is
	// drawn under, for the rest of the view's pass (xblamesh.c): a body the
	// world has already drawn in this view - the teammate a spectator follows -
	// came out under the world's camera, the back or the top of its head. A
	// key of its own for the portrait, and another after it so that nothing
	// later in the pass takes the portrait's.
	xblaMeshPassBegin();
	modelRender(&renderdata, model);
	xblaMeshPassBegin();

	gdl = renderdata.gdl;

	// the lists read the matrices in fixed point when the frame is drawn
	mtxF2LBulk(viewmtxs, n);

	model->matrices = savedmtxs;

	// the view's own viewport, projection and scissor again for what follows
	gdl = func0f0d49c8(gdl);

	gDPPipeSync(gdl++);
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
	gDPSetScissor(gdl++, G_SC_NON_INTERLACE, viGetViewLeft(), viGetViewTop(),
			viGetViewLeft() + viGetViewWidth(), viGetViewTop() + viGetViewHeight());
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetCombineMode(gdl++, G_CC_MODULATEI, G_CC_MODULATEI);
	gSPClearGeometryMode(gdl++, G_CULL_BOTH);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	texSelect(&gdl, NULL, 2, 0, 2, 1, NULL);
	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gSPDisplayList(gdl++, var800613a0);

	*gdlptr = gdl;

	return left;
}
#endif

// The other players' points, in Black Ops' co-op colours: white, blue,
// orange and green by player (your own are yellow)
static u32 zombiesPlayerColour(s32 playernum)
{
	static const u32 colours[] = { 0xffffffc0, 0x60a8ffc0, 0xffa030c0, 0x60ff60c0 };

	return colours[playernum & 3];
}

/**
 * One player's points, right-aligned at right with their bottom at rowbottom,
 * and their head just left of them, a little taller than the digits. Down,
 * the points blink red on a red backing; dead (waiting for the next round),
 * they are grey and there is no head. Returns the row's left edge.
 */
static s32 zombiesRenderPointsRow(Gfx **gdlptr, s32 playernum, s32 right, s32 rowbottom,
		struct fontchar *chars, struct font *font)
{
	Gfx *gdl = *gdlptr;
	struct player *player = g_Vars.players[playernum];
	char text[16];
	s32 textwidth;
	s32 textheight;
	s32 lineheight = zombiesLineHeight(chars, font);
	s32 left;
	u32 colour;
	bool dead = player->isdead;
	bool downed = g_ZombieDowned[playernum];

	sprintf(text, "%d", zombiesGetPoints(NULL, playernum));
	textMeasure(&textheight, &textwidth, text, chars, font, 0);
	left = right - textwidth;

	if (dead) {
		colour = 0x909090a0;
	} else if (downed) {
		colour = (g_Vars.lvframenum & 16) ? 0xff3030e0 : 0xffa0a0e0;
	} else if (playernum == g_Vars.currentplayernum) {
		colour = 0xffff00c0;
	} else {
		colour = zombiesPlayerColour(playernum);
	}

#ifndef PLATFORM_N64
	if (!dead) {
		s32 boxh = lineheight * ZOMBIE_PORTRAIT_OVER;

		// centred on the line, the digits' own height being most of it
		left = zombiesRenderPortrait(&gdl, playernum, right - textwidth - 2,
				rowbottom + (boxh - lineheight) / 2, boxh, downed ? 0xa00000b0 : 0x00000060);
	}

	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeR);
#endif

	gdl = text0f153628(gdl);
	gdl = zombiesDrawTextBR(gdl, text, right, rowbottom, colour, chars, font);
	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

	*gdlptr = gdl;

	return left;
}

/**
 * A glowing diamond in each power-up's colour over it, as Call of Duty's
 * shine from across a room: a gun's pickup is thin seen edge on, however big.
 * Only where the player can see the drop from, smaller the further it is, and
 * pulsing; blinking with it as it runs out.
 */
static Gfx *zombiesRenderDropMarkers(Gfx *gdl)
{
	f32 s = viGetViewHeight() / 240.0f;
	struct prop *me = g_Vars.currentplayer->prop;
	s32 i;

	if (me == NULL) {
		return gdl;
	}

	for (i = 0; i < ARRAYCOUNT(g_ZombieDrops); i++) {
		struct zombiedrop *drop = &g_ZombieDrops[i];
		struct prop *prop = drop->obj.prop;
		struct coord pos;
		struct coord cam;
		f32 screen[2];
		f32 dx;
		f32 dz;
		f32 dist;
		f32 half;
		f32 pulse;
		u32 colour;
		s32 rows;
		s32 r;

		if (!drop->active || prop == NULL || (drop->obj.flags2 & OBJFLAG2_INVISIBLE)) {
			continue;
		}

		pos = prop->pos;
		pos.y += drop->lift + 12;

		if (!cdTestLos04(&me->pos, me->rooms, &pos, CDTYPE_BG | CDTYPE_DOORS)) {
			continue;
		}

		mtx4TransformVec(camGetWorldToScreenMtxf(), &pos, &cam);

		if (cam.z > -10) {
			continue;
		}

		cam0f0b4d68(&cam, screen);

		if (screen[0] < viGetViewLeft() || screen[0] > viGetViewLeft() + viGetViewWidth()
				|| screen[1] < viGetViewTop() || screen[1] > viGetViewTop() + viGetViewHeight()) {
			continue;
		}

		dx = pos.x - me->pos.x;
		dz = pos.z - me->pos.z;
		dist = sqrtf(dx * dx + dz * dz);

		half = 7 * s * 400 / (dist > 1 ? dist : 1);

		if (half > 7 * s) {
			half = 7 * s;
		}

		if (half < 3 * s) {
			half = 3 * s;
		}

		pulse = 0.75f + 0.25f * sinf(drop->age60 * 0.12f);
		colour = (g_ZombiePowerups[drop->kind].colour[0] << 24)
			| (g_ZombiePowerups[drop->kind].colour[1] << 16)
			| (g_ZombiePowerups[drop->kind].colour[2] << 8)
			| (u32)(0xd0 * pulse);

		// a row at a time, as the crests are cut: a dark rim, then the colour
		gdl = text0f153628(gdl);
		rows = (s32)half;

		for (r = -rows - 1; r <= rows + 1; r++) {
			f32 w = half + 1 - (r < 0 ? -r : r);

			if (w > 0) {
				gdl = zombiesFillRect(gdl, screen[0] - w, screen[1] + r, screen[0] + w, screen[1] + r + 1, 0x00000080);
			}
		}

		for (r = -rows; r <= rows; r++) {
			f32 w = half - (r < 0 ? -r : r);

			if (w > 0) {
				gdl = zombiesFillRect(gdl, screen[0] - w, screen[1] + r, screen[0] + w, screen[1] + r + 1, colour);
			}
		}

		gdl = text0f153780(gdl);
	}

	return gdl;
}

// A red cross over each downed teammate in the world, as Call of Duty marks
// someone to revive
static Gfx *zombiesRenderReviveMarkers(Gfx *gdl)
{
	f32 s = viGetViewHeight() / 240.0f;
	s32 i;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		struct prop *prop = g_Vars.players[i]->prop;
		struct coord pos;
		struct coord cam;
		f32 screen[2];
		f32 arm = 5 * s;
		f32 thick = 1.6f * s;
		u32 colour = (g_Vars.lvframenum & 16) ? 0xff2020e0 : 0xff7070e0;

		if (i == g_Vars.currentplayernum || !g_ZombieDowned[i] || !prop) {
			continue;
		}

		pos.x = prop->pos.x;
		pos.y = prop->pos.y + 40;
		pos.z = prop->pos.z;

		mtx4TransformVec(camGetWorldToScreenMtxf(), &pos, &cam);

		// behind the camera
		if (cam.z > -10) {
			continue;
		}

		cam0f0b4d68(&cam, screen);

		if (screen[0] < viGetViewLeft() || screen[0] > viGetViewLeft() + viGetViewWidth()
				|| screen[1] < viGetViewTop() || screen[1] > viGetViewTop() + viGetViewHeight()) {
			continue;
		}

		gdl = text0f153628(gdl);
		gdl = zombiesFillRect(gdl, screen[0] - arm - 1, screen[1] - thick - 1, screen[0] + arm + 1, screen[1] + thick + 1, 0x000000a0);
		gdl = zombiesFillRect(gdl, screen[0] - thick - 1, screen[1] - arm - 1, screen[0] + thick + 1, screen[1] + arm + 1, 0x000000a0);
		gdl = zombiesFillRect(gdl, screen[0] - arm, screen[1] - thick, screen[0] + arm, screen[1] + thick, colour);
		gdl = zombiesFillRect(gdl, screen[0] - thick, screen[1] - arm, screen[0] + thick, screen[1] + arm, colour);
		gdl = text0f153780(gdl);
	}

	return gdl;
}

// A bar centred at centrex, frac of it filled
static Gfx *zombiesRenderBar(Gfx *gdl, s32 centrex, s32 y, f32 frac, u32 colour)
{
	f32 s = viGetViewHeight() / 240.0f;
	f32 halfwidth = 40 * s;
	f32 height = 3 * s;

	if (frac < 0) {
		frac = 0;
	} else if (frac > 1) {
		frac = 1;
	}

	gdl = zombiesFillRect(gdl, centrex - halfwidth - 1, y - 1, centrex + halfwidth + 1, y + height + 1, 0x000000a0);
	gdl = zombiesFillRect(gdl, centrex - halfwidth, y, centrex - halfwidth + halfwidth * 2 * frac, y + height, colour);

	return gdl;
}

/**
 * The middle of the screen: bleeding out or being revived, the prompt to
 * revive a teammate and the revive's progress, waiting dead for the next
 * round, and how far the game got once nobody is left.
 */
static Gfx *zombiesRenderDownedHud(Gfx *gdl, s32 centrex)
{
	s32 me = g_Vars.currentplayernum;
	s32 y = viGetViewTop() + viGetViewHeight() * 58 / 100;
	s32 bary = y + zombiesLineHeight(g_CharsHandelGothicXs, g_FontHandelGothicXs) + 2;
	bool anyoneup = false;
	char text[64];
	s32 i;

	if (me < 0 || me >= MAX_PLAYERS) {
		return gdl;
	}

	for (i = 0; i < PLAYERCOUNT(); i++) {
		if (zombiesPlayerIsUp(i)) {
			anyoneup = true;
		}
	}

#ifndef PLATFORM_N64
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
#endif

	gdl = text0f153628(gdl);

	(void)anyoneup;

	// The game over has its own screen (zombiesRenderGameOver()). Down alone
	// with Quick Revive nobody is up either, but the game is not over: that
	// read "YOU SURVIVED" until the player got up again.
	if (g_ZombieDowned[me] && g_ZombieSelfRevive60[me] > 0) {
		// as Call of Duty's: "Reviving" and the player's own name
		char name[20];
		s32 n;

		strncpy(name, g_PlayerConfigsArray[g_Vars.currentplayerstats->mpindex].base.name, sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';

		for (n = 0; name[n] != '\0'; n++) {
			if (name[n] == '\n') {
				name[n] = '\0';
				break;
			}
		}

		sprintf(text, "REVIVING %s", name);
		gdl = zombiesDrawWords(gdl, text, centrex, y, 0xffffffe0);
		gdl = zombiesRenderBar(gdl, centrex, bary, 1.0f - (f32)g_ZombieSelfRevive60[me] / ZOMBIE_QR_SELF60, 0x4090ffe0);
	} else if (g_Vars.currentplayer->isdead) {
		gdl = zombiesDrawWords(gdl, "YOU WILL RESPAWN NEXT ROUND", centrex, viGetViewTop() + 80, 0xffffffa0);

		if (g_ZombieSpectating[me] && g_ZombieWatch[me] >= 0) {
			sprintf(text, "SPECTATING PLAYER %d   FIRE: NEXT   %s: %s", g_ZombieWatch[me] + 1, zombiesUseName(me),
					g_ZombieSpecFirst[me] ? "THIRD PERSON" : "FIRST PERSON");
			gdl = zombiesDrawWords(gdl, text, centrex,
					viGetViewTop() + viGetViewHeight() - zombiesLineHeight(g_CharsHandelGothicXs, g_FontHandelGothicXs) - 6,
					0xffffffc0);
		}
	} else if (g_ZombieDowned[me]) {
		s32 reviver = g_ZombieReviver[me];

		if (reviver >= 0) {
			sprintf(text, "BEING REVIVED BY PLAYER %d", reviver + 1);
			gdl = zombiesDrawWords(gdl, text, centrex, y, 0xffffffe0);
			gdl = zombiesRenderBar(gdl, centrex, bary, (f32)g_ZombieRevive60[me] / ZOMBIE_REVIVE60, 0xffffffe0);
		} else {
			gdl = zombiesDrawWords(gdl, "BLEEDING OUT", centrex, y, 0xff4040e0);
			gdl = zombiesRenderBar(gdl, centrex, bary, (f32)g_ZombieBleed60[me] / ZOMBIE_BLEEDOUT60, 0xd01010e0);
		}
	} else if (g_ZombieNearDowned[me] >= 0) {
		s32 downed = g_ZombieNearDowned[me];

		if (g_ZombieReviver[downed] == me) {
			sprintf(text, "REVIVING PLAYER %d", downed + 1);
			gdl = zombiesDrawWords(gdl, text, centrex, y, 0xffffffe0);
			gdl = zombiesRenderBar(gdl, centrex, bary, (f32)g_ZombieRevive60[downed] / ZOMBIE_REVIVE60, 0xffffffe0);
		} else {
			sprintf(text, "Hold %s to revive Player %d", zombiesUseName(me), downed + 1);
			gdl = zombiesDrawWords(gdl, text, centrex, y, 0xffffffe0);
		}
	}

	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

	return gdl;
}

// A line centred on centrex with its top at y, in any font, scale times its size
static Gfx *zombiesDrawCentred(Gfx *gdl, char *text, s32 centrex, s32 y, u32 colour, struct fontchar *chars, struct font *font, s32 scale)
{
	s32 textwidth;
	s32 textheight;

	if (font == NULL) {
		return gdl;
	}

	textMeasure(&textheight, &textwidth, text, chars, font, 0);

	return textRenderScaled(gdl, centrex - textwidth * scale / 2, y, text, chars, font, colour, scale);
}

/**
 * GAME OVER over the darkened view, as Call of Duty holds it before its
 * scoreboard: the rounds survived, the player's points, and the arena's best
 * or that this was it (zombiesStartGameOver()). Fading in; everything else
 * the HUD draws is left off.
 */
static Gfx *zombiesRenderGameOver(Gfx *gdl)
{
	s32 me = g_Vars.currentplayernum;
	s32 top = viGetViewTop();
	s32 height = viGetViewHeight();
	s32 centrex = viGetViewLeft() + viGetViewWidth() / 2;
	f32 f = (f32)g_ZombieGameOver60 / ZOMBIE_GAMEOVER_FADE60;
	u32 alpha;
	struct fontchar *bigchars = g_CharsHandelGothicLg;
	struct font *bigfont = g_FontHandelGothicLg;
	s32 y;
	char text[64];

	if (f > 1) {
		f = 1;
	}

	alpha = (u32)(0xff * f);

	if (bigfont == NULL) {
		bigchars = g_CharsHandelGothicMd;
		bigfont = g_FontHandelGothicMd;
	}

	gdl = text0f153628(gdl);
	gdl = zombiesFillRect(gdl, viGetViewLeft(), top, viGetViewLeft() + viGetViewWidth(), top + height, (u32)(0xb0 * f));
	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
#endif

	gdl = text0f153628(gdl);

	y = top + height * 30 / 100;
	gdl = zombiesDrawCentred(gdl, "GAME OVER", centrex, y, 0xc8141400 | alpha, bigchars, bigfont, 2);
	y += zombiesLineHeight(bigchars, bigfont) * 2 + 8;

	sprintf(text, "You Survived %d Round%s", g_ZombieRound, g_ZombieRound == 1 ? "" : "s");
	gdl = zombiesDrawCentred(gdl, text, centrex, y, 0xffffff00 | alpha, g_CharsHandelGothicMd, g_FontHandelGothicMd, 1);
	y += zombiesLineHeight(g_CharsHandelGothicMd, g_FontHandelGothicMd) + 6;

	if (me >= 0 && me < MAX_PLAYERS) {
		sprintf(text, "Points: %d", g_ZombiePoints[me]);
		gdl = zombiesDrawCentred(gdl, text, centrex, y, 0xf0d02000 | alpha, g_CharsHandelGothicSm, g_FontHandelGothicSm, 1);
		y += zombiesLineHeight(g_CharsHandelGothicSm, g_FontHandelGothicSm) + 4;
	}

	if (g_ZombieNewBest) {
		strcpy(text, g_ZombieBestRound > 0 ? "NEW BEST ON THIS ARENA!" : "FIRST RECORD ON THIS ARENA");
		gdl = zombiesDrawCentred(gdl, text, centrex, y, 0x60ff6000 | alpha, g_CharsHandelGothicSm, g_FontHandelGothicSm, 1);
	} else {
		sprintf(text, "Best on this arena: Round %d", g_ZombieBestRound);
		gdl = zombiesDrawCentred(gdl, text, centrex, y, 0xc0c0c000 | alpha, g_CharsHandelGothicSm, g_FontHandelGothicSm, 1);
	}

	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

	return gdl;
}

// Round number (red, bottom-left) and points (yellow, bottom-right, above the ammo).
Gfx *zombiesRenderHud(Gfx *gdl)
{
	s32 left;
	s32 right;
	s32 bottom;
	s32 pointsbottom;
	s32 portraitleft;
	s32 centrex;
	char text[64];

	if (g_ZombieGameOver) {
		return zombiesRenderGameOver(gdl);
	}

	gdl = zombiesRenderChalk(gdl);
	gdl = zombiesRenderJuggernog(gdl);

	if (g_ZombieRound < 1) {
		return gdl;
	}

	// the round number sits tight in the corner, as Black Ops has it; a line's
	// height has a little space under the digits already
	left = viGetViewLeft() + 6;
	right = viGetViewLeft() + viGetViewWidth() - ZOMBIE_HUD_RIGHT;
	bottom = viGetViewTop() + viGetViewHeight() - 2;
	centrex = viGetViewLeft() + viGetViewWidth() / 2;

	// round number: big, bottom-left, anchored to the left edge
	if (zombiesRoundAlpha() > 0) {
		u32 colour = zombiesRoundColour();

#ifndef PLATFORM_N64
		gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeL);
#endif

		gdl = text0f153628(gdl);

		sprintf(text, "%d", g_ZombieRound);

#ifndef PLATFORM_N64
		{
			s32 textheight;
			struct fontchar *chars = g_CharsHandelGothicLg;
			struct font *font = g_FontHandelGothicLg;
			s32 scale = ZOMBIE_ROUND_SCALE;

			// textReset() loads the large font for Zombies; should it ever be
			// missing, the medium one drawn bigger is next best
			if (font == NULL) {
				chars = g_CharsHandelGothicMd;
				font = g_FontHandelGothicMd;
				scale = ZOMBIE_ROUND_SCALE + 1;
			}

			if (font) {
				textheight = zombiesLineHeight(chars, font);
				gdl = textRenderScaled(gdl, left, bottom - textheight * scale, text,
						chars, font, colour, scale);
			}
		}
#else
		gdl = zombiesDrawTextBL(gdl, text, left, bottom, colour, g_CharsHandelGothicLg, g_FontHandelGothicLg);
#endif

		gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
		gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif
	}

	// zombies left this round, out or still to come: top-left
	{
		s32 remaining = g_ZombieRoundTotal - g_ZombieSpawned;

		if (remaining < 0) {
			remaining = 0;
		}

		remaining += zombiesCountAlive();

		// a client's puppets are not the host's count of them
		if (NET_CLIENT) {
			remaining = g_ZombieNetRemaining;
		}

#ifndef PLATFORM_N64
		gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeL);
#endif

		gdl = text0f153628(gdl);

		sprintf(text, "Zombies: %d", remaining);
		gdl = zombiesDrawTextBL(gdl, text, viGetViewLeft() + 12,
				viGetViewTop() + 10 + zombiesLineHeight(g_CharsHandelGothicSm, g_FontHandelGothicSm),
				0xffffffc0, g_CharsHandelGothicSm, g_FontHandelGothicSm);

		gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
		gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif
	}

	// points: right side, anchored to the right edge, on top of the gun's
	// name, ammo and function (zombiesHudNameBottom())
	{
		s32 stacktop = -1;

		if (g_Vars.currentplayernum >= 0 && g_Vars.currentplayernum < MAX_PLAYERS) {
			stacktop = g_ZombieStackTop[g_Vars.currentplayernum];
		}

		if (stacktop < 0) {
			// no gun drawn yet: from bondgun.c's bottom for one player
			stacktop = zombiesHudNameBottom(viGetViewTop() + viGetViewHeight() - 13)
				- zombiesLineHeight(g_CharsHandelGothicXs, g_FontHandelGothicXs);
		}

		pointsbottom = stacktop - 3;
	}

	// your points and head, and above them everyone else's, smaller, as
	// Black Ops stacks a co-op game's
	portraitleft = zombiesRenderPointsRow(&gdl, g_Vars.currentplayernum, right, pointsbottom,
			g_CharsHandelGothicSm, g_FontHandelGothicSm);

	{
		s32 rowbottom = pointsbottom - zombiesLineHeight(g_CharsHandelGothicSm, g_FontHandelGothicSm) - 2;
		s32 rowheight = zombiesLineHeight(g_CharsHandelGothicXs, g_FontHandelGothicXs) + 3;
		s32 i;

		for (i = 0; i < PLAYERCOUNT(); i++) {
			if (i != g_Vars.currentplayernum && !g_ZombieOutOfPlay[i]) {
				zombiesRenderPointsRow(&gdl, i, right, rowbottom,
						g_CharsHandelGothicXs, g_FontHandelGothicXs);
				rowbottom -= rowheight;
			}
		}
	}

#ifndef PLATFORM_N64
	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeR);
#endif

	gdl = text0f153628(gdl);

	// each "+10" starts just left of the points (and the head) and drifts
	// further left as it fades
	{
		s32 i;

		for (i = 0; i < ZOMBIE_MAX_POPUPS; i++) {
			struct zombiepopup *popup = &g_ZombiePopups[i];

			if (popup->age60 >= 0 && popup->playernum == g_Vars.currentplayernum) {
				char popuptext[16];
				s32 alpha = 0xa0 - 0xa0 * popup->age60 / ZOMBIE_POPUP_LIFE60;
				s32 popupright = portraitleft - 8 - popup->age60 * 2 / 3;

				sprintf(popuptext, "%+d", popup->value);
				gdl = zombiesDrawTextBR(gdl, popuptext, popupright, pointsbottom + popup->yoffset,
						0xffff0000 | alpha, g_CharsHandelGothicXs, g_FontHandelGothicXs);
			}
		}
	}

	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

	gdl = zombiesRenderDropMarkers(gdl);

	// where am I (zombiesTickWhere())
	if (g_Vars.currentplayernum >= 0 && g_Vars.currentplayernum < MAX_PLAYERS && g_ZombieWhere[g_Vars.currentplayernum]
			&& g_Vars.currentplayer->prop) {
		struct coord *pos = &g_Vars.currentplayer->prop->pos;

		sprintf(text, "X %d   Y %d   Z %d", (s32)pos->x, (s32)pos->y, (s32)pos->z);
#ifndef PLATFORM_N64
		gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
#endif
		gdl = text0f153628(gdl);
		gdl = zombiesDrawWords(gdl, text, centrex, viGetViewTop() + 34, 0x80ff80ff);
		gdl = text0f153780(gdl);
#ifndef PLATFORM_N64
		gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif
	}
	gdl = zombiesRenderReviveMarkers(gdl);
	gdl = zombiesRenderDownedHud(gdl, centrex);
	gdl = zombiesRenderMaxAmmoBanner(gdl);
	gdl = zombiesRenderPowerupTimers(gdl);
	gdl = zombiesRenderNukeFlash(gdl);

	return gdl;
}

/**
 * Zombies online (protocol 33, CLAUDE-notes/netplay.md "Zombies online").
 *
 * The host plays Zombies as it does offline, a client's player's buys, revives
 * and knife from its commands (zombiesHoldingUse() reads its virtual pad;
 * zombiesUseMask() its NETCMD_INTERACT). A client runs none of zombiesTick()
 * (netplay's C13). Instead:
 *
 * - each machine puts up the perk machines, Pack-a-Punch, the box and the wall
 *   buys itself, the same way: where they go is the arena's alone (its spawn
 *   pads and its walls, no random numbers), so the host sends none of them,
 *   nor the guns over them nor the power-ups, as entities (zombiesNetOwnsProp(),
 *   netents.c), and zombiesRenderMachine(), the chalk and the radar find the
 *   client's own. The block carries where the host's stand, and a client says
 *   in its log if its own are elsewhere;
 * - the host's state rides in the scenario block (zombiesNetCapture(), by
 *   playernum, which is the host's on every machine), and the client's frame
 *   (zombiesClientTick()) shows it: the gun over the box, the gun in
 *   Pack-a-Punch, the guns put up on the walls, the power-ups;
 * - the points' popups and the sounds the host's Zombies made ride in two
 *   rings of the last ZOMBIE_NET_RING, numbered, so a lost snapshot loses none;
 *   a sound of one player's (its buy, its drink) is heard on that player's
 *   machine only;
 * - a client's knife swings on its own machine as it predicts its walk
 *   (zombiesQuickMelee() from bondmove.c, zombiesTickKnife() here); its blow
 *   is the host's.
 */
#define ZOMBIE_NET_VERSION 1

struct zombienetbuf {
	u8 *w;
	const u8 *r;
	s32 at;
	s32 size;
};

static s32 g_ZombieNetPopupSeen = -1;  // client: the next popup and sound to take, -1 before the first block
static s32 g_ZombieNetSoundSeen = -1;
static s32 g_ZombieNetBoxShown = -1;   // client: the gun over the box, as the host has it
static u32 g_ZombieNetWallMask = 0;    // ... the wall buys with their gun up
static u8 g_ZombieDropSerialHere[4];   // ... the host's drop each slot has here
static bool g_ZombieNetChecked = false;
static s32 g_ZombieNetApplied = 0;
static s32 g_ZombieNetPopupsTaken = 0;
static s32 g_ZombieNetSoundsTaken = 0;
static s32 g_ZombieNetDropsMade = 0;

static s32 znClamp(s32 v, s32 lo, s32 hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void znPut(struct zombienetbuf *b, u32 v, s32 n)
{
	s32 i;

	for (i = 0; i < n; i++) {
		if (b->at < b->size) {
			b->w[b->at] = (u8)(v >> (i * 8));
		}

		b->at++;
	}
}

static u32 znGet(struct zombienetbuf *b, s32 n)
{
	u32 v = 0;
	s32 i;

	for (i = 0; i < n; i++) {
		if (b->at < b->size) {
			v |= (u32)b->r[b->at] << (i * 8);
		}

		b->at++;
	}

	return v;
}

static void znPutF(struct zombienetbuf *b, f32 f)
{
	u32 v;

	memcpy(&v, &f, 4);
	znPut(b, v, 4);
}

static f32 znGetF(struct zombienetbuf *b)
{
	u32 v = znGet(b, 4);
	f32 f;

	memcpy(&f, &v, 4);

	return f == f && f < 1e7f && f > -1e7f ? f : 0;
}

// Where the players start in the block (zombiesNetCapture()'s order): a
// resumed match reads them alone (zombiesNetResume())
#define ZOMBIE_NET_WALLS_AT   28  // the wall buys' mask, past the round's, the power-ups' and the box's
#define ZOMBIE_NET_PLAYERS_AT 285

// A player's part of the block, as zombiesNetCapture() writes it
struct zombienetplayer {
	s32 points;
	u32 perks;   // bit k: perk k (Juggernog's bit 0)
	s32 count;
	s32 order[ZOMBIEPERK_COUNT];
	s32 drink;
	s32 drinkperk;
	u32 flags;   // 1 down, 2 respawn due, 4/8/16 near Juggernog/the box/Pack-a-Punch, 32 where, 64 out of play
	s32 nearperk;
	s32 nearwall;
	s32 neardowned;
	s32 reviver;
	s32 bleed;
	s32 revive;
	s32 selfrevive;
	s32 pap60;
	s32 papgun;
	s32 papcount;
	s32 papped[ZOMBIE_PAP_MAX];
};

static void zombiesNetReadPlayer(struct zombienetbuf *b, struct zombienetplayer *pl)
{
	s32 k;

	pl->points = (s32)znGet(b, 4);
	pl->perks = znGet(b, 1);
	pl->count = znClamp(znGet(b, 1), 0, ZOMBIEPERK_COUNT);

	for (k = 0; k < ZOMBIEPERK_COUNT; k++) {
		pl->order[k] = znClamp(znGet(b, 1), 0, ZOMBIEPERK_COUNT - 1);
	}

	pl->drink = (s16)znGet(b, 2);
	pl->drinkperk = znClamp(znGet(b, 1), 0, ZOMBIEPERK_COUNT - 1);
	pl->flags = znGet(b, 1);
	pl->nearperk = (s8)znGet(b, 1);
	pl->nearwall = (s8)znGet(b, 1);
	pl->neardowned = (s8)znGet(b, 1);
	pl->reviver = (s8)znGet(b, 1);
	pl->bleed = znGet(b, 2);
	pl->revive = znGet(b, 2);
	pl->selfrevive = znGet(b, 2);
	pl->pap60 = (s16)znGet(b, 2);
	pl->papgun = znGet(b, 1);
	pl->papcount = znClamp(znGet(b, 1), 0, ZOMBIE_PAP_MAX);

	for (k = 0; k < ZOMBIE_PAP_MAX; k++) {
		pl->papped[k] = znGet(b, 1);
	}
}

/**
 * Host migration (netmigrate.c), a Zombies match carried on under a new host:
 * the stage starts again (its frames from 0, so Zombies' own start runs), and
 * what the players had - the round, each one's points, perks and upgraded
 * guns, the wall buys put up - is the old host's last block, given back as the
 * new round starts (zombiesTick()). Its guns are the players' own records
 * (RESUME). Seats 0 and swap are exchanged, as the kill table's rows are.
 */
static bool g_ZombieResumePending = false;
static s32 g_ZombieResumeRound = 0;
static u32 g_ZombieResumeWalls = 0;
static struct zombienetplayer g_ZombieResumePlayers[MAX_PLAYERS]; // by this match's playernum

void zombiesNetResume(const u8 *in, s32 size, s32 swap)
{
	struct zombienetbuf b = { NULL, in, 0, size };
	struct zombienetplayer old[MAX_PLAYERS];
	s32 i;

	if (znGet(&b, 1) != ZOMBIE_NET_VERSION) {
		return;
	}

	g_ZombieResumeRound = znGet(&b, 2);
	b.at = ZOMBIE_NET_WALLS_AT;
	g_ZombieResumeWalls = znGet(&b, 1);
	b.at = ZOMBIE_NET_PLAYERS_AT;

	for (i = 0; i < MAX_PLAYERS; i++) {
		zombiesNetReadPlayer(&b, &old[i]);
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		// the seat this player had before: 0 and swap exchanged
		s32 slot = i < PLAYERCOUNT() ? g_Vars.playerstats[i].mpindex : i;

		slot = slot == 0 ? swap : slot == swap ? 0 : slot;
		g_ZombieResumePlayers[i] = slot >= 0 && slot < MAX_PLAYERS ? old[slot] : old[i];
	}

	g_ZombieResumePending = g_ZombieResumeRound > 0;

	sysLogPrintf(LOG_NOTE, "zombies: migrate: the match carries on from round %d when its round starts", g_ZombieResumeRound);
}

// zombiesTick(), the first round's start: a resumed match's round and what the
// players had under the old host, when there is one
static void zombiesNetResumeTake(void)
{
	s32 i;
	s32 k;

	if (!g_ZombieResumePending) {
		return;
	}

	g_ZombieResumePending = false;
	g_ZombieRound = g_ZombieResumeRound;

	for (k = 0; k < g_ZombieNumWallBuys; k++) {
		if (g_ZombieResumeWalls & (1 << k)) {
			zombiesWallBuyShowGun(&g_ZombieWallBuys[k]);
		}
	}

	for (i = 0; i < PLAYERCOUNT(); i++) {
		const struct zombienetplayer *pl = &g_ZombieResumePlayers[i];

		if (g_ZombieOutOfPlay[i] || g_Vars.players[i] == NULL) {
			continue;
		}

		g_ZombiePoints[i] = pl->points;

		for (k = 0; k < pl->count; k++) {
			const s32 perk = pl->order[k];

			if (pl->perks & (1 << perk)) {
				zombiesGivePerkQuietly(i, perk);
			}
		}

		g_ZombiePapCount[i] = pl->papcount;

		for (k = 0; k < pl->papcount; k++) {
			g_ZombiePapped[i][k] = pl->papped[k];
		}

		sysLogPrintf(LOG_NOTE, "zombies: migrate: player %d back with %d points, perks 0x%02x, %d upgraded gun%s",
				i + 1, pl->points, pl->perks, pl->papcount, pl->papcount == 1 ? "" : "s");
	}
}

/**
 * netents.c, the host's capture: one of Zombies' own objects, which each
 * machine makes for itself (an s32: bool is not the same size on both sides)
 */
s32 zombiesNetOwnsProp(struct prop *prop)
{
	struct defaultobj *obj;
	s32 i;

	if (!zombiesIsActive() || prop == NULL || (obj = prop->obj) == NULL) {
		return 0;
	}

	if (obj == &g_ZombieBoxObj || obj == &g_ZombiePapObj || obj == &g_ZombiePapGunObj) {
		return 1;
	}

	for (i = 0; i < ARRAYCOUNT(g_ZombieJuggObjs); i++) {
		if (obj == &g_ZombieJuggObjs[i]) {
			return 1;
		}
	}

	for (i = 0; i < ARRAYCOUNT(g_ZombiePerkObjs); i++) {
		if (obj == &g_ZombiePerkObjs[i]) {
			return 1;
		}
	}

	for (i = 0; i < ARRAYCOUNT(g_ZombieBoxGunObjs); i++) {
		if (obj == &g_ZombieBoxGunObjs[i]) {
			return 1;
		}
	}

	for (i = 0; i < ARRAYCOUNT(g_ZombieWallBuys); i++) {
		if (obj == &g_ZombieWallBuys[i].obj) {
			return 1;
		}
	}

	for (i = 0; i < ARRAYCOUNT(g_ZombieDrops); i++) {
		if (obj == &g_ZombieDrops[i].obj) {
			return 1;
		}
	}

	return 0;
}

/**
 * netscen.c, the host's block at a tick's end: Zombies' body, the layout
 * zombiesNetApply() reads in the same order
 */
void zombiesNetCapture(u8 *out, s32 size)
{
	struct zombienetbuf b = { out, NULL, 0, size };
	u32 mask = 0;
	s32 remaining;
	s32 i;
	s32 k;

	znPut(&b, ZOMBIE_NET_VERSION, 1);
	znPut(&b, znClamp(g_ZombieRound, 0, 0xffff), 2);
	znPut(&b, znClamp(g_Vars.lvframenum - g_ZombieRoundStartFrame, 0, 0xffff), 2);
	znPut(&b, (u16)(s16)(g_ZombieNextWaveFrame < 0 ? -1 : znClamp(g_ZombieNextWaveFrame - g_Vars.lvframenum, 0, 0x7fff)), 2);

	remaining = g_ZombieRoundTotal - g_ZombieSpawned;
	remaining = (remaining < 0 ? 0 : remaining) + zombiesCountAlive();
	znPut(&b, znClamp(remaining, 0, 0xffff), 2);

	znPut(&b, g_ZombieGameOver ? 1 : 0, 1);
	znPut(&b, znClamp(g_ZombieGameOver60, 0, 0xffff), 2);
	znPut(&b, znClamp(g_ZombieInstaKill60, 0, 0xffff), 2);
	znPut(&b, znClamp(g_ZombieDoublePoints60, 0, 0xffff), 2);
	znPut(&b, (u16)(s16)znClamp(g_ZombieMaxAmmoBanner60, -1, 0x7fff), 2);
	znPut(&b, znClamp(g_ZombieBannerKind, 0, ZOMBIEPOWERUP_COUNT - 1), 1);
	znPut(&b, (u16)(s16)znClamp(g_ZombieNukeFlash60, -1, 0x7fff), 2);
	znPut(&b, znClamp(g_ZombieQrSoloBuys, 0, 0xff), 1);

	znPut(&b, znClamp(g_ZombieBoxState, 0, ZOMBIEBOX_OFFER), 1);
	znPut(&b, znClamp(g_ZombieBoxTime60, 0, 0xffff), 2);
	znPut(&b, (u8)(s8)znClamp(g_ZombieBoxUser, -1, MAX_PLAYERS - 1), 1);
	znPut(&b, (u16)(s16)znClamp(g_ZombieBoxShown, -1, 0x7fff), 2);

	for (k = 0; k < ARRAYCOUNT(g_ZombieWallBuys); k++) {
		if (g_ZombieWallBuys[k].shown) {
			mask |= 1 << k;
		}
	}

	if (b.at != ZOMBIE_NET_WALLS_AT) {
		sysLogPrintf(LOG_WARNING, "zombies: the online block's wall buys are at %d, not ZOMBIE_NET_WALLS_AT", b.at);
	}

	znPut(&b, mask, 1);

	// the rings
	znPut(&b, g_ZombieNetPopupSeq, 2);

	for (k = 0; k < ZOMBIE_NET_RING; k++) {
		znPut(&b, (u8)g_ZombieNetPopups[k].playernum, 1);
		znPut(&b, (u16)g_ZombieNetPopups[k].value, 2);
	}

	znPut(&b, g_ZombieNetSoundSeq, 2);

	for (k = 0; k < ZOMBIE_NET_RING; k++) {
		znPut(&b, g_ZombieNetSounds[k].sound, 2);
		znPut(&b, g_ZombieNetSounds[k].pitch, 1);
		znPut(&b, (u8)g_ZombieNetSounds[k].who, 1);
		znPut(&b, g_ZombieNetSounds[k].chr, 1);
		znPut(&b, g_ZombieNetSounds[k].mine, 1);
	}

	// the power-ups
	for (k = 0; k < ARRAYCOUNT(g_ZombieDrops); k++) {
		struct zombiedrop *drop = &g_ZombieDrops[k];
		const bool on = drop->active && drop->obj.prop != NULL;

		znPut(&b, on ? 1 : 0, 1);
		znPut(&b, znClamp(drop->kind, 0, ZOMBIEPOWERUP_COUNT - 1), 1);
		znPut(&b, g_ZombieDropSerial[k], 1);
		znPut(&b, znClamp(drop->age60, 0, 0xffff), 2);
		znPutF(&b, on ? drop->obj.prop->pos.x : 0);
		znPutF(&b, on ? drop->obj.prop->pos.z : 0);
		znPutF(&b, on ? drop->floory : 0);
		znPut(&b, (u16)(s16)(on ? drop->rooms[0] : -1), 2);
	}

	// where the machines and the box stand here
	for (k = 0; k < ZOMBIE_MACHINE_IDS; k++) {
		struct defaultobj *obj = zombiesMachineObjAt(k);
		const bool up = obj && obj->prop;

		znPut(&b, (u16)(s16)(up ? znClamp((s32)obj->prop->pos.x, -32767, 32767) : -32768), 2);
		znPut(&b, (u16)(s16)(up ? znClamp((s32)obj->prop->pos.z, -32767, 32767) : -32768), 2);
	}

	// each player, by playernum, where zombiesNetResume() looks for them
	if (b.at != ZOMBIE_NET_PLAYERS_AT) {
		static bool warned = false;

		if (!warned) {
			warned = true;
			sysLogPrintf(LOG_WARNING, "zombies: the online block's players start at %d, not ZOMBIE_NET_PLAYERS_AT %d", b.at, ZOMBIE_NET_PLAYERS_AT);
		}
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		const bool in = i < PLAYERCOUNT() && g_Vars.players[i] != NULL;
		u32 perks = 0;
		u32 flags = 0;
		s32 count = 0;

		if (in) {
			for (k = 0; k < ZOMBIEPERK_COUNT; k++) {
				if (zombiesPlayerHasPerk(i, k)) {
					perks |= 1 << k;
				}
			}

			count = znClamp(g_ZombiePerkOrderCount[i], 0, ZOMBIEPERK_COUNT);
			flags |= g_ZombieDowned[i] ? 0x01 : 0;
			flags |= g_ZombieRespawnDue[i] ? 0x02 : 0;
			flags |= g_ZombieNearJugg[i] ? 0x04 : 0;
			flags |= g_ZombieNearBox[i] ? 0x08 : 0;
			flags |= g_ZombieNearPap[i] ? 0x10 : 0;
			flags |= g_ZombieWhere[i] ? 0x20 : 0;
			flags |= g_ZombieOutOfPlay[i] ? 0x40 : 0;
		}

		znPut(&b, in ? (u32)g_ZombiePoints[i] : 0, 4);
		znPut(&b, perks, 1);
		znPut(&b, count, 1);

		for (k = 0; k < ZOMBIEPERK_COUNT; k++) {
			znPut(&b, k < count ? znClamp(g_ZombiePerkOrder[i][k], 0, ZOMBIEPERK_COUNT - 1) : 0, 1);
		}

		znPut(&b, (u16)(s16)(in ? znClamp(g_ZombieDrink60[i], -1, 0x7fff) : -1), 2);
		znPut(&b, in ? znClamp(g_ZombieDrinkPerk[i], 0, ZOMBIEPERK_COUNT - 1) : 0, 1);
		znPut(&b, flags, 1);
		znPut(&b, (u8)(s8)(in ? znClamp(g_ZombieNearPerk[i], -1, 0x7f) : -1), 1);
		znPut(&b, (u8)(s8)(in ? znClamp(g_ZombieNearWall[i], -1, 0x7f) : -1), 1);
		znPut(&b, (u8)(s8)(in ? znClamp(g_ZombieNearDowned[i], -1, 0x7f) : -1), 1);
		znPut(&b, (u8)(s8)(in ? znClamp(g_ZombieReviver[i], -1, 0x7f) : -1), 1);
		znPut(&b, in ? znClamp(g_ZombieBleed60[i], 0, 0xffff) : 0, 2);
		znPut(&b, in ? znClamp(g_ZombieRevive60[i], 0, 0xffff) : 0, 2);
		znPut(&b, in ? znClamp(g_ZombieSelfRevive60[i], 0, 0xffff) : 0, 2);
		znPut(&b, (u16)(s16)(in ? znClamp(g_ZombiePap60[i], -1, 0x7fff) : -1), 2);
		znPut(&b, in ? znClamp(g_ZombiePapGun[i], 0, 0xff) : 0, 1);
		count = in ? znClamp(g_ZombiePapCount[i], 0, ZOMBIE_PAP_MAX) : 0;
		znPut(&b, count, 1);

		for (k = 0; k < ZOMBIE_PAP_MAX; k++) {
			znPut(&b, k < count ? znClamp(g_ZombiePapped[i][k], 0, 0xff) : 0, 1);
		}
	}

	if (b.at > size) {
		static bool warned = false;

		if (!warned) {
			warned = true;
			sysLogPrintf(LOG_WARNING, "zombies: the online block wants %d bytes, the scenario block has %d", b.at, size);
		}
	}
}

// A sound the host's Zombies made, heard here as it would have been there
static void zombiesNetPlay(const struct zombienetsound *s)
{
	const f32 pitch = s->pitch / 64.0f;

	if (s->sound == 0 || (s->who >= 0 && (s->who != g_NetLocalSlot || s->mine))) {
		return;
	}

	g_ZombieNetSoundsTaken++;

	if (s->chr != 0xff) {
		const s32 index = func0f18d0e8(s->chr);

		if (index >= 0 && index < g_MpNumChrs && g_MpAllChrPtrs[index] && g_MpAllChrPtrs[index]->prop) {
			psCreate(NULL, g_MpAllChrPtrs[index]->prop, s->sound, -1, -1, 0, 0, PSTYPE_NONE, NULL, pitch, NULL, -1, -1, -1, -1);
		}

		return;
	}

	zombiesPlaySoundHere(s->sound, pitch);
}

// The machines here against where the host's stand, once they are up
static void zombiesNetCheckMachines(const s16 hostpos[][2])
{
	s32 off = 0;
	s32 k;

	for (k = 0; k < ZOMBIE_MACHINE_IDS; k++) {
		struct defaultobj *obj = zombiesMachineObjAt(k);
		const bool here = obj && obj->prop;
		const bool there = hostpos[k][0] != -32768;

		if (here != there || (here && (fabsf(obj->prop->pos.x - hostpos[k][0]) > 2 || fabsf(obj->prop->pos.z - hostpos[k][1]) > 2))) {
			off++;
			sysLogPrintf(LOG_WARNING, "zombies: online, %s is at %.0f %.0f here and at %d %d on the host",
					k < ZOMBIE_MACHINE_IDS - 1 ? zombiesMachineName(k) : "the Mystery Box",
					here ? obj->prop->pos.x : 0.0f, here ? obj->prop->pos.z : 0.0f, hostpos[k][0], hostpos[k][1]);
		}
	}

	if (off == 0) {
		sysLogPrintf(LOG_NOTE, "zombies: online, the machines and the box here stand where the host's do");
	}
}

/**
 * netscen.c, a client, the host's block of a tick (and MATCH_END's last, final)
 */
void zombiesNetApply(const u8 *in, s32 size, s32 final)
{
	struct zombienetbuf b = { NULL, in, 0, size };
	struct zombienetpopup popups[ZOMBIE_NET_RING];
	struct zombienetsound sounds[ZOMBIE_NET_RING];
	s16 machines[ZOMBIE_MACHINE_IDS][2];
	const bool wasover = g_ZombieGameOver;
	const s32 wasnext = g_ZombieNextWaveFrame;
	u16 seq;
	s32 v;
	s32 i;
	s32 k;

	if (!NET_CLIENT || znGet(&b, 1) != ZOMBIE_NET_VERSION) {
		return;
	}

	g_ZombieNetApplied++;
	g_ZombieRound = znGet(&b, 2);
	g_ZombieRoundStartFrame = g_Vars.lvframenum - (s32)znGet(&b, 2);
	v = (s16)znGet(&b, 2);
	g_ZombieNextWaveFrame = v < 0 ? -1 : g_Vars.lvframenum + v;
	g_ZombieNetRemaining = znGet(&b, 2);
	g_ZombieGameOver = znGet(&b, 1) != 0;
	g_ZombieGameOver60 = znGet(&b, 2);
	g_ZombieInstaKill60 = znGet(&b, 2);
	g_ZombieDoublePoints60 = znGet(&b, 2);
	g_ZombieMaxAmmoBanner60 = (s16)znGet(&b, 2);
	g_ZombieBannerKind = znClamp(znGet(&b, 1), 0, ZOMBIEPOWERUP_COUNT - 1);
	g_ZombieNukeFlash60 = (s16)znGet(&b, 2);
	g_ZombieQrSoloBuys = znGet(&b, 1);

	g_ZombieBoxState = znClamp(znGet(&b, 1), 0, ZOMBIEBOX_OFFER);
	g_ZombieBoxTime60 = znGet(&b, 2);
	g_ZombieBoxUser = znClamp((s8)znGet(&b, 1), -1, PLAYERCOUNT() - 1);
	v = (s16)znGet(&b, 2);
	g_ZombieNetBoxShown = v >= 0 && v < NUM_MPWEAPONS ? v : -1;
	g_ZombieNetWallMask = znGet(&b, 1);

	// the rings: the ones not had yet, the first block's taken as had
	seq = znGet(&b, 2);

	for (k = 0; k < ZOMBIE_NET_RING; k++) {
		popups[k].playernum = (s8)znGet(&b, 1);
		popups[k].value = (s16)znGet(&b, 2);
	}

	if (g_ZombieNetPopupSeen >= 0) {
		u16 s;

		for (s = (u16)g_ZombieNetPopupSeen; s != seq && (u16)(seq - s) <= ZOMBIE_NET_RING; s++) {
			const struct zombienetpopup *p = &popups[s % ZOMBIE_NET_RING];

			if (p->playernum >= 0 && p->playernum < PLAYERCOUNT()) {
				zombiesAddPopup(p->playernum, p->value);
				g_ZombieNetPopupsTaken++;
			}
		}
	}

	g_ZombieNetPopupSeen = seq;
	seq = znGet(&b, 2);

	for (k = 0; k < ZOMBIE_NET_RING; k++) {
		sounds[k].sound = znGet(&b, 2);
		sounds[k].pitch = znGet(&b, 1);
		sounds[k].who = (s8)znGet(&b, 1);
		sounds[k].chr = znGet(&b, 1);
		sounds[k].mine = znGet(&b, 1);
	}

	if (g_ZombieNetSoundSeen >= 0 && !final) {
		u16 s;

		for (s = (u16)g_ZombieNetSoundSeen; s != seq && (u16)(seq - s) <= ZOMBIE_NET_RING; s++) {
			zombiesNetPlay(&sounds[s % ZOMBIE_NET_RING]);
		}
	}

	g_ZombieNetSoundSeen = seq;

	// the power-ups, each made here at the host's place
	for (k = 0; k < ARRAYCOUNT(g_ZombieDrops); k++) {
		struct zombiedrop *drop = &g_ZombieDrops[k];
		const bool on = znGet(&b, 1) != 0;
		const s32 kind = znClamp(znGet(&b, 1), 0, ZOMBIEPOWERUP_COUNT - 1);
		const u8 serial = znGet(&b, 1);
		const s32 age = znGet(&b, 2);
		struct coord pos;
		f32 floory;
		RoomNum rooms[8];

		pos.x = znGetF(&b);
		pos.z = znGetF(&b);
		floory = znGetF(&b);
		rooms[0] = (s16)znGet(&b, 2);
		rooms[1] = -1;
		pos.y = floory;

		if (!on) {
			if (drop->active) {
				zombiesDropRemove(drop);
			}

			continue;
		}

		if (drop->active && g_ZombieDropSerialHere[k] != serial) {
			zombiesDropRemove(drop);
		}

		// a slot whose last one is still going waits for it
		if (!drop->active && drop->obj.prop == NULL && rooms[0] > 0 && rooms[0] < g_Vars.roomcount
				&& g_ZombieDropSerialHere[k] != serial) {
			if (zombiesDropMake(drop, kind, &pos, floory, rooms)) {
				g_ZombieNetDropsMade++;
			}

			g_ZombieDropSerialHere[k] = serial;
		}

		if (drop->active) {
			drop->age60 = age;
		}
	}

	for (k = 0; k < ZOMBIE_MACHINE_IDS; k++) {
		machines[k][0] = (s16)znGet(&b, 2);
		machines[k][1] = (s16)znGet(&b, 2);
	}

	// once both have put them up (the host's first blocks are from before)
	if (!g_ZombieNetChecked && g_ZombieJuggPlaced && (machines[0][0] != -32768 || machines[ZOMBIE_MACHINE_IDS - 1][0] != -32768)) {
		g_ZombieNetChecked = true;
		zombiesNetCheckMachines(machines);
	}

	// each player
	for (i = 0; i < MAX_PLAYERS; i++) {
		const bool here = i < PLAYERCOUNT() && g_Vars.players[i] != NULL;
		struct zombienetplayer pl;

		zombiesNetReadPlayer(&b, &pl);

		const u32 perks = pl.perks;
		const s32 points = pl.points;
		const s32 count = pl.count;
		const s32 *order = pl.order;
		const s32 drink = pl.drink;
		const s32 drinkperk = pl.drinkperk;
		const u32 flags = pl.flags;
		const s32 nearperk = pl.nearperk;
		const s32 nearwall = pl.nearwall;
		const s32 neardowned = pl.neardowned;
		const s32 reviver = pl.reviver;
		const s32 bleed = pl.bleed;
		const s32 revive = pl.revive;
		const s32 selfrevive = pl.selfrevive;
		const s32 pap60 = pl.pap60;
		const s32 papgun = pl.papgun;
		const s32 papcount = pl.papcount;
		const s32 *papped = pl.papped;
		if (!here) {
			continue;
		}

		g_ZombiePoints[i] = points;
		g_ZombieHasJugg[i] = (perks & 1) != 0;

		for (k = 1; k < ZOMBIEPERK_COUNT; k++) {
			g_ZombieHasPerk[i][k] = (perks & (1 << k)) != 0;
		}

		g_ZombiePerkOrderCount[i] = count;

		for (k = 0; k < ZOMBIEPERK_COUNT; k++) {
			g_ZombiePerkOrder[i][k] = order[k];
		}

		g_ZombieDrink60[i] = drink < 0 ? -1 : znClamp(drink, 0, DRINK_TOTAL - 1);
		g_ZombieDrinkPerk[i] = drinkperk;
		g_ZombieDowned[i] = (flags & 0x01) != 0;
		g_ZombieRespawnDue[i] = (flags & 0x02) != 0;
		g_ZombieNearJugg[i] = (flags & 0x04) != 0;
		g_ZombieNearBox[i] = (flags & 0x08) != 0;
		g_ZombieNearPap[i] = (flags & 0x10) != 0;
		g_ZombieWhere[i] = (flags & 0x20) != 0;
		g_ZombieOutOfPlay[i] = (flags & 0x40) != 0;
		// what the client has not got itself is nobody
		g_ZombieNearPerk[i] = nearperk >= 0 && nearperk < ZOMBIEPERK_COUNT ? nearperk : -1;
		g_ZombieNearWall[i] = nearwall >= 0 && nearwall < g_ZombieNumWallBuys ? nearwall : -1;
		g_ZombieNearDowned[i] = neardowned >= 0 && neardowned < PLAYERCOUNT() ? neardowned : -1;
		g_ZombieReviver[i] = reviver >= 0 && reviver < PLAYERCOUNT() ? reviver : -1;
		g_ZombieBleed60[i] = bleed;
		g_ZombieRevive60[i] = revive;
		g_ZombieSelfRevive60[i] = selfrevive;
		g_ZombiePap60[i] = pap60 < 0 ? -1 : znClamp(pap60, 0, ZOMBIE_PAP_TOTAL60 - 1);
		g_ZombiePapGun[i] = papgun;
		g_ZombiePapCount[i] = papcount;

		for (k = 0; k < ZOMBIE_PAP_MAX; k++) {
			g_ZombiePapped[i][k] = papped[k];
		}
	}

	// the round cleared: its sting, as the host's (zombiesTick())
	if (wasnext < 0 && g_ZombieNextWaveFrame >= 0 && !final && g_ZombieRound > 0) {
		zombiesRoundSting();
	}

	// GAME OVER: this machine's own best for the arena, and its music
	if (!wasover && g_ZombieGameOver) {
		g_ZombieBestRound = zombiesReadBest(g_Vars.stagenum);
		g_ZombieNewBest = g_ZombieRound > g_ZombieBestRound;

		if (g_ZombieNewBest) {
			zombiesWriteBest(g_Vars.stagenum, g_ZombieRound);
		}

		sysLogPrintf(LOG_NOTE, "zombies: online, game over on stage 0x%02x at round %d (best here before %d)",
				g_Vars.stagenum, g_ZombieRound, g_ZombieBestRound);

		if (!final) {
			musicStartSting(MUSIC_MISSION_FAILED, TICKS(240 * 20));
		}
	}

	if (final) {
		sysLogPrintf(LOG_NOTE, "zombies: online, %d blocks applied, %d popups, %d sounds, %d power-ups made here",
				g_ZombieNetApplied, g_ZombieNetPopupsTaken, g_ZombieNetSoundsTaken, g_ZombieNetDropsMade);
	}
}

/**
 * scenarios.c's scenarioClientTick(), a net client's frame (C13: in place of
 * zombiesTick()): the machines put up here, and the host's state shown on them
 */
void zombiesClientTick(void)
{
	const s32 adv = g_Vars.lvupdate60;
	const s32 me = g_NetLocalSlot;
	s32 owner = -1;
	s32 i;
	s32 k;

	if (!NET_CLIENT) {
		return;
	}

	// a new match, as zombiesTick() knows one
	if (g_Vars.lvframenum < g_ZombieLastFrame) {
		zombiesResetJuggernog();
		zombiesResetBox();
		zombiesResetDrops();
		zombiesResetWallBuys();
		zombiesResetPerks();
		zombiesResetPap();
		zombiesResetPoints();
		zombiesResetDowned();
		zombiesResetGuns();

		for (i = 0; i < MAX_PLAYERS; i++) {
			g_ZombieKnife60[i] = 0;
			g_ZombieKnifeStage[i] = ZKNIFE_NONE;
			g_ZombieKnifeEnter60[i] = 0;
			g_ZombieKnifeRaise60[i] = 0;
			g_ZombieKnifeCooldown60[i] = 0;
			g_ZombieWhere[i] = false;
			g_ZombieOutOfPlay[i] = false;
		}

		for (k = 0; k < ARRAYCOUNT(g_ZombieDropSerialHere); k++) {
			g_ZombieDropSerialHere[k] = 0;
		}

		g_ZombieRound = 0;
		g_ZombieNextWaveFrame = -1;
		g_ZombieGameOver = false;
		g_ZombieGameOver60 = 0;
		g_ZombieNetRemaining = 0;
		g_ZombieNetPopupSeen = -1;
		g_ZombieNetSoundSeen = -1;
		g_ZombieNetBoxShown = -1;
		g_ZombieNetWallMask = 0;
		g_ZombieNetChecked = false;
		g_ZombieNetApplied = g_ZombieNetPopupsTaken = g_ZombieNetSoundsTaken = g_ZombieNetDropsMade = 0;
	}

	g_ZombieLastFrame = g_Vars.lvframenum;

	// up where the host's go, the same way
	if (!g_ZombieJuggPlaced && g_Vars.lvframenum >= 3 && g_Vars.players[0]->prop) {
		g_ZombieJuggPlaced = true;
		zombiesPlaceJuggernog();
		zombiesPlaceBox();
		zombiesPlacePerks();
		zombiesCornerMachines();
		zombiesPlaceWallBuys();
	}

	// the sides, as the host's (the radar's and the names' colours)
	if (g_Vars.lvframenum < 60) {
		g_MpSetup.options |= MPOPTION_TEAMSENABLED;

		for (i = 0; i < g_MpNumChrs; i++) {
			struct mpchrconfig *cfg = g_MpAllChrConfigPtrs[i];

			cfg->team = i < PLAYERCOUNT() ? 0 : 1;

			if (g_MpAllChrPtrs[i]) {
				g_MpAllChrPtrs[i]->team = 1 << cfg->team;
			}
		}
	}

	zombiesTickPopups();
	zombiesTintPerkMachines();
	zombiesTintPap();

	// between the host's blocks its clocks run on here as they do there
	if (g_ZombieMaxAmmoBanner60 >= 0) {
		g_ZombieMaxAmmoBanner60 += adv;

		if (g_ZombieMaxAmmoBanner60 > ZOMBIE_DROP_BANNER60) {
			g_ZombieMaxAmmoBanner60 = -1;
		}
	}

	g_ZombieInstaKill60 = g_ZombieInstaKill60 > adv ? g_ZombieInstaKill60 - adv : 0;
	g_ZombieDoublePoints60 = g_ZombieDoublePoints60 > adv ? g_ZombieDoublePoints60 - adv : 0;

	if (g_ZombieNukeFlash60 >= 0) {
		g_ZombieNukeFlash60 += adv;

		if (g_ZombieNukeFlash60 >= ZOMBIE_NUKE_FLASH60) {
			g_ZombieNukeFlash60 = -1;
		}
	}

	if (g_ZombieGameOver) {
		g_ZombieGameOver60 += adv;
	}

	if (g_ZombieBoxState != ZOMBIEBOX_IDLE) {
		g_ZombieBoxTime60 += adv;
	}

	for (i = 0; i < PLAYERCOUNT(); i++) {
		if (g_ZombieDrink60[i] >= 0) {
			g_ZombieDrink60[i] = g_ZombieDrink60[i] + adv >= DRINK_TOTAL ? -1 : g_ZombieDrink60[i] + adv;
		}

		if (g_ZombiePap60[i] >= 0 && g_ZombiePap60[i] + adv < ZOMBIE_PAP_TOTAL60) {
			g_ZombiePap60[i] += adv;
		}

		if (owner < 0 && g_ZombiePap60[i] >= ZOMBIE_PAP_DOWN60) {
			owner = i;
		}
	}

	// the gun over the box
	if (g_ZombieNetBoxShown != g_ZombieBoxShown) {
		if (g_ZombieNetBoxShown < 0) {
			zombiesBoxHideGun();
		} else {
			zombiesBoxShowGun(g_ZombieNetBoxShown);
		}
	}

	zombiesBoxPlaceGun();

	// the gun in Pack-a-Punch, from its going in to its being taken
	if (owner >= 0 && !g_ZombiePapGunUp) {
		zombiesPapShowGun(g_ZombiePapGun[owner]);
	} else if (owner < 0 && g_ZombiePapGunUp) {
		zombiesPapHideGun();
	}

	if (owner >= 0) {
		zombiesPapPlaceGun(g_ZombiePap60[owner]);
	}

	// the wall buys bought
	for (k = 0; k < g_ZombieNumWallBuys; k++) {
		if ((g_ZombieNetWallMask & (1 << k)) && !g_ZombieWallBuys[k].shown) {
			zombiesWallBuyShowGun(&g_ZombieWallBuys[k]);
		}
	}

	// the power-ups turning and bobbing
	for (k = 0; k < ARRAYCOUNT(g_ZombieDrops); k++) {
		struct zombiedrop *drop = &g_ZombieDrops[k];

		if (!drop->active) {
			continue;
		}

		if (drop->obj.prop == NULL) {
			drop->active = false;
			continue;
		}

		drop->age60 += adv;
		zombiesDropPlace(drop);
	}

	// this machine's own knife, swung as its walk is predicted
	if (me >= 0 && me < PLAYERCOUNT() && g_Vars.players[me] && g_Vars.players[me]->prop) {
		zombiesTickKnife(me);
	}

	// back in at the next round: no longer watching anyone. Offline the
	// round's turn (g_ZombieRespawnDue) ends it while still dead; a client
	// can be stood up by the host before any block of its has said so
	if (me >= 0 && me < PLAYERCOUNT() && g_Vars.players[me] && !g_Vars.players[me]->isdead) {
		g_ZombieSpectating[me] = false;
	}
}

/**
 * The Zombies Options page, Game Setup's Options for Zombies: the two of the
 * Combat Simulator's own rules that mean something here (the radar, auto-aim),
 * then Zombies' own (zombieperks.c keeps them in pd.ini, so they last from one
 * run of the game to the next).
 */
static const s32 g_ZombieStartPointChoices[] = { 500, 1000, 2500, 5000, 10000, 20000, 50000, 100000 };

// optionsmenu.c: a key binding row, item->param3 its bind
MenuItemHandlerResult menuhandlerZombiesBind(s32 operation, struct menuitem *item, union handlerdata *data);

MenuItemHandlerResult menuhandlerZombiesStartPoints(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[16];
	s32 i;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(g_ZombieStartPointChoices);
		break;
	case MENUOP_GETOPTIONTEXT:
		sprintf(text, "%d\n", g_ZombieStartPointChoices[data->dropdown.value]);
		return (uintptr_t)text;
	case MENUOP_SET:
		g_ZombieCfgStartPoints = g_ZombieStartPointChoices[data->dropdown.value];
		break;
	case MENUOP_GETSELECTEDINDEX:
		// the choice nearest what pd.ini says, which may be anything
		data->dropdown.value = 0;

		for (i = 0; i < ARRAYCOUNT(g_ZombieStartPointChoices); i++) {
			if (g_ZombieStartPointChoices[i] <= g_ZombieCfgStartPoints) {
				data->dropdown.value = i;
			}
		}
		break;
	}

	return 0;
}

MenuItemHandlerResult menuhandlerZombiesStartRound(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (g_ZombieCfgStartRound > 1 ? g_ZombieCfgStartRound : 1) - 1;
		break;
	case MENUOP_SET:
		g_ZombieCfgStartRound = data->slider.value + 1;
		break;
	case MENUOP_GETSLIDERLABEL:
		// the number alone: the item's own name says what it is, and a
		// narrow page (few digits of points) had "Round" over it
		sprintf(data->slider.label, "%d", data->slider.value + 1);
		break;
	}

	return 0;
}

// A checkbox for one of the settings: param3 says which
#define ZOMBIECFG_POWERUPS   1
#define ZOMBIECFG_ROUNDMUSIC 2
#define ZOMBIECFG_GROANS     3

MenuItemHandlerResult menuhandlerZombiesToggle(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 *setting = item->param3 == ZOMBIECFG_POWERUPS ? &g_ZombieCfgPowerups
		: item->param3 == ZOMBIECFG_ROUNDMUSIC ? &g_ZombieCfgRoundMusic : &g_ZombieCfgGroans;

	switch (operation) {
	case MENUOP_GET:
		return *setting != 0;
	case MENUOP_SET:
		*setting = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

extern struct menudialogdef g_ExtGameOptionsMenuDialog;

/**
 * The Zombies Controls page: Zombies' own buttons, in force in a Zombies
 * match and nowhere else (port/src/input.c, inputLiveBinds()). A row is one
 * of the layout's slots (input.c's zlayout[], in this order), a key and a
 * controller button each; one left alone keeps the normal controls' keys,
 * less any the layout gives to something else. As it comes: Space and A to
 * jump, F and X to interact, R and X to reload, X and the D-pad left for the
 * secondary function.
 */
MenuItemHandlerResult menuhandlerZombiesLayoutBind(s32 operation, struct menuitem *item, union handlerdata *data);
MenuItemHandlerResult menuhandlerZombiesLayoutReset(s32 operation, struct menuitem *item, union handlerdata *data);

#define ZOMBIE_LAYOUT_ROW(name, slot) \
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)(name), (slot), menuhandlerZombiesLayoutBind }

struct menuitem g_ZombiesControlsMenuItems[] = {
	ZOMBIE_LAYOUT_ROW("Jump\n", 0),
	ZOMBIE_LAYOUT_ROW("Fire\n", 1),
	ZOMBIE_LAYOUT_ROW("Aim\n", 2),
	ZOMBIE_LAYOUT_ROW("Reload\n", 3),
	ZOMBIE_LAYOUT_ROW("Knife\n", 4),
	ZOMBIE_LAYOUT_ROW("Interact\n", 5),
	ZOMBIE_LAYOUT_ROW("Secondary Function\n", 6),
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Reset Zombies Controls\n",
		0,
		menuhandlerZombiesLayoutReset,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_MPMENU_239, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ZombiesControlsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Zombies Controls",
	g_ZombiesControlsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_ZombiesOptionsMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR,
		L_MPMENU_226, // "No Radar"
		MPOPTION_NORADAR,
		menuhandlerMpCheckboxOption,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR,
		L_MPMENU_227, // "No Auto-Aim"
		MPOPTION_NOAUTOAIM,
		menuhandlerMpCheckboxOption,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Starting Points\n",
		0,
		menuhandlerZombiesStartPoints,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Starting Round\n",
		49, // rounds 1 to 50
		menuhandlerZombiesStartRound,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Power-Ups\n",
		ZOMBIECFG_POWERUPS,
		menuhandlerZombiesToggle,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Round Music\n",
		ZOMBIECFG_ROUNDMUSIC,
		menuhandlerZombiesToggle,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Zombie Groans\n",
		ZOMBIECFG_GROANS,
		menuhandlerZombiesToggle,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		// Zombies' own controls, a page of their own
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Zombies Controls...\n",
		0,
		(void *)&g_ZombiesControlsMenuDialog,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_MPMENU_239, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ZombiesOptionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Zombies Options",
	g_ZombiesOptionsMenuItems,
	mpOptionsMenuDialog,
	MENUDIALOGFLAG_MPLOCKABLE | MENUDIALOGFLAG_LITERAL_TEXT,
	&g_ExtGameOptionsMenuDialog,
};