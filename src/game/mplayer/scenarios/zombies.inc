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
#define ZOMBIE_REVIVE60       180    // 3 seconds of holding B: _zm_laststand's reviveTime (1.5 with Quick Revive)
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
extern s32 g_ZombieCfgPerkLimit;
#define ZOMBIE_POINTS_HIT      10
#define ZOMBIE_POINTS_KILL     60  // the torso's: _zm_score's 50 and its 10 for torso_upper or torso_lower
#define ZOMBIE_POINTS_LIMB     50  // an arm or a leg: the 50 alone
#define ZOMBIE_POINTS_HEADSHOT 100
#define ZOMBIE_POINTS_MELEE    130
#define ZOMBIE_POINTS_BLAST    50

static s32 g_ZombiePoints[MAX_PLAYERS];
static s32 g_ZombieTeamEarned; // every point anyone has been given this match, spent or not (score_total): power-ups drop by it

// The match's scoreboard (zombiesRenderScoreboard()), as Black Ops II's has
// it: the zombies each player killed, the times they went down, the players
// they got up again, and their kills by a shot to the head
struct zombiestats {
	u16 kills;
	u16 downs;
	u16 revives;
	u16 headshots;
};

static struct zombiestats g_ZombieStats[MAX_PLAYERS];

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

/**
 * Grenades, as Call of Duty's frags: two more at each round's start, four at
 * the most, Max Ammo filling them, thrown with a press of BUTTON_GRENADE
 * (zombiesGrenadePress()) without putting the gun away. Online a client's
 * count is the host's.
 */
#define ZOMBIE_GRENADES_ROUND  2
#define ZOMBIE_GRENADES_MAX    4
#define ZOMBIE_GRENADE_WAIT60  45 // one throw to the next
static s32 g_ZombieGrenades[MAX_PLAYERS];
static s32 g_ZombieGrenadeWait60[MAX_PLAYERS];

/**
 * The throw, as Black Ops II's frag (its weapon file, frag_grenade_zm): the
 * gun down in its quickDropTime, the pin pulled (viewmodel_m67_pullpin, 27
 * frames at 30, the pin out at its 13th), the arm held back while the button
 * is (cookOffHold: the fuse burning from the pin, and a grenade cooked to
 * its 3.5 seconds going off in the hand), then thrown (viewmodel_m67_throw,
 * 15 frames: its grenade is already out of the hand, so it leaves as the
 * throw starts) and the gun back up. The 60ths
 * are tools/zombies/vmbake.py's grenade.zpd's. Its speed is the weapon
 * file's: 920 inches a second forward and 120 up.
 */
#define ZTHROW_NONE 0
#define ZTHROW_DOWN 1
#define ZTHROW_PIN  2
#define ZTHROW_HOLD 3
#define ZTHROW_OUT  4
#define ZTHROW_UP   5
#define ZOMBIE_THROW_DOWN60    9   // quickDropTime 0.15
#define ZOMBIE_THROW_PIN60     53  // the pin pulled and the arm back
#define ZOMBIE_THROW_PINOUT60  26  // ... the pin out: the fuse lit
#define ZOMBIE_THROW_OUT60     29  // the throw
#define ZOMBIE_THROW_RELEASE60 3   // ... the grenade gone
#define ZOMBIE_THROW_UP60      12
#define ZOMBIE_GRENADE_FUSE60  210 // fuseTime 3.5
#define ZOMBIE_GRENADE_SPEED   38.9f // 920 inches a second, a 60th
#define ZOMBIE_GRENADE_UPSPEED 5.08f // 120
static s32 g_ZombieThrowStage[MAX_PLAYERS];
static s32 g_ZombieThrow60[MAX_PLAYERS];  // in the stage
static s32 g_ZombieCook60[MAX_PLAYERS];   // since the pin came out, -1 not yet

static void zombiesResetPoints(void)
{
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ZombiePoints[i] = g_ZombieCfgStartPoints;
		g_ZombieStackTop[i] = -1;
		g_ZombieGrenades[i] = 0; // round 1's two come with it (zombiesTick())
		g_ZombieGrenadeWait60[i] = 0;
		g_ZombieThrowStage[i] = ZTHROW_NONE;
		g_ZombieCook60[i] = -1;
	}

	memset(g_ZombieStats, 0, sizeof(g_ZombieStats));

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
static s32 g_ZombieFireSale60 = 0;

bool zombiesIsActive(void);

// chrDamage() asks: while Insta-Kill is on, a player's hit kills any zombie
bool zombiesInstaKillOn(void)
{
	return zombiesIsActive() && g_ZombieInstaKill60 > 0;
}

static void zombiesNetKeepSound(s32 sound, f32 pitch, s32 chr);

static s32 g_ZombieAtBarrier[MAX_BOTS]; // below: the barrier each zombie is held at
static bool g_ZombieReachThrough = false; // a swipe through a barrier's boards landing (zombiesReachHit())
static f32 g_ZombieBlowDamage = -1;       // a zombie's blow landing: what chrDamage() is to take, -1 none

// chrDamage(), about to take a player's health: a zombie's blow's damage, as
// it is, or -1 when this is no blow of a zombie's (zombiesReachHit())
f32 zombiesBlowDamage(void)
{
	return g_ZombieBlowDamage;
}

/**
 * chrDamage(), a zombie's blow landing on a player: its attack cry, Black
 * Ops II's (zmsfx.c), from the zombie. The host's, heard online from the
 * same zombie on every machine. True when the blow is stopped: a zombie still
 * tearing at a barrier's boards reaches no one through them, as in Call of
 * Duty (the barriers stand against Perfect Dark's walls, so it waits on the
 * player's side of them)
 */
bool zombiesZombieSwipe(struct chrdata *zchr)
{
	s32 vox;
	s32 i;

	if (!zombiesIsActive() || !zchr || !zchr->prop || !zchr->aibot) {
		return false;
	}

	for (i = 0; i < g_BotCount && i < MAX_BOTS; i++) {
		if (g_MpBotChrPtrs[i] == zchr && g_ZombieAtBarrier[i] >= 0 && !g_ZombieReachThrough) {
			return true;
		}
	}

	if (NET_CLIENT) {
		return false;
	}

	// the Groans setting is every zombie voice's
	vox = g_ZombieCfgGroans ? zmSfxPickVox(ZMVOX_ATTACK, rngRandom()) : -1;

	if (vox >= 0) {
		const f32 pitch = 0.94f + RANDOMFRAC() * 0.12f;

		psCreate(NULL, zchr->prop, zmSfxSound(vox), -1, -1, 0, 0, PSTYPE_NONE, NULL, pitch, NULL, -1, -1, -1, -1);
		zombiesNetKeepSound(ZMSFX_NET(vox), pitch, func0f18d074(mpPlayerGetIndex(zchr)));
	}

	return false;
}

static void zombiesMaybeCrawler(struct chrdata *zchr);
static void zombiesMaybeGib(struct chrdata *zchr, s32 hitpart, bool explosion, bool killed);
static void zombiesPickMoveClass(s32 i);

// Called from chrDamage() for every hit that lands on a living zombie.
void zombiesAwardHit(struct prop *aprop, s32 hitpart, bool ismelee, bool explosion, bool killed, struct chrdata *victim)
{
	s32 playernum;
	s32 value;

	// a client's points are the host's (the scenario block)
	if (!aprop || aprop->type != PROPTYPE_PLAYER || g_ZombieGameOver || NET_CLIENT) {
		return;
	}

	// its legs blown off, now and then, by a blast it lives through
	if (explosion && !killed) {
		zombiesMaybeCrawler(victim);
	}

	// an arm shot off, or the head on a killing headshot
	zombiesMaybeGib(victim, hitpart, explosion, killed);

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
	} else if (hitpart == HITPART_HEAD || hitpart == HITPART_HAT) {
		value = ZOMBIE_POINTS_HEADSHOT;
	} else if (hitpart == HITPART_TORSO || hitpart == HITPART_PELVIS) {
		value = ZOMBIE_POINTS_KILL;
	} else {
		value = ZOMBIE_POINTS_LIMB;
	}

	// Double Points
	if (g_ZombieDoublePoints60 > 0) {
		value *= 2;
	}

	g_ZombiePoints[playernum] += value;
	g_ZombieTeamEarned += value;
	zombiesAddPopup(playernum, value);

	if (killed && g_ZombieStats[playernum].kills < 0xffff) {
		g_ZombieStats[playernum].kills++;

		if ((hitpart == HITPART_HEAD || hitpart == HITPART_HAT) && !explosion && !ismelee) {
			g_ZombieStats[playernum].headshots++;
		}
	}

	// its death cry, Black Ops II's, from where it fell (heard online from
	// the same zombie on every machine)
	if (killed && victim && victim->prop) {
		const s32 vox = g_ZombieCfgGroans ? zmSfxPickVox(ZMVOX_DEATH, rngRandom()) : -1;

		if (vox >= 0) {
			const f32 pitch = 0.94f + RANDOMFRAC() * 0.12f;

			psCreate(NULL, victim->prop, zmSfxSound(vox), -1, -1, 0, 0, PSTYPE_NONE, NULL, pitch, NULL, -1, -1, -1, -1);
			zombiesNetKeepSound(ZMSFX_NET(vox), pitch, func0f18d074(mpPlayerGetIndex(victim)));
		}
	}

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
#define ZOMBIE_ROUND_BREAK 600 // frames between clearing a round and the next one starting: Black Ops II's ten seconds (zombie_between_round_time)
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

// Frames between zombie spawns, Black Ops II's zombie_spawn_delay: 2 seconds
// in round 1, a twentieth quicker each round after (1.26 by round 10, 0.75
// by 20), never under 0.08 of a second. It was 4 seconds and a tenth off a
// round, with half a second the least: round 1 came in at half the pace.
#define ZOMBIE_SPAWN_DELAY_FIRST 120
#define ZOMBIE_SPAWN_DELAY_MIN   5

static s32 zombiesSpawnDelay(s32 round)
{
	f32 delay = ZOMBIE_SPAWN_DELAY_FIRST;
	s32 i;

	for (i = 1; i < round && delay > ZOMBIE_SPAWN_DELAY_MIN; i++) {
		delay *= 0.95f;
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

/**
 * Crawlers: an explosion that leaves a zombie standing blows its legs off one
 * time in ZOMBIE_CRAWLER_ODDS, as Call of Duty's gibs them; it goes on on its
 * hands (zmchar.c's crawler, Black Ops II's _g_upclean and _g_legsoff) at a
 * crawl's pace. The host's, online in the scenario block (version 6).
 */
#define ZOMBIE_CRAWLER_ODDS  3

static u8 g_ZombieCrawler[MAX_BOTS];

static s32 zombiesBotIndex(struct chrdata *chr)
{
	s32 i;

	for (i = 0; chr && i < g_BotCount && i < MAX_BOTS; i++) {
		if (g_MpBotChrPtrs[i] == chr) {
			return i;
		}
	}

	return -1;
}

static void zombiesTestShoot(void);

static bool zombiesChrIsCrawler(struct chrdata *chr)
{
	const s32 i = zombiesBotIndex(chr);

	return i >= 0 && g_ZombieCrawler[i];
}

/**
 * Gibs, Call of Duty's: a hit on an arm that leaves a zombie standing takes
 * the arm off one time in ZOMBIE_GIB_ARM_ODDS (one arm, the first), and a
 * killing headshot pops the head one time in ZOMBIE_GIB_HEAD_ODDS. The host's,
 * online in the scenario block (version 7); every machine throws the arm
 * itself when it sees it go (zombiesRenderCodZombie()).
 */
#define ZGIB_LARM 0x01
#define ZGIB_RARM 0x02
#define ZGIB_HEAD 0x04

#define ZOMBIE_GIB_ARM_ODDS  3
#define ZOMBIE_GIB_HEAD_ODDS 2

static u8 g_ZombieGib[MAX_BOTS];

static void zombiesMaybeGib(struct chrdata *zchr, s32 hitpart, bool explosion, bool killed)
{
	const s32 i = zombiesBotIndex(zchr);

	if (i < 0) {
		return;
	}

	if (!killed && !(g_ZombieGib[i] & (ZGIB_LARM | ZGIB_RARM))) {
		const bool left = hitpart == HITPART_LBICEP || hitpart == HITPART_LFOREARM || hitpart == HITPART_LHAND;
		const bool right = hitpart == HITPART_RBICEP || hitpart == HITPART_RFOREARM || hitpart == HITPART_RHAND;

		if ((left || right) && rngRandom() % ZOMBIE_GIB_ARM_ODDS == 0) {
			g_ZombieGib[i] |= left ? ZGIB_LARM : ZGIB_RARM;
		}
	}

	if (killed && hitpart == HITPART_HEAD && !explosion && rngRandom() % ZOMBIE_GIB_HEAD_ODDS == 0) {
		g_ZombieGib[i] |= ZGIB_HEAD;
	}
}

static void zombiesMaybeCrawler(struct chrdata *zchr)
{
	const s32 i = zombiesBotIndex(zchr);

	if (i >= 0 && !g_ZombieCrawler[i] && !chrIsDead(zchr) && rngRandom() % ZOMBIE_CRAWLER_ODDS == 0) {
		g_ZombieCrawler[i] = 1;
	}
}

// botGetStatScale()'s, for a zombie's speed: the round's, or a crawl
/**
 * Each zombie's pace, Call of Duty's: at its spawn the round's speed (the
 * round times ZOMBIE_MOVE_PER_ROUND) give or take ZOMBIE_MOVE_SPREAD, under 35
 * a walker, under 70 a runner, else a sprinter - every one walks the first
 * three rounds, walkers and runners mix to round 8, then runners and
 * sprinters. Each goes at its own animation's speed over the ground (the
 * walk's 37.6 inches a second, the run's 108, the sprint's 142), as a share of
 * a simulant's run, which measures 147 (0.55 of it was 81: every zombie of
 * round 1 went at a run). A crawler crawls: a walker at the crawl's 14, the
 * others at the crawl sprint's 40.
 */
#define ZOMBIE_MOVE_PER_ROUND 8
#define ZOMBIE_MOVE_SPREAD    20
#define ZOMBIE_SIM_RUN        147.0f // inches a second, a simulant's run

enum { ZOMBIE_WALKER, ZOMBIE_RUNNER, ZOMBIE_SPRINTER };

static u8 g_ZombieMoveClass[MAX_BOTS];

static void zombiesPickMoveClass(s32 i)
{
	const s32 speed = g_ZombieRound * ZOMBIE_MOVE_PER_ROUND
		+ (s32)(rngRandom() % (ZOMBIE_MOVE_SPREAD * 2 + 1)) - ZOMBIE_MOVE_SPREAD;

	g_ZombieMoveClass[i] = speed <= 35 ? ZOMBIE_WALKER : speed <= 70 ? ZOMBIE_RUNNER : ZOMBIE_SPRINTER;
}

f32 zombiesChrSpeedScale(struct chrdata *chr)
{
	static const f32 inches[3] = { 37.6f, 108.3f, 142.1f };
	const s32 i = zombiesBotIndex(chr);
	const s32 cls = i >= 0 ? g_ZombieMoveClass[i] : ZOMBIE_WALKER;

	if (zombiesChrIsCrawler(chr)) {
		return (cls == ZOMBIE_WALKER ? 14.3f : 39.6f) / ZOMBIE_SIM_RUN;
	}

	return inches[cls] / ZOMBIE_SIM_RUN;
}

// Barriers and buyable doors (below, before zombiesTick())
static s32 g_ZombieSpawnAt = -1;            // the barrier the zombie being sent in goes to, -1 a pad
static s32 g_ZombieAtBarrier[MAX_BOTS];     // the barrier each zombie is held at, -1 none

// Each zombie put down at the start and not sent in since: not drawn, so the
// match does not open on the whole horde falling dead round the players
// (zombiesRenderCodZombie(), zombiesHideCorpse())
static bool g_ZombieHidden[MAX_BOTS];

// Each zombie's jump up a ladder (zombiesTickClimb())
static struct {
	bool on;
	s32 anim;
	s32 t60;
	s32 len60;
	struct coord from;  // where it stood at the foot, its feet's height
	f32 theta;          // facing the ladder
	f32 stretch;        // the ledge's rise over the clip's own
	struct coord to;    // its place on the floor at the top
	RoomNum torooms[8];
} g_ZombieClimb[MAX_BOTS];
static s32 zombiesBarrierPick(void);
static void zombiesBarrierTake(s32 b, s32 botnum);

// Sends one finished-dying zombie back in. Returns true if one went.
static bool zombiesSpawnOne(void)
{
	s32 i;

	for (i = 0; i < g_BotCount; i++) {
		struct chrdata *zchr = g_MpBotChrPtrs[i];

		if (zchr && zchr->aibot && zchr->actiontype == ACT_DEAD
				&& zchr->act_dead.fadetimer60 >= TICKS(90)) {
			zchr->aibot->config->difficulty = zombiesDifficultyForRound(g_ZombieRound);
			// most come in at a barrier, as Call of Duty's: held at its
			// boards until they are torn off (zombiesChooseSpawnLocation())
			g_ZombieSpawnAt = zombiesBarrierPick();
			g_ZombieAtBarrier[i] = -1;
			g_ZombieCrawler[i] = 0;
			g_ZombieGib[i] = 0;
			g_ZombieClimb[i].on = false;
			g_ZombieHidden[i] = false;
			zombiesPickMoveClass(i);
			botSpawn(zchr, true);

			if (g_ZombieSpawnAt >= 0) {
				zombiesBarrierTake(g_ZombieSpawnAt, i);
			}

			g_ZombieSpawnAt = -1;
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

			if (i < MAX_BOTS) {
				g_ZombieHidden[i] = true;
			}
		}
	}
}

// chrRender() asks (via zombiesRenderCodZombie()): a zombie put down at the
// start, still waiting to be sent in, is drawn as nothing at all
bool zombiesHideCorpse(struct chrdata *chr)
{
	s32 i;

	if (!zombiesIsActive() || !chr || !chr->aibot) {
		return false;
	}

	for (i = 0; i < g_BotCount && i < MAX_BOTS; i++) {
		if (g_MpBotChrPtrs[i] == chr) {
			return g_ZombieHidden[i] && chrIsDead(chr);
		}
	}

	return false;
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

/**
 * One of Black Ops II's own sounds (zmsfx.c, a ZMSFX_ slot), from prop
 * (heard from where it is) or, with none, as zombiesPlaySoundHere() plays;
 * online, the same on every machine. False when its file is not there, for
 * the caller to play the game's own instead.
 */
static bool zombiesPlayCodSound(s32 slot, struct prop *prop)
{
	const s32 id = zmSfxSound(slot);

	if (id <= 0) {
		return false;
	}

	zombiesNetKeepSound(ZMSFX_NET(slot), 1.0f, -1);

	if (prop) {
		psCreate(NULL, prop, id, -1, -1, 0, 0, PSTYPE_NONE, NULL, 1.0f, NULL, -1, -1, -1, -1);
	} else if (g_NetMode == NETMODE_NONE || g_ZombieSoundFor < 0 || netIsLocalSlot(g_ZombieSoundFor)) {
		// online, one player's own (a drink) only on that player's machine
		zombiesPlaySoundHere(id, 1.0f);
	}

	return true;
}

static void zombiesPlaySound(s32 sound, f32 pitch);

// Black Ops II's sound for something (zombiesPlayCodSound()), or the game's
// own in its place when its file is not there (fallback 0: none)
static void zombiesCodOr(s32 slot, struct prop *prop, s32 fallback, f32 pitch)
{
	if (!zombiesPlayCodSound(slot, prop) && fallback) {
		zombiesPlaySound(fallback, pitch);
	}
}

/**
 * A perk bought, as Black Ops II sounds it: zmb_cha_ching for the buyer, and
 * from the machine the bottle dropping out and the perk's sting
 * (mus_perks_*_sting; Electric Cherry has none). The game's own when the
 * files are not there.
 */
static void zombiesPerkBought(s32 perk, struct prop *machine)
{
	if (!zombiesPlayCodSound(ZMSFX_PERK_BUY, NULL)) {
		zombiesPlaySound(SFX_MENU_SELECT, 1.0f);
	}

	zombiesPlayCodSound(ZMSFX_PERK_DISPENSE, machine);
	zombiesPlayCodSound(ZMSFX_PERK_STING + perk, machine);
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

static void zombiesMakeDoors(void);
static s32 zombiesPadComp(s32 padnum);
static s32 g_ZombieStartComp = -1; // the zone the players start in, -1 for none (no doors)

/**
 * An arena's own layout, where it has one: a Black Ops II map converted by
 * codmap.c names where its perk machines, Pack-a-Punch, Mystery Box and
 * boarded windows stand (the mod's zombies/<arena>.txt, each spot a pad of the
 * arena's own), and they are put there and faced as Black Ops II has them in
 * place of being chosen from the spawn pads. Perks the layout has no machine
 * for are not in the match, as they are not in that map.
 */
#define ZOMBIE_LAYOUT_PERKS    16
#define ZOMBIE_LAYOUT_BARRIERS 16

struct zombielayout {
	s32 stagenum;   // what it was read for
	bool have;
	s32 perkpad[ZOMBIE_LAYOUT_PERKS];
	f32 perkback[ZOMBIE_LAYOUT_PERKS][2]; // a machine's back, the way it is turned from
	s32 pappad;
	f32 papback[2];
	s32 boxpad;
	f32 boxback[2];
	s32 numbarriers;
	s32 barpad[ZOMBIE_LAYOUT_BARRIERS];
	f32 barwall[ZOMBIE_LAYOUT_BARRIERS][2]; // from its pad out to the window
	f32 bardist[ZOMBIE_LAYOUT_BARRIERS];
};

static struct zombielayout g_ZombieLayout = { -1 };

const char *modloaderGetStageModDir(s32 stagenum);
const char *modloaderGetStageMapName(s32 stagenum);

static const struct zombielayout *zombiesLayout(void)
{
	struct zombielayout *l = &g_ZombieLayout;
	const char *dir;
	const char *name;
	char path[FS_MAXPATH + 1];
	FILE *f;
	char line[256];
	s32 i;

	if (l->stagenum == g_Vars.stagenum) {
		return l->have ? l : NULL;
	}

	l->stagenum = g_Vars.stagenum;
	l->have = false;
	l->pappad = l->boxpad = -1;
	l->numbarriers = 0;

	for (i = 0; i < ZOMBIE_LAYOUT_PERKS; i++) {
		l->perkpad[i] = -1;
	}

	dir = modloaderGetStageModDir(g_Vars.stagenum);
	name = modloaderGetStageMapName(g_Vars.stagenum);

	if (!dir || !name) {
		return NULL;
	}

	snprintf(path, sizeof(path), "%s/zombies/%s.txt", dir, name);
	f = fopen(fsFullPath(path), "rb");

	if (!f) {
		return NULL;
	}

	while (fgets(line, sizeof(line), f)) {
		s32 id, pad;
		f32 x, z, d;

		if (sscanf(line, "perk %d %d %f %f", &id, &pad, &x, &z) == 4 && id >= 0 && id < ZOMBIE_LAYOUT_PERKS) {
			l->perkpad[id] = pad;
			l->perkback[id][0] = x;
			l->perkback[id][1] = z;
		} else if (sscanf(line, "pap %d %f %f", &pad, &x, &z) == 3) {
			l->pappad = pad;
			l->papback[0] = x;
			l->papback[1] = z;
		} else if (sscanf(line, "box %d %f %f", &pad, &x, &z) == 3) {
			l->boxpad = pad;
			l->boxback[0] = x;
			l->boxback[1] = z;
		} else if (sscanf(line, "barrier %d %f %f %f", &pad, &x, &z, &d) == 4 && l->numbarriers < ZOMBIE_LAYOUT_BARRIERS) {
			l->barpad[l->numbarriers] = pad;
			l->barwall[l->numbarriers][0] = x;
			l->barwall[l->numbarriers][1] = z;
			l->bardist[l->numbarriers] = d;
			l->numbarriers++;
		}
	}

	fclose(f);
	l->have = true;
	sysLogPrintf(LOG_NOTE, "zombies: stage 0x%02x has its own layout (%d windows)", g_Vars.stagenum, l->numbarriers);

	return l;
}

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

	// the arena's own spot, which is no spawn pad
	if (zombiesLayout() && zombiesLayout()->boxpad >= 0) {
		g_ZombieBoxPad = zombiesLayout()->boxpad;
		return;
	}

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

		// in the zone the players start in, where there is one: the box is
		// the first thing a match spends on, before any door
		if (g_ZombieStartComp >= 0 && zombiesPadComp(g_SpawnPoints[i]) != g_ZombieStartComp) {
			score += 1e13f;
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

	// the arena's zones and its locked doors, once a stage, from the whole
	// spawn list (zombiesMakeDoors())
	zombiesMakeDoors();

	// with fewer, losing one would crowd the zombies coming in; an arena with
	// a spot of its own needs no pad
	if (n < 3 || zombiesFindJuggSpot(g_Vars.stagenum) || zombiesLayout()) {
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
	// an arena's own layout first: the player's place, ZOMBIE_JUGG_FRONT before
	// the machine, looking at its back (perk 0, ZOMBIEPERK_JUGGERNOG,
	// which is defined below)
	if (zombiesLayout() && zombiesLayout()->perkpad[0] >= 0) {
		static struct zombiespot spot;
		const struct zombielayout *l = zombiesLayout();
		struct pad pad;

		padUnpack(l->perkpad[0], PADFIELD_POS | PADFIELD_ROOM, &pad);
		spot.stagenum = stagenum;
		spot.x = pad.pos.x - l->perkback[0][0] * ZOMBIE_JUGG_FRONT;
		spot.y = pad.pos.y;
		spot.z = pad.pos.z - l->perkback[0][1] * ZOMBIE_JUGG_FRONT;
		spot.lookx = l->perkback[0][0];
		spot.lookz = l->perkback[0][1];
		spot.room = pad.room;

		return &spot;
	}

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
 * - Juggernog (2500): a zombie's blow is half a player's health, a fifth with
 *   it - two hits down, or five (zombiesReachHit()).
 * - Speed Cola (3000): reloads twice as fast (zombiesHandRate(), bondgun.c),
 *   and puts a barrier's boards back twice as fast.
 * - Double Tap (2000): fires a third faster, and every bullet counts twice,
 *   as Black Ops 2's Double Tap II (zombiesHandRate(), zombiesBulletScale()).
 * - Stamin-Up (2000): moves 7% faster and sprints twice as long
 *   (zombiesMoveScale(), bondwalk.c; zombiesSprintTick()).
 *
 * Four at once at the most, as Black Ops II has it: the Settings page's Perk
 * Limit (zombiesPerksFull()).
 * - Mule Kick (4000): a third gun (zombiesGunLimit()). Going down takes the
 *   third gun with it; drinking Mule Kick again gives that gun back.
 * - Deadshot Daiquiri (1500): aimed down the sights, a bullet that hits a
 *   zombie hits its head, as Black Ops II's snaps the aim there
 *   (zombiesDeadshotHitpart(), chrDamage()).
 * - PhD Flopper (2000): no harm from the player's own blasts, and a dolphin
 *   dive, or a drop from a height, lands with a blast of its own
 *   (zombiesPhdBlast()).
 * - Electric Cherry (2000): a reload sends out a shock, wider and harder the
 *   emptier the magazine was (zombiesReloadStarted(), bondgun.c).
 */
#define ZOMBIEPERK_JUGGERNOG   0
#define ZOMBIEPERK_QUICKREVIVE 1
#define ZOMBIEPERK_SPEEDCOLA   2
#define ZOMBIEPERK_DOUBLETAP   3
#define ZOMBIEPERK_STAMINUP    4
#define ZOMBIEPERK_MULEKICK    5
#define ZOMBIEPERK_DEADSHOT    6
#define ZOMBIEPERK_PHDFLOPPER  7
#define ZOMBIEPERK_CHERRY      8
#define ZOMBIEPERK_COUNT       9   // zombieperks.c's pictures are in this order

#define ZOMBIE_PERK_WALLREACH   400.0f // how far from its pad a machine's wall may be (200 left Skedar three short)
#define ZOMBIE_PERK_PADCLEAR    140.0f // and how far at least, so the pad stays clear
#define ZOMBIE_PERK_HALFDEPTH   45.0f  // the machine's, at ZOMBIE_JUGG_SCALE
#define ZOMBIE_QR_SOLO_COST     500
#define ZOMBIE_QR_SOLO_BUYS     3
#define ZOMBIE_QR_SELF60        600    // 10 seconds down, then up again: _zm_laststand's solo_revive_time
#define ZOMBIE_SPEEDCOLA_RATE   2.0f
#define ZOMBIE_DOUBLETAP_RATE   1.33f
#define ZOMBIE_DOUBLETAP_DAMAGE 2.0f
#define ZOMBIE_STAMINUP_SPEED   1.07f  // Call of Duty's 7% (it was 15%); its sprint twice as long besides
#define ZOMBIE_DOWNED_SPEED     0.35f  // a downed player's crawl, of their squatting pace
#define ZOMBIE_COD_WALK         1.0f   // Call of Duty's 190 inches a second is Perfect Dark's own run (zombiesMoveScale())

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
	{ "Deadshot Daiquiri", 1500,        { 0x90, 0x60, 0x38 }, 0x90603800 },
	{ "PhD Flopper",  2000,             { 0xc0, 0x40, 0xd0 }, 0xc040d000 },
	{ "Electric Cherry", 2000,          { 0x40, 0xd0, 0xff }, 0x40d0ff00 },
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
static s32 g_ZombieTestDrive60 = 0;            // --zombies-test-gap: 60ths player 1 walks on, crouched
static s32 g_ZombieQrLeave60 = -1;             // Quick Revive alone, its machine going (zombiesTickQrMachine()), -1 not
#define ZOMBIE_QR_LEAVE_WAIT60 120
#define ZOMBIE_QR_LEAVE_RISE60 180
#define ZOMBIE_QR_LEAVE_RISE   (40 * 2.54f) // inches, our units
#define ZOMBIE_QR_LEAVE_ROCK   10.0f // degrees
#define ZOMBIE_QR_LEAVE_SWING  30.0f // 60ths
static s32 g_ZombieSelfRevive60[MAX_PLAYERS];  // Quick Revive alone: 60ths until up again, 0 for none

// Alive, standing or down (zombiesPlayerIsUp() is standing only)
static bool zombiesPlayerAlive(s32 playernum)
{
	struct player *player = g_Vars.players[playernum];

	return player && player->prop && !player->isdead;
}

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

/**
 * The perk limit: how many a player may have at once, the Settings page's
 * Perk Limit, 1 to 99 (Black Ops II's four by default; nine and over is
 * every perk). A
 * player with that many is offered nothing at a machine until going down
 * loses them.
 */
static bool zombiesPerksFull(s32 playernum)
{
	s32 n = 0;
	s32 perk;

	if (g_ZombieCfgPerkLimit >= ZOMBIEPERK_COUNT) {
		return false;
	}

	for (perk = 0; perk < ZOMBIEPERK_COUNT; perk++) {
		n += zombiesPlayerHasPerk(playernum, perk);
	}

	return n >= (g_ZombieCfgPerkLimit > 0 ? g_ZombieCfgPerkLimit : 1);
}

static void zombiesResetPerks(void)
{
	s32 i;
	s32 k;

	// a new stage: the last one's props went with it
	bzero(g_ZombiePerkObjs, sizeof(g_ZombiePerkObjs));
	g_ZombieQrSoloBuys = 0;
	g_ZombieQrLeave60 = -1;

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
	Gfx *xlugdl;      // its glass and decals, seen through, after gdl's end; NULL for none
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
	"juggernog", "quickrevive", "speedcola", "doubletap", "staminup", "mulekick",
	"deadshot", "phdflopper", "electriccherry", "packapunch",
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

// The Ray Gun's glow (its coil's bands): vmconv.py's "raygun_2", a plain
// white picture Black Ops II's shader colours green. Lit, like a sign, and
// green (zombiesMeshBuildFrom()).
static bool zombiesMaterialIsRayGlow(const char *name)
{
	return strcmp(name, "raygun_2") == 0;
}

/**
 * The coil's own camo, animated as Pack-a-Punch's is (SHADER_OPT_CAMO): green
 * on the Ray Gun, red Pack-a-Punched. Its material calls the first list as it
 * starts and the second as it ends (zombiesMeshBuildFrom()), and the gun's
 * renderer writes both each frame (zombiesRayGlowWrite()): the coil's camo,
 * and then the gun's own again (the upgraded gun's camo, or none).
 */
static Gfx g_ZombieRayGlowOn[4];
static Gfx g_ZombieRayGlowOff[4];

#define ZOMBIE_CAMO_PLAIN 0 // the camo's styles (port/fast3d/gfx_cc.cpp)
#define ZOMBIE_CAMO_GREEN 1
#define ZOMBIE_CAMO_RED   2

static void zombiesRayGlowWrite(bool papped)
{
	u32 phase = (u32)(g_Vars.lvframe60 % 480) * 65536 / 480;
	Gfx *g = g_ZombieRayGlowOn;

	gDPSetGrayscaleColorEXT(g++, (phase >> 8) & 0xff, phase & 0xff,
			papped ? ZOMBIE_CAMO_RED : ZOMBIE_CAMO_GREEN, 0xff);
	gDPGrayscaleEXT(g++, 2);
	gSPEndDisplayList(g++);

	g = g_ZombieRayGlowOff;

	if (papped) {
		gDPSetGrayscaleColorEXT(g++, (phase >> 8) & 0xff, phase & 0xff, ZOMBIE_CAMO_PLAIN, 0xff);
		gDPGrayscaleEXT(g++, 2);
	} else {
		gDPGrayscaleEXT(g++, 0);
	}

	gSPEndDisplayList(g++);
}

// A material lit with the power on: its name ends "_on" (Black Ops II's
// "mtl_zombie_vending_jugg_on", "..._vent_power_on", "..._logo_on"). Not the
// Mob of the Dead machines' bodies, which carry the name whole (Deadshot's
// "..._al_vending_ads_on", PhD's "..._nuke_on", Electric Cherry's): lit up,
// the machine was a lamp. Deadshot's logo of its own still is.
static bool zombiesMaterialIsOn(const char *name)
{
	size_t len = strlen(name);

	if (strstr(name, "_ads_on") || strstr(name, "_nuke_on") || strstr(name, "electric_cherry_on")) {
		return false;
	}

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
	s32 pass;

	zm->tried = true;

	// the coil's camo lists, filled before any list calls them
	zombiesRayGlowWrite(false);

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
	maxgfx = m->numtris * 3 + m->numdraws * 18 + 9; // 18: two for the Ray Gun coil's camo lists; 9: both lists' ends
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

	// Two lists in the one buffer: the solid, then the seen-through (glass,
	// a blood decal) drawn in the translucent pass (zombiesRenderMachine())
	for (pass = 0; pass < 2; pass++) {
	if (pass == 1) {
		gSPEndDisplayList(&zm->gdl[numgfx++]);
		zm->xlugdl = &zm->gdl[numgfx];
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

		// Glass and a blood decal: see-through, in the second list alone.
		// Drawn solid, the glass was a gray slab over Deadshot's front and
		// Electric Cherry's, and the blood a dark block; cut out in the one
		// list (an alpha compare, then the opaque mode put back after it)
		// every draw after the first such one was gone, the whole of Deadshot
		// and PhD Flopper with it. Their pictures' alpha is how much they hide:
		// Deadshot's grime a third, Electric Cherry's frost clear in the middle
		if ((mat && (strstr(mat->name, "glass") || strstr(mat->name, "blood"))) != (pass == 1)) {
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

		// Lit with the power on - Black Ops II's "_on" materials, a sign, the
		// Ray Gun's coil - its picture as it is, not darkened by the room (the
		// second cycle's primitive colour, set round the list, is the room's
		// light). They used to be brightened by their own shade on top, with
		// a glow round the machine besides: the "_on" pictures are lit
		// already, and the machines looked like lamps.
		if (pass == 1) {
			gDPSetCombineLERP(gdl++, TEXEL0, 0, SHADE, 0, 0, 0, 0, TEXEL0,
					COMBINED, 0, PRIMITIVE, 0, 0, 0, 0, COMBINED);
		} else if (mat && (zombiesMaterialIsOn(mat->name) || strstr(mat->name, "sign") || zombiesMaterialIsRayGlow(mat->name))) {
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

		// the Ray Gun's coil in its own camo (zombiesRayGlowWrite())
		if (mat && zombiesMaterialIsRayGlow(mat->name)) {
			gSPDisplayList(gdl++, g_ZombieRayGlowOn);
		}

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

					// the Ray Gun's coil: Black Ops II's shader makes its white
					// green, and this is the colour it is drawn in
					if (mat && zombiesMaterialIsRayGlow(mat->name)) {
						zm->col[numvtx + j].r = 0x50;
						zm->col[numvtx + j].g = 0xff;
						zm->col[numvtx + j].b = 0x40;
					}
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

		// and the gun's own camo again after it
		if (mat && zombiesMaterialIsRayGlow(mat->name)) {
			gSPDisplayList(&zm->gdl[numgfx++], g_ZombieRayGlowOff);
		}

		// the draw's vertices, a run of their own, when they turn
		if (mat && strstr(mat->name, "_moving") && numvtx > drawfirstvtx && zm->numruns < 4) {
			zm->runfirst[zm->numruns] = drawfirstvtx;
			zm->runcount[zm->numruns] = numvtx - drawfirstvtx;
			zm->numruns++;
		}
	}
	}

	gSPEndDisplayList(&zm->gdl[numgfx++]);

	// a second list with nothing in it but its end: none
	if (zm->xlugdl == &zm->gdl[numgfx - 1]) {
		zm->xlugdl = NULL;
	}

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
 * A round falloff, added onto what is there (G_ADDITIVE_EXT), tested against
 * the depth and not written to it: the Ray Gun's bolts' (zombiesRenderRayFx())
 */
#define ZOMBIE_GLOW_TEX   32

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

// One quad: corners a, b, c, d in the space mtx takes into the view
// The texture the next quads are drawn with, across (zombiesRayTex() loads
// the Ray Gun's 64 ring as well as the 32 glow): the quad's s and t span it
// whole, or the ring showed only its top left quarter
static s32 g_ZombieQuadTex = ZOMBIE_GLOW_TEX;

static Gfx *zombiesGlowQuad(Gfx *gdl, Mtxf *mtx, const s16 corners[4][3])
{
	Vtx *vtx = gfxAllocate(sizeof(Vtx) * 4);
	Col *col = gfxAllocate(sizeof(Col) * 4);
	const s16 full = g_ZombieQuadTex << 5;
	const s16 st[4][2] = { { 0, 0 }, { full, 0 }, { full, full }, { 0, full } };
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

/**
 * The Ray Gun's shots, as Black Ops II's: a green bolt that flies (its weapon
 * file's projectileSpeed, 3200 inches a second) in place of the host's
 * tracer, and where it lands a green flash and Call of Duty's own ring
 * (fxt_fx_raygun_ring, baked by tools/zombies/fxbake.py into the build's
 * zombies/viewmodel/raygun_ring.i8) widening and fading, in place of the
 * explosive shell's explosion. gunfx.c's beamCreate() hands a shot here
 * (zombiesRayBoltCreate()) on every machine that draws it; prop.c's shell
 * leaves its explosion to it (zombiesShellBlast()). Drawn with the world's
 * beams (propsRenderBeams()), added light, the machines' glow texture for
 * the bolt and the flash.
 */
#define ZOMBIE_RAYFX_MAX     24
#define ZOMBIE_RAYFX_SPEED   135.0f  // a 60th: 3200 inches a second
#define ZOMBIE_RAYFX_TAIL    110.0f  // the streak's length behind its head
#define ZOMBIE_RAYFX_WIDTH   10.0f
#define ZOMBIE_RAYFX_HEAD    26.0f
#define ZOMBIE_RAYFX_RING60  22      // the ring's life
#define ZOMBIE_RAYFX_RING    110.0f  // its size at the end
#define ZOMBIE_RAYFX_FLASH60 9
#define ZOMBIE_RAYFX_FLASH   90.0f
#define ZOMBIE_RAYFX_RINGTEX 64

struct zombierayfx {
	bool on;
	struct coord from;
	struct coord dir;
	f32 dist;     // the head's way along
	f32 maxdist;  // where it lands
	f32 age60;    // since it landed, -1 still in flight
	bool red;     // a Pack-a-Punched Ray Gun's: red, as Call of Duty's Porter's X2
};

static struct zombierayfx g_ZombieRayFx[ZOMBIE_RAYFX_MAX];
static s32 g_ZombieRayFxFrame = -1;
static u8 g_ZombieRayRing[ZOMBIE_RAYFX_RINGTEX * ZOMBIE_RAYFX_RINGTEX] __attribute__((aligned(8)));
static s32 g_ZombieRayRingState = 0; // 0 not read, 1 read, -1 not there (the glow's used)

// gunfx.c: a shot's tracer; true when it is one of the Ray Gun's, made a
// bolt here and no tracer of the host's wanted
static bool zombiesGunIsPapped(s32 playernum, s32 weaponnum);

bool zombiesRayBoltCreate(s32 weaponnum, struct coord *from, struct coord *to)
{
	struct zombierayfx *fx = NULL;
	struct coord start = *from;
	f32 len;
	s32 i;

	if (!zombiesIsActive() || weaponnum != WEAPON_ZM_RAYGUN) {
		return false;
	}

	// the first person view's own: from under the eye at the right, where
	// the viewmodel's muzzle is, since the host's model is not drawn there
	for (i = 0; i < 2; i++) {
		if (from == &g_Vars.currentplayer->hands[i].muzzlepos) {
			Mtxf *m = camGetProjectionMtxF();
			struct coord cam = { 9, -9, -38 };

			mtx4TransformVec(m, &cam, &start);
		}
	}

	for (i = 0; i < ZOMBIE_RAYFX_MAX; i++) {
		if (!g_ZombieRayFx[i].on) {
			fx = &g_ZombieRayFx[i];
			break;
		}
	}

	if (!fx) {
		fx = &g_ZombieRayFx[0];
	}

	fx->from = start;
	fx->dir.x = to->x - start.x;
	fx->dir.y = to->y - start.y;
	fx->dir.z = to->z - start.z;
	len = sqrtf(fx->dir.x * fx->dir.x + fx->dir.y * fx->dir.y + fx->dir.z * fx->dir.z);

	if (len > 0) {
		fx->dir.x /= len;
		fx->dir.y /= len;
		fx->dir.z /= len;
	}

	fx->maxdist = len > 10000 ? 10000 : len;
	fx->dist = 0;
	fx->age60 = -1;
	fx->on = true;
	fx->red = false;

	// whose muzzle it left: theirs upgraded is red
	for (i = 0; i < PLAYERCOUNT(); i++) {
		if (g_Vars.players[i] && (from == &g_Vars.players[i]->hands[0].muzzlepos
					|| from == &g_Vars.players[i]->hands[1].muzzlepos)) {
			fx->red = zombiesGunIsPapped(i, WEAPON_ZM_RAYGUN);
		}
	}

	return true;
}

// The bolts on and the rings out, once a frame
static void zombiesRayFxTick(void)
{
	s32 adv;
	s32 i;

	if (g_ZombieRayFxFrame < 0 || g_Vars.lvframe60 < g_ZombieRayFxFrame) {
		g_ZombieRayFxFrame = g_Vars.lvframe60;
	}

	adv = g_Vars.lvframe60 - g_ZombieRayFxFrame;
	g_ZombieRayFxFrame = g_Vars.lvframe60;

	if (adv <= 0) {
		return;
	}

	for (i = 0; i < ZOMBIE_RAYFX_MAX; i++) {
		struct zombierayfx *fx = &g_ZombieRayFx[i];

		if (!fx->on) {
			continue;
		}

		if (fx->age60 < 0) {
			fx->dist += ZOMBIE_RAYFX_SPEED * adv;

			if (fx->dist >= fx->maxdist) {
				fx->dist = fx->maxdist;
				fx->age60 = 0;
			}
		} else {
			fx->age60 += adv;

			if (fx->age60 >= ZOMBIE_RAYFX_RING60) {
				fx->on = false;
			}
		}
	}
}

static void zombiesRayRingLoad(void)
{
	u8 *data;
	u32 size = 0;
	const char *rel = "$E/zombies/viewmodel/raygun_ring.i8";

	g_ZombieRayRingState = -1;

	if (fsFileSize(fsFullPath(rel)) <= 0 || (data = fsFileLoad(rel, &size)) == NULL) {
		return;
	}

	if (size == sizeof(g_ZombieRayRing)) {
		memcpy(g_ZombieRayRing, data, sizeof(g_ZombieRayRing));
		g_ZombieRayRingState = 1;
	}

	sysMemFree(data);
}

// A quad round pos: corners at pos + a * (+-1) + b * (+-1), in the world
static Gfx *zombiesRayQuad(Gfx *gdl, Mtxf *cam, struct coord *pos, const f32 a[3], const f32 b[3])
{
	Mtxf world;
	Mtxf *mtx = gfxAllocateMatrix();
	const s16 corners[4][3] = {
		{ (s16)(-a[0] - b[0]), (s16)(-a[1] - b[1]), (s16)(-a[2] - b[2]) },
		{ (s16)(a[0] - b[0]), (s16)(a[1] - b[1]), (s16)(a[2] - b[2]) },
		{ (s16)(a[0] + b[0]), (s16)(a[1] + b[1]), (s16)(a[2] + b[2]) },
		{ (s16)(-a[0] + b[0]), (s16)(-a[1] + b[1]), (s16)(-a[2] + b[2]) },
	};

	mtx4LoadTranslation(pos, &world);
	mtx4MultMtx4(cam, &world, mtx);

	return zombiesGlowQuad(gdl, mtx, corners);
}

static Gfx *zombiesRayTex(Gfx *gdl, bool ring)
{
	gDPPipeSync(gdl++);

	if (ring && g_ZombieRayRingState > 0) {
		g_ZombieQuadTex = ZOMBIE_RAYFX_RINGTEX;
		gDPLoadTextureBlock(gdl++, g_ZombieRayRing, G_IM_FMT_I, G_IM_SIZ_8b,
				ZOMBIE_RAYFX_RINGTEX, ZOMBIE_RAYFX_RINGTEX, 0,
				G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
				G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
	} else {
		g_ZombieQuadTex = ZOMBIE_GLOW_TEX;
		gDPLoadTextureBlock(gdl++, g_ZombieGlowTex, G_IM_FMT_I, G_IM_SIZ_8b,
				ZOMBIE_GLOW_TEX, ZOMBIE_GLOW_TEX, 0,
				G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
				G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
	}

	return gdl;
}

/**
 * Electric Cherry's burst as a reload begins, from Black Ops II's own pictures
 * (Mob of the Dead's fx_alcatraz_electric_cherry_player: its effect file does
 * not dump, its pictures do; tools/zombies/fxbake.py's cherry into the build's
 * zombies/viewmodel/): a blue flash at the player's middle (fxt_light_flare_
 * cool), the electric ring widening out to the shock's reach over the floor
 * (fxt_fx_emp_ring_wave), and lightning (fxt_env_lighting_bolt_ground, eight
 * bolts) from the player to each zombie it shocks, or out across the floor
 * when there are none, flickering from one bolt to the next. Added light,
 * drawn with the Ray Gun's (zombiesRenderRayFx()). Each machine draws the
 * bursts it is told of (zombiesCherryBurst()).
 */
#define ZOMBIE_CHERRY_FX60     40     // the burst's 60ths: its ring and its bolts (Black Ops II's is near 0.8 s)
#define ZOMBIE_CHERRY_ZAP60    50     // a shocked zombie's crackle (fx_alcatraz_electric_cherry_sm), and the sparkles
#define ZOMBIE_CHERRY_FLASH60  10.0f  // the flash's
#define ZOMBIE_CHERRY_STAND60  30.0f  // the bolts standing round the player
#define ZOMBIE_CHERRY_STANDS   6      // how many
#define ZOMBIE_CHERRY_SPARKLES 18
// its colour, as Black Ops II's: violet round a white core (the pictures are white)
#define ZOMBIE_CHERRY_R 0xc8
#define ZOMBIE_CHERRY_G 0xb8
#define ZOMBIE_CHERRY_B 0xff
#define ZOMBIE_CHERRY_BOLTS    8      // the most bolts a burst throws
#define ZOMBIE_CHERRY_BOLTW    18.0f  // a bolt's half width
#define ZOMBIE_CHERRY_TEX      64     // the ring's and the flash's side
#define ZOMBIE_CHERRY_BOLTTEX_W 32
#define ZOMBIE_CHERRY_BOLTTEX_H 64

static struct {
	bool on;
	struct coord at;      // the player's middle
	f32 reach;            // the ring's widest
	f32 age60;
	s32 numbolts;
	struct coord to[ZOMBIE_CHERRY_BOLTS];
	s32 bot[ZOMBIE_CHERRY_BOLTS]; // the zombie each bolt struck, -1 the floor
} g_ZombieCherryFx[MAX_PLAYERS];

static s32 g_ZombieCherryFxFrame = -1;
static s32 g_ZombieCherryTexState = 0; // 0 not read, 1 read, -1 not there (no burst drawn)
static u8 g_ZombieCherryRing[ZOMBIE_CHERRY_TEX * ZOMBIE_CHERRY_TEX] __attribute__((aligned(8)));
static u8 g_ZombieCherryFlare[ZOMBIE_CHERRY_TEX * ZOMBIE_CHERRY_TEX] __attribute__((aligned(8)));
static u8 g_ZombieCherryBolts[8][ZOMBIE_CHERRY_BOLTTEX_W * ZOMBIE_CHERRY_BOLTTEX_H] __attribute__((aligned(8)));

static bool zombiesCherryTexRead(const char *rel, void *dst, u32 want)
{
	u8 *data;
	u32 size = 0;

	if (fsFileSize(fsFullPath(rel)) <= 0 || (data = fsFileLoad(rel, &size)) == NULL) {
		return false;
	}

	if (size == want) {
		memcpy(dst, data, want);
	}

	sysMemFree(data);

	return size == want;
}

static void zombiesCherryTexLoad(void)
{
	g_ZombieCherryTexState = zombiesCherryTexRead("$E/zombies/viewmodel/cherry_ring.i8", g_ZombieCherryRing, sizeof(g_ZombieCherryRing))
		&& zombiesCherryTexRead("$E/zombies/viewmodel/cherry_flare.i8", g_ZombieCherryFlare, sizeof(g_ZombieCherryFlare))
		&& zombiesCherryTexRead("$E/zombies/viewmodel/cherry_bolts.i8", g_ZombieCherryBolts, sizeof(g_ZombieCherryBolts))
		? 1 : -1;
}

// A quad round pos, as zombiesRayQuad()'s, its picture's s across b and t
// along a (a bolt's length), sw by th texels
static Gfx *zombiesCherryQuad(Gfx *gdl, Mtxf *cam, struct coord *pos, const f32 a[3], const f32 b[3], s32 sw, s32 th)
{
	Mtxf world;
	Mtxf *mtx = gfxAllocateMatrix();
	Vtx *vtx = gfxAllocate(sizeof(Vtx) * 4);
	Col *col = gfxAllocate(sizeof(Col) * 4);
	const f32 sa[4] = { -1, 1, 1, -1 };
	const f32 sb[4] = { -1, -1, 1, 1 };
	s32 i;

	if (!vtx || !col) {
		return gdl;
	}

	for (i = 0; i < 4; i++) {
		vtx[i].x = (s16)(a[0] * sa[i] + b[0] * sb[i]);
		vtx[i].y = (s16)(a[1] * sa[i] + b[1] * sb[i]);
		vtx[i].z = (s16)(a[2] * sa[i] + b[2] * sb[i]);
		vtx[i].flags = 0;
		vtx[i].colour = (u8)(i * 4);
		vtx[i].s = sb[i] > 0 ? sw << 5 : 0;
		vtx[i].t = sa[i] > 0 ? th << 5 : 0;
		col[i].r = col[i].g = col[i].b = col[i].a = 0xff;
	}

	mtx4LoadTranslation(pos, &world);
	mtx4MultMtx4(cam, &world, mtx);

	gSPMatrix(gdl++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW | G_MTX_FLOATS);
	gSPColor(gdl++, col, 4);
	gSPVertex(gdl++, vtx, 4, 0);
	gSPTri4(gdl++, 0, 1, 2, 0, 2, 3, 0, 0, 0, 0, 0, 0);

	return gdl;
}

static Gfx *zombiesCherryTex(Gfx *gdl, const void *tex, s32 w, s32 h)
{
	gDPPipeSync(gdl++);
	gDPLoadTextureBlock(gdl++, tex, G_IM_FMT_I, G_IM_SIZ_8b, w, h, 0,
			G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
			G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);

	return gdl;
}

// zombiesRenderRayFx()'s, its render state set: every burst going, aged
static Gfx *zombiesRenderCherryFx(Gfx *gdl, Mtxf *cam, struct coord *campos, const f32 right[3], const f32 up[3])
{
	s32 adv;
	s32 p;

	if (g_ZombieCherryFxFrame < 0 || g_Vars.lvframe60 < g_ZombieCherryFxFrame) {
		g_ZombieCherryFxFrame = g_Vars.lvframe60;
	}

	adv = g_Vars.lvframe60 - g_ZombieCherryFxFrame;
	g_ZombieCherryFxFrame = g_Vars.lvframe60;

	for (p = 0; p < MAX_PLAYERS; p++) {
		struct coord centre;
		bool mine;
		f32 f;
		f32 a[3];
		f32 b[3];
		s32 k;
		s32 n;

		if (!g_ZombieCherryFx[p].on) {
			continue;
		}

		g_ZombieCherryFx[p].age60 += adv;

		if (g_ZombieCherryFx[p].age60 >= ZOMBIE_CHERRY_ZAP60) {
			g_ZombieCherryFx[p].on = false;
			continue;
		}

		// each zombie it struck crackling, where it is now: a small flare
		// at its chest and two short bolts across it, turning, going out
		for (n = 0; n < g_ZombieCherryFx[p].numbolts; n++) {
			const s32 bot = g_ZombieCherryFx[p].bot[n];
			const f32 g = 1 - g_ZombieCherryFx[p].age60 / ZOMBIE_CHERRY_ZAP60;
			struct chrdata *zchr = bot >= 0 && bot < g_BotCount ? g_MpBotChrPtrs[bot] : NULL;
			struct coord chest;
			s32 j;

			if (!zchr || !zchr->prop) {
				continue;
			}

			chest = zchr->prop->pos;
			chest.y = zchr->ground + 75;

			// in front of the body, toward the camera: at its middle the body
			// hid all but the edges
			{
				f32 tx = campos->x - chest.x;
				f32 ty = campos->y - chest.y;
				f32 tz = campos->z - chest.z;
				const f32 tl = sqrtf(tx * tx + ty * ty + tz * tz);

				if (tl > 1) {
					chest.x += tx / tl * 25;
					chest.y += ty / tl * 25;
					chest.z += tz / tl * 25;
				}
			}

			for (k = 0; k < 3; k++) {
				a[k] = up[k] * 32;
				b[k] = right[k] * 32;
			}

			gdl = zombiesCherryTex(gdl, g_ZombieCherryFlare, ZOMBIE_CHERRY_TEX, ZOMBIE_CHERRY_TEX);
			gDPSetPrimColor(gdl++, 0, 0, ZOMBIE_CHERRY_R, ZOMBIE_CHERRY_G, ZOMBIE_CHERRY_B, (u8)(0xff * g * (((s32)g_ZombieCherryFx[p].age60 + n) % 3 ? 1.0f : 0.5f)));
			gdl = zombiesCherryQuad(gdl, cam, &chest, a, b, ZOMBIE_CHERRY_TEX, ZOMBIE_CHERRY_TEX);

			for (j = 0; j < 2; j++) {
				const f32 ang = (n * 2.1f + j * 1.7f) + g_ZombieCherryFx[p].age60 * 0.35f;
				const s32 frame = (n + j * 4 + (s32)(g_ZombieCherryFx[p].age60 / 2)) % 8;

				// a bolt up and down the body, leaning round in the camera's plane
				for (k = 0; k < 3; k++) {
					a[k] = (up[k] * cosf(ang) + right[k] * sinf(ang)) * 45;
					b[k] = (right[k] * cosf(ang) - up[k] * sinf(ang)) * 10;
				}

				gdl = zombiesCherryTex(gdl, g_ZombieCherryBolts[frame], ZOMBIE_CHERRY_BOLTTEX_W, ZOMBIE_CHERRY_BOLTTEX_H);
				gDPSetPrimColor(gdl++, 0, 0, ZOMBIE_CHERRY_R, ZOMBIE_CHERRY_G, ZOMBIE_CHERRY_B, (u8)(0xff * g));
				gdl = zombiesCherryQuad(gdl, cam, &chest, a, b, ZOMBIE_CHERRY_BOLTTEX_W, ZOMBIE_CHERRY_BOLTTEX_H);
			}
		}

		if (g_ZombieCherryFx[p].age60 >= ZOMBIE_CHERRY_FX60) {
			continue;
		}

		f = g_ZombieCherryFx[p].age60 / ZOMBIE_CHERRY_FX60;

		// Seen by the player it is round: from their eye the middle is under
		// the camera and the floor's ring and bolts below the view, behind
		// the gun. So for them it goes from the gun - ahead of the eye, low -
		// and the ring spreads at their chest.
		if (p == g_Vars.currentplayernum && g_Vars.players[p] && g_Vars.players[p]->prop
				&& (campos->x - g_Vars.players[p]->prop->pos.x) * (campos->x - g_Vars.players[p]->prop->pos.x)
				+ (campos->z - g_Vars.players[p]->prop->pos.z) * (campos->z - g_Vars.players[p]->prop->pos.z) < 40 * 40) {
			struct player *viewer = g_Vars.players[p];

			mine = true;
			centre.x = campos->x + viewer->cam_look.x * 40;
			centre.y = campos->y - 25;
			centre.z = campos->z + viewer->cam_look.z * 40;
		} else {
			mine = false;
			centre = g_ZombieCherryFx[p].at;
		}

		// the ring, flat, widening and going
		{
			struct coord at = g_ZombieCherryFx[p].at;
			const f32 r = 30 + (g_ZombieCherryFx[p].reach - 30) * sqrtf(f);

			at.y = mine ? campos->y - 45 : at.y - 60;

			a[0] = r; a[1] = 0; a[2] = 0;
			b[0] = 0; b[1] = 0; b[2] = r;

			gdl = zombiesCherryTex(gdl, g_ZombieCherryRing, ZOMBIE_CHERRY_TEX, ZOMBIE_CHERRY_TEX);
			gDPSetPrimColor(gdl++, 0, 0, ZOMBIE_CHERRY_R, ZOMBIE_CHERRY_G, ZOMBIE_CHERRY_B, (u8)(0x90 * (1 - f)));
			gdl = zombiesCherryQuad(gdl, cam, &at, a, b, ZOMBIE_CHERRY_TEX, ZOMBIE_CHERRY_TEX);
		}

		// the flash, facing the camera
		if (g_ZombieCherryFx[p].age60 < ZOMBIE_CHERRY_FLASH60) {
			const f32 g = 1 - g_ZombieCherryFx[p].age60 / ZOMBIE_CHERRY_FLASH60;
			// the player's own small and softer: 40 ahead of the eye a big one
			// washed out a quarter of the screen
			const f32 size = (mine ? 22 : 90) * (0.5f + 0.5f * g);

			for (k = 0; k < 3; k++) {
				a[k] = up[k] * size;
				b[k] = right[k] * size;
			}

			gdl = zombiesCherryTex(gdl, g_ZombieCherryFlare, ZOMBIE_CHERRY_TEX, ZOMBIE_CHERRY_TEX);
			gDPSetPrimColor(gdl++, 0, 0, ZOMBIE_CHERRY_R, ZOMBIE_CHERRY_G, ZOMBIE_CHERRY_B, (u8)((mine ? 0xb0 : 0xff) * g));
			gdl = zombiesCherryQuad(gdl, cam, &centre, a, b, ZOMBIE_CHERRY_TEX, ZOMBIE_CHERRY_TEX);
		}

		// The bolts standing round the player, as Black Ops II's: tall and
		// thick, from the floor to well over the head, a ring of them a
		// stride out, each a new one of the eight every two 60ths and moving
		// round a little at each, turned to the camera about the upright. The
		// player sees them filling the view ahead and at its sides.
		if (g_ZombieCherryFx[p].age60 < ZOMBIE_CHERRY_STAND60) {
			const f32 g = g_ZombieCherryFx[p].age60 < ZOMBIE_CHERRY_STAND60 / 2 ? 1
				: 2 - 2 * g_ZombieCherryFx[p].age60 / ZOMBIE_CHERRY_STAND60;
			const s32 tick = (s32)(g_ZombieCherryFx[p].age60 / 2);
			const f32 floor = g_ZombieCherryFx[p].at.y - 80;
			s32 j;

			for (j = 0; j < ZOMBIE_CHERRY_STANDS; j++) {
				const f32 ang = j * (2 * M_PI / ZOMBIE_CHERRY_STANDS) + ((tick * 5 + j * 3) % 7) * 0.12f;
				const f32 radius = 50 + ((tick * 3 + j * 7) % 5) * 8;
				const s32 frame = (tick + j * 3) % 8;
				struct coord at;
				f32 tx;
				f32 tz;
				f32 tl;
				s32 twice;

				at.x = g_ZombieCherryFx[p].at.x + sinf(ang) * radius;
				at.y = floor + 130;
				at.z = g_ZombieCherryFx[p].at.z + cosf(ang) * radius;

				// across: level, square to the way to the camera
				tx = campos->x - at.x;
				tz = campos->z - at.z;
				tl = sqrtf(tx * tx + tz * tz);

				if (tl < 1) {
					continue;
				}

				a[0] = 0; a[1] = 135; a[2] = 0;
				b[0] = -tz / tl * 34; b[1] = 0; b[2] = tx / tl * 34;

				gdl = zombiesCherryTex(gdl, g_ZombieCherryBolts[frame], ZOMBIE_CHERRY_BOLTTEX_W, ZOMBIE_CHERRY_BOLTTEX_H);
				gDPSetPrimColor(gdl++, 0, 0, ZOMBIE_CHERRY_R, ZOMBIE_CHERRY_G, ZOMBIE_CHERRY_B,
						(u8)(0xff * g * ((tick + j) % 3 ? 1.0f : 0.6f)));

				// twice over: some of the eight are faint, and added once
				// they barely showed against a bright wall
				for (twice = 0; twice < 2; twice++) {
					gdl = zombiesCherryQuad(gdl, cam, &at, a, b, ZOMBIE_CHERRY_BOLTTEX_W, ZOMBIE_CHERRY_BOLTTEX_H);
				}
			}
		}

		// Sparkles hanging in the air round the player, drifting up, going
		// out one by one: each its own place, pace and time, from its number
		if (g_ZombieCherryFx[p].age60 < ZOMBIE_CHERRY_FX60) {
			s32 j;

			gdl = zombiesCherryTex(gdl, g_ZombieCherryFlare, ZOMBIE_CHERRY_TEX, ZOMBIE_CHERRY_TEX);

			for (j = 0; j < ZOMBIE_CHERRY_SPARKLES; j++) {
				const u32 h = (u32)(j + 1) * 2654435761u;
				const f32 life = 0.5f + (h >> 8 & 0xff) / 512.0f; // its share of the time
				const f32 t = g_ZombieCherryFx[p].age60 / ZOMBIE_CHERRY_FX60 / life;
				const f32 ang = (h & 0xff) * (2 * M_PI / 256);
				const f32 radius = 25 + (h >> 16 & 0xff) * 110 / 255.0f;
				struct coord at;

				if (t >= 1) {
					continue;
				}

				at.x = g_ZombieCherryFx[p].at.x + sinf(ang) * radius;
				at.y = g_ZombieCherryFx[p].at.y - 70 + (h >> 24) * 200 / 255.0f + t * 40;
				at.z = g_ZombieCherryFx[p].at.z + cosf(ang) * radius;

				for (k = 0; k < 3; k++) {
					a[k] = up[k] * 5;
					b[k] = right[k] * 5;
				}

				gDPSetPrimColor(gdl++, 0, 0, 0xf0, 0xec, 0xff, (u8)(0xff * (1 - t) * ((j + (s32)g_ZombieCherryFx[p].age60) % 4 ? 1.0f : 0.4f)));
				gdl = zombiesCherryQuad(gdl, cam, &at, a, b, ZOMBIE_CHERRY_TEX, ZOMBIE_CHERRY_TEX);
			}
		}

		// the bolts, each a new one of the eight every few 60ths, flickering
		for (n = 0; n < g_ZombieCherryFx[p].numbolts && f < 0.6f; n++) {
			const s32 frame = (n * 3 + (s32)(g_ZombieCherryFx[p].age60 / 3)) % 8;
			struct coord *from = &centre;
			struct coord *to = &g_ZombieCherryFx[p].to[n];
			const s32 bot = g_ZombieCherryFx[p].bot[n];

			// to the zombie where it is now
			if (bot >= 0 && bot < g_BotCount && g_MpBotChrPtrs[bot] && g_MpBotChrPtrs[bot]->prop) {
				*to = g_MpBotChrPtrs[bot]->prop->pos;
				to->y = g_MpBotChrPtrs[bot]->ground + 70;
			}
			struct coord mid;
			f32 along[3];
			f32 tocam[3];
			f32 side[3];
			f32 len;

			along[0] = to->x - from->x;
			along[1] = to->y - from->y;
			along[2] = to->z - from->z;
			mid.x = from->x + along[0] / 2;
			mid.y = from->y + along[1] / 2;
			mid.z = from->z + along[2] / 2;
			tocam[0] = campos->x - mid.x;
			tocam[1] = campos->y - mid.y;
			tocam[2] = campos->z - mid.z;
			side[0] = along[1] * tocam[2] - along[2] * tocam[1];
			side[1] = along[2] * tocam[0] - along[0] * tocam[2];
			side[2] = along[0] * tocam[1] - along[1] * tocam[0];
			len = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);

			if (len <= 0) {
				continue;
			}

			for (k = 0; k < 3; k++) {
				a[k] = along[k] / 2;
				b[k] = side[k] / len * ZOMBIE_CHERRY_BOLTW;
			}

			gdl = zombiesCherryTex(gdl, g_ZombieCherryBolts[frame], ZOMBIE_CHERRY_BOLTTEX_W, ZOMBIE_CHERRY_BOLTTEX_H);
			gDPSetPrimColor(gdl++, 0, 0, ZOMBIE_CHERRY_R, ZOMBIE_CHERRY_G, ZOMBIE_CHERRY_B, (u8)(((s32)g_ZombieCherryFx[p].age60 + n) % 4 == 0 ? 0x90 : 0xff));
			gdl = zombiesCherryQuad(gdl, cam, &mid, a, b, ZOMBIE_CHERRY_BOLTTEX_W, ZOMBIE_CHERRY_BOLTTEX_H);
		}
	}

	return gdl;
}

static bool zombiesCherryFxAny(void)
{
	s32 p;

	for (p = 0; p < MAX_PLAYERS; p++) {
		if (g_ZombieCherryFx[p].on) {
			return true;
		}
	}

	return false;
}

// propobj.c's propsRenderBeams(): every bolt and ring, in the world
Gfx *zombiesRenderGibs(Gfx *gdl);

static Gfx *zombiesRenderPowerupGlows(Gfx *gdl, Mtxf *cam, struct coord *campos, const f32 right[3], const f32 up[3], bool draw);

Gfx *zombiesRenderRayFx(Gfx *gdl)
{
	Mtxf *cam = camGetWorldToScreenMtxf();
	struct coord *campos = &g_Vars.currentplayer->cam_pos;
	f32 right[3];
	f32 up[3];
	bool any = false;
	s32 i;

	if (!zombiesIsActive()) {
		return gdl;
	}

	// arms shot off, flying (below)
	gdl = zombiesRenderGibs(gdl);

	zombiesRayFxTick();

	for (i = 0; i < ZOMBIE_RAYFX_MAX; i++) {
		any |= g_ZombieRayFx[i].on;
	}

	// Electric Cherry's bursts too (above)
	if (g_ZombieCherryTexState == 0) {
		zombiesCherryTexLoad();
	}

	if (g_ZombieCherryTexState > 0) {
		any |= zombiesCherryFxAny();
	}

	// and the power-ups' glow
	any |= zombiesRenderPowerupGlows(NULL, NULL, NULL, NULL, NULL, false) != NULL;

	if (!any) {
		return gdl;
	}

	if (!g_ZombieGlowTexMade) {
		zombiesGlowTexMake();
	}

	if (g_ZombieRayRingState == 0) {
		zombiesRayRingLoad();
	}

	// the camera's own right and up, in the world, for the flat sprites
	for (i = 0; i < 3; i++) {
		right[i] = cam->m[i][0];
		up[i] = cam->m[i][1];
	}

	{
		f32 lr = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
		f32 lu = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);

		for (i = 0; i < 3; i++) {
			right[i] /= lr > 0 ? lr : 1;
			up[i] /= lu > 0 ? lu : 1;
		}
	}

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
	gSPClearGeometryMode(gdl++, G_LIGHTING | G_TEXTURE_GEN | G_FOG | G_CULL_BOTH);
	gSPSetGeometryMode(gdl++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
	gSPSetExtraGeometryModeEXT(gdl++, G_ADDITIVE_EXT);

	if (g_ZombieCherryTexState > 0) {
		gdl = zombiesRenderCherryFx(gdl, cam, campos, right, up);
	}

	gdl = zombiesRayTex(gdl, false);
	gdl = zombiesRenderPowerupGlows(gdl, cam, campos, right, up, true);

	// the bolts: a streak along the way and a bright head
	for (i = 0; i < ZOMBIE_RAYFX_MAX; i++) {
		struct zombierayfx *fx = &g_ZombieRayFx[i];
		struct coord head;
		struct coord mid;
		f32 tocam[3];
		f32 side[3];
		f32 along[3];
		f32 a[3];
		f32 b[3];
		f32 tail;
		f32 len;
		s32 k;

		if (!fx->on || fx->age60 >= 0) {
			continue;
		}

		head.x = fx->from.x + fx->dir.x * fx->dist;
		head.y = fx->from.y + fx->dir.y * fx->dist;
		head.z = fx->from.z + fx->dir.z * fx->dist;
		tail = fx->dist < ZOMBIE_RAYFX_TAIL ? fx->dist : ZOMBIE_RAYFX_TAIL;

		mid.x = head.x - fx->dir.x * tail / 2;
		mid.y = head.y - fx->dir.y * tail / 2;
		mid.z = head.z - fx->dir.z * tail / 2;

		tocam[0] = campos->x - mid.x;
		tocam[1] = campos->y - mid.y;
		tocam[2] = campos->z - mid.z;
		along[0] = fx->dir.x;
		along[1] = fx->dir.y;
		along[2] = fx->dir.z;
		side[0] = along[1] * tocam[2] - along[2] * tocam[1];
		side[1] = along[2] * tocam[0] - along[0] * tocam[2];
		side[2] = along[0] * tocam[1] - along[1] * tocam[0];
		len = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);

		if (len > 0 && tail > 1) {
			for (k = 0; k < 3; k++) {
				a[k] = along[k] * tail / 2;
				b[k] = side[k] / len * ZOMBIE_RAYFX_WIDTH;
			}

			gDPSetPrimColor(gdl++, 0, 0, fx->red ? 0xff : 0x40, fx->red ? 0x30 : 0xff, fx->red ? 0x20 : 0x30, 0xe0);
			gdl = zombiesRayQuad(gdl, cam, &mid, a, b);
		}

		for (k = 0; k < 3; k++) {
			a[k] = right[k] * ZOMBIE_RAYFX_HEAD;
			b[k] = up[k] * ZOMBIE_RAYFX_HEAD;
		}

		gDPSetPrimColor(gdl++, 0, 0, fx->red ? 0xff : 0xa0, fx->red ? 0x90 : 0xff, fx->red ? 0x80 : 0x90, 0xff);
		gdl = zombiesRayQuad(gdl, cam, &head, a, b);
	}

	// where they landed: a flash, then the ring
	for (i = 0; i < ZOMBIE_RAYFX_MAX; i++) {
		struct zombierayfx *fx = &g_ZombieRayFx[i];
		struct coord at;
		f32 f;
		f32 size;
		f32 a[3];
		f32 b[3];
		s32 k;

		if (!fx->on || fx->age60 < 0) {
			continue;
		}

		// a little back along the way, off the wall it hit
		at.x = fx->from.x + fx->dir.x * (fx->maxdist - 6);
		at.y = fx->from.y + fx->dir.y * (fx->maxdist - 6);
		at.z = fx->from.z + fx->dir.z * (fx->maxdist - 6);

		if (fx->age60 < ZOMBIE_RAYFX_FLASH60) {
			f = 1 - fx->age60 / ZOMBIE_RAYFX_FLASH60;
			size = ZOMBIE_RAYFX_FLASH * (0.6f + 0.4f * f);

			for (k = 0; k < 3; k++) {
				a[k] = right[k] * size;
				b[k] = up[k] * size;
			}

			gdl = zombiesRayTex(gdl, false);
			gDPSetPrimColor(gdl++, 0, 0, fx->red ? 0xff : 0x60, fx->red ? 0x50 : 0xff, fx->red ? 0x40 : 0x50, (u8)(0xff * f));
			gdl = zombiesRayQuad(gdl, cam, &at, a, b);
		}

		f = fx->age60 / ZOMBIE_RAYFX_RING60;
		size = ZOMBIE_RAYFX_RING * (0.25f + 0.75f * f);

		for (k = 0; k < 3; k++) {
			a[k] = right[k] * size;
			b[k] = up[k] * size;
		}

		gdl = zombiesRayTex(gdl, true);
		gDPSetPrimColor(gdl++, 0, 0, fx->red ? 0xff : 0x50, fx->red ? 0x40 : 0xff, fx->red ? 0x30 : 0x40, (u8)(0xff * (1 - f)));
		gdl = zombiesRayQuad(gdl, cam, &at, a, b);
	}

	// the next quads are the glow texture's
	g_ZombieQuadTex = ZOMBIE_GLOW_TEX;

	gDPPipeSync(gdl++);
	gSPClearExtraGeometryModeEXT(gdl++, G_ADDITIVE_EXT);
	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);

	return gdl;
}


static bool zombiesRenderBarrier(struct modelrenderdata *renderdata, struct defaultobj *obj);
static bool zombiesRenderZmGun(struct modelrenderdata *renderdata, struct defaultobj *obj);
static bool zombiesRenderPowerup(struct modelrenderdata *renderdata, struct defaultobj *obj);

/**
 * propobj.c's objRender(), for each object before it draws its model: true
 * when this is a machine with a model of its own, drawn here instead.
 */
/**
 * Quick Revive's machine as it goes: risen so far, and rocked about its own
 * side-to-side axis through its foot (vibrate's swing); nothing while it stays
 */
static void zombiesQrLeavePose(Mtxf *world, const struct coord *foot, const f32 side[3])
{
	const s32 t = g_ZombieQrLeave60 - ZOMBIE_QR_LEAVE_WAIT60;
	f32 a[3];
	f32 r[3][3];
	f32 m[3][4];
	f32 len;
	f32 ang;
	f32 c;
	f32 s;
	s32 i;
	s32 j;

	if (t <= 0) {
		return;
	}

	len = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);

	if (len <= 0) {
		return;
	}

	for (i = 0; i < 3; i++) {
		a[i] = side[i] / len;
	}

	ang = ZOMBIE_QR_LEAVE_ROCK * (M_BADPI / 180.0f) * sinf(t * (2 * M_BADPI) / ZOMBIE_QR_LEAVE_SWING);
	c = cosf(ang);
	s = sinf(ang);

	// Rodrigues: r[row][col], turning about a
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			r[i][j] = (1 - c) * a[i] * a[j] + (i == j ? c : 0);
		}
	}

	r[0][1] -= s * a[2]; r[0][2] += s * a[1];
	r[1][0] += s * a[2]; r[1][2] -= s * a[0];
	r[2][0] -= s * a[1]; r[2][1] += s * a[0];

	// world's columns (m[col][row]) turned, its place turned about the foot
	for (j = 0; j < 4; j++) {
		const f32 v[3] = {
			world->m[j][0] - (j == 3 ? foot->x : 0),
			world->m[j][1] - (j == 3 ? foot->y : 0),
			world->m[j][2] - (j == 3 ? foot->z : 0),
		};

		for (i = 0; i < 3; i++) {
			m[i][j] = r[i][0] * v[0] + r[i][1] * v[1] + r[i][2] * v[2];
		}
	}

	for (j = 0; j < 4; j++) {
		for (i = 0; i < 3; i++) {
			world->m[j][i] = m[i][j] + (j == 3 ? (&foot->x)[i] : 0);
		}
	}

	world->m[3][1] += ZOMBIE_QR_LEAVE_RISE * (t < ZOMBIE_QR_LEAVE_RISE60 ? t : ZOMBIE_QR_LEAVE_RISE60) / ZOMBIE_QR_LEAVE_RISE60;
}

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

	// a barrier: its boards, drawn here
	if (zombiesRenderBarrier(renderdata, obj)) {
		return true;
	}

	// one of Zombies' own guns, as Black Ops II's world model
	if (zombiesRenderZmGun(renderdata, obj)) {
		return true;
	}

	// a power-up, as Black Ops II's own model
	if (zombiesRenderPowerup(renderdata, obj)) {
		return true;
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

	// the solid in the opaque pass, the glass in the translucent (a glow
	// round it there made the machines look like lamps: gone)
	if (!(renderdata->flags & MODELRENDERFLAG_OPA) && zm->xlugdl == NULL) {
		return true;
	}

	if (renderdata->flags & MODELRENDERFLAG_OPA) {
		zombiesMeshSpin(zm);
	}

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

	// Quick Revive alone, going (zombiesTickQrMachine())
	if (id == ZOMBIEPERK_QUICKREVIVE) {
		zombiesQrLeavePose(&world, &obj->prop->pos, obj->realrot[0]);
	}

	mtx = gfxAllocateMatrix();
	mtx4MultMtx4(camGetWorldToScreenMtxf(), &world, mtx);

	gDPPipeSync(renderdata->gdl++);
	gDPSetCycleType(renderdata->gdl++, G_CYC_2CYCLE);

	if (renderdata->flags & MODELRENDERFLAG_OPA) {
		gDPSetRenderMode(renderdata->gdl++, G_RM_PASS,
				renderdata->zbufferenabled ? G_RM_AA_ZB_OPA_SURF2 : G_RM_AA_OPA_SURF2);
	} else {
		gDPSetRenderMode(renderdata->gdl++, G_RM_PASS,
				renderdata->zbufferenabled ? G_RM_AA_ZB_XLU_SURF2 : G_RM_AA_XLU_SURF2);
	}

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
	gSPDisplayList(renderdata->gdl++, (renderdata->flags & MODELRENDERFLAG_OPA) ? zm->gdl : zm->xlugdl);
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

/**
 * A grenade's throw in the player's own hands (zombiesTickThrow()): Black Ops
 * II's pin pulled and throw, retargeted onto the character's hands as the
 * drink is, and the frag in them, from tools/zombies/vmbake.py's grenade.zpd
 * (its last header field the 60th the throw starts at) and frag_body.obj and
 * frag_top.obj (the grenade and its pin). Without them the gun still goes
 * down for the throw, and nothing is drawn in its place.
 */
static struct {
	bool tried;
	s32 ticks;
	s32 throwstart;
	const f32 *mtx;
} g_ZombieThrowPose;

static struct zombiemesh g_ZombieFragMeshes[ZOMBIE_DRINK_PARTS];

static bool zombiesThrowPoseLoad(void)
{
	const char *rel = "$E/zombies/viewmodel/grenade.zpd";
	u8 *data;
	u32 size = 0;
	u16 hdr[4];

	if (g_ZombieThrowPose.tried) {
		return g_ZombieThrowPose.mtx != NULL;
	}

	g_ZombieThrowPose.tried = true;

	if (fsFileSize(fsFullPath(rel)) <= 0) {
		return false;
	}

	data = fsFileLoad(rel, &size);

	if (!data || size < 12 || memcmp(data, "ZPD1", 4) != 0) {
		return false;
	}

	memcpy(hdr, data + 4, sizeof(hdr));

	if (hdr[1] != ZOMBIE_DRINK_JOINTS || hdr[2] != ZOMBIE_DRINK_PARTS || hdr[0] == 0 || hdr[3] == 0 || hdr[3] >= hdr[0]
			|| size < 12 + (u32)hdr[0] * (ZOMBIE_DRINK_JOINTS + ZOMBIE_DRINK_PARTS) * 12 * sizeof(f32)) {
		sysLogPrintf(LOG_WARNING, "zombies: %s is not vmbake.py's", rel);
		return false;
	}

	g_ZombieThrowPose.ticks = hdr[0];
	g_ZombieThrowPose.throwstart = hdr[3];
	g_ZombieThrowPose.mtx = (const f32 *)(data + 12);

	sysLogPrintf(LOG_NOTE, "zombies: the grenade thrown in the player's own hands, %d 60ths", hdr[0]);

	return true;
}

static struct zombiemesh *zombiesFragMesh(s32 part)
{
	struct zombiemesh *zm = &g_ZombieFragMeshes[part];

	if (!zm->tried) {
		zombiesMeshBuildFrom(zm, part ? "$E/zombies/viewmodel/frag_top.obj" : "$E/zombies/viewmodel/frag_body.obj",
				part ? "the frag's pin" : "the frag");
	}

	return zm->gdl ? zm : NULL;
}

// The grenade.zpd 60th a throw is at, -1 for none drawn; *grenade whether
// the frag is still in the hand
static s32 zombiesThrowTick(s32 playernum, bool *grenade)
{
	const s32 t = g_ZombieThrow60[playernum];
	s32 tick;

	*grenade = true;

	switch (g_ZombieThrowStage[playernum]) {
	case ZTHROW_PIN:
		tick = t;
		break;
	case ZTHROW_HOLD:
		tick = g_ZombieThrowPose.throwstart - 1;
		break;
	case ZTHROW_OUT:
		tick = g_ZombieThrowPose.throwstart + t;
		*grenade = t < ZOMBIE_THROW_RELEASE60;
		break;
	default:
		return -1;
	}

	if (tick >= g_ZombieThrowPose.ticks) {
		tick = g_ZombieThrowPose.ticks - 1;
	}

	if (g_ZombieThrowStage[playernum] == ZTHROW_PIN && tick >= g_ZombieThrowPose.throwstart) {
		tick = g_ZombieThrowPose.throwstart - 1;
	}

	return tick;
}

static Gfx *zombiesRenderThrowViewmodel(Gfx *gdl)
{
	const s32 playernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;
	struct model *handmodel;
	struct modelrenderdata renderdata = {NULL, true, 3};
	const f32 *frame;
	Mtxf *matrices;
	Mtxf *saved;
	bool grenade;
	s32 tick;
	s32 n;
	s32 j;

	if (!zombiesThrowPoseLoad()) {
		return gdl;
	}

	tick = zombiesThrowTick(playernum, &grenade);
	handmodel = zombiesDrinkHandModel(player);

	if (tick < 0 || handmodel == NULL) {
		return gdl;
	}

	frame = g_ZombieThrowPose.mtx + tick * (ZOMBIE_DRINK_JOINTS + ZOMBIE_DRINK_PARTS) * 12;
	n = handmodel->definition->nummatrices;

	if (n < ZOMBIE_DRINK_JOINTS) {
		n = ZOMBIE_DRINK_JOINTS;
	}

	if (!gfxHasVtxSpace(n * sizeof(Mtxf))) {
		return gdl;
	}

	matrices = gfxAllocate(n * sizeof(Mtxf));

	// both hands: the left pulls the pin
	for (j = 0; j < n; j++) {
		zombiesDrinkMtx(frame + (j < ZOMBIE_DRINK_JOINTS ? j : 0) * 12, NULL, &matrices[j]);
	}

	gSPPerspNormalize(gdl++, mtx00016dcc(0, 300));

	renderdata.gdl = gdl;
	renderdata.unk30 = 4;
	renderdata.envcolour = player->gunshadecol[0] << 24 | player->gunshadecol[1] << 16
		| player->gunshadecol[2] << 8 | player->gunshadecol[3];
	renderdata.zbufferenabled = true;

	mtx00016760();
	handtintTick();

	saved = handmodel->matrices;
	handmodel->matrices = matrices;
	modelUpdateRelations(handmodel);

	// the left hand switched on (a gun's hand model holds it off): it pulls
	// the pin, and is put back as it was
	{
		struct modelnode *node = modelGetPart(handmodel->definition, MODELPART_HAND_LEFT);
		struct modelrwdata_toggle *left = NULL;
		bool wasvisible = false;

		if (node && (node->type & 0xff) == MODELNODETYPE_TOGGLE) {
			left = modelGetNodeRwData(handmodel, node);
		}

		if (left) {
			wasvisible = left->visible;
			left->visible = true;
		}

		xblaMeshPassBegin();
		modelRender(&renderdata, handmodel);
		xblaMeshPassBegin();

		if (left) {
			left->visible = wasvisible;
		}
	}

	handmodel->matrices = saved;
	gdl = renderdata.gdl;

	if (grenade) {
		for (j = 0; j < ZOMBIE_DRINK_PARTS; j++) {
			struct zombiemesh *zm = zombiesFragMesh(j);
			Mtxf *mtx;

			if (!zm) {
				continue;
			}

			mtx = gfxAllocateMatrix();
			zombiesDrinkMtx(frame + (ZOMBIE_DRINK_JOINTS + j) * 12, g_ZombieBottleObjAxes, mtx);
			gdl = zombiesBottleDraw(gdl, zm, mtx, 0xff, 0xff, 0xff);
		}
	}

	mtxF2LBulk(matrices, n);
	mtx00016784();

	gSPPerspNormalize(gdl++, viGetPerspScale());

	return gdl;
}

f32 zombiesSprintGunFrac(void);
f32 zombiesGunLowerFrac(void);
static f32 zombiesGameOverGunLower(void);
f32 zombiesHandRate(s32 handstate);

/**
 * Zombies' own guns held in the player's own hands (constants.h,
 * WEAPON_ZM_FIRST): Black Ops II's viewmodel - its arms' animations
 * retargeted onto the character's hands, as the drink is, and the gun's
 * moving parts - drawn in place of the host gun's model (bgunRender() asks
 * zombiesGunViewmodelOwns()). tools/zombies/vmbake.py's gun mode bakes them
 * into the build's zombies/viewmodel/: <name>.parts (the parts' bones),
 * <name>_<part>.obj, and <name>_<clip>.zpd for each clip below. Without them
 * the host's model is drawn as it is.
 *
 * The clip follows the hand: the pull-out as the gun comes in, the fire clip
 * from each round (a magazine going down), the reload's for as long as the
 * host's reload lasts (ZOMBIE_VMGUN_PDRELOAD, Speed Cola's rate and all), the
 * sprint's in, loop and out, and the idle's loop otherwise; a change of clip
 * crossfades. The gun goes down for a drink, a throw or the knife as the host
 * gun would (zombiesGunLowerFrac()).
 */
enum {
	ZVM_IDLE,
	ZVM_FIRE,
	ZVM_RELOAD,
	ZVM_PULLOUT,
	ZVM_SPRINTIN,
	ZVM_SPRINTLOOP,
	ZVM_SPRINTOUT,
	ZVM_COUNT
};

static const char *const g_ZombieVmClipNames[ZVM_COUNT] = {
	"idle", "fire", "reload", "pullout", "sprint_in", "sprint_loop", "sprint_out",
};

#define ZOMBIE_VMGUN_PARTS    12
#define ZOMBIE_VMGUN_FLOATS   ((ZOMBIE_DRINK_JOINTS + ZOMBIE_VMGUN_PARTS) * 12)
#define ZOMBIE_VMGUN_BLEND60  6     // a change of clip crossfades this long
#define ZOMBIE_VMGUN_PDRELOAD 128.0f // the Phoenix's reload, the Ray Gun's host's: the reload clip is played over it
#define ZOMBIE_VMGUN_LOWER    40.0f  // how far down the gun goes for a drink or a throw, at its fullest

struct zombievmgun {
	s32 weaponnum;
	const char *name;
	f32 sight[3];  // its sight's middle on the gun's own bone (j_gun), in its OBJ's tenths of an inch
	bool tried;
	bool ok;
	s32 numparts;
	s32 sightpart; // which part is j_gun, -1 none
	u32 capsules;  // the parts that are its vials (tag_battery_*), a bit each: tinted
	u32 dials;     // ... and its dial (tag_knob): no camo on either, Pack-a-Punched
	struct zombiemesh meshes[ZOMBIE_VMGUN_PARTS];
	s32 ticks[ZVM_COUNT];
	const f32 *mtx[ZVM_COUNT];
};

static struct zombievmgun g_ZombieVmGuns[] = {
	// the ring sight's middle (t6_wpn_zmb_raygun_view: the hoop 1.9 inches
	// across standing over the back of the gun)
	{ WEAPON_ZM_RAYGUN, "raygun", { -47.3f, 0, 46.0f } },
};

static struct {
	s32 weaponnum;
	s32 clip;
	f32 t;
	s32 ammo;
	bool reloading;
	bool sprinting;
	f32 blend;                         // what of the last pose is left, 1 to 0
	f32 from[ZOMBIE_VMGUN_FLOATS];     // the last pose, as the clip changed
	f32 now[ZOMBIE_VMGUN_FLOATS];      // the pose last drawn
} g_ZombieVm[MAX_PLAYERS];

static struct zombievmgun *zombiesVmGun(s32 weaponnum)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_ZombieVmGuns); i++) {
		if (g_ZombieVmGuns[i].weaponnum == weaponnum) {
			return &g_ZombieVmGuns[i];
		}
	}

	return NULL;
}

// A gun's files, read the first time it is wanted; false when not all there
static bool zombiesVmGunLoad(struct zombievmgun *gun)
{
	char rel[128];
	char *text;
	u32 size = 0;
	s32 c;

	if (gun->tried) {
		return gun->ok;
	}

	gun->tried = true;
	gun->sightpart = -1;

	snprintf(rel, sizeof(rel), "$E/zombies/viewmodel/%s.parts", gun->name);

	if (fsFileSize(fsFullPath(rel)) <= 0 || (text = fsFileLoad(rel, &size)) == NULL) {
		return false;
	}

	// a part's OBJ for each line
	{
		char *p = text;

		while (p < text + size && gun->numparts < ZOMBIE_VMGUN_PARTS) {
			char bone[48];
			s32 n = 0;

			while (p < text + size && (*p == '\n' || *p == '\r')) {
				p++;
			}

			while (p < text + size && *p != '\n' && *p != '\r' && n < (s32)sizeof(bone) - 1) {
				bone[n++] = *p++;
			}

			bone[n] = '\0';

			while (p < text + size && *p != '\n') {
				p++;
			}

			if (n == 0) {
				continue;
			}

			snprintf(rel, sizeof(rel), "$E/zombies/viewmodel/%s_%s.obj", gun->name, bone);
			zombiesMeshBuildFrom(&gun->meshes[gun->numparts], rel, bone);

			if (strcmp(bone, "j_gun") == 0) {
				gun->sightpart = gun->numparts;
			}

			if (strncmp(bone, "tag_battery", 11) == 0) {
				gun->capsules |= 1u << gun->numparts;
			}

			if (strcmp(bone, "tag_knob") == 0) {
				gun->dials |= 1u << gun->numparts;
			}

			gun->numparts++;
		}
	}

	sysMemFree(text);

	for (c = 0; c < ZVM_COUNT; c++) {
		u8 *data;
		u16 hdr[4];

		snprintf(rel, sizeof(rel), "$E/zombies/viewmodel/%s_%s.zpd", gun->name, g_ZombieVmClipNames[c]);
		size = 0;

		if (fsFileSize(fsFullPath(rel)) <= 0 || (data = fsFileLoad(rel, &size)) == NULL || size < 12
				|| memcmp(data, "ZPD1", 4) != 0) {
			continue;
		}

		memcpy(hdr, data + 4, sizeof(hdr));

		if (hdr[1] != ZOMBIE_DRINK_JOINTS || hdr[2] != gun->numparts || hdr[0] == 0
				|| size < 12 + (u32)hdr[0] * (ZOMBIE_DRINK_JOINTS + hdr[2]) * 12 * sizeof(f32)) {
			sysLogPrintf(LOG_WARNING, "zombies: %s is not vmbake.py's", rel);
			continue;
		}

		gun->ticks[c] = hdr[0];
		gun->mtx[c] = (const f32 *)(data + 12);
	}

	gun->ok = gun->numparts > 0 && gun->mtx[ZVM_IDLE] != NULL;

	sysLogPrintf(LOG_NOTE, "zombies: %s in the player's own hands: %d parts, %s", gun->name, gun->numparts,
			gun->ok ? "its clips read" : "no idle clip, the host's model drawn");

	return gun->ok;
}

// bgunRender(): whether the current player's own view draws this hand's gun
// itself (Zombies' own guns), so the host's model is not drawn
bool zombiesGunViewmodelOwns(s32 handnum)
{
	struct player *player = g_Vars.currentplayer;
	struct zombievmgun *gun;

	if (!zombiesIsActive() || !player) {
		return false;
	}

	gun = zombiesVmGun(player->hands[HAND_RIGHT].gset.weaponnum);

	return gun && zombiesVmGunLoad(gun);
}

// The clip a player's hands are in now, moved on by a frame's 60ths
static void zombiesVmGunTick(s32 playernum, struct zombievmgun *gun, s32 weaponnum)
{
	struct player *player = g_Vars.players[playernum];
	struct hand *hand = &player->hands[HAND_RIGHT];
	const f32 dt = g_Vars.lvupdate60freal;
	const bool reloading = hand->state == HANDSTATE_RELOAD;
	const bool sprinting = zombiesSprintGunFrac() > 0.5f;
	s32 ammo = hand->loadedammo[0];
	s32 want = -1;

	if (g_ZombieVm[playernum].weaponnum != weaponnum) {
		// a gun come in: its pull-out, from nothing to blend from
		g_ZombieVm[playernum].weaponnum = weaponnum;
		g_ZombieVm[playernum].clip = gun->mtx[ZVM_PULLOUT] ? ZVM_PULLOUT : ZVM_IDLE;
		g_ZombieVm[playernum].t = 0;
		g_ZombieVm[playernum].ammo = ammo;
		g_ZombieVm[playernum].reloading = false;
		g_ZombieVm[playernum].sprinting = false;
		g_ZombieVm[playernum].blend = 0;
		return;
	}

	if (reloading && !g_ZombieVm[playernum].reloading) {
		want = ZVM_RELOAD;
	} else if (!reloading && ammo < g_ZombieVm[playernum].ammo) {
		want = ZVM_FIRE;
	} else if (sprinting && !g_ZombieVm[playernum].sprinting && !reloading) {
		want = gun->mtx[ZVM_SPRINTIN] ? ZVM_SPRINTIN : ZVM_SPRINTLOOP;
	} else if (!sprinting && g_ZombieVm[playernum].sprinting && !reloading) {
		want = ZVM_SPRINTOUT;
	}

	g_ZombieVm[playernum].reloading = reloading;
	g_ZombieVm[playernum].sprinting = sprinting;
	g_ZombieVm[playernum].ammo = ammo;

	if (want >= 0 && gun->mtx[want] == NULL) {
		want = ZVM_IDLE;
	}

	if (want >= 0) {
		memcpy(g_ZombieVm[playernum].from, g_ZombieVm[playernum].now, sizeof(g_ZombieVm[playernum].from));
		g_ZombieVm[playernum].blend = 1;
		g_ZombieVm[playernum].clip = want;
		g_ZombieVm[playernum].t = 0;
		return;
	}

	// on through the clip: the reload's over the host's reload, Speed Cola's
	// rate and all
	if (g_ZombieVm[playernum].clip == ZVM_RELOAD) {
		// Black Ops II's own reload sounds, as the clip passes each: the
		// lid open, the vial out, the new one in, the lid shut (zmsfx.c)
		static const f32 at[4] = { 0.04f, 0.26f, 0.52f, 0.80f };
		static const s32 slot[4] = { ZMSFX_RAYGUN_RELOAD_OPEN, ZMSFX_RAYGUN_RELOAD_OUT, ZMSFX_RAYGUN_RELOAD_IN, ZMSFX_RAYGUN_RELOAD_CLOSE };
		const f32 was = g_ZombieVm[playernum].t / gun->ticks[ZVM_RELOAD];
		f32 now;
		s32 k;

		g_ZombieVm[playernum].t += dt * zombiesHandRate(HANDSTATE_RELOAD) * gun->ticks[ZVM_RELOAD] / ZOMBIE_VMGUN_PDRELOAD;
		now = g_ZombieVm[playernum].t / gun->ticks[ZVM_RELOAD];

		for (k = 0; k < 4; k++) {
			if (weaponnum == WEAPON_ZM_RAYGUN && was < at[k] && now >= at[k] && zmSfxSound(slot[k])) {
				zombiesPlaySoundHere(zmSfxSound(slot[k]), 1.0f);
			}
		}
	} else {
		g_ZombieVm[playernum].t += dt;
	}

	if (g_ZombieVm[playernum].blend > 0) {
		g_ZombieVm[playernum].blend -= dt / ZOMBIE_VMGUN_BLEND60;
	}

	// a clip run out: the sprint's loop or the idle's, carried on
	if (g_ZombieVm[playernum].t >= gun->ticks[g_ZombieVm[playernum].clip] - 1) {
		s32 clip = g_ZombieVm[playernum].clip;
		s32 next = clip == ZVM_SPRINTIN || clip == ZVM_SPRINTLOOP ? ZVM_SPRINTLOOP : ZVM_IDLE;

		if (clip == ZVM_RELOAD && reloading) {
			// the host's reload still going: hold the clip's end
			g_ZombieVm[playernum].t = gun->ticks[clip] - 1;
			return;
		}

		if (next == clip) {
			g_ZombieVm[playernum].t -= gun->ticks[clip] - 1;
			return;
		}

		memcpy(g_ZombieVm[playernum].from, g_ZombieVm[playernum].now, sizeof(g_ZombieVm[playernum].from));
		g_ZombieVm[playernum].blend = 1;
		g_ZombieVm[playernum].clip = gun->mtx[next] ? next : ZVM_IDLE;
		g_ZombieVm[playernum].t = 0;
	}
}

/**
 * Down the sights (COD Style Aiming's codaimfrac, or the sights' own aim):
 * the whole viewmodel turned so the gun points straight ahead and moved so
 * its sight's middle is on the eye's line, as far as the aim has come, in
 * Call of Duty's view space (x forward, y left, z up) before
 * zombiesDrinkMtx(). The forward distance is the sight's own.
 */
static void zombiesVmGunAds(struct zombievmgun *gun, f32 *pose, f32 frac)
{
	const f32 *g;
	f32 sight[3];
	f32 fwd[3];
	f32 axis[3];
	f32 r[3][3];
	f32 len;
	f32 angle;
	f32 c;
	f32 sn;
	s32 floats;
	s32 i;
	s32 k;

	if (frac <= 0 || gun->sightpart < 0) {
		return;
	}

	frac = frac > 1 ? 1 : frac;
	g = pose + (ZOMBIE_DRINK_JOINTS + gun->sightpart) * 12;

	for (i = 0; i < 3; i++) {
		sight[i] = g[i * 4 + 0] * gun->sight[0] + g[i * 4 + 1] * gun->sight[1] + g[i * 4 + 2] * gun->sight[2] + g[i * 4 + 3];
		fwd[i] = g[i * 4 + 0];
	}

	// the turn taking the gun's own forward to straight ahead, about its
	// sight, as much of it as the aim has come
	len = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);

	if (len <= 0) {
		return;
	}

	fwd[0] /= len;
	fwd[1] /= len;
	fwd[2] /= len;

	// fwd x (1, 0, 0)
	axis[0] = 0;
	axis[1] = fwd[2];
	axis[2] = -fwd[1];
	sn = sqrtf(axis[1] * axis[1] + axis[2] * axis[2]);
	angle = atan2f(sn, fwd[0]) * frac;

	if (sn > 1e-5f) {
		axis[1] /= sn;
		axis[2] /= sn;
	} else {
		axis[1] = 0;
		axis[2] = 1;
		angle = 0;
	}

	c = cosf(angle);
	sn = sinf(angle);

	for (i = 0; i < 3; i++) {
		for (k = 0; k < 3; k++) {
			r[i][k] = (i == k ? c : 0) + (1 - c) * axis[i] * axis[k];
		}
	}

	r[0][1] -= sn * axis[2];
	r[0][2] += sn * axis[1];
	r[1][0] += sn * axis[2];
	r[1][2] -= sn * axis[0];
	r[2][0] -= sn * axis[1];
	r[2][1] += sn * axis[0];

	floats = (ZOMBIE_DRINK_JOINTS + gun->numparts) * 12;

	for (k = 0; k < floats; k += 12) {
		f32 *m = pose + k;
		f32 out[12];

		for (i = 0; i < 3; i++) {
			s32 col;

			for (col = 0; col < 3; col++) {
				out[i * 4 + col] = r[i][0] * m[0 * 4 + col] + r[i][1] * m[1 * 4 + col] + r[i][2] * m[2 * 4 + col];
			}

			// turned about the sight, then the sight onto the eye's line:
			// forward as far as it was, across and up by the aim
			out[i * 4 + 3] = r[i][0] * (m[3] - sight[0]) + r[i][1] * (m[7] - sight[1]) + r[i][2] * (m[11] - sight[2]);
		}

		out[3] += sight[0];
		out[7] += sight[1] * (1 - frac);
		out[11] += sight[2] * (1 - frac);

		memcpy(m, out, sizeof(out));
	}
}

static bool zombiesGunIsPapped(s32 playernum, s32 weaponnum);

/**
 * The gun and the hands a little further off than the bake has them: it
 * filled too much of the view. Moved out along the line from the eye, so
 * the gun is the same shape, smaller in the view and where it was, and down
 * the sights its ring stays on the eye's line.
 */
#define ZOMBIE_VMGUN_FAR 1.2f

static void zombiesVmGunFar(Mtxf *m)
{
	m->m[3][0] *= ZOMBIE_VMGUN_FAR;
	m->m[3][1] *= ZOMBIE_VMGUN_FAR;
	m->m[3][2] *= ZOMBIE_VMGUN_FAR;
}

static Gfx *zombiesCamoAt(Gfx *gdl);

Gfx *zombiesRenderGunViewmodel(Gfx *gdl)
{
	const s32 playernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;
	bool papped;
	struct zombievmgun *gun;
	struct model *handmodel;
	struct modelrenderdata renderdata = {NULL, true, 3};
	f32 *pose;
	const f32 *frame;
	Mtxf *matrices;
	Mtxf *saved;
	f32 lower;
	s32 weaponnum;
	s32 floats;
	s32 tick;
	s32 n;
	s32 j;
	s32 k;

	if (!zombiesIsActive() || !player || playernum < 0 || playernum >= MAX_PLAYERS) {
		return gdl;
	}

	weaponnum = player->hands[HAND_RIGHT].gset.weaponnum;
	gun = zombiesVmGun(weaponnum);

	if (!gun || !zombiesVmGunLoad(gun)) {
		g_ZombieVm[playernum].weaponnum = WEAPON_NONE;
		return gdl;
	}

	// only where bgunRender() draws, and not while the drink or a throw has
	// the hands
	if (player->thirdpersondist > 0 || player->visionmode == VISIONMODE_XRAY || geWatchHidesGun()
			|| zombiesDrinkViewmodelActive(playernum) || g_ZombieThrowStage[playernum] != ZTHROW_NONE) {
		return gdl;
	}

	zombiesVmGunTick(playernum, gun, weaponnum);

	lower = zombiesGunLowerFrac();

	if (lower >= 1) {
		return gdl;
	}

	handmodel = zombiesDrinkHandModel(player);

	if (handmodel == NULL) {
		return gdl;
	}

	// the clip's frame, crossfaded from the last clip's pose
	floats = (ZOMBIE_DRINK_JOINTS + gun->numparts) * 12;
	tick = (s32)g_ZombieVm[playernum].t;
	tick = tick < 0 ? 0 : tick >= gun->ticks[g_ZombieVm[playernum].clip] ? gun->ticks[g_ZombieVm[playernum].clip] - 1 : tick;
	frame = gun->mtx[g_ZombieVm[playernum].clip] + tick * floats;
	pose = g_ZombieVm[playernum].now;

	for (k = 0; k < floats; k++) {
		const f32 b = g_ZombieVm[playernum].blend;

		pose[k] = b > 0 ? frame[k] + (g_ZombieVm[playernum].from[k] - frame[k]) * b : frame[k];
	}

	n = handmodel->definition->nummatrices;

	if (n < ZOMBIE_DRINK_JOINTS) {
		n = ZOMBIE_DRINK_JOINTS;
	}

	if (!gfxHasVtxSpace(n * sizeof(Mtxf))) {
		return gdl;
	}

	// down the sights, the sight on the eye's line (the pose kept for the
	// next clip's crossfade is the hip's)
	{
		static f32 aimed[ZOMBIE_VMGUN_FLOATS];
		const f32 frac = player->insightaimmode && player->codaimfrac <= 0 ? 1 : player->codaimfrac;

		if (frac > 0) {
			memcpy(aimed, pose, floats * sizeof(f32));
			zombiesVmGunAds(gun, aimed, frac);
			pose = aimed;
		}
	}

	matrices = gfxAllocate(n * sizeof(Mtxf));

	for (j = 0; j < n; j++) {
		zombiesDrinkMtx(pose + (j < ZOMBIE_DRINK_JOINTS ? j : 0) * 12, NULL, &matrices[j]);
		zombiesVmGunFar(&matrices[j]);
		matrices[j].m[3][1] -= lower * ZOMBIE_VMGUN_LOWER;
	}

	gSPPerspNormalize(gdl++, mtx00016dcc(0, 300));

	renderdata.gdl = gdl;
	renderdata.unk30 = 4;
	renderdata.envcolour = player->gunshadecol[0] << 24 | player->gunshadecol[1] << 16
		| player->gunshadecol[2] << 8 | player->gunshadecol[3];
	renderdata.zbufferenabled = true;

	mtx00016760();
	handtintTick();

	saved = handmodel->matrices;
	handmodel->matrices = matrices;
	modelUpdateRelations(handmodel);

	// both hands: Black Ops II's left one steadies the gun
	{
		struct modelnode *node = modelGetPart(handmodel->definition, MODELPART_HAND_LEFT);
		struct modelrwdata_toggle *left = NULL;
		bool wasvisible = false;

		if (node && (node->type & 0xff) == MODELNODETYPE_TOGGLE) {
			left = modelGetNodeRwData(handmodel, node);
		}

		if (left) {
			wasvisible = left->visible;
			left->visible = true;
		}

		xblaMeshPassBegin();
		modelRender(&renderdata, handmodel);
		xblaMeshPassBegin();

		if (left) {
			left->visible = wasvisible;
		}
	}

	handmodel->matrices = saved;
	gdl = renderdata.gdl;

	papped = zombiesGunIsPapped(playernum, weaponnum);

	// the coil's camo, this frame's and the gun's (green, or red upgraded)
	zombiesRayGlowWrite(papped);

	// the gun's parts, each as its bone has it
	for (j = 0; j < gun->numparts; j++) {
		struct zombiemesh *zm = &gun->meshes[j];
		Mtxf *mtx;

		if (!zm->gdl) {
			continue;
		}

		mtx = gfxAllocateMatrix();
		zombiesDrinkMtx(pose + (ZOMBIE_DRINK_JOINTS + j) * 12, g_ZombieBottleObjAxes, mtx);
		zombiesVmGunFar(mtx);
		mtx->m[3][1] -= lower * ZOMBIE_VMGUN_LOWER;

		// a third of the room's light over a bright base: the room's alone
		// left a dim room's gun near black, and half of it still looked dull
		{
			u8 rgb[3];
			s32 c;

			for (c = 0; c < 3; c++) {
				rgb[c] = 0xb0 + player->gunshadecol[c] * 0x4f / 0xff;
			}

			// the vials: Buried's green, Pack-a-Punch's red
			if (gun->capsules & (1u << j)) {
				if (papped) {
					rgb[0] = 0xff;
					rgb[1] = rgb[1] / 5;
					rgb[2] = rgb[2] / 6;
				} else {
					rgb[0] = rgb[0] / 4;
					rgb[1] = 0xff;
					rgb[2] = rgb[2] / 4;
				}
			}

			// Pack-a-Punched: camo over the gun, as Call of Duty's, but not
			// over the dial or the vials
			if (papped && !((gun->capsules | gun->dials) & (1u << j))) {
				gdl = zombiesCamoAt(gdl);
				gdl = zombiesBottleDraw(gdl, zm, mtx, rgb[0], rgb[1], rgb[2]);
				gDPGrayscaleEXT(gdl++, 0);
			} else {
				gdl = zombiesBottleDraw(gdl, zm, mtx, rgb[0], rgb[1], rgb[2]);
			}
		}
	}

	mtxF2LBulk(matrices, n);
	mtx00016784();

	gSPPerspNormalize(gdl++, viGetPerspScale());

	return gdl;
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
		// a grenade's throw, where the drink would be: the same hands, and
		// only where bgunRender() draws them
		if (zombiesIsActive() && playernum >= 0 && playernum < MAX_PLAYERS
				&& g_ZombieThrowStage[playernum] != ZTHROW_NONE
				&& player->thirdpersondist <= 0 && player->visionmode != VISIONMODE_XRAY && !geWatchHidesGun()) {
			return zombiesRenderThrowViewmodel(gdl);
		}

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

	// Both hands, as Black Ops II's drink has them: the left one takes the
	// cap off (its sndnt#evt_perk_bottle_open, frame 16). It was put behind
	// the eye, and the cap came off the bottle by itself.
	for (j = 0; j < n; j++) {
		zombiesDrinkMtx(frame + (j < ZOMBIE_DRINK_JOINTS ? j : 0) * 12, NULL, &matrices[j]);
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
	// (xblamesh.c), and the hands may have been drawn on the gun already;
	// the left hand switched on (a gun's hand model holds it off), and put
	// back as it was, as the grenade's throw does
	{
		struct modelnode *node = modelGetPart(handmodel->definition, MODELPART_HAND_LEFT);
		struct modelrwdata_toggle *left = NULL;
		bool wasvisible = false;

		if (node && (node->type & 0xff) == MODELNODETYPE_TOGGLE) {
			left = modelGetNodeRwData(handmodel, node);
		}

		if (left) {
			wasvisible = left->visible;
			left->visible = true;
		}

		xblaMeshPassBegin();
		modelRender(&renderdata, handmodel);
		xblaMeshPassBegin();

		if (left) {
			left->visible = wasvisible;
		}
	}

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

static bool zombiesBodyArmOf(struct model *model, struct zombiebodyarm *arm, s32 handpart);
static bool zombiesThrowBodyLoad(void);
static bool zombiesDrinkClipOn(void);

static bool zombiesBodyArm(struct model *model, struct zombiebodyarm *arm)
{
	return zombiesBodyArmOf(model, arm, MODELPART_CHR_RIGHTHAND);
}

// The arm up from a hand (MODELPART_CHR_RIGHTHAND or _LEFTHAND)
static bool zombiesBodyArmOf(struct model *model, struct zombiebodyarm *arm, s32 handpart)
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

	hand = modelGetPart(model->definition, handpart);

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

	// Black Ops II's own drink on the body, when its clip is there
	// (zombiesPoseThrowArms()): this arm bent to it is what was had before
	if (t < 0 || zombiesDrinkClipOn() || !zombiesBodyArm(model, &arm)) {
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

/**
 * The dolphin dive and lying prone on the player's body: Black Ops II's
 * (common_zm's pb_* animations) retargeted onto the chr skeleton (g_SkelChr)
 * by tools/zombies/divebake.py into the build's zombies/viewmodel/dive.zbd:
 * each of the fourteen animation parts' turn at every frame of Call of Duty's,
 * and the pelvis's place over the floor. The art stays on the player's disk;
 * without the file the body runs on as it was.
 *
 * Zombies has no prone, so its full crouch (CROUCHPOS_SQUAT) is Call of Duty's
 * prone: going into it lays the body down (pb_crouch2prone), it lies there
 * (pb_prone_aim) and crawls as the player moves (pb_prone_crawl, and its back,
 * left and right, each loop for the inches it carries the body), and coming
 * out of it gets up (pb_prone2crouch). A dive (pb_dive_prone) holds its last
 * frame while the player is in the air and lands (pb_dive_prone_land) into
 * prone, as zombiesTickDive() squats the player on landing.
 *
 * Every machine sees every player's alike, from what the snapshots carry for
 * each player: a push (rolltime60) along the body's facing is a dive
 * (bwalkTryDive()), a roll's is across it; isfalling's end after the hop is
 * the landing; crouchpos is the squat; the body's own moves are the crawl. So
 * nothing of it goes on the wire.
 *
 * Landing, getting down and getting up run at twice Call of Duty's speed, as
 * the player can move again at once. Each change of pose blends from the last
 * one, and from and back to the body's own animation. Joints are placed down
 * the body's own offsets, the pelvis's height scaled by its legs, so any human
 * body will do; the elbows' and knees' second matrices are the parent's turn
 * half way to the joint's, as modelPositionJointUsingQuatRot() makes them.
 */
#define ZOMBIE_DIVEBODY_PARTS     14
#define ZOMBIE_DIVEBODY_MIX       8     // 60ths from one pose to the next
#define ZOMBIE_DIVEBODY_SPEED     2     // landing, getting down and up, against Call of Duty's
#define ZOMBIE_DIVEBODY_OUT       12    // 60ths at getting up's end, back to the body's own
#define ZOMBIE_DIVEBODY_AIRMAX    90    // a dive's most in the air, landed or not
#define ZOMBIE_DIVEBODY_CRAWLFULL 0.05f // inches a 60th crawling at all

enum {
	ZDCLIP_DIVE,
	ZDCLIP_LAND,
	ZDCLIP_GETUP,
	ZDCLIP_GETDOWN,
	ZDCLIP_PRONE,
	ZDCLIP_CRAWL,
	ZDCLIP_CRAWLBACK,
	ZDCLIP_CRAWLLEFT,
	ZDCLIP_CRAWLRIGHT,
	ZDCLIP_COUNT
};

enum {
	ZDMODE_NONE,
	ZDMODE_DIVE,
	ZDMODE_DOWN,
	ZDMODE_PRONE,
	ZDMODE_UP,
};

static struct {
	bool tried;
	f32 legs;                          // the leg length the pelvis is in
	f32 perinch;                       // the model's units to the inch
	s32 frames[ZDCLIP_COUNT];
	bool loops[ZDCLIP_COUNT];
	f32 fps[ZDCLIP_COUNT];
	f32 travel[ZDCLIP_COUNT];          // inches a loop carries the body
	const f32 *data[ZDCLIP_COUNT];     // per frame: pelvis[3], 14 x 3x3 rows
} g_ZombieDiveBody;

struct zombiedivepose {
	f32 q[ZOMBIE_DIVEBODY_PARTS + 1][4]; // each part's turn into the model's axes
	f32 pelvis[3];
};

static struct {
	s32 mode;
	s32 rolltime;                // the push last seen
	s32 since60;                 // lvframe60 the mode began
	s32 land60;                  // a dive's landing, -1 in the air
	s32 last60;                  // the frame last worked out
	struct zombiedivepose from;  // the pose and its weight as the mode began
	f32 fromweight;
	struct zombiedivepose pose;  // and as last worked out
	f32 weight;
	f32 crawl;                   // how far through a crawl's loop, 0 to 1
	f32 speed[2];                // forward and left, inches a 60th
	struct coord prevpos;
} g_ZombieDiveSeen[MAX_PLAYERS];

// part -> its parent part, 0 for the pelvis
static const u8 g_ZombieDiveParent[ZOMBIE_DIVEBODY_PARTS + 1] = {
	0, 0, 1, 2, 2, 3, 4, 5, 6, 1, 1, 9, 10, 11, 12,
};

static bool zombiesDiveBodyLoad(void)
{
	const char *rel = "$E/zombies/viewmodel/dive.zbd";
	u8 *data;
	u32 size = 0;
	u32 at;
	u16 hdr[2];
	s32 c;

	if (g_ZombieDiveBody.tried) {
		return g_ZombieDiveBody.data[0] != NULL;
	}

	g_ZombieDiveBody.tried = true;

	if (fsFileSize(fsFullPath(rel)) <= 0) {
		return false;
	}

	data = fsFileLoad(rel, &size);

	if (!data || size < 16 || memcmp(data, "ZBD2", 4) != 0) {
		sysLogPrintf(LOG_WARNING, "zombies: %s is not divebake.py's", rel);
		return false;
	}

	memcpy(hdr, data + 4, sizeof(hdr));
	memcpy(&g_ZombieDiveBody.legs, data + 8, sizeof(f32));
	memcpy(&g_ZombieDiveBody.perinch, data + 12, sizeof(f32));

	if (hdr[0] != ZDCLIP_COUNT || hdr[1] != ZOMBIE_DIVEBODY_PARTS
			|| g_ZombieDiveBody.legs <= 0 || g_ZombieDiveBody.perinch <= 0) {
		sysLogPrintf(LOG_WARNING, "zombies: %s is not divebake.py's", rel);
		return false;
	}

	at = 16;

	for (c = 0; c < ZDCLIP_COUNT; c++) {
		u16 head[2];
		f32 rates[2];
		u32 len;

		if (at + 12 > size) {
			break;
		}

		memcpy(head, data + at, sizeof(head));
		memcpy(rates, data + at + 4, sizeof(rates));
		len = (u32)head[0] * (3 + ZOMBIE_DIVEBODY_PARTS * 9) * sizeof(f32);
		at += 12;

		if (head[0] == 0 || rates[0] <= 0 || at + len > size) {
			break;
		}

		g_ZombieDiveBody.frames[c] = head[0];
		g_ZombieDiveBody.loops[c] = head[1] & 1;
		g_ZombieDiveBody.fps[c] = rates[0];
		g_ZombieDiveBody.travel[c] = rates[1];
		g_ZombieDiveBody.data[c] = (const f32 *)(data + at);
		at += len;
	}

	if (c < ZDCLIP_COUNT) {
		sysLogPrintf(LOG_WARNING, "zombies: %s is short", rel);
		g_ZombieDiveBody.data[0] = NULL;
		return false;
	}

	sysLogPrintf(LOG_NOTE, "zombies: the dolphin dive and prone on the body, %d clips", ZDCLIP_COUNT);

	return true;
}

// A rotation matrix's quaternion (the game's: w first); the matrix may be scaled
static void zombiesMtxQuat(const Mtxf *m, f32 q[4])
{
	Mtxf r;
	s32 c;
	s32 i;

	for (c = 0; c < 3; c++) {
		f32 len = sqrtf(m->m[c][0] * m->m[c][0] + m->m[c][1] * m->m[c][1] + m->m[c][2] * m->m[c][2]);

		for (i = 0; i < 3; i++) {
			r.m[c][i] = len > 0 ? m->m[c][i] / len : (c == i ? 1.0f : 0.0f);
		}
	}

	quaternion0f097044(&r, q);
}

static void zombiesDiveFrame(s32 clip, s32 frame, struct zombiedivepose *pose)
{
	const f32 *d = g_ZombieDiveBody.data[clip] + frame * (3 + ZOMBIE_DIVEBODY_PARTS * 9);
	s32 j;

	pose->pelvis[0] = d[0];
	pose->pelvis[1] = d[1];
	pose->pelvis[2] = d[2];
	d += 3;

	for (j = 1; j <= ZOMBIE_DIVEBODY_PARTS; j++, d += 9) {
		Mtxf m;
		s32 r;
		s32 c;

		// divebake.py's rows; a Mtxf is by columns
		for (r = 0; r < 3; r++) {
			for (c = 0; c < 3; c++) {
				m.m[c][r] = d[r * 3 + c];
			}
		}

		quaternion0f097044(&m, pose->q[j]);
	}
}

// a = a towards b by t
static void zombiesDivePoseMix(struct zombiedivepose *a, struct zombiedivepose *b, f32 t)
{
	s32 j;
	s32 i;

	if (t <= 0) {
		return;
	}

	if (t >= 1) {
		*a = *b;
		return;
	}

	for (j = 1; j <= ZOMBIE_DIVEBODY_PARTS; j++) {
		f32 q[4];

		quaternion0f0976c0(a->q[j], b->q[j]);
		quaternionSlerp(a->q[j], b->q[j], t, q);

		for (i = 0; i < 4; i++) {
			a->q[j][i] = q[i];
		}
	}

	for (i = 0; i < 3; i++) {
		a->pelvis[i] += (b->pelvis[i] - a->pelvis[i]) * t;
	}
}

// A clip at a frame (fractions between them), a loop's wrapped round
static void zombiesDiveSampleFrame(s32 clip, f32 f, struct zombiedivepose *pose)
{
	const s32 frames = g_ZombieDiveBody.frames[clip];
	struct zombiedivepose next;
	s32 i0;
	s32 i1;

	if (g_ZombieDiveBody.loops[clip]) {
		f -= (s32)(f / frames) * frames;
		f = f < 0 ? f + frames : f;
	} else {
		f = f < 0 ? 0 : f > frames - 1 ? frames - 1 : f;
	}

	i0 = (s32)f;
	i0 = i0 >= frames ? frames - 1 : i0;
	i1 = i0 + 1 < frames ? i0 + 1 : g_ZombieDiveBody.loops[clip] ? 0 : i0;

	zombiesDiveFrame(clip, i0, pose);

	if (i1 != i0 && f > i0) {
		zombiesDiveFrame(clip, i1, &next);
		zombiesDivePoseMix(pose, &next, f - i0);
	}
}

// A clip t 60ths in at speed times its own
static void zombiesDiveSample(s32 clip, f32 t, f32 speed, struct zombiedivepose *pose)
{
	zombiesDiveSampleFrame(clip, t * speed * g_ZombieDiveBody.fps[clip] / 60.0f, pose);
}

// A clip's length in 60ths at speed times its own
static f32 zombiesDiveClip60(s32 clip, f32 speed)
{
	return (g_ZombieDiveBody.frames[clip] - 1) * 60.0f / (g_ZombieDiveBody.fps[clip] * speed);
}

static void zombiesDiveSetMode(s32 playernum, s32 mode, s32 now)
{
	g_ZombieDiveSeen[playernum].from = g_ZombieDiveSeen[playernum].pose;
	g_ZombieDiveSeen[playernum].fromweight = g_ZombieDiveSeen[playernum].mode == ZDMODE_NONE
		? 0 : g_ZombieDiveSeen[playernum].weight;
	g_ZombieDiveSeen[playernum].mode = mode;
	g_ZombieDiveSeen[playernum].since60 = now;
}

// Lying prone: still, or crawling the way the body is going, for as far
static void zombiesDiveProne(s32 playernum, s32 t, struct zombiedivepose *pose)
{
	const f32 fwd = g_ZombieDiveSeen[playernum].speed[0];
	const f32 left = g_ZombieDiveSeen[playernum].speed[1];
	const f32 speed = sqrtf(fwd * fwd + left * left);
	struct zombiedivepose crawl;
	struct zombiedivepose side;
	s32 fb = fwd >= 0 ? ZDCLIP_CRAWL : ZDCLIP_CRAWLBACK;
	s32 lr = left >= 0 ? ZDCLIP_CRAWLLEFT : ZDCLIP_CRAWLRIGHT;
	f32 across = speed > 0 ? fabsf(left) / (fabsf(fwd) + fabsf(left)) : 0;
	f32 phase = g_ZombieDiveSeen[playernum].crawl;

	zombiesDiveSample(ZDCLIP_PRONE, t, 1, pose);

	if (speed <= 0.001f) {
		return;
	}

	zombiesDiveSampleFrame(fb, phase * g_ZombieDiveBody.frames[fb], &crawl);
	zombiesDiveSampleFrame(lr, phase * g_ZombieDiveBody.frames[lr], &side);
	zombiesDivePoseMix(&crawl, &side, across);
	zombiesDivePoseMix(pose, &crawl, speed >= ZOMBIE_DIVEBODY_CRAWLFULL ? 1 : speed / ZOMBIE_DIVEBODY_CRAWLFULL);
}

/**
 * The player whose body this is, diving or prone: the pose wanted, and how
 * much of it against the body's own animation (0 to 1). False when neither.
 */
static bool zombiesDiveBodyPose(struct chrdata *chr, f32 yrot, f32 scale, struct zombiedivepose *pose, f32 *weight)
{
	struct player *player;
	struct zombiedivepose target;
	f32 targetweight = 1;
	s32 playernum;
	s32 now = g_Vars.lvframe60;
	s32 dt;
	s32 t;
	bool squat;

	if (!zombiesIsActive() || !chr || !chr->prop || chr->prop->type != PROPTYPE_PLAYER) {
		return false;
	}

	playernum = playermgrGetPlayerNumByProp(chr->prop);

	if (playernum < 0 || playernum >= MAX_PLAYERS || !g_Vars.players[playernum]) {
		return false;
	}

	player = g_Vars.players[playernum];

	// once a frame, however many screens draw the body
	if (now == g_ZombieDiveSeen[playernum].last60 && g_ZombieDiveSeen[playernum].mode != ZDMODE_NONE) {
		*pose = g_ZombieDiveSeen[playernum].pose;
		*weight = g_ZombieDiveSeen[playernum].weight;
		return *weight > 0;
	}

	dt = now - g_ZombieDiveSeen[playernum].last60;
	dt = dt < 0 || dt > 30 ? 0 : dt;
	g_ZombieDiveSeen[playernum].last60 = now;

	// the body's own moves, along and across its facing, for a crawl
	{
		struct coord *pos = &chr->prop->pos;
		f32 dx = pos->x - g_ZombieDiveSeen[playernum].prevpos.x;
		f32 dz = pos->z - g_ZombieDiveSeen[playernum].prevpos.z;
		f32 inch = g_ZombieDiveBody.perinch * scale;

		g_ZombieDiveSeen[playernum].prevpos = *pos;

		if (dt > 0 && inch > 0) {
			f32 fwd = (dx * sinf(yrot) + dz * cosf(yrot)) / inch / dt;
			f32 left = (dx * cosf(yrot) - dz * sinf(yrot)) / inch / dt;
			f32 k = dt * 0.15f > 1 ? 1 : dt * 0.15f;
			f32 speed;

			g_ZombieDiveSeen[playernum].speed[0] += (fwd - g_ZombieDiveSeen[playernum].speed[0]) * k;
			g_ZombieDiveSeen[playernum].speed[1] += (left - g_ZombieDiveSeen[playernum].speed[1]) * k;

			speed = sqrtf(fwd * fwd + left * left) * dt;

			if (g_ZombieDiveSeen[playernum].mode == ZDMODE_PRONE && speed > 0) {
				s32 fb = g_ZombieDiveSeen[playernum].speed[0] >= 0 ? ZDCLIP_CRAWL : ZDCLIP_CRAWLBACK;
				s32 lr = g_ZombieDiveSeen[playernum].speed[1] >= 0 ? ZDCLIP_CRAWLLEFT : ZDCLIP_CRAWLRIGHT;
				f32 a = fabsf(g_ZombieDiveSeen[playernum].speed[1])
					/ (fabsf(g_ZombieDiveSeen[playernum].speed[0]) + fabsf(g_ZombieDiveSeen[playernum].speed[1]) + 0.0001f);
				f32 travel = g_ZombieDiveBody.travel[fb] * (1 - a) + g_ZombieDiveBody.travel[lr] * a;

				if (travel > 0) {
					g_ZombieDiveSeen[playernum].crawl += speed / travel;
					g_ZombieDiveSeen[playernum].crawl -= (s32)g_ZombieDiveSeen[playernum].crawl;
				}
			}
		}
	}

	if (player->isdead || g_ZombieDowned[playernum]) {
		g_ZombieDiveSeen[playernum].mode = ZDMODE_NONE;
		g_ZombieDiveSeen[playernum].rolltime = player->rolltime60;
		return false;
	}

	squat = player->crouchpos == CROUCHPOS_SQUAT && player->bondmovemode == MOVEMODE_WALK
		&& !player->onladder && zombiesBodyDrinkT(chr, NULL) < 0;

	// a push begun since the last look: along the body's facing, a dive
	if (player->rolltime60 != g_ZombieDiveSeen[playernum].rolltime) {
		f32 x = player->rollspeed.x;
		f32 z = player->rollspeed.z;
		f32 len = sqrtf(x * x + z * z);

		g_ZombieDiveSeen[playernum].rolltime = player->rolltime60;

		if (player->rolltime60 != 0 && len > 0 && (x * sinf(yrot) + z * cosf(yrot)) > 0.5f * len) {
			zombiesDiveSetMode(playernum, ZDMODE_DIVE, now);
			g_ZombieDiveSeen[playernum].land60 = -1;
		}
	}

	t = now - g_ZombieDiveSeen[playernum].since60;

	switch (g_ZombieDiveSeen[playernum].mode) {
	case ZDMODE_NONE:
		if (squat) {
			zombiesDiveSetMode(playernum, ZDMODE_DOWN, now);
		}
		break;
	case ZDMODE_DIVE:
		if (g_ZombieDiveSeen[playernum].land60 < 0
				&& ((t > 12 && !player->isfalling) || t > ZOMBIE_DIVEBODY_AIRMAX)) {
			g_ZombieDiveSeen[playernum].land60 = now;
		}

		if (g_ZombieDiveSeen[playernum].land60 >= 0
				&& now - g_ZombieDiveSeen[playernum].land60 >= zombiesDiveClip60(ZDCLIP_LAND, ZOMBIE_DIVEBODY_SPEED)) {
			zombiesDiveSetMode(playernum, squat ? ZDMODE_PRONE : ZDMODE_UP, now);
		}
		break;
	case ZDMODE_DOWN:
		if (!squat) {
			zombiesDiveSetMode(playernum, ZDMODE_UP, now);
		} else if (t >= zombiesDiveClip60(ZDCLIP_GETDOWN, ZOMBIE_DIVEBODY_SPEED)) {
			zombiesDiveSetMode(playernum, ZDMODE_PRONE, now);
		}
		break;
	case ZDMODE_PRONE:
		if (!squat) {
			zombiesDiveSetMode(playernum, ZDMODE_UP, now);
		}
		break;
	case ZDMODE_UP:
		if (squat) {
			zombiesDiveSetMode(playernum, ZDMODE_DOWN, now);
		} else if (t >= zombiesDiveClip60(ZDCLIP_GETUP, ZOMBIE_DIVEBODY_SPEED)) {
			g_ZombieDiveSeen[playernum].mode = ZDMODE_NONE;
		}
		break;
	}

	t = now - g_ZombieDiveSeen[playernum].since60;

	switch (g_ZombieDiveSeen[playernum].mode) {
	case ZDMODE_NONE:
		g_ZombieDiveSeen[playernum].weight = 0;
		return false;
	case ZDMODE_DIVE:
		zombiesDiveSample(ZDCLIP_DIVE, t, 1, &target);

		if (g_ZombieDiveSeen[playernum].land60 >= 0) {
			struct zombiedivepose land;
			s32 l = now - g_ZombieDiveSeen[playernum].land60;

			zombiesDiveSample(ZDCLIP_LAND, l, ZOMBIE_DIVEBODY_SPEED, &land);
			zombiesDivePoseMix(&target, &land, zombiesEase(0, ZOMBIE_DIVEBODY_MIX, l));
		}
		break;
	case ZDMODE_DOWN:
		zombiesDiveSample(ZDCLIP_GETDOWN, t, ZOMBIE_DIVEBODY_SPEED, &target);
		break;
	case ZDMODE_PRONE:
		zombiesDiveProne(playernum, t, &target);
		break;
	case ZDMODE_UP: {
		f32 end = zombiesDiveClip60(ZDCLIP_GETUP, ZOMBIE_DIVEBODY_SPEED);

		zombiesDiveSample(ZDCLIP_GETUP, t, ZOMBIE_DIVEBODY_SPEED, &target);
		targetweight = 1 - zombiesEase(end - ZOMBIE_DIVEBODY_OUT, end, t);
		break;
	}
	}

	// from the pose the mode began in, or in from the body's own
	{
		f32 e = zombiesEase(0, ZOMBIE_DIVEBODY_MIX, t);

		if (g_ZombieDiveSeen[playernum].fromweight > 0) {
			*pose = g_ZombieDiveSeen[playernum].from;
			zombiesDivePoseMix(pose, &target, e);
		} else {
			*pose = target;
		}

		*weight = (g_ZombieDiveSeen[playernum].fromweight + (1 - g_ZombieDiveSeen[playernum].fromweight) * e)
			* targetweight;
	}

	g_ZombieDiveSeen[playernum].pose = *pose;
	g_ZombieDiveSeen[playernum].weight = *weight;

	return *weight > 0;
}

/**
 * bwalkApplyCrouchSpeed()'s, on the full crouch's 0.35: lying prone, the
 * player crawls at some twice Call of Duty's crawl (about 12 inches a second
 * against Black Ops II's 8), where the full crouch went 37 and the crawl
 * played four and a half times over; Call of Duty's legs (ZOMBIE_COD_WALK)
 * taken back off. Only where the prone is drawn (dive.zbd is there), and not
 * for the downed, whose crawl is ZOMBIE_DOWNED_SPEED's.
 */
f32 zombiesProneSpeed(void)
{
	const s32 playernum = g_Vars.currentplayernum;

	if (!zombiesIsActive() || !zombiesDiveBodyLoad()
			|| (playernum >= 0 && playernum < MAX_PLAYERS && g_ZombieDowned[playernum])) {
		return 1.0f;
	}

	return 0.3f / ZOMBIE_COD_WALK;
}

// The chr's model matrices: the dive's or prone's pose, while the player is in one
void zombiesPoseDive(struct chrdata *chr, struct model *model)
{
	struct modelnode *nodes[ZOMBIE_DIVEBODY_PARTS + 1] = { NULL };
	struct modelnode *node;
	union modelrwdata *rwdata;
	struct zombiedivepose pose;
	Mtxf base;
	Mtxf rot[ZOMBIE_DIVEBODY_PARTS + 1];
	f32 pos[ZOMBIE_DIVEBODY_PARTS + 1][3];
	f32 weight;
	f32 legs;
	s32 n;
	s32 j;
	s32 i;

	if (!model || !model->definition || !model->matrices || model->definition->skel != &g_SkelChr
			|| (model->definition->rootnode->type & 0xff) != MODELNODETYPE_CHRINFO
			|| !zombiesDiveBodyLoad()) {
		return;
	}

	rwdata = modelGetNodeRwData(model, model->definition->rootnode);

	if (!zombiesDiveBodyPose(chr, rwdata->chrinfo.yrot, model->scale, &pose, &weight)) {
		return;
	}

	// the body's own position nodes by part, not the head's grafted under it
	for (node = model->definition->rootnode; node;) {
		u32 type = node->type & 0xff;

		if (type == MODELNODETYPE_POSITION && node->rodata->position.part >= 1
				&& node->rodata->position.part <= ZOMBIE_DIVEBODY_PARTS) {
			nodes[node->rodata->position.part] = node;
		}

		if (node->child && type != MODELNODETYPE_HEADSPOT) {
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

	n = model->definition->nummatrices;

	for (j = 1; j <= ZOMBIE_DIVEBODY_PARTS; j++) {
		if (!nodes[j] || nodes[j]->rodata->position.mtxindex0 < 0 || nodes[j]->rodata->position.mtxindex0 >= n
				|| nodes[j]->rodata->position.mtxindex1 >= n) {
			return;
		}
	}

	// the legs' length here against the body divebake.py had
	legs = 0;

	for (j = 11; j <= 14; j++) {
		struct coord *p = &nodes[j]->rodata->position.pos;
		legs += sqrtf(p->x * p->x + p->y * p->y + p->z * p->z) * 0.5f;
	}

	// the floor under the body, its facing and its scale, into the view
	{
		struct coord floor;
		Mtxf local;

		floor.x = rwdata->chrinfo.pos.x;
		floor.y = rwdata->chrinfo.ground;
		floor.z = rwdata->chrinfo.pos.z;

		mtx4LoadYRotationWithTranslation(&floor, rwdata->chrinfo.yrot, &local);
		mtx00015f4c(model->scale, &local);
		mtx00015be4(camGetWorldToScreenMtxf(), &local, &base);
	}

	// each part's turn and place in the model's own space, down the offsets
	for (j = 1; j <= ZOMBIE_DIVEBODY_PARTS; j++) {
		quaternionToMtx(pose.q[j], &rot[j]);

		if (j == 1) {
			for (i = 0; i < 3; i++) {
				pos[j][i] = pose.pelvis[i] * legs / g_ZombieDiveBody.legs;
			}
		} else {
			const s32 p = g_ZombieDiveParent[j];
			struct coord *off = &nodes[j]->rodata->position.pos;

			for (i = 0; i < 3; i++) {
				pos[j][i] = pos[p][i] + rot[p].m[0][i] * off->x + rot[p].m[1][i] * off->y + rot[p].m[2][i] * off->z;
			}
		}
	}

	for (j = 1; j <= ZOMBIE_DIVEBODY_PARTS; j++) {
		struct modelrodata_position *rodata = &nodes[j]->rodata->position;
		s32 k;

		// the pelvis's third: the root's own matrix (chrinfo), which some of
		// the body hangs from; left be, it stood up out of a body lying down
		for (k = 0; k < (j == 1 ? 3 : 2); k++) {
			s32 index = k == 2 ? model->definition->rootnode->rodata->chrinfo.mtxindex
				: k ? rodata->mtxindex1 : rodata->mtxindex0;
			f32 at[3] = { pos[j][0], pos[j][1], pos[j][2] };
			Mtxf local;
			Mtxf want;
			Mtxf *mtx;
			f32 qlive[4];
			f32 qwant[4];
			f32 q[4];
			f32 s;

			if (index < 0 || index >= n || (k == 2 && index == rodata->mtxindex0)) {
				continue;
			}

			if (k == 0) {
				local = rot[j];
			} else if (k == 2) {
				// where the pelvis's offset from it puts it, turned as the pelvis
				local = rot[j];

				for (i = 0; i < 3; i++) {
					at[i] -= rot[j].m[0][i] * rodata->pos.x + rot[j].m[1][i] * rodata->pos.y + rot[j].m[2][i] * rodata->pos.z;
				}
			} else {
				// an elbow's or knee's second: its parent's turn half way to its own
				const s32 p = g_ZombieDiveParent[j];
				Mtxf rel;
				f32 qr[4];
				f32 half[4];
				s32 a;
				s32 b;

				for (a = 0; a < 3; a++) {
					for (b = 0; b < 3; b++) {
						rel.m[b][a] = rot[p].m[a][0] * rot[j].m[b][0] + rot[p].m[a][1] * rot[j].m[b][1]
							+ rot[p].m[a][2] * rot[j].m[b][2];
					}
				}

				quaternion0f097044(&rel, qr);
				quaternion0f097518(qr, 0.5f, half);
				quaternionToMtx(half, &rel);
				mtx00015be4(&rot[p], &rel, &local);
			}

			local.m[3][0] = at[0];
			local.m[3][1] = at[1];
			local.m[3][2] = at[2];
			local.m[0][3] = local.m[1][3] = local.m[2][3] = 0;
			local.m[3][3] = 1;

			mtx00015be4(&base, &local, &want);

			// blended from the body's own pose in the view: the same scale
			// both ways, so the turn and the place each go their own way
			mtx = &model->matrices[index];

			if (weight >= 1) {
				*mtx = want;
				continue;
			}

			s = sqrtf(want.m[0][0] * want.m[0][0] + want.m[0][1] * want.m[0][1] + want.m[0][2] * want.m[0][2]);
			zombiesMtxQuat(mtx, qlive);
			zombiesMtxQuat(&want, qwant);
			quaternion0f0976c0(qlive, qwant);
			quaternionSlerp(qlive, qwant, weight, q);
			quaternionToMtx(q, &local);

			for (i = 0; i < 3; i++) {
				mtx->m[0][i] = local.m[0][i] * s;
				mtx->m[1][i] = local.m[1][i] * s;
				mtx->m[2][i] = local.m[2][i] * s;
				mtx->m[3][i] += (want.m[3][i] - mtx->m[3][i]) * weight;
			}
		}
	}
}

/**
 * A grenade's throw on the body, seen in third person or by anybody else:
 * Black Ops II's torso clips (pt_hold_throw while the pin is pulled and the
 * arm held back, then pt_grenade_stand_throw, pt_crouch_grenade_throw or
 * pt_prone_grenade_throw as the body stands, crouches or lies), retargeted by
 * tools/zombies/throwbake.py into the build's zombies/viewmodel/throw.ztb. The
 * chest's turn is the clip's in the pelvis's frame and each arm part's in the
 * chest's, so whatever the body's own animation (or a prone, zombiesPoseDive())
 * has the legs and hips doing, the throw is laid on it; the arms are placed
 * down the body's own offsets from the chest. Without the file the body
 * throws with nothing.
 */
#define ZOMBIE_THROWBODY_PARTS 8 // the chest (part 2), the arms (3 to 8) and the head's turn in the chest's
#define ZOMBIE_THROWBODY_IN    9 // 60ths from the body's own pose into the clip
#define ZOMBIE_THROWBODY_LETGO 10 // the frag gone from the body's hand: the arm out (the stand throw's 7th frame)

enum {
	ZTCLIP_HOLD,
	ZTCLIP_STAND,
	ZTCLIP_CROUCH,
	ZTCLIP_PRONE,
	ZTCLIP_DRINK, // a perk's drink, Black Ops II's third person p_zombie_perkacola_drink
	ZTCLIP_COUNT
};

/**
 * The drink on the body, Black Ops II's own: its clip (65 frames at 30) from
 * the bottle coming up, two of the drink's 60ths a frame, easing in and out
 * over ZOMBIE_THROWBODY_IN. Its notes: the bottle picked up (14), drunk (31),
 * thrown (46); held in the left hand all the while (anim_gunhand "left").
 */
// Off until it is right (2026-10-22): the clip's check sheet curls the right
// arm over the head to drink, as Black Ops II's does, but laid on the body in
// the game an arm stood out straight in front at that frame. The bent arm
// (zombiesPoseDrinkArm()) brings the bottle to the mouth meanwhile.
#define ZOMBIE_DRINKBODY_CLIP   0
#define ZOMBIE_DRINKBODY_PICKUP 28 // the drink's 60ths: the bottle in the left hand,
#define ZOMBIE_DRINKBODY_UNCAP  40 // its cap off,
#define ZOMBIE_DRINKBODY_THROW  92 // and thrown away
#define ZOMBIE_DRINKBODY_END    128

static struct {
	bool tried;
	s32 frames[ZTCLIP_COUNT];
	f32 fps[ZTCLIP_COUNT];
	const f32 *data[ZTCLIP_COUNT]; // per frame: 7 x 3x3 rows
} g_ZombieThrowBody;

static bool zombiesThrowBodyLoad(void)
{
	const char *rel = "$E/zombies/viewmodel/throw.ztb";
	u8 *data;
	u32 size = 0;
	u32 at;
	u16 hdr[2];
	s32 c;

	if (g_ZombieThrowBody.tried) {
		return g_ZombieThrowBody.data[0] != NULL;
	}

	g_ZombieThrowBody.tried = true;

	if (fsFileSize(fsFullPath(rel)) <= 0) {
		return false;
	}

	data = fsFileLoad(rel, &size);

	if (!data || size < 8 || memcmp(data, "ZTB1", 4) != 0) {
		return false;
	}

	memcpy(hdr, data + 4, sizeof(hdr));

	if (hdr[0] != ZTCLIP_COUNT || hdr[1] != ZOMBIE_THROWBODY_PARTS) {
		sysLogPrintf(LOG_WARNING, "zombies: %s is not throwbake.py's", rel);
		return false;
	}

	at = 8;

	for (c = 0; c < ZTCLIP_COUNT; c++) {
		u16 frames;
		f32 fps;

		if (at + 8 > size) {
			return false;
		}

		memcpy(&frames, data + at, 2);
		memcpy(&fps, data + at + 4, 4);
		at += 8;

		if (frames == 0 || fps <= 0 || at + (u32)frames * ZOMBIE_THROWBODY_PARTS * 9 * sizeof(f32) > size) {
			sysLogPrintf(LOG_WARNING, "zombies: %s is short", rel);
			return false;
		}

		g_ZombieThrowBody.frames[c] = frames;
		g_ZombieThrowBody.fps[c] = fps;
		g_ZombieThrowBody.data[c] = (const f32 *)(data + at);
		at += frames * ZOMBIE_THROWBODY_PARTS * 9 * sizeof(f32);
	}

	sysLogPrintf(LOG_NOTE, "zombies: the grenade thrown on the body, %d clips", ZTCLIP_COUNT);

	return true;
}

// The drink on the body is Black Ops II's clip (ZOMBIE_DRINKBODY_CLIP, and the file there)
static bool zombiesDrinkClipOn(void)
{
	return ZOMBIE_DRINKBODY_CLIP && zombiesThrowBodyLoad();
}

// A clip's parts at frame f (fractions between frames), as quaternions
static void zombiesThrowBodySample(s32 clip, f32 f, f32 q[ZOMBIE_THROWBODY_PARTS][4])
{
	const s32 frames = g_ZombieThrowBody.frames[clip];
	s32 frame[2];
	f32 qq[2][4];
	s32 j;
	s32 e;

	f = f < 0 ? 0 : f > frames - 1 ? frames - 1 : f;
	frame[0] = (s32)f;
	frame[1] = frame[0] + 1 < frames ? frame[0] + 1 : frame[0];

	for (j = 0; j < ZOMBIE_THROWBODY_PARTS; j++) {
		for (e = 0; e < 2; e++) {
			const f32 *d = g_ZombieThrowBody.data[clip] + (frame[e] * ZOMBIE_THROWBODY_PARTS + j) * 9;
			Mtxf m;
			s32 r;
			s32 c;

			// throwbake.py's rows; a Mtxf is by columns
			for (r = 0; r < 3; r++) {
				for (c = 0; c < 3; c++) {
					m.m[c][r] = d[r * 3 + c];
				}
			}

			quaternion0f097044(&m, qq[e]);
		}

		quaternion0f0976c0(qq[0], qq[1]);
		quaternionSlerp(qq[0], qq[1], f - frame[0], q[j]);
	}
}

// Which clip, how far in and how much of it a player's throw is now: false
// for none
static bool zombiesThrowBodyAt(s32 playernum, s32 *clip, f32 *frame, f32 *weight)
{
	const s32 t = g_ZombieThrow60[playernum];
	s32 crouch;
	f32 f;

	*weight = 1;

	// a perk being drunk (no throw meanwhile)
	if (g_ZombieThrowStage[playernum] == ZTHROW_NONE) {
		if (!ZOMBIE_DRINKBODY_CLIP) {
			return false;
		}

		const s32 dt = g_ZombieDrink60[playernum] - DRINK_GUNDOWN;

		if (g_ZombieDrink60[playernum] < 0 || dt < 0 || dt >= ZOMBIE_DRINKBODY_END) {
			return false;
		}

		*clip = ZTCLIP_DRINK;
		*frame = dt / 2.0f;
		*weight = MIN((f32)dt / ZOMBIE_THROWBODY_IN, (f32)(ZOMBIE_DRINKBODY_END - dt) / ZOMBIE_THROWBODY_IN);
		*weight = *weight < 0 ? 0 : *weight > 1 ? 1 : *weight;
		*weight = *weight * *weight * (3 - 2 * *weight);

		return *weight > 0;
	}

	switch (g_ZombieThrowStage[playernum]) {
	case ZTHROW_DOWN:
		*clip = ZTCLIP_HOLD;
		*frame = 0;
		*weight = (f32)t / ZOMBIE_THROWBODY_IN;
		break;
	case ZTHROW_PIN:
		*clip = ZTCLIP_HOLD;
		*frame = (f32)t / ZOMBIE_THROW_PIN60 * (g_ZombieThrowBody.frames[ZTCLIP_HOLD] - 1);
		break;
	case ZTHROW_HOLD:
		*clip = ZTCLIP_HOLD;
		*frame = g_ZombieThrowBody.frames[ZTCLIP_HOLD] - 1;
		break;
	case ZTHROW_OUT:
	case ZTHROW_UP:
		crouch = g_Vars.players[playernum]->crouchpos;
		// Zombies' full crouch is Call of Duty's prone
		*clip = crouch == CROUCHPOS_SQUAT ? ZTCLIP_PRONE : crouch == CROUCHPOS_DUCK ? ZTCLIP_CROUCH : ZTCLIP_STAND;
		f = (f32)(g_ZombieThrowStage[playernum] == ZTHROW_UP ? ZOMBIE_THROW_OUT60 + t : t)
			/ (ZOMBIE_THROW_OUT60 + ZOMBIE_THROW_UP60);
		*frame = f * (g_ZombieThrowBody.frames[*clip] - 1);

		if (g_ZombieThrowStage[playernum] == ZTHROW_UP) {
			*weight = 1 - (f32)t / ZOMBIE_THROW_UP60;
		}
		break;
	default:
		return false;
	}

	*weight = *weight < 0 ? 0 : *weight > 1 ? 1 : *weight;
	*weight = *weight * *weight * (3 - 2 * *weight);

	return *weight > 0;
}

// The matrix at index: rot (unit axes) times s at pos, mixed into the one
// there by weight
static void zombiesThrowBodySet(struct model *model, s32 index, Mtxf *rot, f32 s, const f32 pos[3], f32 weight)
{
	Mtxf *mtx = &model->matrices[index];
	Mtxf local;
	f32 qlive[4];
	f32 qwant[4];
	f32 q[4];
	s32 i;

	if (weight >= 1) {
		quaternion0f097044(rot, q);
	} else {
		zombiesMtxQuat(mtx, qlive);
		quaternion0f097044(rot, qwant);
		quaternion0f0976c0(qlive, qwant);
		quaternionSlerp(qlive, qwant, weight, q);
	}

	quaternionToMtx(q, &local);

	for (i = 0; i < 3; i++) {
		mtx->m[0][i] = local.m[0][i] * s;
		mtx->m[1][i] = local.m[1][i] * s;
		mtx->m[2][i] = local.m[2][i] * s;
		mtx->m[3][i] += (pos[i] - mtx->m[3][i]) * weight;
	}
}

// A player's body throwing (the gun put away, zombiesBodyHidesHeld()), and
// whether the frag is in its hand
static bool zombiesBodyThrowing(struct chrdata *chr, bool *inhand)
{
	s32 playernum;
	s32 stage;

	if (!zombiesIsActive() || !chr || !chr->prop || chr->prop->type != PROPTYPE_PLAYER) {
		return false;
	}

	playernum = playermgrGetPlayerNumByProp(chr->prop);

	if (playernum < 0 || playernum >= MAX_PLAYERS) {
		return false;
	}

	stage = g_ZombieThrowStage[playernum];

	if (inhand) {
		*inhand = stage == ZTHROW_PIN || stage == ZTHROW_HOLD
			|| (stage == ZTHROW_OUT && g_ZombieThrow60[playernum] < ZOMBIE_THROWBODY_LETGO);
	}

	return stage == ZTHROW_PIN || stage == ZTHROW_HOLD || stage == ZTHROW_OUT;
}

/**
 * zombiesPoseThrowArms()'s head, before the chest is set: every matrix of the
 * nodes under the body's head spot turned by D = H * L^-1 about the neck (the
 * first of them), where L is the chest's turn as the body has it (chest, live)
 * and H the clip's head on the chest it is about to have (want, mixed in by
 * weight); and carried as the chest's turn carries the neck.
 */
static void zombiesThrowBodyHead(struct model *model, Mtxf *chest, Mtxf *want, const f32 qhead[4], f32 weight)
{
	static u8 seen[512];
	const f32 qnone[4] = { 1, 0, 0, 0 };
	struct modelnode *spot = NULL;
	struct modelnode *node;
	Mtxf live;
	Mtxf mixed;
	Mtxf head;
	Mtxf h;
	f32 ql[4];
	f32 qw[4];
	f32 qc[4];
	f32 qh[4];
	f32 r[3][3];
	f32 pivot[3];
	f32 moved[3];
	f32 off[3];
	bool havepivot = false;
	s32 n = model->definition->nummatrices;
	s32 i;
	s32 k;

	for (node = model->definition->rootnode; node && !spot;) {
		if ((node->type & 0xff) == MODELNODETYPE_HEADSPOT) {
			spot = node;
			break;
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

	if (!spot || !spot->child || n > (s32)sizeof(seen)) {
		return;
	}

	// the chest as it is and as it will be, the head's turn on it
	zombiesMtxQuat(chest, ql);
	quaternion0f097044(want, qw);
	quaternion0f0976c0(ql, qw);
	quaternionSlerp(ql, qw, weight, qc);
	quaternionToMtx(ql, &live);
	quaternionToMtx(qc, &mixed);

	{
		f32 qn[4] = { qnone[0], qnone[1], qnone[2], qnone[3] };
		f32 qt[4] = { qhead[0], qhead[1], qhead[2], qhead[3] };

		quaternion0f0976c0(qn, qt);
		quaternionSlerp(qn, qt, weight, qh);
		quaternionToMtx(qh, &head);
	}

	mtx00015be4(&mixed, &head, &h);

	// D = H * L^-1 (L a turn: its inverse its transpose), as rows
	for (i = 0; i < 3; i++) {
		for (k = 0; k < 3; k++) {
			r[i][k] = h.m[0][i] * live.m[0][k] + h.m[1][i] * live.m[1][k] + h.m[2][i] * live.m[2][k];
		}
	}

	bzero(seen, n);

	for (node = spot->child; node;) {
		if ((node->type & 0xff) == MODELNODETYPE_POSITION) {
			struct modelrodata_position *rodata = &node->rodata->position;
			s32 e;

			for (e = 0; e < 3; e++) {
				const s32 idx = rodata->mtxindexes[e];

				if (idx < 0 || idx >= n || seen[idx]) {
					continue;
				}

				seen[idx] = 1;

				// the neck: the first, the pivot, carried with the chest's
				// own turn about its middle
				if (!havepivot) {
					havepivot = true;

					for (i = 0; i < 3; i++) {
						pivot[i] = model->matrices[idx].m[3][i];
						off[i] = pivot[i] - chest->m[3][i];
					}

					{
						f32 local[3];

						for (i = 0; i < 3; i++) {
							local[i] = live.m[i][0] * off[0] + live.m[i][1] * off[1] + live.m[i][2] * off[2];
						}

						for (i = 0; i < 3; i++) {
							moved[i] = chest->m[3][i] + mixed.m[0][i] * local[0] + mixed.m[1][i] * local[1]
								+ mixed.m[2][i] * local[2] - pivot[i];
						}
					}
				}

				zombiesMtxTurnAbout(&model->matrices[idx], r, pivot);

				for (i = 0; i < 3; i++) {
					model->matrices[idx].m[3][i] += moved[i];
				}
			}
		}

		if (node->child) {
			node = node->child;
		} else {
			while (node && node != spot) {
				if (node->next) {
					node = node->next;
					break;
				}

				node = node->parent;
			}

			if (node == spot) {
				break;
			}
		}
	}
}

// chr.c, after the body's animation, a dive's pose and a drink's arm
void zombiesPoseThrowArms(struct chrdata *chr, struct model *model)
{
	struct modelnode *nodes[9] = { NULL };
	struct modelnode *node;
	f32 q[ZOMBIE_THROWBODY_PARTS][4];
	Mtxf rot[9];
	f32 pos[9][3];
	f32 frame;
	f32 weight;
	f32 s;
	s32 playernum;
	s32 clip;
	s32 n;
	s32 j;
	s32 i;

	if (!zombiesIsActive() || !chr || !chr->prop || chr->prop->type != PROPTYPE_PLAYER
			|| !model || !model->definition || !model->matrices || model->definition->skel != &g_SkelChr) {
		return;
	}

	playernum = playermgrGetPlayerNumByProp(chr->prop);

	// a throw, or a drink (zombiesThrowBodyAt())
	if (playernum < 0 || playernum >= MAX_PLAYERS
			|| !zombiesThrowBodyLoad() || !zombiesThrowBodyAt(playernum, &clip, &frame, &weight)) {
		return;
	}

	// the body's own position nodes by part, not the head's grafted under it
	for (node = model->definition->rootnode; node;) {
		u32 type = node->type & 0xff;

		if (type == MODELNODETYPE_POSITION && node->rodata->position.part >= 1 && node->rodata->position.part <= 8) {
			nodes[node->rodata->position.part] = node;
		}

		if (node->child && type != MODELNODETYPE_HEADSPOT) {
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

	n = model->definition->nummatrices;

	for (j = 1; j <= 8; j++) {
		if (!nodes[j] || nodes[j]->rodata->position.mtxindex0 < 0 || nodes[j]->rodata->position.mtxindex0 >= n
				|| nodes[j]->rodata->position.mtxindex1 >= n) {
			return;
		}
	}

	zombiesThrowBodySample(clip, frame, q);

	// the pelvis as the body has it, in the view: its turn and its scale
	{
		Mtxf *m1 = &model->matrices[nodes[1]->rodata->position.mtxindex0];
		f32 q1[4];

		s = sqrtf(m1->m[0][0] * m1->m[0][0] + m1->m[0][1] * m1->m[0][1] + m1->m[0][2] * m1->m[0][2]);
		zombiesMtxQuat(m1, q1);
		quaternionToMtx(q1, &rot[1]);
	}

	// the chest turned on it, where the body has it
	{
		Mtxf m;

		quaternionToMtx(q[0], &m);
		mtx00015be4(&rot[1], &m, &rot[2]);

		for (i = 0; i < 3; i++) {
			pos[2][i] = model->matrices[nodes[2]->rodata->position.mtxindex0].m[3][i];
		}
	}

	// the arms on the chest. A shoulder stays where the body has it against
	// the chest, turned with it: the chest's matrix is not where the
	// shoulders' offsets are from (a shoulder 33 units off it by them, 19 as
	// the body has it, and the arms came out long). The elbows and hands go
	// down the body's own offsets, which do hold.
	{
		Mtxf *m2 = &model->matrices[nodes[2]->rodata->position.mtxindex0];
		Mtxf live2;
		f32 q2[4];

		zombiesMtxQuat(m2, q2);
		quaternionToMtx(q2, &live2);

		for (j = 3; j <= 8; j++) {
			const s32 p = g_ZombieDiveParent[j];
			struct coord *off = &nodes[j]->rodata->position.pos;
			Mtxf m;

			quaternionToMtx(q[j - 2], &m);
			mtx00015be4(&rot[2], &m, &rot[j]);

			if (p == 2) {
				f32 *at = model->matrices[nodes[j]->rodata->position.mtxindex0].m[3];
				f32 d[3];
				f32 local[3];

				for (i = 0; i < 3; i++) {
					d[i] = at[i] - m2->m[3][i];
				}

				for (i = 0; i < 3; i++) {
					local[i] = live2.m[i][0] * d[0] + live2.m[i][1] * d[1] + live2.m[i][2] * d[2];
				}

				for (i = 0; i < 3; i++) {
					pos[j][i] = pos[2][i] + rot[2].m[0][i] * local[0] + rot[2].m[1][i] * local[1] + rot[2].m[2][i] * local[2];
				}
			} else {
				for (i = 0; i < 3; i++) {
					pos[j][i] = pos[p][i] + (rot[p].m[0][i] * off->x + rot[p].m[1][i] * off->y + rot[p].m[2][i] * off->z) * s;
				}
			}
		}
	}

	// the head: everything under the body's head spot (the head's own nodes)
	// turned as the clip has it in the chest's frame. Perfect Dark's body has
	// no neck, and the head was left as the body's own animation had it -
	// upright through a drink, its bottle held up before the face.
	zombiesThrowBodyHead(model, &model->matrices[nodes[2]->rodata->position.mtxindex0], &rot[2], q[7], weight);

	for (j = 2; j <= 8; j++) {
		struct modelrodata_position *rodata = &nodes[j]->rodata->position;

		zombiesThrowBodySet(model, rodata->mtxindex0, &rot[j], s, pos[j], weight);

		// an elbow's second: its parent's turn half way to its own
		if (rodata->mtxindex1 >= 0 && rodata->mtxindex1 != rodata->mtxindex0) {
			const s32 p = g_ZombieDiveParent[j];
			Mtxf rel;
			Mtxf half;
			f32 qr[4];
			f32 qh[4];
			s32 a;
			s32 b;

			for (a = 0; a < 3; a++) {
				for (b = 0; b < 3; b++) {
					rel.m[b][a] = rot[p].m[a][0] * rot[j].m[b][0] + rot[p].m[a][1] * rot[j].m[b][1]
						+ rot[p].m[a][2] * rot[j].m[b][2];
				}
			}

			rel.m[3][0] = rel.m[3][1] = rel.m[3][2] = 0;
			rel.m[0][3] = rel.m[1][3] = rel.m[2][3] = 0;
			rel.m[3][3] = 1;
			quaternion0f097044(&rel, qr);
			quaternion0f097518(qr, 0.5f, qh);
			quaternionToMtx(qh, &rel);
			mtx00015be4(&rot[p], &rel, &half);
			zombiesThrowBodySet(model, rodata->mtxindex1, &half, s, pos[j], weight);
		}
	}
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

	if ((t < 0 && !zombiesBodyThrowing(chr, NULL)) || !prop) {
		return false;
	}

	return prop == chr->weapons_held[0] || prop == chr->weapons_held[1];
}

/**
 * Black Ops II's own zombie drawn in place of a zombie's Perfect Dark body
 * (zmchar.c): chrRender(), for each body it is about to draw. True when it
 * was drawn here and the body is not; the body still stands, unseen, for
 * everything else (shots, the hit parts, the walk), and its held props are
 * drawn as they were.
 *
 * Its animation is chosen by how fast the zombie is going - idle, walk, run,
 * sprint, Call of Duty's own speeds between them - and played as fast as it
 * goes, so that its feet keep up with the ground; a change blends over
 * ZOMBIE_COD_BLEND60. Dying, it plays one of Call of Duty's four deaths
 * through once and lies at its end.
 */
#define ZOMBIE_COD_SCALE   2.54f // our units an inch: Call of Duty's zombie at its own size
#define ZOMBIE_COD_BLEND60 12    // one animation into the next
#define ZOMBIE_COD_IDLE    8.0f  // inches a second: slower than this stands
#define ZOMBIE_COD_RUN     72.0f // between the walk's 37.6 and the run's 108
#define ZOMBIE_COD_SPRINT  125.0f
#define ZOMBIE_COD_CRAWLFAST 25.0f // between the crawl's 14 and its sprint's 40
#define ZOMBIE_COD_REACH     150.0f // how near a player a zombie swipes at them: Call of Duty's 60 inches (a bot's own punch reaches 210)

static struct {
	s32 anim;
	f32 frame;
	s32 from;          // the animation blended out of, and its frame
	f32 fromframe;
	f32 blend;         // what is left of it, 1 to 0
	f32 speed;         // inches a second, smoothed
	struct coord last; // where it was at its last tick
	s32 tickframe;     // the level frame it was last ticked on
	struct chrdata *chr;
} g_ZombieCod[MAX_BOTS];

/**
 * A zombie's swipe, Black Ops II's: begun in the game's tick (the host's) when
 * a player is in reach, and dealt at its clip's own "fire" notes - the walking
 * swipe's frame 6, the standing one's 25 and 39, the crawler's 24 and 54 - to
 * a player still in reach and in sight, a zombie's blow each
 * (ZOMBIE_REACH_BLOW). The body under it no longer hurts anyone with its own
 * punch (chrPunchInflictDamage()), so what is seen is what lands.
 */
static struct {
	s32 anim;          // its swipe, -1 none
	s32 t60;           // how far through
	u32 fired;         // which of its blows have been dealt
	s32 rest60;        // till it may swing again
	s32 near60;        // how long a player has been in its reach, for the swing to begin
	struct coord last; // where it was a tick ago, for whether it is walking
} g_ZombieSwing[MAX_BOTS];

/**
 * A zombie off a ledge: Black Ops II's jump down (zmcharJumpDown()) nearest
 * the drop, taken up where its root leaves the ledge. Perfect Dark's own fall
 * carries the body; the clip goes as far down as the zombie has gone, drawn at
 * its feet rather than on the floor below, and plays its landing out once it
 * is down.
 */
#define ZOMBIE_COD_DROP 30.0f // inches: a fall deeper is a jump down (Call of Duty's least is 48; a stair's is a few)

static struct {
	s32 stage;   // 0 on its floor, 1 in the air, 2 landing
	s32 anim;
	f32 floor;   // its feet's height the last time it stood
	f32 top;     // the ledge's
	f32 leave;   // the clip's frame off the ledge
	f32 land;    // and on the floor
} g_ZombieCodFall[MAX_BOTS];

/**
 * A zombie up a ladder. Black Ops II's zombies have no ladders: at a ledge
 * they jump up it (ai_zombie_jump_up_48 to _222, a crawler's
 * ai_zombie_crawl_jump_up_*) and down it the other way (zombiesCodFall(),
 * which a ladder's head already was). A zombie whose way brings it onto a
 * ladder's foot (chr->onladder, standing on its floor) is held there instead
 * of climbing (zombiesTickClimb(), the host's), plays the jump up nearest the
 * ladder's height, carried by its root with its rise stretched to the
 * ledge's (zombiesCodClimbPlace()), and is put on the floor at the top past
 * the ladder's face as it ends. A ladder with no floor found behind its head
 * is climbed as before.
 */
struct prop *chrGetTargetProp(struct chrdata *chr);
void botApplyHold(struct chrdata *chr, struct coord *pos, RoomNum *room, f32 angle);

#define ZOMBIE_CLIMB_MIN  60.0f // a ladder reaching at least this far over its feet
#define ZOMBIE_CLIMB_PAST 45.0f // the floor at the top looked for this far past the ladder's face

// (g_ZombieClimb is above zombiesSpawnOne(), which clears it)


static Mtxf g_ZombieCodPlace[MAX_BOTS]; // each one's place as last drawn: its vertices' units into the world
static bool g_ZombieCodPlaced[MAX_BOTS];

static bool zombiesCodIsDeath(s32 anim)
{
	return (anim >= ZMCHAR_DEATH_A && anim <= ZMCHAR_DEATH_D) || anim == ZMCHAR_CRAWL_DEATH_A || anim == ZMCHAR_CRAWL_DEATH_B;
}

// Whether a player who is up stands in a zombie's reach
static bool zombiesCodNearPlayer(struct chrdata *chr)
{
	s32 p;

	for (p = 0; p < PLAYERCOUNT(); p++) {
		struct prop *pp = g_Vars.players[p] ? g_Vars.players[p]->prop : NULL;

		if (pp && zombiesPlayerIsUp(p)) {
			const f32 dx = pp->pos.x - chr->prop->pos.x;
			const f32 dy = pp->pos.y - chr->prop->pos.y;
			const f32 dz = pp->pos.z - chr->prop->pos.z;

			if (dx * dx + dz * dz < ZOMBIE_COD_REACH * ZOMBIE_COD_REACH && dy < 120 && dy > -120) {
				return true;
			}
		}
	}

	return false;
}

/**
 * bot.c asks, turning a zombie: whether it turns to face its target (a player
 * up close, in sight) or the way it is walking, as Call of Duty's zombies do.
 * Twice a swipe's reach, so it has turned by the time it swings.
 */
bool zombiesFacesTarget(struct chrdata *chr)
{
	struct prop *target = chr->target != -1 ? chrGetTargetProp(chr) : NULL;
	f32 dx;
	f32 dy;
	f32 dz;

	if (target == NULL || chr->prop == NULL) {
		return false;
	}

	dx = target->pos.x - chr->prop->pos.x;
	dy = target->pos.y - chr->prop->pos.y;
	dz = target->pos.z - chr->prop->pos.z;

	return dx * dx + dz * dz < ZOMBIE_COD_REACH * ZOMBIE_COD_REACH * 4 && dy < 150 && dy > -150
		&& chr->aibot && chr->aibot->targetinsight;
}

static void zombiesCodSet(s32 i, s32 anim)
{
	if (g_ZombieCod[i].anim == anim) {
		return;
	}

	g_ZombieCod[i].from = g_ZombieCod[i].anim;
	g_ZombieCod[i].fromframe = g_ZombieCod[i].frame;
	g_ZombieCod[i].blend = 1;
	g_ZombieCod[i].anim = anim;
	g_ZombieCod[i].frame = 0;
}

static bool zombiesCodBarrier(s32 i, struct chrdata *chr, s32 *anim, f32 *frame);
static void zombiesCodBarrierPlace(s32 i, Mtxf *world);

// Up a ledge: facing the ladder from where it stood, its clip's root motion
// carrying it, the rise stretched to the ledge's (zombiesTickClimb())
static void zombiesCodClimbPlace(s32 i, Mtxf *world)
{
	const f32 s = sinf(g_ZombieClimb[i].theta);
	const f32 c = cosf(g_ZombieClimb[i].theta);
	f32 root[3] = { 0, 0, 0 };
	f32 q[4];

	world->m[0][0] = s * ZOMBIE_COD_SCALE / 4.0f;
	world->m[0][1] = 0;
	world->m[0][2] = c * ZOMBIE_COD_SCALE / 4.0f;
	world->m[1][0] = c * ZOMBIE_COD_SCALE / 4.0f;
	world->m[1][1] = 0;
	world->m[1][2] = -s * ZOMBIE_COD_SCALE / 4.0f;
	world->m[2][0] = 0;
	world->m[2][1] = ZOMBIE_COD_SCALE / 4.0f;
	world->m[2][2] = 0;

	zmcharAnimRoot(g_ZombieCod[i].anim, g_ZombieCod[i].frame, root, q);

	// the clip's root is where the body is drawn from: the frame's x and y
	// on the floor, its z up the ledge, stretched
	world->m[3][0] = g_ZombieClimb[i].from.x + (s * root[0] + c * root[1]) * ZOMBIE_COD_SCALE;
	world->m[3][1] = g_ZombieClimb[i].from.y + root[2] * g_ZombieClimb[i].stretch * ZOMBIE_COD_SCALE;
	world->m[3][2] = g_ZombieClimb[i].from.z + (c * root[0] - s * root[1]) * ZOMBIE_COD_SCALE;
}

static void zombiesCodSet(s32 i, s32 anim);

/**
 * zombiesTick()'s, the host's, for each zombie before its orders: a jump up
 * a ladder begun (its foot reached), moved on, and ended on the floor at the
 * top. True while it is held for one.
 */
static bool zombiesTickClimb(s32 i, struct chrdata *chr, struct prop *target)
{
	struct coord n;
	struct coord above;
	struct coord land;
	RoomNum aboverooms[8];
	f32 top = -1e9f;
	f32 face = 0;
	f32 ground;
	f32 rise;
	f32 forward;
	f32 len;
	f32 d;
	s32 anim;
	s32 k;

	if (g_ZombieClimb[i].on) {
		g_ZombieClimb[i].t60 += g_Vars.lvupdate60;

		if (g_ZombieClimb[i].t60 < g_ZombieClimb[i].len60) {
			if (chr->aibot->command != AIBOTCMD_HOLD) {
				botApplyHold(chr, &chr->prop->pos, chr->prop->rooms, g_ZombieClimb[i].theta);
			}

			if (target) {
				botSetTarget(chr, target - g_Vars.props);
			}

			return true;
		}

		// up: on the floor at the top, facing on, and after the players again
		g_ZombieClimb[i].on = false;
		chrMoveToPos(chr, &g_ZombieClimb[i].to, g_ZombieClimb[i].torooms, g_ZombieClimb[i].theta, true);
		return false;
	}

	if (!chr->onladder || chr->manground - chr->ground > 5 || g_ZombieAtBarrier[i] >= 0
			|| zmcharAnimFrames(ZMCHAR_JUMPUP) <= 0) {
		return false;
	}

	// the ladder's head: its tiles stacked, each asked again above the last
	for (k = 0; k < 8; k++) {
		const f32 base = k == 0 ? chr->manground + 1 : top + 5;
		struct coord kn;
		f32 kface;
		f32 ktop;

		if (!cdFindLadderDist(&chr->prop->pos, chr->radius * 2.5f, base + 40 - chr->prop->pos.y,
					base - chr->prop->pos.y, chr->prop->rooms, GEOFLAG_LADDER, &kn, &kface, &ktop)
				|| ktop <= top) {
			break;
		}

		if (k == 0) {
			n = kn;
			face = kface;
		}

		top = ktop;
	}

	if (k == 0 || top - chr->manground < ZOMBIE_CLIMB_MIN) {
		return false;
	}

	len = sqrtf(n.x * n.x + n.z * n.z);

	if (len <= 0) {
		return false;
	}

	n.x /= len;
	n.z /= len;

	// the floor at the top, past the ladder's face (the normal faces the
	// zombie: the ledge is the other way)
	above = chr->prop->pos;
	above.y = top + 30;
	func0f065e74(&chr->prop->pos, chr->prop->rooms, &above, aboverooms);

	anim = -1;
	rise = 0;
	forward = 0;
	ground = -1e9f;

	for (d = face + ZOMBIE_CLIMB_PAST; d <= face + ZOMBIE_CLIMB_PAST * 3; d += ZOMBIE_CLIMB_PAST) {
		land = above;
		land.x -= n.x * d;
		land.z -= n.z * d;
		func0f065e74(&above, aboverooms, &land, g_ZombieClimb[i].torooms);
		land.y = top + 30;
		ground = cdFindGroundAtCyl(&land, chr->radius, g_ZombieClimb[i].torooms, NULL, NULL);

		if (ground > top - 60 && ground < top + 40) {
			break;
		}
	}

	if (ground <= top - 60 || ground >= top + 40) {
		return false;
	}

	anim = zmcharJumpUp((ground - chr->manground) / ZOMBIE_COD_SCALE, g_ZombieCrawler[i] != 0, &rise, &forward);

	if (anim < 0) {
		return false;
	}

	// where the clip itself ends, when that is further on than the floor found
	if (forward * ZOMBIE_COD_SCALE > d) {
		struct coord further = above;

		further.x -= n.x * forward * ZOMBIE_COD_SCALE;
		further.z -= n.z * forward * ZOMBIE_COD_SCALE;
		func0f065e74(&above, aboverooms, &further, aboverooms);
		further.y = top + 30;

		if (fabsf(cdFindGroundAtCyl(&further, chr->radius, aboverooms, NULL, NULL) - ground) < 20) {
			land = further;
			roomsCopy(aboverooms, g_ZombieClimb[i].torooms);
		}
	}

	g_ZombieClimb[i].on = true;
	g_ZombieClimb[i].anim = anim;
	g_ZombieClimb[i].t60 = 0;
	g_ZombieClimb[i].len60 = (s32)(zmcharAnimFrames(anim) * 60 / zmcharAnimRate(anim));
	g_ZombieClimb[i].from = chr->prop->pos;
	g_ZombieClimb[i].from.y = chr->manground;
	g_ZombieClimb[i].theta = atan2f(-n.x, -n.z);
	g_ZombieClimb[i].stretch = (ground - chr->manground) / ZOMBIE_COD_SCALE / MAX(1.0f, rise);
	g_ZombieClimb[i].to = land;
	g_ZombieClimb[i].to.y = ground + (chr->prop->pos.y - chr->manground);

	botApplyHold(chr, &chr->prop->pos, chr->prop->rooms, g_ZombieClimb[i].theta);

	sysLogPrintf(LOG_NOTE, "zombies: zombie %d jumps up a ledge of %.0f inches (clip %d, %.0f), from %.0f %.0f %.0f at frame %d",
			i, (ground - chr->manground) / ZOMBIE_COD_SCALE, anim, rise, chr->prop->pos.x, chr->manground, chr->prop->pos.z, g_Vars.lvframenum);

	return true;
}

static void zombiesReachHit(struct chrdata *zchr, s32 playernum);

// A swipe's blows, its clip's "fire" notes (frames at 30 a second), 0 ending
#define ZOMBIE_SWING_REST60 90 // a breath between one swipe and the next (45 was still too quick, a tester)
// A player in reach this long before the swing begins: begun the tick one
// came in reach, the walking swipe's blow (its frame 6, a fifth of a second)
// caught a player running past for a moment
#define ZOMBIE_SWING_WINDUP60 21

static const u8 *zombiesSwingFires(s32 anim)
{
	static const u8 walking[] = { 6, 0 };
	// one blow a swing: the standing swipe's and the crawler's second (39, 54)
	// landed hard on the first and a player went down in one swing
	static const u8 standing[] = { 25, 0 };
	static const u8 crawling[] = { 24, 0 };

	return anim == ZMCHAR_ATTACK ? walking : anim == ZMCHAR_STAND_ATTACK ? standing : crawling;
}

// zombiesCodOwnsBlows() asks, for chrPunchInflictDamage(): whether the swipes are these
bool zombiesCodOwnsBlows(void)
{
	return zombiesIsActive() && zmcharReady();
}

/**
 * zombiesTick()'s, the host's, each tick: every zombie's swipe begun, moved
 * on and its blows dealt, whether it is drawn or not
 */
static void zombiesTickSwings(void)
{
	const f32 secs = g_Vars.lvupdate60freal / 60.0f;
	s32 i;

	if (!zombiesCodOwnsBlows() || NET_CLIENT) {
		return;
	}

	for (i = 0; i < g_BotCount && i < MAX_BOTS; i++) {
		struct chrdata *zchr = g_MpBotChrPtrs[i];
		f32 moved;

		// no zombie carries a shield: a darksim (round 9 on) spawned with
		// one on an arena that has them, and could pick one up
		if (zchr && zchr->cshield > 0) {
			zchr->cshield = 0;
		}

		if (!zchr || !zchr->prop || chrIsDead(zchr) || zchr->actiontype == ACT_DIE || zchr->actiontype == ACT_DEAD
				|| g_ZombieAtBarrier[i] >= 0) {
			g_ZombieSwing[i].anim = -1;

			if (zchr && zchr->prop) {
				g_ZombieSwing[i].last = zchr->prop->pos;
			}

			continue;
		}

		moved = sqrtf((zchr->prop->pos.x - g_ZombieSwing[i].last.x) * (zchr->prop->pos.x - g_ZombieSwing[i].last.x)
				+ (zchr->prop->pos.z - g_ZombieSwing[i].last.z) * (zchr->prop->pos.z - g_ZombieSwing[i].last.z));
		g_ZombieSwing[i].last = zchr->prop->pos;

		if (g_ZombieSwing[i].anim < 0 && g_ZombieSwing[i].rest60 > 0) {
			g_ZombieSwing[i].rest60 -= g_Vars.lvupdate60;
			continue;
		}

		if (g_ZombieSwing[i].anim < 0) {
			if (!zombiesCodNearPlayer(zchr)) {
				g_ZombieSwing[i].near60 = 0;
				continue;
			}

			g_ZombieSwing[i].near60 += g_Vars.lvupdate60;

			if (g_ZombieSwing[i].near60 >= ZOMBIE_SWING_WINDUP60) {
				const bool walking = secs > 0 && moved / ZOMBIE_COD_SCALE / secs >= ZOMBIE_COD_IDLE;

				g_ZombieSwing[i].anim = g_ZombieCrawler[i] ? ZMCHAR_CRAWL_ATTACK : walking ? ZMCHAR_ATTACK : ZMCHAR_STAND_ATTACK;
				g_ZombieSwing[i].t60 = 0;
				g_ZombieSwing[i].fired = 0;
				g_ZombieSwing[i].near60 = 0;
			}

			continue;
		}

		{
			const s32 anim = g_ZombieSwing[i].anim;
			const u8 *fires = zombiesSwingFires(anim);
			f32 frame;
			s32 f;

			g_ZombieSwing[i].t60 += g_Vars.lvupdate60;
			frame = g_ZombieSwing[i].t60 * zmcharAnimRate(anim) / 60.0f;

			for (f = 0; fires[f]; f++) {
				if (frame >= fires[f] && !(g_ZombieSwing[i].fired & (1u << f))) {
					s32 p;

					g_ZombieSwing[i].fired |= 1u << f;

					// the nearest player still in its reach, and in sight of it
					for (p = 0; p < PLAYERCOUNT(); p++) {
						struct prop *pp = g_Vars.players[p] ? g_Vars.players[p]->prop : NULL;
						f32 dx;
						f32 dy;
						f32 dz;

						if (!pp || !zombiesPlayerIsUp(p)) {
							continue;
						}

						dx = pp->pos.x - zchr->prop->pos.x;
						dy = pp->pos.y - zchr->prop->pos.y;
						dz = pp->pos.z - zchr->prop->pos.z;

						// in its reach still, no more (it had 15 to spare)
						if (dx * dx + dz * dz < ZOMBIE_COD_REACH * ZOMBIE_COD_REACH
								&& dy < 120 && dy > -120
								&& cdTestLos04(&zchr->prop->pos, zchr->prop->rooms, &pp->pos,
									CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_BG)) {
							zombiesReachHit(zchr, p);
							break;
						}
					}
				}
			}

			if (frame >= zmcharAnimFrames(anim) - 1) {
				g_ZombieSwing[i].anim = -1;
				g_ZombieSwing[i].rest60 = ZOMBIE_SWING_REST60;
			}
		}
	}
}

// zombiesCodTick()'s: true while the zombie is jumping down a ledge, its
// animation and frame set
static bool zombiesCodFall(s32 i, struct chrdata *chr, bool dying, bool crawler)
{
	const f32 above = chr->manground - chr->ground;

	if (dying || chr->ground <= -100000) {
		g_ZombieCodFall[i].stage = 0;
		g_ZombieCodFall[i].floor = chr->manground;
		return false;
	}

	if (g_ZombieCodFall[i].stage == 0) {
		f32 drop;
		s32 anim;

		if (above < ZOMBIE_COD_DROP * ZOMBIE_COD_SCALE) {
			if (above < 5) {
				g_ZombieCodFall[i].floor = chr->manground;
			}

			return false;
		}

		drop = MAX(g_ZombieCodFall[i].floor, chr->manground) - chr->ground;
		anim = zmcharJumpDown(drop / ZOMBIE_COD_SCALE, crawler, &g_ZombieCodFall[i].leave, &g_ZombieCodFall[i].land);

		if (anim < 0) {
			return false;
		}

		g_ZombieCodFall[i].stage = 1;
		g_ZombieCodFall[i].anim = anim;
		g_ZombieCodFall[i].top = MAX(g_ZombieCodFall[i].floor, chr->manground);
		zombiesCodSet(i, anim);
		g_ZombieCodFall[i].floor = chr->manground;
	}

	if (g_ZombieCodFall[i].stage == 1) {
		if (above > 2) {
			// the clip's root as far down its fall as the zombie is down its own
			const f32 frac = MIN(1.0f, MAX(0.0f, (g_ZombieCodFall[i].top - chr->manground)
						/ MAX(1.0f, g_ZombieCodFall[i].top - chr->ground)));
			f32 pos[3];
			f32 q[4];
			f32 bottom;
			f32 f;

			zmcharAnimRoot(g_ZombieCodFall[i].anim, g_ZombieCodFall[i].land, pos, q);
			bottom = pos[2];

			for (f = g_ZombieCodFall[i].leave; f < g_ZombieCodFall[i].land; f += 0.25f) {
				zmcharAnimRoot(g_ZombieCodFall[i].anim, f, pos, q);

				if (pos[2] <= bottom * frac) {
					break;
				}
			}

			// never back up it: a floor further down found on the way
			g_ZombieCodFall[i].leave = f;
			g_ZombieCod[i].frame = f;
		} else {
			g_ZombieCodFall[i].stage = 2;
			g_ZombieCod[i].frame = g_ZombieCodFall[i].land;
		}
	} else {
		// down: its landing at the clip's own pace
		g_ZombieCod[i].frame += zmcharAnimRate(g_ZombieCodFall[i].anim) * g_Vars.lvupdate60freal / 60.0f;

		if (g_ZombieCod[i].frame >= zmcharAnimFrames(g_ZombieCodFall[i].anim) - 1 || above > ZOMBIE_COD_DROP * ZOMBIE_COD_SCALE) {
			g_ZombieCodFall[i].stage = 0;
			g_ZombieCodFall[i].floor = chr->manground;
			return false;
		}
	}

	if (g_ZombieCod[i].blend > 0) {
		g_ZombieCod[i].blend = MAX(0, g_ZombieCod[i].blend - g_Vars.lvupdate60freal / ZOMBIE_COD_BLEND60);
	}

	return true;
}

// The zombie's animation moved on by this frame's time, once a level frame;
// forced (0 or more) at a barrier, which keeps its own time (zombiesCodBarrier())
static void zombiesCodTick(s32 i, struct chrdata *chr, s32 forced, f32 forcedframe)
{
	const f32 secs = g_Vars.lvupdate60freal / 60.0f;
	const bool dying = chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD;
	const bool crawler = g_ZombieCrawler[i] != 0;
	f32 frames;
	f32 rate;

	// a new zombie in the slot: from its idle, where it is
	if (g_ZombieCod[i].chr != chr) {
		g_ZombieCod[i].chr = chr;
		g_ZombieCod[i].anim = ZMCHAR_IDLE;
		g_ZombieCod[i].frame = (f32)((i * 7) % 30); // not the game's random: a drawing never steers the game
		g_ZombieCod[i].blend = 0;
		g_ZombieCod[i].speed = 0;
		g_ZombieCod[i].last = chr->prop->pos;
		g_ZombieCod[i].tickframe = g_Vars.lvframenum;
		g_ZombieCodPlaced[i] = false;
		g_ZombieCodFall[i].stage = 0;
		g_ZombieCodFall[i].floor = chr->manground;
	}

	if (g_ZombieCod[i].tickframe == g_Vars.lvframenum) {
		return;
	}

	g_ZombieCod[i].tickframe = g_Vars.lvframenum;

	// its pace over the floor, inches a second, eased so a step or a stop
	// for a frame does not flick it from one animation to another
	if (secs > 0) {
		const f32 dx = chr->prop->pos.x - g_ZombieCod[i].last.x;
		const f32 dz = chr->prop->pos.z - g_ZombieCod[i].last.z;
		f32 now = sqrtf(dx * dx + dz * dz) / ZOMBIE_COD_SCALE / secs;

		// a jump across the map (a respawn) is no pace
		if (now > 600) {
			now = g_ZombieCod[i].speed;
		}

		g_ZombieCod[i].speed += (now - g_ZombieCod[i].speed) * MIN(1.0f, secs * 6);
	}

	g_ZombieCod[i].last = chr->prop->pos;

	// off a ledge: before a swipe or a barrier, which never happen in the air
	if (zombiesCodFall(i, chr, dying, crawler)) {
		return;
	}

	if (forced >= 0 && !dying) {
		zombiesCodSet(i, forced);
		g_ZombieCod[i].frame = forcedframe;

		if (g_ZombieCod[i].blend > 0) {
			g_ZombieCod[i].blend -= g_Vars.lvupdate60freal / ZOMBIE_COD_BLEND60;
			g_ZombieCod[i].blend = MAX(0, g_ZombieCod[i].blend);
		}

		return;
	}

	if (dying) {
		if (!zombiesCodIsDeath(g_ZombieCod[i].anim)) {
			zombiesCodSet(i, crawler ? ZMCHAR_CRAWL_DEATH_A + (s32)((i + g_Vars.lvframenum) % 2)
					: ZMCHAR_DEATH_A + (s32)((i + g_Vars.lvframenum) % 4));
		}
	} else if (NET_CLIENT && zombiesCodNearPlayer(chr)) {
		// in reach of a player: its swipes, on the move or standing (the
		// host's own are its swings, zombiesTickSwings(), which a client has not)
		zombiesCodSet(i, crawler ? ZMCHAR_CRAWL_ATTACK
				: g_ZombieCod[i].speed >= ZOMBIE_COD_IDLE ? ZMCHAR_ATTACK : ZMCHAR_STAND_ATTACK);
	} else if (crawler) {
		zombiesCodSet(i, g_ZombieCod[i].speed < ZOMBIE_COD_CRAWLFAST ? ZMCHAR_CRAWL : ZMCHAR_CRAWL_SPRINT);
	} else if (g_ZombieCod[i].speed < ZOMBIE_COD_IDLE) {
		zombiesCodSet(i, ZMCHAR_IDLE);
	} else if (g_ZombieCod[i].speed < ZOMBIE_COD_RUN) {
		zombiesCodSet(i, ZMCHAR_WALK);
	} else if (g_ZombieCod[i].speed < ZOMBIE_COD_SPRINT) {
		zombiesCodSet(i, ZMCHAR_RUN);
	} else {
		zombiesCodSet(i, ZMCHAR_SPRINT);
	}

	// as fast as it goes over the ground, or a death at its own pace
	rate = zmcharAnimRate(g_ZombieCod[i].anim);

	if (zmcharAnimSpeed(g_ZombieCod[i].anim) > 0) {
		f32 k = g_ZombieCod[i].speed / zmcharAnimSpeed(g_ZombieCod[i].anim);

		rate *= k < 0.6f ? 0.6f : k > 1.6f ? 1.6f : k;
	}

	frames = zmcharAnimFrames(g_ZombieCod[i].anim);
	g_ZombieCod[i].frame += rate * secs;

	if (frames > 1) {
		if (zombiesCodIsDeath(g_ZombieCod[i].anim)) {
			if (g_ZombieCod[i].frame > frames - 1) {
				g_ZombieCod[i].frame = frames - 1;
			}
		} else {
			while (g_ZombieCod[i].frame >= frames - 1) {
				g_ZombieCod[i].frame -= frames - 1;
			}
		}
	}

	if (g_ZombieCod[i].blend > 0) {
		g_ZombieCod[i].blend -= g_Vars.lvupdate60freal / ZOMBIE_COD_BLEND60;

		if (g_ZombieCod[i].blend < 0) {
			g_ZombieCod[i].blend = 0;
		}
	}
}

/**
 * Which of Tranzit's bodies and heads a zombie wears (zmchar.h's
 * ZMCHAR_LOOK): a hash of its bot slot and the round, the same on every
 * machine online without a word between them and never the game's random,
 * a new mix each round; and its legs, a crawler's or not
 */
static s32 zombiesCodLook(s32 i)
{
	const u32 h = (u32)(i + 1) * 2654435761u ^ (u32)g_ZombieRound * 40503u;

	return ZMCHAR_LOOK((h >> 8) % ZMCHAR_BODIES, (h >> 16) % ZMCHAR_HEADS, g_ZombieCrawler[i] != 0)
		| (g_ZombieGib[i] & ZGIB_LARM ? ZMCHAR_LOOK_NOLARM : 0)
		| (g_ZombieGib[i] & ZGIB_RARM ? ZMCHAR_LOOK_NORARM : 0)
		| (g_ZombieGib[i] & ZGIB_HEAD ? ZMCHAR_LOOK_NOHEAD : 0);
}

static void zombiesCodGibFresh(s32 i, struct chrdata *chr);

bool zombiesRenderCodZombie(struct modelrenderdata *renderdata, struct chrdata *chr)
{
	f32 theta;
	bool atbarrier;
	Mtxf world;
	Mtxf *mtx;
	s32 i;
	u32 room;

	if (!zombiesIsActive() || !chr || !chr->aibot || !chr->prop || chr->prop->type != PROPTYPE_CHR) {
		return false;
	}

	// put down at the start, not yet sent in: nothing to see
	if (zombiesHideCorpse(chr)) {
		return true;
	}

	if (!zmcharReady()) {
		return false;
	}

	for (i = 0; i < g_BotCount && i < MAX_BOTS && i < ZMCHAR_SLOTS; i++) {
		if (g_MpBotChrPtrs[i] == chr) {
			break;
		}
	}

	if (i >= g_BotCount || i >= MAX_BOTS || i >= ZMCHAR_SLOTS) {
		return false;
	}

	// all of it in the opaque pass
	if (!(renderdata->flags & MODELRENDERFLAG_OPA)) {
		return true;
	}

	{
		s32 forced = -1;
		f32 forcedframe = 0;

		atbarrier = zombiesCodBarrier(i, chr, &forced, &forcedframe);

		// up a ledge, as the game's tick has it (zombiesTickClimb())
		if (!atbarrier && g_ZombieClimb[i].on) {
			forced = g_ZombieClimb[i].anim;
			forcedframe = MIN(zmcharAnimFrames(forced) - 1, g_ZombieClimb[i].t60 * zmcharAnimRate(forced) / 60.0f);
		} else if (!atbarrier && g_ZombieSwing[i].anim >= 0) {
			// a swipe, as the game's tick has it (zombiesTickSwings())
			forced = g_ZombieSwing[i].anim;
			forcedframe = g_ZombieSwing[i].t60 * zmcharAnimRate(forced) / 60.0f;
		}

		zombiesCodTick(i, chr, forced, forcedframe);
	}

	// a body chrRender() fades (a corpse going, chrRender()'s alpha in
	// envcolour's low byte) goes at half way: this one is drawn solid
	if (renderdata->unk30 == 8 && (renderdata->envcolour & 0xff) < 0x80) {
		return true;
	}

	// Call of Duty's x (the way it faces), y (its left) and z (up), into the
	// world at its feet
	theta = chrGetInverseTheta(chr);
	mtx4LoadIdentity(&world);
	world.m[0][0] = sinf(theta) * ZOMBIE_COD_SCALE / 4.0f;
	world.m[0][2] = cosf(theta) * ZOMBIE_COD_SCALE / 4.0f;
	world.m[1][0] = cosf(theta) * ZOMBIE_COD_SCALE / 4.0f;
	world.m[1][1] = 0;
	world.m[1][2] = -sinf(theta) * ZOMBIE_COD_SCALE / 4.0f;
	world.m[2][2] = 0;
	world.m[2][1] = ZOMBIE_COD_SCALE / 4.0f;
	world.m[3][0] = chr->prop->pos.x;
	world.m[3][1] = chr->ground;
	world.m[3][2] = chr->prop->pos.z;

	// in the air off a ledge: at its feet, not on the floor it is falling to
	if (g_ZombieCodFall[i].stage == 1) {
		world.m[3][1] = chr->manground;
	}

	// at a barrier: in its frame, carried by the clip's own root motion
	if (atbarrier) {
		zombiesCodBarrierPlace(i, &world);
	} else if (g_ZombieClimb[i].on) {
		zombiesCodClimbPlace(i, &world);
	}

	// kept, for a shot's test against its bones (zombiesCodTestHit())
	g_ZombieCodPlace[i] = world;
	g_ZombieCodPlaced[i] = true;

	mtx = gfxAllocateMatrix();
	mtx4MultMtx4(camGetWorldToScreenMtxf(), &world, mtx);

	// the room's light on the body (not the colour the scenario marks a
	// zombie with, which renderdata carries: it came out yellow-green)
	room = (u32)chr->shadecol[0] << 24 | (u32)chr->shadecol[1] << 16 | (u32)chr->shadecol[2] << 8;

	renderdata->gdl = zmcharDraw(renderdata->gdl, i, zombiesCodLook(i),
			mtx, g_ZombieCod[i].anim, g_ZombieCod[i].frame,
			g_ZombieCod[i].from, g_ZombieCod[i].fromframe, g_ZombieCod[i].blend,
			0x80 + ((room >> 24) & 0xff) * 0x7f / 0xff,
			0x80 + ((room >> 16) & 0xff) * 0x7f / 0xff,
			0x80 + ((room >> 8) & 0xff) * 0x7f / 0xff);

	// an arm or the head newly shot off: thrown, as this machine sees it go
	zombiesCodGibFresh(i, chr);

	return true;
}

/**
 * An arm shot off flies (Call of Duty's c_zom_zombie_g_larmspawn and
 * _rarmspawn, as models/gib_*), tumbling, from the shoulder it left, falls to
 * the floor the zombie stood on and lies there a few seconds; a head popped
 * is a burst of blood. Each machine starts its own when it sees a zombie's
 * gibs change (zombiesCodGibFresh()), drawn after the props
 * (zombiesRenderGibs(), from zombiesRenderRayFx()).
 */
#define ZOMBIE_FLYING_MAX   12
#define ZOMBIE_FLYING_LIFE  240     // 60ths it lies before it goes
#define ZOMBIE_FLYING_ARM   60.0f   // an arm's length, its longest side
#define ZOMBIE_FLYING_GRAV  0.35f   // units a 60th a 60th

static struct {
	bool on;
	s32 mesh;          // 0 the left arm, 1 the right
	struct coord pos;
	struct coord vel;  // units a 60th
	f32 axis[3];       // its tumble
	f32 angle;
	f32 spin;          // radians a 60th
	f32 floor;
	f32 life60;
} g_ZombieFlying[ZOMBIE_FLYING_MAX];

static struct zombiemesh g_ZombieGibMeshes[2];
static u8 g_ZombieGibSeen[MAX_BOTS];
static s32 g_ZombieFlyingTick = -1;

// A place on the zombie as it was last drawn, from a joint of its own
static bool zombiesCodJointWorld(s32 i, const char *name, f32 up, struct coord *out)
{
	const f32 (*pose)[3] = zmcharSlotPose(i);
	const s32 j = zmcharJoint(name);
	const Mtxf *place = &g_ZombieCodPlace[i];
	f32 p[3];
	s32 c;

	if (pose == NULL || j < 0 || !g_ZombieCodPlaced[i]) {
		return false;
	}

	for (c = 0; c < 3; c++) {
		p[c] = pose[j][c] * 4;
	}

	p[2] += up * 4;

	out->x = place->m[0][0] * p[0] + place->m[1][0] * p[1] + place->m[2][0] * p[2] + place->m[3][0];
	out->y = place->m[0][1] * p[0] + place->m[1][1] * p[1] + place->m[2][1] * p[2] + place->m[3][1];
	out->z = place->m[0][2] * p[0] + place->m[1][2] * p[1] + place->m[2][2] * p[2] + place->m[3][2];

	return true;
}

// zombiesRenderCodZombie(), as it draws one: what has newly come off it, thrown
static void zombiesCodGibFresh(s32 i, struct chrdata *chr)
{
	u8 fresh;

	// respawned whole: what came off its last life is forgotten
	g_ZombieGibSeen[i] &= g_ZombieGib[i];
	fresh = g_ZombieGib[i] & ~g_ZombieGibSeen[i];
	s32 side;

	if (!fresh) {
		return;
	}

	g_ZombieGibSeen[i] |= fresh;

	for (side = 0; side < 2; side++) {
		struct coord at;
		s32 k;

		if (!(fresh & (side == 0 ? ZGIB_LARM : ZGIB_RARM))
				|| !zombiesCodJointWorld(i, side == 0 ? "j_shoulder_le" : "j_shoulder_ri", 0, &at)) {
			continue;
		}

		for (k = 0; k < ZOMBIE_FLYING_MAX && g_ZombieFlying[k].on; k++);

		if (k == ZOMBIE_FLYING_MAX) {
			k = (i + side) % ZOMBIE_FLYING_MAX;
		}

		g_ZombieFlying[k].on = true;
		g_ZombieFlying[k].mesh = side;
		g_ZombieFlying[k].pos = at;
		// out to its own side and up, away from where the shot came: a
		// hash of the slot for the rest, never the game's random
		g_ZombieFlying[k].vel.x = (at.x - chr->prop->pos.x) * 0.06f + ((i * 37) % 7 - 3) * 0.3f;
		g_ZombieFlying[k].vel.y = 3.2f;
		g_ZombieFlying[k].vel.z = (at.z - chr->prop->pos.z) * 0.06f + ((i * 53) % 7 - 3) * 0.3f;
		g_ZombieFlying[k].axis[0] = 0.6f;
		g_ZombieFlying[k].axis[1] = 0.3f;
		g_ZombieFlying[k].axis[2] = side ? 0.74f : -0.74f;
		g_ZombieFlying[k].angle = 0;
		g_ZombieFlying[k].spin = 0.18f + (i % 5) * 0.03f;
		g_ZombieFlying[k].floor = chr->ground + 4;
		g_ZombieFlying[k].life60 = 0;
	}

	// the head: a burst of blood where it was
	if (fresh & ZGIB_HEAD) {
		struct coord at;
		struct coord up = { 0, 1, 0 };
		s32 n;

		if (zombiesCodJointWorld(i, "j_head", 4, &at)) {
			for (n = 0; n < 3; n++) {
				sparksCreate(chr->prop->rooms[0], chr->prop, &at, &up, NULL, SPARKTYPE_FLESH_LARGE);
				sparksCreate(chr->prop->rooms[0], chr->prop, &at, &up, NULL, SPARKTYPE_BLOOD);
			}
		}
	}
}

Gfx *zombiesRenderGibs(Gfx *gdl)
{
	static const char *const files[2] = {
		"$E/zombies/models/gib_larmspawn/gib_larmspawn.obj",
		"$E/zombies/models/gib_rarmspawn/gib_rarmspawn.obj",
	};
	s32 k;

	// flown on once a level frame
	if (g_ZombieFlyingTick != g_Vars.lvframenum) {
		const f32 dt = g_Vars.lvupdate60freal;

		g_ZombieFlyingTick = g_Vars.lvframenum;

		for (k = 0; k < ZOMBIE_FLYING_MAX; k++) {
			if (!g_ZombieFlying[k].on) {
				continue;
			}

			g_ZombieFlying[k].life60 += dt;

			if (g_ZombieFlying[k].life60 > ZOMBIE_FLYING_LIFE) {
				g_ZombieFlying[k].on = false;
				continue;
			}

			if (g_ZombieFlying[k].pos.y > g_ZombieFlying[k].floor || g_ZombieFlying[k].vel.y > 0) {
				g_ZombieFlying[k].vel.y -= ZOMBIE_FLYING_GRAV * dt;
				g_ZombieFlying[k].pos.x += g_ZombieFlying[k].vel.x * dt;
				g_ZombieFlying[k].pos.y += g_ZombieFlying[k].vel.y * dt;
				g_ZombieFlying[k].pos.z += g_ZombieFlying[k].vel.z * dt;
				g_ZombieFlying[k].angle += g_ZombieFlying[k].spin * dt;

				// on the floor: it stops there
				if (g_ZombieFlying[k].pos.y <= g_ZombieFlying[k].floor && g_ZombieFlying[k].vel.y < 0) {
					g_ZombieFlying[k].pos.y = g_ZombieFlying[k].floor;
					g_ZombieFlying[k].vel.x = g_ZombieFlying[k].vel.z = 0;
				}
			}
		}
	}

	for (k = 0; k < ZOMBIE_FLYING_MAX; k++) {
		struct zombiemesh *zm;
		f32 longest;
		f32 s;
		f32 a[3];
		f32 c;
		f32 sn;
		f32 len;
		f32 mid[3];
		Mtxf local;
		Mtxf *mtx;
		s32 r;
		s32 col;

		if (!g_ZombieFlying[k].on) {
			continue;
		}

		zm = &g_ZombieGibMeshes[g_ZombieFlying[k].mesh];

		if (!zm->tried) {
			zombiesMeshBuildFrom(zm, files[g_ZombieFlying[k].mesh], g_ZombieFlying[k].mesh ? "a right arm" : "a left arm");
		}

		if (!zm->gdl) {
			continue;
		}

		longest = MAX(zm->width, MAX(zm->height, zm->depth));
		s = longest > 0 ? ZOMBIE_FLYING_ARM / longest / ZOMBIE_MESH_FIXED : 1;

		// its tumble: a turn about its axis (Rodrigues')
		len = sqrtf(g_ZombieFlying[k].axis[0] * g_ZombieFlying[k].axis[0] + g_ZombieFlying[k].axis[1] * g_ZombieFlying[k].axis[1]
				+ g_ZombieFlying[k].axis[2] * g_ZombieFlying[k].axis[2]);

		for (r = 0; r < 3; r++) {
			a[r] = g_ZombieFlying[k].axis[r] / len;
		}

		c = cosf(g_ZombieFlying[k].angle);
		sn = sinf(g_ZombieFlying[k].angle);
		mtx4LoadIdentity(&local);

		for (col = 0; col < 3; col++) {
			for (r = 0; r < 3; r++) {
				f32 v = a[r] * a[col] * (1 - c);

				if (r == col) {
					v += c;
				} else {
					// the cross term: +a[3-r-col] one way round, - the other
					const s32 o = 3 - r - col;
					const f32 sign = ((col + 1) % 3 == r) ? 1 : -1;

					v += sign * a[o] * sn;
				}

				local.m[col][r] = v * s;
			}
		}

		// its middle where it is
		mid[0] = zm->midx * ZOMBIE_MESH_FIXED;
		mid[1] = (zm->ymin + zm->height / 2) * ZOMBIE_MESH_FIXED;
		mid[2] = (zm->zmin + zm->depth / 2) * ZOMBIE_MESH_FIXED;

		local.m[3][0] = g_ZombieFlying[k].pos.x - (local.m[0][0] * mid[0] + local.m[1][0] * mid[1] + local.m[2][0] * mid[2]);
		local.m[3][1] = g_ZombieFlying[k].pos.y - (local.m[0][1] * mid[0] + local.m[1][1] * mid[1] + local.m[2][1] * mid[2]);
		local.m[3][2] = g_ZombieFlying[k].pos.z - (local.m[0][2] * mid[0] + local.m[1][2] * mid[1] + local.m[2][2] * mid[2]);

		mtx = gfxAllocateMatrix();
		mtx4MultMtx4(camGetWorldToScreenMtxf(), &local, mtx);
		gdl = zombiesBottleDraw(gdl, zm, mtx, 0xc0, 0xc0, 0xc0);
	}

	return gdl;
}

/**
 * A shot at one of Black Ops II's zombies is tested against its own bones as
 * it was last drawn (zmchar.c), not Perfect Dark's body under it, which
 * stands upright wherever the zombie leans, reaches or crawls: a crawler's
 * head is two feet in front of that body, and the air over it was its chest.
 * Each part a capsule on a bone (inches: the bone's two joints and a radius),
 * the head a ball over j_head along the neck. A crawler has no legs.
 */
struct zombiecodpart {
	const char *from;
	const char *to;
	f32 radius;
	s32 hitpart;
	bool legs;
};

static const struct zombiecodpart g_ZombieCodParts[] = {
	{ "j_mainroot",    "j_spinelower", 7.0f, HITPART_PELVIS,   false },
	{ "j_spinelower",  "j_spineupper", 7.5f, HITPART_TORSO,    false },
	{ "j_spineupper",  "j_neck",       4.5f, HITPART_TORSO,    false }, // slim: the head's ball is just over it
	{ "j_shoulder_le", "j_elbow_le",   3.0f, HITPART_LBICEP,   false },
	{ "j_elbow_le",    "j_wrist_le",   2.6f, HITPART_LFOREARM, false },
	{ "j_wrist_le",    "j_mid_le_1",   2.6f, HITPART_LHAND,    false },
	{ "j_shoulder_ri", "j_elbow_ri",   3.0f, HITPART_RBICEP,   false },
	{ "j_elbow_ri",    "j_wrist_ri",   2.6f, HITPART_RFOREARM, false },
	{ "j_wrist_ri",    "j_mid_ri_1",   2.6f, HITPART_RHAND,    false },
	{ "j_hip_le",      "j_knee_le",    4.0f, HITPART_LTHIGH,   true },
	{ "j_knee_le",     "j_ankle_le",   3.2f, HITPART_LSHIN,    true },
	{ "j_ankle_le",    "j_ball_le",    2.6f, HITPART_LFOOT,    true },
	{ "j_hip_ri",      "j_knee_ri",    4.0f, HITPART_RTHIGH,   true },
	{ "j_knee_ri",     "j_ankle_ri",   3.2f, HITPART_RSHIN,    true },
	{ "j_ankle_ri",    "j_ball_ri",    2.6f, HITPART_RFOOT,    true },
};

#define ZOMBIE_COD_HEADRADIUS 5.0f // inches: a ball 1.6 of the neck's length above j_head

// The ray's distance along itself to its nearest to the segment a-b, and how near
static f32 zombiesRaySegment(const f32 o[3], const f32 d[3], const f32 a[3], const f32 b[3], f32 *t)
{
	f32 ab[3];
	f32 ao[3];
	f32 dd = 0;
	f32 de = 0;
	f32 ee = 0;
	f32 df = 0;
	f32 ef = 0;
	f32 den;
	f32 s;
	f32 diff[3];
	f32 len = 0;
	s32 c;

	for (c = 0; c < 3; c++) {
		ab[c] = b[c] - a[c];
		ao[c] = o[c] - a[c];
		dd += d[c] * d[c];
		de += d[c] * ab[c];
		ee += ab[c] * ab[c];
		df += d[c] * ao[c];
		ef += ab[c] * ao[c];
	}

	den = dd * ee - de * de;
	s = den > 1e-6f ? (dd * ef - de * df) / den : 0;
	s = s < 0 ? 0 : s > 1 ? 1 : s;
	*t = dd > 0 ? (de * s - df) / dd : 0;

	if (*t < 0) {
		*t = 0;
		s = ee > 0 ? ef / ee : 0;
		s = s < 0 ? 0 : s > 1 ? 1 : s;
	}

	for (c = 0; c < 3; c++) {
		diff[c] = o[c] + d[c] * *t - (a[c] + ab[c] * s);
		len += diff[c] * diff[c];
	}

	return sqrtf(len);
}

// One of Perfect Dark's body's boxes for a hit part, for blood and bruises
static struct modelnode *zombiesChrBboxNode(struct model *model, s32 hitpart)
{
	struct modelnode *node = model->definition->rootnode;
	struct modelnode *any = NULL;

	while (node) {
		if ((node->type & 0xff) == MODELNODETYPE_BBOX) {
			union modelrodata *rodata = node->rodata;

			if (rodata->bbox.hitpart == hitpart) {
				return node;
			}

			if (any == NULL || rodata->bbox.hitpart == HITPART_TORSO) {
				any = node;
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

	return any;
}

/**
 * --zombies-test-shoot WHAT (head, chest or air: a foot over the head): with
 * --zombies-test-zombie's camera, a real shot from it every half second at
 * that point of the zombie it follows, as it was last drawn; the hit test
 * logs what it hit (part 0, none). For a run nobody is playing.
 */
static bool g_ZombieTestShooting = false;

static void zombiesTestShoot(void)
{
	static s32 what = -2;
	const char *arg;
	s32 b;

	if (what == -2) {
		arg = sysArgGetString("--zombies-test-shoot");
		what = arg == NULL ? -1 : strcmp(arg, "head") == 0 ? 0 : strcmp(arg, "chest") == 0 ? 1 : 2;
	}

	if (what < 0 || g_Vars.lvframenum % 30 != 0 || !zmcharReady()) {
		return;
	}

	for (b = 0; b < g_BotCount && b < ZMCHAR_SLOTS; b++) {
		struct chrdata *zchr = g_MpBotChrPtrs[b];
		const f32 (*pose)[3] = zmcharSlotPose(b);
		const s32 head = zmcharJoint("j_head");
		const s32 chest = zmcharJoint("j_spineupper");
		f32 p[3];
		struct coord target;
		struct coord pos3d;
		struct coord dir3d;
		struct coord pos2d;
		struct coord dir2d;
		Mtxf *place = &g_ZombieCodPlace[b];
		s32 c;

		if (!zchr || !zchr->prop || chrIsDead(zchr) || !g_ZombieCodPlaced[b] || pose == NULL || head < 0 || chest < 0
				|| g_ZombieCod[b].chr != zchr || (g_ZombieAtBarrier[b] >= 0) != (sysArgGetInt("--zombies-test-atbarrier", 0) != 0)) {
			continue;
		}

		for (c = 0; c < 3; c++) {
			p[c] = (what == 1 ? pose[chest][c] : pose[head][c]) * 4;
		}

		if (what == 2) {
			p[2] += 14 * 4;
		} else if (what == 0) {
			p[2] += 3 * 4;
		}

		target.x = place->m[0][0] * p[0] + place->m[1][0] * p[1] + place->m[2][0] * p[2] + place->m[3][0];
		target.y = place->m[0][1] * p[0] + place->m[1][1] * p[1] + place->m[2][1] * p[2] + place->m[3][1];
		target.z = place->m[0][2] * p[0] + place->m[1][2] * p[1] + place->m[2][2] * p[2] + place->m[3][2];

		pos3d = g_Vars.currentplayer->cam_pos;
		dir3d.x = target.x - pos3d.x;
		dir3d.y = target.y - pos3d.y;
		dir3d.z = target.z - pos3d.z;
		guNormalize(&dir3d.x, &dir3d.y, &dir3d.z);

		mtx4TransformVec(camGetWorldToScreenMtxf(), &pos3d, &pos2d);
		mtx4RotateVec(camGetWorldToScreenMtxf(), &dir3d, &dir2d);

		g_ZombieTestShooting = true;
		shotCalculateHits(HAND_RIGHT, true, &pos2d, &dir2d, &pos3d, &dir3d, 0, 4294836224, false);
		g_ZombieTestShooting = false;
		return;
	}
}

bool zombiesCodTestHit(struct prop *prop, struct shotdata *shotdata)
{
	struct chrdata *chr = prop->chr;
	const f32 (*pose)[3];
	Mtxf view;
	f32 o[3];
	f32 d[3];
	f32 dlen;
	f32 best = 1e30f;
	s32 hitpart = 0;
	f32 inch;
	bool crawler;
	s32 i;
	s32 k;

	if (!zombiesIsActive() || !chr || !chr->aibot || !zmcharReady()) {
		return false;
	}

	i = zombiesBotIndex(chr);

	if (i < 0 || i >= ZMCHAR_SLOTS || !g_ZombieCodPlaced[i] || g_ZombieCod[i].chr != chr
			|| (pose = zmcharSlotPose(i)) == NULL) {
		return false;
	}

	crawler = g_ZombieCrawler[i] != 0;

	// the zombie's own inches into the camera's space, where the shot is
	mtx4MultMtx4(camGetWorldToScreenMtxf(), &g_ZombieCodPlace[i], &view);

	for (k = 0; k < 3; k++) {
		o[k] = shotdata->gunpos2d.f[k];
		d[k] = shotdata->gundir2d.f[k];
	}

	dlen = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);

	if (dlen <= 0) {
		return true;
	}

	for (k = 0; k < 3; k++) {
		d[k] /= dlen;
	}

	// an inch in the camera's space: the place's scale, four units an inch
	inch = 4.0f * sqrtf(view.m[0][0] * view.m[0][0] + view.m[0][1] * view.m[0][1] + view.m[0][2] * view.m[0][2]);

	for (k = 0; k <= (s32)ARRAYCOUNT(g_ZombieCodParts); k++) {
		s32 ja;
		s32 jb;
		f32 a[3];
		f32 b[3];
		f32 r;
		f32 t;
		f32 near;
		s32 part;
		s32 c;

		if (k < (s32)ARRAYCOUNT(g_ZombieCodParts)) {
			const struct zombiecodpart *p = &g_ZombieCodParts[k];

			if (p->legs && crawler) {
				continue;
			}

			// an arm shot off is not there to be shot again
			if (((g_ZombieGib[i] & ZGIB_LARM) && (p->hitpart == HITPART_LBICEP || p->hitpart == HITPART_LFOREARM || p->hitpart == HITPART_LHAND))
					|| ((g_ZombieGib[i] & ZGIB_RARM) && (p->hitpart == HITPART_RBICEP || p->hitpart == HITPART_RFOREARM || p->hitpart == HITPART_RHAND))) {
				continue;
			}

			ja = zmcharJoint(p->from);
			jb = zmcharJoint(p->to);
			r = p->radius;
			part = p->hitpart;
		} else {
			if (g_ZombieGib[i] & ZGIB_HEAD) {
				continue;
			}

			// the head: a ball over j_head, along the neck
			ja = zmcharJoint("j_neck");
			jb = zmcharJoint("j_head");
			r = ZOMBIE_COD_HEADRADIUS;
			part = HITPART_HEAD;
		}

		if (ja < 0 || jb < 0) {
			continue;
		}

		for (c = 0; c < 3; c++) {
			const f32 pa = pose[ja][c] * 4;
			const f32 pb = pose[jb][c] * 4;

			a[c] = pa;
			b[c] = pb;
		}

		if (part == HITPART_HEAD) {
			f32 head[3];

			for (c = 0; c < 3; c++) {
				head[c] = b[c] + (b[c] - a[c]) * 0.6f;
			}

			memcpy(a, head, sizeof(a));
			memcpy(b, head, sizeof(b));
		}

		// both ends into the camera's space
		{
			f32 ca[3];
			f32 cb[3];

			for (c = 0; c < 3; c++) {
				ca[c] = view.m[0][c] * a[0] + view.m[1][c] * a[1] + view.m[2][c] * a[2] + view.m[3][c];
				cb[c] = view.m[0][c] * b[0] + view.m[1][c] * b[1] + view.m[2][c] * b[2] + view.m[3][c];
			}

			near = zombiesRaySegment(o, d, ca, cb, &t);
			r *= inch;

			if (near < r) {
				t -= sqrtf(r * r - near * near);
				t = t < 0 ? 0 : t;

				if (t < best) {
					best = t;
					hitpart = part;
				}
			}
		}
	}

	if (g_ZombieTestShooting) {
		sysLogPrintf(LOG_NOTE, "zombies: --zombies-test-shoot: bot %d%s hit at part %d", i, crawler ? " (a crawler)" : "", hitpart);
	}

	if (hitpart > 0) {
		struct hitthing thing;
		struct coord at;
		struct coord facing;
		f32 depth;

		at.x = o[0] + d[0] * best;
		at.y = o[1] + d[1] * best;
		at.z = o[2] + d[2] * best;
		depth = -at.z;

		if (depth < shotdata->distance) {
			struct coord world;
			struct modelnode *node = zombiesChrBboxNode(chr->model, hitpart);

			bzero(&thing, sizeof(thing));
			thing.texturenum = -1;
			mtx4TransformVec(camGetProjectionMtxF(), &at, &world);
			facing.x = -shotdata->gundir3d.x;
			facing.y = -shotdata->gundir3d.y;
			facing.z = -shotdata->gundir3d.z;

			if (node) {
				hitCreate(shotdata, prop, depth, hitpart, node, &thing, 0, NULL, chr->model, true,
						chrGetShield(chr) > 0.0f, &world, &facing);
			}
		}
	}

	return true;
}

// chrRender()'s, after the body in its opaque pass: the bottle in the hand
// A mesh held in an arm's hand (a frag, a perk's bottle): at the middle of the
// palm, in the hand's own turn, its OBJ's tenths of an inch to model units
static Gfx *zombiesBodyMeshInHand(Gfx *gdl, struct chrdata *chr, struct model *model,
		struct zombiebodyarm *arm, struct zombiemesh *zm)
{
	Mtxf *mtx = model->matrices;
	Mtxf *out;
	f32 e[3];
	f32 h[3];
	f32 palm[3];
	f32 lower;
	f32 scale;
	s32 i;
	s32 j;

	for (i = 0; i < 3; i++) {
		e[i] = mtx[arm->elbow[0]].m[3][i];
		h[i] = mtx[arm->hand].m[3][i];
	}

	lower = sqrtf((h[0] - e[0]) * (h[0] - e[0]) + (h[1] - e[1]) * (h[1] - e[1]) + (h[2] - e[2]) * (h[2] - e[2]));
	scale = sqrtf(mtx[arm->hand].m[1][0] * mtx[arm->hand].m[1][0]
			+ mtx[arm->hand].m[1][1] * mtx[arm->hand].m[1][1]
			+ mtx[arm->hand].m[1][2] * mtx[arm->hand].m[1][2]);

	if (lower <= 0 || scale <= 0) {
		return gdl;
	}

	for (i = 0; i < 3; i++) {
		palm[i] = h[i] + (h[i] - e[i]) * ZOMBIE_BODYDRINK_PALM * scale / lower;
	}

	out = gfxAllocateMatrix();

	for (j = 0; j < 3; j++) {
		for (i = 0; i < 3; i++) {
			out->m[j][i] = (mtx[arm->hand].m[0][i] * g_ZombieBottleObjAxes[0][j]
				+ mtx[arm->hand].m[1][i] * g_ZombieBottleObjAxes[1][j]
				+ mtx[arm->hand].m[2][i] * g_ZombieBottleObjAxes[2][j]) * arm->perinch / 10.0f;
		}

		out->m[j][3] = 0;
	}

	out->m[3][0] = palm[0];
	out->m[3][1] = palm[1];
	out->m[3][2] = palm[2];
	out->m[3][3] = 1;

	return zombiesBottleDraw(gdl, zm, out,
			0x80 + (chr->shadecol[0] >> 1), 0x80 + (chr->shadecol[1] >> 1), 0x80 + (chr->shadecol[2] >> 1));
}

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

	// a grenade in the hand instead, being thrown
	if (t < 0) {
		bool inhand = false;

		if (zombiesBodyThrowing(chr, &inhand) && inhand && zombiesFragMesh(0) && zombiesBodyArm(model, &arm)) {
			gdl = zombiesBodyMeshInHand(gdl, chr, model, &arm, zombiesFragMesh(0));
		}

		return gdl;
	}

	// Black Ops II's drink on the body (ZTCLIP_DRINK): the bottle in the hand
	// that lifts it to the mouth from its pick-up to its throw, its cap on it
	// until it comes off. The clip's note says the left (anim_gunhand), but
	// retargeted onto Perfect Dark's body it is the right arm that drinks:
	// in the left the bottle hung at the hip.
	if (zombiesDrinkClipOn()) {
		if (t >= ZOMBIE_DRINKBODY_PICKUP && t < ZOMBIE_DRINKBODY_THROW
				&& zombiesBodyArmOf(model, &arm, MODELPART_CHR_RIGHTHAND)) {
			for (part = 0; part < ZOMBIE_DRINK_PARTS; part++) {
				struct zombiemesh *zm = zombiesBottleMesh(perk, part);

				// the bottle's second part is its cap
				if (zm && (part == 0 || t < ZOMBIE_DRINKBODY_UNCAP)) {
					gdl = zombiesBodyMeshInHand(gdl, chr, model, &arm, zm);
				}
			}
		}

		return gdl;
	}

	if (t >= ZOMBIE_BODYDRINK_THROW || !zombiesBodyArm(model, &arm)) {
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

	// an arena's own layout: its machines where it has them, and no others
	if (zombiesLayout()) {
		const struct zombielayout *l = zombiesLayout();

		for (perk = ZOMBIEPERK_QUICKREVIVE; perk < ZOMBIEPERK_COUNT; perk++) {
			struct pad pad;
			struct coord back;

			if (l->perkpad[perk] < 0) {
				continue;
			}

			padUnpack(l->perkpad[perk], PADFIELD_POS | PADFIELD_ROOM, &pad);
			back.x = l->perkback[perk][0];
			back.y = 0;
			back.z = l->perkback[perk][1];

			// walldist puts the machine on the pad itself (zombiesPlaceMachineObj())
			if (zombiesPlacePerkMachine(perk, &pad, ZOMBIE_PERK_HALFDEPTH * zombiesMachineExtrascale(perk) / ZOMBIE_JUGG_SCALE + 8, &back)) {
				sysLogPrintf(LOG_NOTE, "zombies: %s at the arena's own spot", g_ZombiePerks[perk].name);
			}
		}

		return;
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
static bool zombiesMachinesClear(struct coord *pos, f32 radius, struct defaultobj *self);

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

	return zombiesMachinesClear(pos, radius, self);
}

/**
 * A spot picked by hand wins over a spawn pad under it (Sewers' Speed Cola at
 * the end of a dead end with a pad in it): the pad comes off the spawn list,
 * the same pad on every machine, so nobody spawns inside the machine and the
 * wall buys and barriers placed after it look elsewhere
 */
static void zombiesTakePadsUnder(struct coord *pos, f32 radius)
{
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	s32 i;

	for (i = n - 1; i >= 0; i--) {
		struct pad pad;
		f32 dx;
		f32 dz;

		padUnpack(g_SpawnPoints[i], PADFIELD_POS, &pad);
		dx = pad.pos.x - pos->x;
		dz = pad.pos.z - pos->z;

		if (dx * dx + dz * dz < (radius + 30) * (radius + 30) && fabsf(pad.pos.y - pos->y) < 200) {
			sysLogPrintf(LOG_NOTE, "zombies: spawn pad %d under a picked machine; off the list", g_SpawnPoints[i]);
			g_SpawnPoints[i] = g_SpawnPoints[--n];
		}
	}

	g_NumSpawnPoints = n;
}

static bool zombiesMachinesClear(struct coord *pos, f32 radius, struct defaultobj *self)
{
	s32 i;

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
	// Warehouse's Pack-a-Punch against the long slanted wall (30 degrees),
	// in the middle between its two pillars: the wall's nearest point to
	// where the player stood (794, 1857), found by a fan of rays
	{ 0x3c, ZOMBIE_PAP_ID,         { 794, 158, 1857 },     { 1012, 158, 1480 },    ZPICK_WALL   }, // Warehouse
	// Sewers' Speed Cola at the end of the short dead end off the brick room
	// (its back wall at z -1276); its Juggernog in the red room's corner,
	// the near wall (x -3076) meeting the far one (z 1400)
	{ 0x42, ZOMBIEPERK_SPEEDCOLA,  { -2941, 238, -913 },   { -2930, 238, -1276 },  ZPICK_WALL   }, // Sewers
	{ 0x42, ZOMBIEPERK_JUGGERNOG,  { -3106, -341, 981 },   { -3080, -341, 1396 },  ZPICK_CORNER }, // Sewers
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

	if (!zombiesMachinesClear(&pos, radius, obj)) {
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

	if (fabsf(obj->prop->pos.y - (floor + lift)) > 30) {
		return false;
	}

	zombiesTakePadsUnder(&obj->prop->pos, radius);

	return true;
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

	// an arena's own layout: every machine stays where it put it, and
	// Pack-a-Punch at its own spot
	if (zombiesLayout()) {
		const struct zombielayout *l = zombiesLayout();

		g_ZombiePapPlaced = false;

		if (l->pappad >= 0) {
			struct pad pad;
			struct coord back;

			bzero(&g_ZombiePapObj, sizeof(g_ZombiePapObj));
			padUnpack(l->pappad, PADFIELD_POS | PADFIELD_ROOM, &pad);
			back.x = l->papback[0];
			back.y = 0;
			back.z = l->papback[1];
			g_ZombiePapPlaced = zombiesPlaceMachineObj(ZOMBIE_PAP_ID, &g_ZombiePapObj, &pad,
					ZOMBIE_PERK_HALFDEPTH * zombiesMachineExtrascale(ZOMBIE_PAP_ID) / ZOMBIE_JUGG_SCALE + 8, &back);
		}

		zombiesRaiseSolid(g_ZombieBox);
		return;
	}

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
			|| g_ZombieNearDowned[playernum] >= 0 || zombiesPerksFull(playernum)) {
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

	// not with a grenade in the hand
	if (g_ZombieThrowStage[playernum] != ZTHROW_NONE) {
		return;
	}

	g_ZombiePerkNeedsRelease[playernum] = true;
	g_ZombiePerkHeld60[playernum] = 0;
	cost = zombiesPerkCost(near);

	if (g_ZombiePoints[playernum] < cost) {
		if (!zombiesPlayCodSound(ZMSFX_PERK_DENY, NULL)) {
			zombiesPlaySound(SFX_MENU_ERROR, 0.4f);
		}

		return;
	}

	g_ZombiePoints[playernum] -= cost;
	zombiesAddPopup(playernum, -cost);
	zombiesPerkBought(near, zombiesPerkMachineProp(near));

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

	// the drink's belch is Black Ops II's (zombiesTickJuggernog()); the
	// game's own pickup only when its file is not there
	if (!quiet && zmSfxSound(ZMSFX_PERK_BELCH) <= 0) {
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

/**
 * Call of Duty's legs. Black Ops II's player runs 190 inches a second (g_speed),
 * and Perfect Dark's at full speed is already that: some 494 units a second,
 * where a player's eye at 159 units against Call of Duty's 60 inches makes an
 * inch 2.65 units, 190 inches 503. ZOMBIE_COD_WALK is 1 so. It was 1.66 from
 * the dive's body measured as Call of Duty's (divebake.py's units to the
 * inch), and played far too fast: the eye is the size that matches. Then
 * Perfect Dark's own ramp to a quarter more after three seconds at full speed
 * is not Zombies' (it ran past Call of Duty's), and
 * Perfect Dark's own ramp to a quarter more after three seconds at full speed
 * (speedboost) is not Zombies'. Sprint is in its place: pressed (Shift, the
 * left stick) while going forward on foot it is half as fast again forwards,
 * Black Ops II's player_sprintSpeedScale, for four seconds (eight with
 * Stamin-Up), and ends on letting forward go, a crouch, aiming, firing or the
 * breath running out, which comes back a 60th a 60th while not sprinting.
 *
 * Its state is in the two player fields Perfect Dark's ramp had, which online
 * already go in every snapshot and come back in a client's replays
 * (netsnap.c, netpredict.c): speedboost the forward speed's scale, easing to
 * the sprint's and back, and speedmaxtime60 the breath, 1 + 60ths used while
 * sprinting and minus the 60ths used when not.
 */
#define ZOMBIE_SPRINT_SCALE    1.5f
#define ZOMBIE_SPRINT_TIME     240   // 60ths of sprint in a breath
#define ZOMBIE_SPRINT_START    30    // 60ths of it needed to start
#define ZOMBIE_SPRINT_EASE     0.05f // the scale's change a 60th

static bool g_ZombieSprintPress[MAX_PLAYERS];

/**
 * The gun in hand's weight on the legs, as Black Ops II's weapon files have
 * it (Mob of the Dead's dump): moveSpeedScale on foot - pistols, SMGs,
 * shotguns and the knife 1, rifles and snipers 0.95, LMGs and launchers 0.9,
 * the Death Machine 0.6 - and adsMoveSpeedScale down the sights, pistols and
 * SMGs 2, rifles 1, the LSAT 0.8. Black Ops II multiplies that second by a
 * speed of its own for aiming, which is not in the weapon files; it is taken
 * as ZOMBIE_ADS_SPEED, so a rifle aimed goes at some half its pace and a
 * pistol at nine tenths. Perfect Dark's guns take the class of the nearest
 * Black Ops II one.
 */
#define ZOMBIE_ADS_SPEED 0.45f

struct zombiegunlegs {
	s32 weaponnum;
	f32 move;
	f32 ads;
};

static const struct zombiegunlegs g_ZombieGunLegs[] = {
	// rifles and snipers (AK-47, FAL, M14, DSR-50, Barrett)
	{ WEAPON_DRAGON,         0.95f, 1.0f },
	{ WEAPON_K7AVENGER,      0.95f, 1.0f },
	{ WEAPON_AR34,           0.95f, 1.0f },
	{ WEAPON_SUPERDRAGON,    0.95f, 1.0f },
	{ WEAPON_SNIPERRIFLE,    0.95f, 1.0f },
	{ WEAPON_FARSIGHT,       0.95f, 1.0f },
	{ WEAPON_CROSSBOW,       0.95f, 1.0f },
	{ WEAPON_KF7SPECIAL,     0.95f, 1.0f },
	{ WEAPON_AR53,           0.95f, 1.0f },
	// light machine guns (the LSAT)
	{ WEAPON_RCP120,         0.9f,  0.8f },
	{ WEAPON_REAPER,         0.9f,  0.8f },
	// launchers (the RPG)
	{ WEAPON_DEVASTATOR,     0.9f,  1.0f },
	{ WEAPON_ROCKETLAUNCHER, 0.9f,  1.0f },
	{ WEAPON_SLAYER,         0.9f,  1.0f },
	// shotguns (the Rottweil 72 aims as a rifle; the 870 and Saiga as SMGs)
	{ WEAPON_SHOTGUN,        1.0f,  2.0f },
};

static f32 zombiesGunMoveScale(void)
{
	struct player *player = g_Vars.currentplayer;
	const s32 weaponnum = weaponHost(player->gunctrl.weaponnum);
	f32 move = 1.0f;
	f32 ads = 2.0f; // pistols, SMGs, the knife, the hands and anything else
	f32 aimed;
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_ZombieGunLegs); i++) {
		if (g_ZombieGunLegs[i].weaponnum == weaponnum) {
			move = g_ZombieGunLegs[i].move;
			ads = g_ZombieGunLegs[i].ads;
			break;
		}
	}

	// down the sights: Perfect Dark's aim mode, or COD Style Aiming's sights
	if (player->insightaimmode || player->codaimfrac > 0.5f) {
		aimed = ZOMBIE_ADS_SPEED * ads;
		move *= aimed < 1.0f ? aimed : 1.0f;
	}

	return move;
}

// bondwalk.c asks: Call of Duty's legs, and Stamin-Up's quicker ones, for the
// current player
f32 zombiesMoveScale(void)
{
	s32 playernum = g_Vars.currentplayernum;

	// the game's end: on the floor, not going anywhere
	if (zombiesIsActive() && g_ZombieGameOver) {
		return 0;
	}

	// down, a crawl: the squat alone was still nearly a walk
	if (zombiesIsActive() && playernum >= 0 && playernum < MAX_PLAYERS && g_ZombieDowned[playernum]) {
		return ZOMBIE_DOWNED_SPEED;
	}

	if (!zombiesIsActive()) {
		return 1;
	}

	return ZOMBIE_COD_WALK * zombiesGunMoveScale()
		* (zombiesPlayerHasPerk(g_Vars.currentplayernum, ZOMBIEPERK_STAMINUP) ? ZOMBIE_STAMINUP_SPEED : 1);
}

/**
 * bondgun.c: how far into its sprint the current player's gun is, 0 to 1,
 * for the sprint's pose - the gun tipped down and swung across the body, as
 * Black Ops II's viewmodels hold theirs (sprint_in, _loop, _out, which are
 * made for its own guns and not ours, so the pose is laid on in code). It
 * follows the sprint's own ease (speedboost, ZOMBIE_SPRINT_EASE: a sixth of a
 * second in and out, where Black Ops II's sprintInTime is 0.15 to 0.3).
 */
f32 zombiesSprintGunFrac(void)
{
	f32 f;

	if (!zombiesIsActive() || !g_Vars.currentplayer) {
		return 0;
	}

	f = (g_Vars.currentplayer->speedboost - 1.0f) / (ZOMBIE_SPRINT_SCALE - 1.0f);
	f = f < 0 ? 0 : f > 1 ? 1 : f;

	return f * f * (3 - 2 * f);
}

/**
 * The crosshair from the hip (sight.c draws it): Perfect Dark's own small
 * cross in the player's sight colour, its four arms pushed out by the spread
 * as Black Ops II's are. It opens a little as the player moves and pops with
 * every round fired (a magazine going down, so an automatic's every shot
 * counts), settling back once the firing stops; crouched it is tighter,
 * prone tighter still, and a shotgun's is wider. Gone at the sights (the
 * game's own sight is drawn there), while sprinting, and while the gun is
 * down for a drink or the knife.
 *
 * A grenade held keeps it up and beats on the fuse from the pin: a pop a
 * second, then four in its last second, so the player knows when it will go.
 * Spreads are the sight's own units, before its size setting.
 */
#define ZOMBIE_XHAIR_MOVE    1.5f  // out at a full run
#define ZOMBIE_XHAIR_POP     0.8f  // out a round
#define ZOMBIE_XHAIR_BEAT    2.0f  // out a beat of a grenade's fuse
#define ZOMBIE_XHAIR_MAX     4.0f
#define ZOMBIE_XHAIR_SETTLE  9.0f  // the pop's fall a second (a fraction of it)
#define ZOMBIE_XHAIR_EASE    12.0f // the move's way to its target a second
#define ZOMBIE_XHAIR_BEAT60  60    // a beat a second ...
#define ZOMBIE_XHAIR_RUSH60  15    // ... and four in the fuse's last

static f32 g_ZombieXhairPop[MAX_PLAYERS];
static f32 g_ZombieXhairMove[MAX_PLAYERS];
static s32 g_ZombieXhairAmmo[MAX_PLAYERS];   // the magazines as last seen, with ...
static s32 g_ZombieXhairWeapon[MAX_PLAYERS]; // ... the gun they were (none at first: no pop)
static s32 g_ZombieXhairBeat[MAX_PLAYERS];   // the fuse's beats so far, -1 none

f32 zombiesGunLowerFrac(void);

// Which beat of the fuse a grenade cooked this long is on, -1 before the pin
static s32 zombiesFuseBeat(s32 cook)
{
	const s32 rush = ZOMBIE_GRENADE_FUSE60 - ZOMBIE_XHAIR_BEAT60;

	if (cook < 0) {
		return -1;
	}

	if (cook < rush) {
		return cook / ZOMBIE_XHAIR_BEAT60;
	}

	return (rush + ZOMBIE_XHAIR_BEAT60 - 1) / ZOMBIE_XHAIR_BEAT60 + (cook - rush) / ZOMBIE_XHAIR_RUSH60;
}

// sight.c: 0 for the game's own sight, 1 for none, 2 for Zombies' with its
// arms *spread out
s32 zombiesCrosshair(f32 *spread)
{
	const s32 playernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;
	const f32 dt = g_Vars.lvupdate60freal / 60.0f;
	bool holding;
	f32 move;
	f32 k;
	s32 ammo = 0;
	s32 weaponnum;
	s32 beat;
	s32 h;

	if (!zombiesIsActive() || !player || playernum < 0 || playernum >= MAX_PLAYERS) {
		return 0;
	}

	// rounds fired: the magazines down since the last frame, the gun the same
	weaponnum = player->gunctrl.weaponnum;

	for (h = 0; h < 2; h++) {
		if (player->hands[h].inuse) {
			ammo += player->hands[h].loadedammo[0] + player->hands[h].loadedammo[1];
		}
	}

	if (weaponnum == g_ZombieXhairWeapon[playernum] && g_ZombieXhairAmmo[playernum] > ammo) {
		s32 shots = g_ZombieXhairAmmo[playernum] - ammo;

		g_ZombieXhairPop[playernum] += ZOMBIE_XHAIR_POP * (shots > 3 ? 3 : shots);
	}

	g_ZombieXhairAmmo[playernum] = ammo;
	g_ZombieXhairWeapon[playernum] = weaponnum;

	// a grenade's fuse beating
	holding = g_ZombieThrowStage[playernum] == ZTHROW_PIN || g_ZombieThrowStage[playernum] == ZTHROW_HOLD;
	beat = holding ? zombiesFuseBeat(g_ZombieCook60[playernum]) : -1;

	if (beat > g_ZombieXhairBeat[playernum]) {
		g_ZombieXhairPop[playernum] += ZOMBIE_XHAIR_BEAT;
	}

	g_ZombieXhairBeat[playernum] = beat;

	// the pop settling, the move easing to how fast the player goes
	k = dt * ZOMBIE_XHAIR_SETTLE;
	g_ZombieXhairPop[playernum] -= g_ZombieXhairPop[playernum] * (k > 1 ? 1 : k);

	move = sqrtf(player->speedforwards * player->speedforwards + player->speedsideways * player->speedsideways);
	move = (move > 1 ? 1 : move) * ZOMBIE_XHAIR_MOVE;
	k = dt * ZOMBIE_XHAIR_EASE;
	g_ZombieXhairMove[playernum] += (move - g_ZombieXhairMove[playernum]) * (k > 1 ? 1 : k);

	// at the sights, the sight's own (the count kept all the same); the Ray
	// Gun's sight is the ring on its model, and the Phoenix's reticle it
	// would borrow is not drawn over it
	if (player->insightaimmode || player->codaimfrac > 0.5f) {
		return weaponnum == WEAPON_ZM_RAYGUN ? 1 : 0;
	}

	if (g_ZombieGameOver || (zombiesGunLowerFrac() > 0 && !holding) || zombiesSprintGunFrac() > 0.05f) {
		return 1;
	}

	*spread = g_ZombieXhairMove[playernum] + g_ZombieXhairPop[playernum];

	if (player->crouchpos == CROUCHPOS_SQUAT) {
		*spread *= 0.55f;
	} else if (player->crouchpos == CROUCHPOS_DUCK) {
		*spread *= 0.75f;
	}

	if (!holding && weaponHost(weaponnum) == WEAPON_SHOTGUN) {
		*spread += 1.5f;
	}

	// Deadshot Daiquiri tightens the hip's spread, as Black Ops II's does
	if (!holding && zombiesPlayerHasPerk(playernum, ZOMBIEPERK_DEADSHOT)) {
		*spread *= 0.5f;
	}

	*spread = *spread > ZOMBIE_XHAIR_MAX ? ZOMBIE_XHAIR_MAX : *spread;

	return 2;
}

// bondmove.c: the Sprint button pressed (BUTTON_SPRINT), for the next tick
void zombiesSprintPress(void)
{
	if (g_Vars.currentplayernum >= 0 && g_Vars.currentplayernum < MAX_PLAYERS) {
		g_ZombieSprintPress[g_Vars.currentplayernum] = true;
	}
}

// bondmove.c, a tick of the current player's walk: the sprint, in Perfect
// Dark's speed ramp's place
void zombiesSprintTick(void)
{
	struct player *player = g_Vars.currentplayer;
	const s32 playernum = g_Vars.currentplayernum;
	s32 breath = ZOMBIE_SPRINT_TIME;
	bool sprinting = player->speedmaxtime60 > 0;
	s32 used = sprinting ? player->speedmaxtime60 - 1 : -player->speedmaxtime60;
	bool pressed = false;
	bool can;
	f32 target;
	f32 step;

	if (playernum >= 0 && playernum < MAX_PLAYERS) {
		pressed = g_ZombieSprintPress[playernum];
		g_ZombieSprintPress[playernum] = false;

		if (zombiesPlayerHasPerk(playernum, ZOMBIEPERK_STAMINUP)) {
			breath *= 2;
		}

		if (g_ZombieDowned[playernum]) {
			pressed = false;
			sprinting = false;
		}
	}

	can = player->crouchpos == CROUCHPOS_STAND && player->bondmovemode == MOVEMODE_WALK
		&& player->speedgo > 0.5f && !player->insightaimmode
		&& !player->hands[HAND_RIGHT].triggeron && !player->hands[HAND_LEFT].triggeron;

	used = used < 0 ? 0 : used > breath ? breath : used;

	if (sprinting) {
		used += g_Vars.lvupdate60;

		if (!can || used >= breath) {
			sprinting = false;
		}
	} else {
		used -= g_Vars.lvupdate60;
		used = used < 0 ? 0 : used;

		if (pressed && can && breath - used >= ZOMBIE_SPRINT_START) {
			sprinting = true;
		}
	}

	player->speedmaxtime60 = sprinting ? used + 1 : -used;

	target = sprinting ? ZOMBIE_SPRINT_SCALE : 1.0f;
	step = ZOMBIE_SPRINT_EASE * g_Vars.lvupdate60freal;

	if (player->speedboost < target) {
		player->speedboost = player->speedboost + step > target ? target : player->speedboost + step;
	} else {
		player->speedboost = player->speedboost - step < target ? target : player->speedboost - step;
	}
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

/**
 * chrDamage() asks, before anything is taken: Deadshot Daiquiri's aim. A
 * player with it looking down the sights (Perfect Dark's aim mode, or COD
 * Style Aiming's sights) puts a bullet that hits a zombie anywhere into its
 * head, as Black Ops II's snaps the sights to the head: four times the hit
 * a limb is, and a headshot's points. Not a knife, nor a blast.
 */
s32 zombiesDeadshotHitpart(struct chrdata *chr, struct prop *aprop, struct gset *gset, s32 hitpart, bool explosion)
{
	struct player *player;
	struct weaponfunc *func;
	s32 playernum;

	if (!zombiesIsActive() || explosion || !chr || !chr->aibot || !aprop || aprop->type != PROPTYPE_PLAYER) {
		return hitpart;
	}

	if (hitpart <= 0 || hitpart == HITPART_HEAD || hitpart == HITPART_GUN || hitpart == HITPART_HAT
			|| hitpart >= HITPART_GENERAL) {
		return hitpart;
	}

	playernum = playermgrGetPlayerNumByProp(aprop);

	if (playernum < 0 || playernum >= MAX_PLAYERS || !zombiesPlayerHasPerk(playernum, ZOMBIEPERK_DEADSHOT)) {
		return hitpart;
	}

	func = gset ? gsetGetWeaponFunction(gset) : NULL;

	if (func && (func->type & 0xff) == INVENTORYFUNCTYPE_MELEE) {
		return hitpart;
	}

	player = g_Vars.players[playernum];

	if (!player->insightaimmode && player->codaimfrac < 0.5f) {
		return hitpart;
	}

	return HITPART_HEAD;
}

// chrDamage() asks, of a blast: a player with PhD Flopper takes none of it
bool zombiesPhdShrugs(struct chrdata *chr)
{
	s32 playernum;

	if (!zombiesIsActive() || !chr || !chr->prop || chr->prop->type != PROPTYPE_PLAYER) {
		return false;
	}

	playernum = playermgrGetPlayerNumByProp(chr->prop);

	return playernum >= 0 && zombiesPlayerHasPerk(playernum, ZOMBIEPERK_PHDFLOPPER);
}

// Call of Duty's damage, in a zombie's own (zombiesHealthForRound()'s scale)
static f32 zombiesCodDamage(f32 cod)
{
	return ZOMBIE_PD_HEALTH * cod / ZOMBIE_COD_HEALTH;
}

/**
 * Every zombie standing within radius of pos takes damage (Call of Duty's),
 * less with distance down to half at the edge, as playernum's: their points
 * and kills. A shock (Electric Cherry) leaves sparks on each one it reaches.
 * The host deals it (chrDamage() is the host's online).
 */
static bool g_ZombieHurtNoThrow = false; // zombiesShellBlast()'s: no explosion's death throw

static s32 zombiesHurtAround(s32 playernum, struct coord *pos, f32 radius, f32 coddamage, bool shock)
{
	struct prop *aprop = playernum >= 0 && playernum < MAX_PLAYERS && g_Vars.players[playernum]
		? g_Vars.players[playernum]->prop : NULL;
	s32 hit = 0;
	s32 i;

	for (i = 0; i < g_BotCount; i++) {
		struct chrdata *zchr = g_MpBotChrPtrs[i];
		struct coord dir;
		f32 dist;

		if (!zchr || !zchr->aibot || !zchr->prop || chrIsDead(zchr)) {
			continue;
		}

		dir.x = zchr->prop->pos.x - pos->x;
		dir.y = zchr->prop->pos.y - pos->y;
		dir.z = zchr->prop->pos.z - pos->z;
		dist = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);

		if (dist > radius || dir.y > 200 || dir.y < -200) {
			continue;
		}

		if (dist > 0) {
			dir.x /= dist;
			dir.y /= dist;
			dir.z /= dist;
		}

		if (shock) {
			struct coord up = { 0, 1, 0 };
			struct coord at = zchr->prop->pos;

			at.y += 40;
			sparksCreate(zchr->prop->rooms[0], zchr->prop, &at, &up, &dir, SPARKTYPE_ELECTRICAL);
		}

		// the Ray Gun's bolt kills where they stand, as Black Ops II's does;
		// an explosion's damage throws the dead across the room
		if (g_ZombieHurtNoThrow) {
			chrDamageByMisc(zchr, zombiesCodDamage(coddamage) * (1.0f - 0.5f * dist / radius), &dir, NULL, aprop);
		} else {
			chrDamageByExplosion(zchr, zombiesCodDamage(coddamage) * (1.0f - 0.5f * dist / radius), &dir, aprop, pos);
		}

		hit++;
	}

	return hit;
}

/**
 * The Ray Gun's blast, Black Ops II's (ray_gun_zm: explosionRadius 64 inches,
 * explosionInnerDamage 1500), where its bolt lands: prop.c, in place of the
 * explosive shell's own explosion it stands on (true: none of the host's).
 * Every zombie in it takes Call of Duty's damage, the shooter's points and
 * kills; the host's alone.
 */
#define ZOMBIE_RAYGUN_RADIUS 163.0f
#define ZOMBIE_RAYGUN_DAMAGE 1500.0f
#define ZOMBIE_RAYGUN_SELF   0.6f    // the shooter's own, of their full health, at the blast's middle

bool zombiesShellBlast(struct gset *gset, struct coord *pos, RoomNum *rooms)
{
	if (!zombiesIsActive() || !gset || gset->weaponnum != WEAPON_ZM_RAYGUN) {
		return false;
	}

	// its look is the bolt's landing (zombiesRenderRayFx()), on every machine
	if (!NET_CLIENT) {
		struct prop *me = g_Vars.currentplayer->prop;
		const s32 exp = zmSfxSound(ZMSFX_RAYGUN_EXP);

		// Black Ops II's own burst, where it lands; online, heard by the
		// others from the ring
		if (exp > 0 && rooms) {
			psCreate(NULL, NULL, exp, -1, -1, 0, 0, PSTYPE_NONE, pos, -1.0f, rooms, -1, -1.0f, -1.0f, -1.0f);
			zombiesNetKeepSound(ZMSFX_NET(ZMSFX_RAYGUN_EXP), 1.0f, -1);
		}

		g_ZombieHurtNoThrow = true;
		zombiesHurtAround(g_Vars.currentplayernum, pos, ZOMBIE_RAYGUN_RADIUS, ZOMBIE_RAYGUN_DAMAGE
				* (gset->weaponfunc == FUNC_SECONDARY ? gset->unk063a / 30.0f + 1.0f : 1.0f), false);
		g_ZombieHurtNoThrow = false;

		// too close, or at the feet: the shooter's own blast, as Black Ops
		// II's splash hurts its own (PhD Flopper shrugs it, chrDamage())
		if (me && me->chr && !g_Vars.currentplayer->isdead) {
			struct coord dir;
			f32 dist;

			dir.x = me->pos.x - pos->x;
			dir.y = me->pos.y - pos->y;
			dir.z = me->pos.z - pos->z;
			dist = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);

			if (dist < ZOMBIE_RAYGUN_RADIUS) {
				if (dist > 0) {
					dir.x /= dist;
					dir.y /= dist;
					dir.z /= dist;
				}

				// chrDamage() takes a player's at an eighth, over its health scale
				chrDamageByExplosion(me->chr, ZOMBIE_RAYGUN_SELF * (1.0f - dist / ZOMBIE_RAYGUN_RADIUS)
						* g_ZombieMaxHealth[g_Vars.currentplayernum] * 8.0f * g_Vars.currentplayer->healthscale,
						&dir, me, pos);
			}
		}
	}

	return true;
}

/**
 * PhD Flopper's blast, where a dive or a drop lands: Call of Duty's, round
 * the player, the player and the other players untouched (zombiesPhdShrugs(),
 * and teams in chrDamage()). The game's own blast is laid on for its sight
 * and sound, and its own damage goes on the zombies with the rest.
 */
#define ZOMBIE_PHD_RADIUS 300.0f
#define ZOMBIE_PHD_DAMAGE 1500.0f // Call of Duty's: a dive kills the early rounds' zombies outright

static void zombiesPhdBlast(s32 playernum)
{
	struct prop *prop = g_Vars.players[playernum]->prop;
	struct coord pos;

	if (!prop || !zombiesPlayerHasPerk(playernum, ZOMBIEPERK_PHDFLOPPER)) {
		return;
	}

	pos = prop->pos;
	pos.y = prop->chr ? prop->chr->ground + 10 : pos.y;

	explosionCreateSimple(prop, &pos, prop->rooms, EXPLOSIONTYPE_12, playernum);
	zombiesHurtAround(playernum, &pos, ZOMBIE_PHD_RADIUS, ZOMBIE_PHD_DAMAGE, false);
}

/**
 * Electric Cherry: bondgun.c, a hand starting a reload. The shock reaches
 * further and hits harder the emptier the magazine was - an empty one most -
 * and a reload hard after another gives a weaker one, as Black Ops II's do.
 */
#define ZOMBIE_CHERRY_RADIUS_MIN 100.0f
#define ZOMBIE_CHERRY_RADIUS_MAX 350.0f
#define ZOMBIE_CHERRY_DAMAGE     1000.0f // Call of Duty's, at an empty magazine
#define ZOMBIE_CHERRY_COOL60     180     // three seconds for a whole one again

static s32 g_ZombieCherryLast60[MAX_PLAYERS];

/**
 * The shock seen (zombiesRenderCherryFx()): the burst at the player's middle,
 * its ring as wide as the shock reaches, its bolts to the zombies it will
 * reach (as zombiesHurtAround() finds them: before they fall), or four out
 * across the floor when there are none
 */
static void zombiesCherryBurst(s32 playernum, f32 reach)
{
	struct prop *prop = g_Vars.players[playernum]->prop;
	s32 n = 0;
	s32 i;

	if (!prop) {
		return;
	}

	// the bursts' clock stands still while none is drawn: from now, or the
	// time since the last went would age this one out at its first frame
	if (!zombiesCherryFxAny()) {
		g_ZombieCherryFxFrame = g_Vars.lvframe60;
	}

	g_ZombieCherryFx[playernum].on = true;
	g_ZombieCherryFx[playernum].age60 = 0;
	g_ZombieCherryFx[playernum].reach = reach;
	g_ZombieCherryFx[playernum].at = prop->pos;
	g_ZombieCherryFx[playernum].at.y = (prop->chr ? prop->chr->ground : prop->pos.y - 100) + 80;

	for (i = 0; i < g_BotCount && n < ZOMBIE_CHERRY_BOLTS; i++) {
		struct chrdata *zchr = g_MpBotChrPtrs[i];
		f32 dx;
		f32 dy;
		f32 dz;

		if (!zchr || !zchr->prop || chrIsDead(zchr)) {
			continue;
		}

		dx = zchr->prop->pos.x - prop->pos.x;
		dy = zchr->prop->pos.y - prop->pos.y;
		dz = zchr->prop->pos.z - prop->pos.z;

		if (dx * dx + dy * dy + dz * dz > reach * reach || dy > 200 || dy < -200) {
			continue;
		}

		g_ZombieCherryFx[playernum].to[n] = zchr->prop->pos;
		g_ZombieCherryFx[playernum].to[n].y = zchr->ground + 70;
		g_ZombieCherryFx[playernum].bot[n] = i;
		n++;
	}

	// none in reach: across the floor, the way the player faces and round
	for (i = 0; n == 0 && i < 4; i++) {
		const f32 a = i * (M_PI / 2) + 0.4f;

		g_ZombieCherryFx[playernum].bot[i] = -1;
		g_ZombieCherryFx[playernum].to[i] = g_ZombieCherryFx[playernum].at;
		g_ZombieCherryFx[playernum].to[i].x += sinf(a) * reach * 0.6f;
		g_ZombieCherryFx[playernum].to[i].y -= 60;
		g_ZombieCherryFx[playernum].to[i].z += cosf(a) * reach * 0.6f;
	}

	g_ZombieCherryFx[playernum].numbolts = n > 0 ? n : 4;
}

void zombiesReloadStarted(s32 handnum)
{
	const s32 playernum = g_Vars.currentplayernum;
	struct hand *hand;
	f32 spent;
	f32 strength = 1.0f;
	s32 since;
	struct coord pos;

	if (!zombiesIsActive() || NET_CLIENT || playernum < 0 || playernum >= MAX_PLAYERS
			|| !zombiesPlayerHasPerk(playernum, ZOMBIEPERK_CHERRY) || !zombiesPlayerIsUp(playernum)) {
		return;
	}

	hand = &g_Vars.currentplayer->hands[handnum];

	if (hand->clipsizes[0] <= 0) {
		return;
	}

	spent = 1.0f - (f32)hand->loadedammo[0] / hand->clipsizes[0];
	spent = spent < 0 ? 0 : spent > 1 ? 1 : spent;

	since = g_Vars.lvframe60 - g_ZombieCherryLast60[playernum];

	if (since >= 0 && since < ZOMBIE_CHERRY_COOL60) {
		strength = (f32)since / ZOMBIE_CHERRY_COOL60;
	}

	g_ZombieCherryLast60[playernum] = g_Vars.lvframe60;

	if (spent * strength < 0.05f) {
		return;
	}

	pos = g_Vars.currentplayer->prop->pos;

	zombiesCherryBurst(playernum, ZOMBIE_CHERRY_RADIUS_MIN + (ZOMBIE_CHERRY_RADIUS_MAX - ZOMBIE_CHERRY_RADIUS_MIN) * spent);

	zombiesHurtAround(playernum,
			&pos,
			ZOMBIE_CHERRY_RADIUS_MIN + (ZOMBIE_CHERRY_RADIUS_MAX - ZOMBIE_CHERRY_RADIUS_MIN) * spent,
			ZOMBIE_CHERRY_DAMAGE * spent * strength,
			true);

	// Black Ops II's own (zmb_cherry_explode, the taser mine's blast); the
	// Mauler's charge, sped up, when its file is not there
	{
		const s32 boom = zmSfxSound(ZMSFX_CHERRY_EXPLODE);

		if (boom > 0) {
			zombiesNetKeepSound(ZMSFX_NET(ZMSFX_CHERRY_EXPLODE), 1.0f, -1);
			zombiesPlaySoundHere(boom, 1.0f);
		} else {
			zombiesPlaySound(SFX_MAULER_CHARGE, 1.8f);
		}
	}
}

/**
 * The dolphin dive: crouch while sprinting forward throws the player forward and
 * low (bwalkTryDive(), bondwalk.c), and they land flat on the floor, squatting,
 * as Call of Duty's dive to prone; with PhD Flopper the landing is a blast.
 * A drop from a height lands one too, with PhD. bondmove.c asks when crouch
 * goes down; zombiesTickDive() watches for the landing.
 */
#define ZOMBIE_DIVE_RUN      0.75f // how hard forward the player must be going
#define ZOMBIE_DIVE_MAX60    90    // a dive's most, before it counts as landed anyway
#define ZOMBIE_PHD_DROP      200.0f // a drop from this high lands PhD's blast

static s32 g_ZombieDive60[MAX_PLAYERS];       // 60ths into a dive, 0 for none
static f32 g_ZombieFallTop[MAX_PLAYERS];      // the highest ground a fall left from
static bool g_ZombieWasFalling[MAX_PLAYERS];

bool zombiesTryDive(void)
{
	const s32 playernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;

	// Black Ops II dives from a sprint only (speedmaxtime60 above 0 is a
	// sprint, zombiesSprintTick()); a crouch at a run is just a crouch
	if (!zombiesIsActive() || playernum < 0 || playernum >= MAX_PLAYERS
			|| !zombiesPlayerIsUp(playernum) || g_ZombieDowned[playernum]
			|| g_ZombieDive60[playernum] > 0
			|| player->speedmaxtime60 <= 0
			|| player->speedforwards < ZOMBIE_DIVE_RUN) {
		return false;
	}

	if (!bwalkTryDive()) {
		return false;
	}

	g_ZombieDive60[playernum] = 1;

	// where it left from: PhD's blast is for a dive off something high
	if (player->prop && player->prop->chr) {
		g_ZombieFallTop[playernum] = player->prop->chr->ground;
	}

	// the dive is the sprint's end; the breath it used stays used
	player->speedmaxtime60 = -(player->speedmaxtime60 - 1);

	return true;
}

// Each frame, the current player's: a dive landing, and a drop
void zombiesTickDive(void)
{
	const s32 playernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;

	if (!zombiesIsActive() || playernum < 0 || playernum >= MAX_PLAYERS || !player->prop || !player->prop->chr) {
		return;
	}

	if (g_ZombieDive60[playernum] > 0) {
		g_ZombieDive60[playernum] += g_Vars.lvupdate60;

		// down again: flat out, as a dive ends
		if ((g_ZombieDive60[playernum] > 12 && !player->isfalling && player->bdeltapos.y <= 0)
				|| g_ZombieDive60[playernum] > ZOMBIE_DIVE_MAX60) {
			g_ZombieDive60[playernum] = 0;
			player->crouchpos = CROUCHPOS_SQUAT;

			// only a dive that came down from somewhere high: a dive on the
			// flat, or a plain jump or drop off a ledge, blasts nothing
			if (!NET_CLIENT && g_ZombieFallTop[playernum] - player->prop->chr->ground > ZOMBIE_PHD_DROP) {
				zombiesPhdBlast(playernum);
			}
		}
	}

	g_ZombieWasFalling[playernum] = player->isfalling;
}

/**
 * The stance button (BUTTON_STANCE, a controller's B in Zombies' layout), as
 * Call of Duty's controller has it: a tap crouches, or stands up from a
 * crouch, or comes up from prone to a crouch; held, it lies prone the moment
 * the hold is long enough. From bondmove.c, after the crouch keys, for the
 * current player.
 */
#define ZOMBIE_STANCE_HOLD60 24

static s32 g_ZombieStanceHeld60[MAX_PLAYERS]; // 60ths the button has been down, -1 once the hold went prone

void zombiesStanceTick(s32 contpad)
{
	const s32 playernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;
	bool held;

	if (!zombiesIsActive() || playernum < 0 || playernum >= MAX_PLAYERS) {
		return;
	}

	held = joyGetButtons(contpad, BUTTON_STANCE) != 0 && zombiesPlayerIsUp(playernum);

	if (held) {
		if (g_ZombieStanceHeld60[playernum] >= 0) {
			g_ZombieStanceHeld60[playernum] += g_Vars.lvupdate60;

			if (g_ZombieStanceHeld60[playernum] >= ZOMBIE_STANCE_HOLD60) {
				player->crouchpos = CROUCHPOS_SQUAT;
				g_ZombieStanceHeld60[playernum] = -1;
			}
		}

		return;
	}

	// let go before the hold was long enough: a tap
	if (g_ZombieStanceHeld60[playernum] > 0) {
		player->crouchpos = player->crouchpos == CROUCHPOS_DUCK ? CROUCHPOS_STAND : CROUCHPOS_DUCK;
	}

	g_ZombieStanceHeld60[playernum] = 0;
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
static f32 zombiesThrowLowerFrac(s32 playernum);

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

	// the game's end: the hands reach out, then the view leaves them
	if (g_ZombieGameOver) {
		return zombiesGameOverGunLower();
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

	// a grenade's throw
	if (zombiesThrowLowerFrac(playernum) > drink) {
		drink = zombiesThrowLowerFrac(playernum);
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

	// down (a crawl) knifes as well, as Black Ops II has it; dead does not
	if (!zombiesPlayerAlive(playernum) || g_ZombieGameOver || g_ZombieDrink60[playernum] >= 0 || g_ZombieKnifeStage[playernum] != ZKNIFE_NONE
			|| g_ZombieKnifeCooldown60[playernum] > 0 || zombiesPapLowerFrac(playernum) > 0
			|| g_ZombieThrowStage[playernum] != ZTHROW_NONE) {
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

bool bgunZombiesThrowGrenade(f32 speed, f32 upspeed, s32 fuse60);

static bool zombiesHoldingGrenade(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	bool held;

	setCurrentPlayerNum(playernum);
	held = joyGetButtons(optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex), BUTTON_GRENADE) != 0;
	setCurrentPlayerNum(prevplayernum);

	return held;
}

// bondmove.c, on a press of BUTTON_GRENADE: the current player starts a
// throw, if it has a grenade. Online a client plays its own throw at once,
// as it does its knife (zombiesClientTick()); the grenade is the host's, a
// projectile, and the count rides in its block.
void zombiesGrenadePress(void)
{
	s32 playernum = g_Vars.currentplayernum;

	if (!zombiesIsActive() || playernum < 0 || playernum >= MAX_PLAYERS || g_NetReplaying || g_ZombieGameOver) {
		return;
	}

	if (!zombiesPlayerIsUp(playernum) || g_ZombieDrink60[playernum] >= 0 || g_ZombieKnifeStage[playernum] != ZKNIFE_NONE
			|| g_ZombieThrowStage[playernum] != ZTHROW_NONE || zombiesPapLowerFrac(playernum) > 0
			|| g_ZombieGrenadeWait60[playernum] > 0) {
		return;
	}

	// none left: nothing happens, as Black Ops II has it (no sound)
	if (g_ZombieGrenades[playernum] <= 0) {
		return;
	}

	g_ZombieThrowStage[playernum] = ZTHROW_DOWN;
	g_ZombieThrow60[playernum] = 0;
	g_ZombieCook60[playernum] = -1;
}

// The grenade out of the hand, cooked as long as it was: the host's alone
static void zombiesThrowRelease(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	s32 fuse = ZOMBIE_GRENADE_FUSE60 - (g_ZombieCook60[playernum] > 0 ? g_ZombieCook60[playernum] : 0);

	if (NET_CLIENT) {
		return;
	}

	setCurrentPlayerNum(playernum);

	if (bgunZombiesThrowGrenade(ZOMBIE_GRENADE_SPEED, ZOMBIE_GRENADE_UPSPEED, fuse > 0 ? fuse : 0)) {
		if (g_ZombieGrenades[playernum] > 0) {
			g_ZombieGrenades[playernum]--;
		}

		g_ZombieSoundFor = playernum;
		zombiesPlaySound(SFX_THROW, 1.0f);
		g_ZombieSoundFor = -1;
	}

	setCurrentPlayerNum(prevplayernum);
}

// A throw on: its stages, the button held keeping the arm back
static void zombiesTickThrow(s32 playernum)
{
	s32 *t = &g_ZombieThrow60[playernum];
	s32 *stage = &g_ZombieThrowStage[playernum];
	s32 prev = *t;

	if (*stage == ZTHROW_NONE) {
		return;
	}

	// downed: the throw is gone, the grenade with it (unless it was out)
	if (!zombiesPlayerIsUp(playernum) || g_ZombieGameOver) {
		*stage = ZTHROW_NONE;
		g_ZombieCook60[playernum] = -1;
		return;
	}

	*t += g_Vars.lvupdate60;

	if (g_ZombieCook60[playernum] >= 0) {
		g_ZombieCook60[playernum] += g_Vars.lvupdate60;
	}

	switch (*stage) {
	case ZTHROW_DOWN:
		if (*t >= ZOMBIE_THROW_DOWN60) {
			*stage = ZTHROW_PIN;
			*t = 0;
		}
		break;
	case ZTHROW_PIN:
		if (prev < ZOMBIE_THROW_PINOUT60 && *t >= ZOMBIE_THROW_PINOUT60) {
			g_ZombieCook60[playernum] = *t - ZOMBIE_THROW_PINOUT60;
		}

		if (*t >= ZOMBIE_THROW_PIN60) {
			*stage = ZTHROW_HOLD;
			*t = 0;
		}
		break;
	case ZTHROW_HOLD:
		// let go, or cooked to the end of its fuse: thrown
		if (!zombiesHoldingGrenade(playernum) || g_ZombieCook60[playernum] >= ZOMBIE_GRENADE_FUSE60 - ZOMBIE_THROW_RELEASE60) {
			*stage = ZTHROW_OUT;
			*t = 0;
		}
		break;
	case ZTHROW_OUT:
		if (prev < ZOMBIE_THROW_RELEASE60 && *t >= ZOMBIE_THROW_RELEASE60) {
			zombiesThrowRelease(playernum);
			g_ZombieCook60[playernum] = -1;
		}

		if (*t >= ZOMBIE_THROW_OUT60) {
			*stage = ZTHROW_UP;
			*t = 0;
		}
		break;
	case ZTHROW_UP:
		if (*t >= ZOMBIE_THROW_UP60) {
			*stage = ZTHROW_NONE;
		}
		break;
	}
}

// zombiesGunLowerFrac(): how far down the throw has the gun
static f32 zombiesThrowLowerFrac(s32 playernum)
{
	f32 f;

	switch (g_ZombieThrowStage[playernum]) {
	case ZTHROW_DOWN:
		f = (f32)g_ZombieThrow60[playernum] / ZOMBIE_THROW_DOWN60;
		break;
	case ZTHROW_PIN:
	case ZTHROW_HOLD:
	case ZTHROW_OUT:
		return 1;
	case ZTHROW_UP:
		f = 1 - (f32)g_ZombieThrow60[playernum] / ZOMBIE_THROW_UP60;
		break;
	default:
		return 0;
	}

	f = f < 0 ? 0 : f > 1 ? 1 : f;

	return f * f * (3 - 2 * f);
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

	if (!zombiesPlayerAlive(playernum)) {
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
		// 60ths each, from the bottle coming up), its own sounds
		// (evt_perk_bottle_open, evt_perk_swallow, evt_bottle_break: the
		// common bank, zmb_common.all.sabs), Perfect Dark's when they are not
		// there; and its belch as the perk takes
		if (zombiesCrossed(prev, now, DRINK_GUNDOWN + 32) && !zombiesPlayCodSound(ZMSFX_PERK_OPEN, NULL)) {
			zombiesPlaySound(SFX_PICKUP_AMMO, 0.45f);
		}

		if (zombiesCrossed(prev, now, DRINK_GUNDOWN + 58) && !zombiesPlayCodSound(ZMSFX_PERK_SWALLOW, NULL)) {
			zombiesPlaySound(SFX_PICKUP_AMMO, 0.45f);
		}

		if (zombiesCrossed(prev, now, DRINK_GUNDOWN + 114) && !zombiesPlayCodSound(ZMSFX_PERK_BREAK, NULL)) {
			zombiesPlaySound(SFX_BOTTLE_BREAK, 0.5f);
		}

		// the perk takes as the last of it goes down
		if (zombiesCrossed(prev, now, DRINK_GULPEND)) {
			zombiesPlayCodSound(ZMSFX_PERK_BELCH, NULL);
			zombiesGivePerk(playernum, g_ZombieDrinkPerk[playernum]);
		}

		g_ZombieDrink60[playernum] = now >= DRINK_TOTAL ? -1 : now;
		return;
	}

	if (machine == NULL || g_ZombieHasJugg[playernum] || zombiesPerksFull(playernum)) {
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

	if (g_ZombieThrowStage[playernum] != ZTHROW_NONE) {
		return;
	}

	g_ZombieUseNeedsRelease[playernum] = true;

	if (g_ZombiePoints[playernum] < ZOMBIE_JUGG_COST) {
		if (!zombiesPlayCodSound(ZMSFX_PERK_DENY, NULL)) {
			zombiesPlaySound(SFX_MENU_ERROR, 0.4f);
		}

		return;
	}

	g_ZombiePoints[playernum] -= ZOMBIE_JUGG_COST;
	zombiesAddPopup(playernum, -ZOMBIE_JUGG_COST);
	zombiesPerkBought(ZOMBIEPERK_JUGGERNOG, machine);
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
/**
 * The last stand, as Black Ops II's: down, the player's best pistol is in
 * their hand - the Ray Gun first, it being a pistol and the strongest, then
 * the magnums, the Phoenix, the Mauler, the MagSec and the Falcons - or, with
 * none, a Falcon 2 lent for the while. It has a full magazine and
 * ZOMBIE_LASTSTAND_CLIPS more of its own: what that ammunition held before
 * comes back as they get up, as does the gun they had in hand, and a lent
 * Falcon goes again. Bled out, it does not matter (a death's inventory is
 * the respawn's). The host's, whose inventory a client's is.
 */
#define ZOMBIE_LASTSTAND_CLIPS 3

static const s32 g_ZombieLastStandGuns[] = {
	WEAPON_ZM_RAYGUN, WEAPON_DY357LX, WEAPON_DY357MAGNUM, WEAPON_PHOENIX, WEAPON_MAULER,
	WEAPON_MAGSEC4, WEAPON_FALCON2_SCOPE, WEAPON_FALCON2_SILENCER, WEAPON_FALCON2,
};

static struct {
	bool on;
	s32 before;    // the gun in hand as they went down
	s32 gun;       // the last stand's
	bool lent;     // given for the down, and taken back
	s32 ammotype;
	s32 ammo;      // its ammunition as it was, to be put back
} g_ZombieLastStand[MAX_PLAYERS];

static void zombiesDropReload(void);
static void zombiesSyncGuns(s32 playernum);
static void zombiesFillGun(s32 weaponnum, s32 clips, bool loadclip);
static void zombiesKnifeCancel(s32 playernum);

static void zombiesLastStandBegin(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.players[playernum];
	s32 gun = WEAPON_NONE;
	s32 type;
	s32 i;

	g_ZombieLastStand[playernum].on = false;

	if (NET_CLIENT || !player || !player->prop) {
		return;
	}

	setCurrentPlayerNum(playernum);

	for (i = 0; i < ARRAYCOUNT(g_ZombieLastStandGuns); i++) {
		if (invHasSingleWeaponIncAllGuns(g_ZombieLastStandGuns[i])) {
			gun = g_ZombieLastStandGuns[i];
			break;
		}
	}

	g_ZombieLastStand[playernum].before = player->gunctrl.switchtoweaponnum >= 0
		? player->gunctrl.switchtoweaponnum : player->gunctrl.weaponnum;
	g_ZombieLastStand[playernum].lent = gun == WEAPON_NONE;

	if (gun == WEAPON_NONE) {
		gun = WEAPON_FALCON2;
		invGiveSingleWeapon(gun);
	}

	type = bgunGetAmmoTypeForWeapon(gun, FUNC_PRIMARY);

	g_ZombieLastStand[playernum].gun = gun;
	g_ZombieLastStand[playernum].ammotype = type;
	g_ZombieLastStand[playernum].ammo = type > 0 ? player->ammoheldarr[type] : 0;

	// what is in the hand already counts as held, if it is this gun's
	if (type > 0 && player->gunctrl.weaponnum == gun) {
		g_ZombieLastStand[playernum].ammo += player->hands[HAND_RIGHT].loadedammo[0];
		player->hands[HAND_RIGHT].loadedammo[0] = 0;
	}

	if (type > 0) {
		bgunSetAmmoQuantity(type, 0);
	}

	zombiesFillGun(gun, ZOMBIE_LASTSTAND_CLIPS, true);

	zombiesKnifeCancel(playernum);
	zombiesDropReload();
	player->gunctrl.dualwielding = false;
	player->gunctrl.leftwant = WEAPON_NONE;
	bgunEquipWeapon(gun);

	g_ZombieLastStand[playernum].on = true;

	setCurrentPlayerNum(prevplayernum);
}

static void zombiesLastStandEnd(s32 playernum)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	struct player *player = g_Vars.players[playernum];
	s32 gun = g_ZombieLastStand[playernum].gun;
	s32 type = g_ZombieLastStand[playernum].ammotype;

	if (!g_ZombieLastStand[playernum].on || NET_CLIENT || !player || !player->prop) {
		g_ZombieLastStand[playernum].on = false;
		return;
	}

	g_ZombieLastStand[playernum].on = false;

	setCurrentPlayerNum(playernum);

	// the last stand's own rounds gone with it, in the hand as well
	if (player->gunctrl.weaponnum == gun) {
		player->hands[HAND_RIGHT].loadedammo[0] = 0;
		player->hands[HAND_LEFT].loadedammo[0] = 0;
	}

	if (type > 0) {
		bgunSetAmmoQuantity(type, g_ZombieLastStand[playernum].ammo);
	}

	zombiesDropReload();

	if (g_ZombieLastStand[playernum].before > WEAPON_NONE
			&& invHasSingleWeaponIncAllGuns(g_ZombieLastStand[playernum].before)) {
		bgunEquipWeapon(g_ZombieLastStand[playernum].before);
	}

	if (g_ZombieLastStand[playernum].lent) {
		invRemoveItemByNum(gun);
		zombiesSyncGuns(playernum);
	}

	setCurrentPlayerNum(prevplayernum);
}

// Bondgun.c: no change of gun while down, as Black Ops II has it
bool zombiesLastStandHoldsGun(void)
{
	s32 me = g_Vars.currentplayernum;

	return zombiesIsActive() && me >= 0 && me < MAX_PLAYERS && g_ZombieDowned[me] && !g_ZombieGameOver;
}

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

	solo = zombiesPlayersInPlay() < 2;

	if (force || g_ZombieRound < 1 || player->invincible) {
		return false;
	}

	// Alone, Quick Revive gets the player up again by themselves. Without it
	// the down is the last: they go down all the same rather than die as
	// Perfect Dark has it, and the game is over at once
	// (zombiesEveryoneIsDown()), the view falling to the floor
	g_ZombieDowned[playernum] = true;
	g_ZombieStats[playernum].downs++;
	g_ZombieBleed60[playernum] = ZOMBIE_BLEEDOUT60;
	g_ZombieRevive60[playernum] = 0;
	g_ZombieReviver[playernum] = -1;
	g_ZombieSavedInvincible[playernum] = player->invincible;
	g_ZombieSelfRevive60[playernum] = solo && g_ZombieHasPerk[playernum][ZOMBIEPERK_QUICKREVIVE] ? ZOMBIE_QR_SELF60 : 0;

	player->invincible = true;
	player->bondhealth = g_ZombieMaxHealth[playernum] * 0.01f;
	player->crouchpos = CROUCHPOS_SQUAT;
	player->autocrouchpos = CROUCHPOS_SQUAT;

	// going down costs the perks, Quick Revive with them, and Mule Kick's
	// third gun (asked before the perk goes)
	g_ZombieDrink60[playernum] = -1;
	zombiesLoseMuleGun(playernum);
	zombiesLosePerks(playernum);

	// the last stand's pistol in hand
	zombiesLastStandBegin(playernum);

	// Black Ops II's own cue is the last stand's loop (zombiesTickLastStandSound());
	// Perfect Dark's buzz only when that sound was never baked
	if (zmSfxSound(ZMSFX_LASTSTAND) <= 0) {
		zombiesPlaySound(SFX_MENU_ERROR, 0.6f);
	}

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
static bool zombiesGameOverHidesHud(void);

bool zombiesSpectatorHidesProp(struct prop *prop)
{
	s32 me = g_Vars.currentplayernum;
	s32 target;

	// the game's end, over the arena: the zombies have gone, and the
	// scoreboard has it to itself
	if (zombiesIsActive() && g_ZombieGameOver && zombiesGameOverHidesHud()
			&& prop->type == PROPTYPE_CHR && prop->chr && prop->chr->aibot) {
		return true;
	}

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

	zombiesLastStandEnd(playernum);
	zombiesClearDowned(playernum);
	player->bondhealth = g_ZombieMaxHealth[playernum] * 0.5f;
	player->crouchpos = CROUCHPOS_STAND;
	g_ZombieLastHealth[playernum] = player->bondhealth;
	g_ZombieRegenDelay60[playernum] = 0;

	// no sound: Black Ops II has none for getting up (its characters' lines are it)
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
				g_ZombieStats[reviver].revives++;
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

static s32 zombiesOpenSpawnPads(s16 *out);

static void zombiesScatter(s32 botnum, struct chrdata *zchr)
{
	// the pads in the zones open so far: none behind a door still shut
	s16 pads[MAX_SPAWNPOINTS];
	s32 n = zombiesOpenSpawnPads(pads);
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

		padUnpack(pads[i], PADFIELD_POS, &pad);

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

	padUnpack(pads[pick], PADFIELD_POS | PADFIELD_ROOM, &pad);
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

	// alone and down with no Quick Revive: that was the last (zombiesPlayerGoesDown())
	if (zombiesPlayersInPlay() < 2) {
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
static s32 zombiesBoxCost(void); // ZOMBIE_BOX_COST, or Fire Sale's
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

/**
 * Zombies' own guns as things in the world - over the Mystery Box, in
 * Pack-a-Punch, in a player's hand in third person, lying on the floor -
 * drawn as Black Ops II's world model in place of their host's prop
 * (tools/zombies/glb2obj.py's OBJ in the build's zombies/models/<name>_world/).
 * The mesh is laid along the host prop's longest side and made as long, its
 * muzzle the way the host's points (ZOMBIE_ZMGUN_FLIP); without the OBJ the
 * host's prop is drawn.
 */
#define ZOMBIE_ZMGUN_FLIP 1 // 1: the host prop's muzzle at the low end of its long side, as the mesh's is; -1 the high

struct zombiezmworld {
	s32 weaponnum;
	const char *rel;
	struct zombiemesh mesh;
};

static struct zombiezmworld g_ZombieZmWorld[] = {
	{ WEAPON_ZM_RAYGUN, "$E/zombies/models/raygun_world/raygun_world.obj" },
};

static s32 zombiesPapShownZmGun(struct defaultobj *obj);

// Which of Zombies' own guns an object shows, WEAPON_NONE for none
static s32 zombiesObjZmGun(struct defaultobj *obj)
{
	if (zombiesPapShownZmGun(obj) != WEAPON_NONE) {
		return zombiesPapShownZmGun(obj);
	}

	if (obj->type == OBJTYPE_WEAPON && WEAPON_IS_ZM(((struct weaponobj *)obj)->weaponnum)) {
		return ((struct weaponobj *)obj)->weaponnum;
	}

	if (obj == g_ZombieBoxGun && g_ZombieBoxShown >= 0 && g_ZombieBoxShown < NUM_MPWEAPONS
			&& WEAPON_IS_ZM(g_MpWeapons[g_ZombieBoxShown].weaponnum)) {
		return g_MpWeapons[g_ZombieBoxShown].weaponnum;
	}

	return WEAPON_NONE;
}

static bool zombiesRenderZmGun(struct modelrenderdata *renderdata, struct defaultobj *obj)
{
	struct zombiezmworld *world = NULL;
	struct modelrodata_bbox *bbox;
	const s32 weaponnum = zombiesObjZmGun(obj);
	struct zombiemesh *zm;
	f32 ext[3];
	f32 mid[3];
	f32 k;
	Mtxf local;
	Mtxf *mtx;
	s32 i;

	if (weaponnum == WEAPON_NONE || !obj->model || !obj->model->matrices) {
		return false;
	}

	for (i = 0; i < ARRAYCOUNT(g_ZombieZmWorld); i++) {
		if (g_ZombieZmWorld[i].weaponnum == weaponnum) {
			world = &g_ZombieZmWorld[i];
		}
	}

	if (!world) {
		return false;
	}

	zm = &world->mesh;

	if (!zm->tried) {
		zombiesMeshBuildFrom(zm, world->rel, bgunGetName(weaponnum));
	}

	bbox = objFindBboxRodata(obj);

	if (!zm->gdl || !bbox) {
		return false;
	}

	// nothing of it in the translucent pass
	if (!(renderdata->flags & MODELRENDERFLAG_OPA)) {
		return true;
	}

	ext[0] = bbox->xmax - bbox->xmin;
	ext[1] = bbox->ymax - bbox->ymin;
	ext[2] = bbox->zmax - bbox->zmin;
	mid[0] = (bbox->xmax + bbox->xmin) / 2;
	mid[1] = (bbox->ymax + bbox->ymin) / 2;
	mid[2] = (bbox->zmax + bbox->zmin) / 2;

	{
		static bool logged = false;

		if (!logged) {
			logged = true;
			sysLogPrintf(LOG_NOTE, "zombies: the host prop's box x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f, the mesh x %.2f..%.2f",
					bbox->xmin, bbox->xmax, bbox->ymin, bbox->ymax, bbox->zmin, bbox->zmax, zm->midx - zm->width / 2, zm->midx + zm->width / 2);
		}
	}

	// The mesh's stored axes are its length along x (the muzzle at its low
	// end), up along y and across along z; a Perfect Dark gun prop is long
	// along x, its grip down -z and across along y (the Phoenix's: x -204 to
	// 101, y 27 either side, z -84 to 47). As long as the host's longest
	// side, and turned without a mirror (up z, across -y)
	k = ext[0] / (zm->width * ZOMBIE_MESH_FIXED);

	mtx4LoadIdentity(&local);
	local.m[0][0] = k * ZOMBIE_ZMGUN_FLIP;
	local.m[1][1] = 0;
	local.m[1][2] = k;
	local.m[2][2] = 0;
	local.m[2][1] = -k * ZOMBIE_ZMGUN_FLIP;

	// its middle at the host's
	for (i = 0; i < 3; i++) {
		const f32 centre = zm->midx * ZOMBIE_MESH_FIXED;

		local.m[3][i] = mid[i] - local.m[0][i] * centre;
	}

	mtx = gfxAllocateMatrix();
	mtx4MultMtx4(&obj->model->matrices[0], &local, mtx);

	// as bright as the one in the hands (zombiesVmGunTick()): at half the
	// room's light Black Ops II's dark metal read as a black pistol
	renderdata->gdl = zombiesBottleDraw(renderdata->gdl, zm, mtx,
			0xb0 + obj->shadecol[0] * 0x4f / 0xff, 0xb0 + obj->shadecol[1] * 0x4f / 0xff, 0xb0 + obj->shadecol[2] * 0x4f / 0xff);

	return true;
}


/**
 * Zombies' own guns' names (constants.h, WEAPON_ZM_FIRST), which no language
 * file has: given once, the first match. Their definitions are invitems.c's.
 */
static void zombiesNameOwnGuns(void)
{
	static bool named = false;
	struct weapon *raygun = weaponFindById(WEAPON_ZM_RAYGUN);

	if (named || raygun == NULL) {
		return;
	}

	named = true;
	raygun->name = raygun->shortname = langAddPortText("Ray Gun\n");

	// its second function, the charged burst (invitems.c)
	if (raygun->functions[1]) {
		((struct weaponfunc *)raygun->functions[1])->name = langAddPortText("Charged Burst\n");
	}
}

static void zombiesResetBox(void)
{
	s32 i;

	zombiesNameOwnGuns();

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
#define ZOMBIE_BOX_RAYGUN_ODDS 8

static s32 zombiesBoxPick(s32 playernum, bool final)
{
	s32 tries;

	if (g_ZombieBoxNumGuns <= 0) {
		return -1;
	}

	// The Ray Gun lands one time in ZOMBIE_BOX_RAYGUN_ODDS, not one in the
	// box's thirty-odd guns (a tester spent near 90,000 points for one)
	if (final && rngRandom() % ZOMBIE_BOX_RAYGUN_ODDS == 0) {
		s32 i;

		for (i = 0; i < g_ZombieBoxNumGuns; i++) {
			s32 index = g_ZombieBoxGuns[i];

			if (g_MpWeapons[index].weaponnum == WEAPON_ZM_RAYGUN) {
				const s32 prevplayernum = g_Vars.currentplayernum;
				bool has = false;

				if (playernum >= 0) {
					setCurrentPlayerNum(playernum);
					has = invHasSingleWeaponIncAllGuns(WEAPON_ZM_RAYGUN);
					setCurrentPlayerNum(prevplayernum);
				}

				if (!has) {
					return index;
				}

				break;
			}
		}
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
	{ WEAPON_ZM_RAYGUN,        "Porter's X2 Ray Gun\n" },
};

// The gun in the machine, shown as the box shows its (zombiesBoxShowGun())
static struct defaultobj g_ZombiePapGunObj;
static s32 g_ZombiePapGunWeapon = WEAPON_NONE; // the gun it shows
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

	g_ZombiePapGunWeapon = weaponnum;
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

// zombiesObjZmGun(): one of Zombies' own guns in Pack-a-Punch
static s32 zombiesPapShownZmGun(struct defaultobj *obj)
{
	return obj == &g_ZombiePapGunObj && g_ZombiePapGunUp && WEAPON_IS_ZM(g_ZombiePapGunWeapon)
		? g_ZombiePapGunWeapon : WEAPON_NONE;
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

			// in: the machine at work (zmb_perks_packa_upgrade, its crunch)
			zombiesPlayCodSound(ZMSFX_PAP_UPGRADE, machine);
			zombiesPlayCodSound(ZMSFX_PAP_CRUNCH, machine);
		}

		// out of the machine, upgraded, to be taken
		if (prev < ZOMBIE_PAP_OFFERAT && now >= ZOMBIE_PAP_OFFERAT) {
			zombiesCodOr(ZMSFX_PAP_READY, machine, SFX_PICKUP_SHIELD, 0.8f);
		}

		// not taken in time: gone, as Call of Duty's is
		if (now >= ZOMBIE_PAP_TOTAL60) {
			g_ZombiePap60[playernum] = -1;
			// back in without a sound, as Black Ops II's
			zombiesPapHideGun();
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
			zombiesCodOr(ZMSFX_PAP_KNUCKLE, NULL, SFX_MENU_SELECT, 1.0f);
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
		zombiesCodOr(ZMSFX_PAP_DENY, machine, SFX_MENU_ERROR, 0.4f);
		return;
	}

	g_ZombiePoints[playernum] -= ZOMBIE_PAP_COST;
	zombiesAddPopup(playernum, -ZOMBIE_PAP_COST);
	zombiesCodOr(ZMSFX_PERK_BUY, NULL, SFX_MENU_SELECT, 1.0f);

	if (zmSfxSound(ZMSFX_PAP_UPGRADE) <= 0) {
		zombiesPlaySound(SFX_MAULER_CHARGE, 0.6f);
	}

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
// A gun handed over mid-reload comes up at once, as Call of Duty's does: the
// reload is dropped (bondgun.c holds a switch until the hands are idle). For
// the current player.
static void zombiesDropReload(void)
{
	s32 h;

	for (h = 0; h < 2; h++) {
		if (g_Vars.currentplayer->hands[h].state == HANDSTATE_RELOAD) {
			bgunSetState(h, HANDSTATE_IDLE);
		}
	}
}

static void zombiesBoxGive(s32 playernum, s32 index)
{
	const s32 prevplayernum = g_Vars.currentplayernum;
	struct mpweapon *mpweapon = &g_MpWeapons[index];

	setCurrentPlayerNum(playernum);
	zombiesDropReload();

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

			if (g_ZombiePoints[i] < zombiesBoxCost() || g_ZombieBoxNumGuns <= 0) {
				zombiesCodOr(ZMSFX_BUY_DENY, NULL, SFX_MENU_ERROR, 0.4f);
				g_ZombieSoundFor = -1;
				continue;
			}

			g_ZombiePoints[i] -= zombiesBoxCost();
			zombiesAddPopup(i, -zombiesBoxCost());
			zombiesCodOr(ZMSFX_PERK_BUY, NULL, SFX_MENU_SELECT, 0.8f);
			g_ZombieSoundFor = -1;

			// Black Ops II's: the lid up, the music box and the guns going round,
			// from the box
			zombiesPlayCodSound(ZMSFX_BOX_OPEN, box);
			zombiesPlayCodSound(ZMSFX_BOX_MUSIC, box);
			zombiesPlayCodSound(ZMSFX_BOX_SPIN, box);

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

			// landed: Black Ops II's music box has its own end
			if (zmSfxSound(ZMSFX_BOX_MUSIC) <= 0) {
				zombiesPlaySound(SFX_PICKUP_SHIELD, 1.0f);
			}

			g_ZombieBoxState = ZOMBIEBOX_OFFER;
			g_ZombieBoxTime60 = 0;
		} else if (g_ZombieBoxTime60 >= g_ZombieBoxNextSwap60) {
			// round they go, slowing: every 4 60ths at first, every 20 at the end
			s32 index = zombiesBoxPick(-1, false);

			if (index >= 0) {
				zombiesBoxShowGun(index);
			}

			// a tick each gun, the game's own, when Black Ops II's spin is not there
			if (zmSfxSound(ZMSFX_BOX_SPIN) <= 0) {
				zombiesPlaySound(SFX_PICKUP_AMMO, 1.6f);
			}

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
			zombiesCodOr(ZMSFX_BOX_CLOSE, box, SFX_PICKUP_AMMO, 1.0f);

			zombiesBoxHideGun();
			g_ZombieBoxState = ZOMBIEBOX_IDLE;
			g_ZombieBoxUser = -1;
		} else if (g_ZombieBoxTime60 >= ZOMBIE_BOX_OFFER60) {
			// not taken: back into the box, the lid down
			zombiesPlayCodSound(ZMSFX_BOX_CLOSE, box);
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
 * where it fell, no more than ZOMBIE_DROPS_PER_ROUND a round. As Black Ops
 * II's _zm_powerups.gsc has it, that is not a flat chance: the next kill
 * after the team's points earned (spent or not) pass a mark drops one, the
 * mark ZOMBIE_DROP_INCREMENT on at first and 1.14 times further each time;
 * besides, any kill drops one ZOMBIE_DROP_CHANCE times in a hundred. It is
 * Black Ops II's model (zombiesRenderPowerup()) turning and bobbing over the
 * floor, on the radar in green; it lasts ZOMBIE_DROP_LIFE60, blinking after
 * ZOMBIE_DROP_SOLID60 (zombiesDropShown()). Any player standing who walks
 * into it fills every gun everyone alive is carrying, and "MAX AMMO!" goes up
 * on every screen.
 */
#define ZOMBIE_DROP_CHANCE      3       // in a hundred, a kill: randomint(100) > 2 drops none
#define ZOMBIE_DROP_INCREMENT   2000.0f // zombie_powerup_drop_increment
#define ZOMBIE_DROPS_PER_ROUND  4
#define ZOMBIE_DROP_SOLID60     900     // _zm_powerups' powerup_timeout: 15 seconds as it is,
#define ZOMBIE_DROP_LIFE60      1590    // then 40 blinks, slower to quicker: 26.5 seconds in all
#define ZOMBIE_DROP_RANGE       90.0f   // how near a player picks it up
#define ZOMBIE_DROP_SIZE        80.0f   // how long the crate is (34 was easy to miss)
#define ZOMBIE_DROP_FLOAT       40.0f   // its bottom this far over the floor: waist high, as Call of Duty's float
#define ZOMBIE_DROP_BANNER60    150     // how long "MAX AMMO!" stays up

/**
 * And the rest of Call of Duty's power-ups, from the same drops: which one a
 * drop is goes round all of them in a shuffled order before any comes again, as
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
 * - Carpenter: every barrier's boards back up at once, and each player
 *   standing gets 200. Not dealt on an arena with no barriers.
 * - Fire Sale: for ZOMBIE_POWERUP_TIME60 the Mystery Box is
 *   ZOMBIE_FIRESALE_COST a go.
 *
 * Each has its own thing on the floor until there are models of their own:
 * Max Ammo a green crate, Insta-Kill a red knife, Double Points gold pills,
 * the Nuke an orange N-Bomb, Carpenter a hammer, Fire Sale a red card. The
 * three that last count down at the bottom of the screen, blinking for their
 * last five seconds.
 */
#define ZOMBIEPOWERUP_MAXAMMO      0
#define ZOMBIEPOWERUP_INSTAKILL    1
#define ZOMBIEPOWERUP_DOUBLEPOINTS 2
#define ZOMBIEPOWERUP_NUKE         3
#define ZOMBIEPOWERUP_CARPENTER    4
#define ZOMBIEPOWERUP_FIRESALE     5
#define ZOMBIEPOWERUP_COUNT        6

#define ZOMBIE_NUKE_POINTS      400
#define ZOMBIE_NUKE_FLASH60     45  // the white of the flash, fading
#define ZOMBIE_CARPENTER_POINTS 200
#define ZOMBIE_FIRESALE_COST    10

struct zombiepowerup {
	s32 mpweaponnum;   // whose pickup model it is, -1 for none
	s32 model;         // or a model of its own, 0 for none (and the ammo crate when neither)
	u8 colour[3];      // what it is lit, whatever light it is in
	u32 radar;
	char *banner;
	bool stand;        // stood on end as it turns, as a gun's pickup is (a flat thing seen edge on otherwise)
};

static struct zombiepowerup g_ZombiePowerups[ZOMBIEPOWERUP_COUNT] = {
	{ -1,                   0,                     { 0x70, 0xff, 0x70 }, 0x40ff4000, "MAX AMMO!",      false },
	{ MPWEAPON_COMBATKNIFE, 0,                     { 0xff, 0x50, 0x50 }, 0xff404000, "INSTA-KILL!",    true  },
	// Hold the Briefcase's case: a case of cash, in gold (the Combat Boost's
	// pills said nothing about points)
	{ -1,                   MODEL_CHRBRIEFCASE,    { 0xff, 0xe0, 0x40 }, 0xffe04000, "DOUBLE POINTS!", false },
	{ MPWEAPON_NBOMB,       0,                     { 0xff, 0x90, 0x30 }, 0xff902000, "KABOOM!",        true  },
	{ -1,                   MODEL_CHRLUMPHAMMER,   { 0x80, 0xc0, 0xff }, 0x80c0ff00, "CARPENTER!",     true  },
	{ -1,                   MODEL_KEYCARD,         { 0xff, 0x40, 0x30 }, 0xff403000, "FIRE SALE!",     true  },
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
static f32 g_ZombieDropIncrement = ZOMBIE_DROP_INCREMENT;
static s32 g_ZombieScoreToDrop = 0;        // over g_ZombieTeamEarned
static bool g_ZombieDropDue = false;       // zombie_drop_item: the next kill drops one
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
	g_ZombieTeamEarned = 0;
	g_ZombieDropIncrement = ZOMBIE_DROP_INCREMENT;
	g_ZombieScoreToDrop = (s32)ZOMBIE_DROP_INCREMENT;
	g_ZombieDropDue = false;
	g_ZombieMaxAmmoBanner60 = -1;
	g_ZombiePowerupBagLeft = 0;
	g_ZombieInstaKill60 = 0;
	g_ZombieDoublePoints60 = 0;
	g_ZombieFireSale60 = 0;
	g_ZombieNukeFlash60 = -1;
}

static s32 zombiesNextPowerupInBag(void);
static s32 g_ZombieNumBarriers;

// The next power-up: all of them in a shuffled order, then shuffled again.
// Carpenter is passed over where there are no barriers to put back.
static s32 zombiesNextPowerup(void)
{
	s32 kind = zombiesNextPowerupInBag();
	s32 tries;

	// (a fresh shuffle can put it first again)
	for (tries = 0; tries < 4 && kind == ZOMBIEPOWERUP_CARPENTER && g_ZombieNumBarriers == 0; tries++) {
		kind = zombiesNextPowerupInBag();
	}

	return kind;
}

static s32 zombiesNextPowerupInBag(void)
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

/**
 * Whether a power-up is seen this age: as powerup_timeout has it, 15 seconds
 * whole, then 40 turns of shown and hidden, the first 15 half a second each,
 * the next 10 a quarter, the last 15 a tenth
 */
static bool zombiesDropShown(s32 age60)
{
	s32 t = age60 - ZOMBIE_DROP_SOLID60;
	s32 i;

	if (t < 0) {
		return true;
	}

	for (i = 0; i < 40; i++) {
		const s32 wait = i < 15 ? 30 : i < 25 ? 15 : 6;

		if (t < wait) {
			return (i % 2) == 0;
		}

		t -= wait;
	}

	return false;
}

/**
 * Black Ops II's power-ups (zombie_ammocan, zombie_skull, zombie_x2_icon,
 * zombie_bomb, zombie_carpenter, zombie_firesale: tools/zombies/glb2obj.py
 * into the build's zombies/models/pu_<name>/), drawn in place of the stand-in
 * the drop is made from, ZOMBIE_POWERUP_MESHSIZE on its longest side, its middle
 * ZOMBIE_POWERUP_FLOAT over the floor, turning and bobbing as the stand-in does. Without the OBJ the stand-in is drawn.
 */
#define ZOMBIE_POWERUP_MESHSIZE 40.0f
#define ZOMBIE_POWERUP_GLOW     48.0f // the green glow's half size about it (fx_zombie_powerup_on)
#define ZOMBIE_POWERUP_FLOAT    85.0f // its middle over the floor: a man's waist

static struct zombiemesh g_ZombiePowerupMeshes[ZOMBIEPOWERUP_COUNT];

static const char *g_ZombiePowerupMeshNames[ZOMBIEPOWERUP_COUNT] = {
	"pu_ammocan", "pu_skull", "pu_x2_icon", "pu_bomb", "pu_carpenter", "pu_firesale",
};

static struct zombiemesh *zombiesPowerupMesh(s32 kind)
{
	struct zombiemesh *zm;

	if (kind < 0 || kind >= ZOMBIEPOWERUP_COUNT) {
		return NULL;
	}

	zm = &g_ZombiePowerupMeshes[kind];

	if (!zm->tried) {
		char rel[96];

		snprintf(rel, sizeof(rel), "$E/zombies/models/%s/%s.obj", g_ZombiePowerupMeshNames[kind], g_ZombiePowerupMeshNames[kind]);
		zombiesMeshBuildFrom(zm, rel, g_ZombiePowerups[kind].banner);
	}

	return zm->gdl ? zm : NULL;
}

// A drop's middle this frame: floating at the waist, bobbing (zombiesDropPlace())
static void zombiesPowerupCentre(struct zombiedrop *drop, f32 pos[3])
{
	pos[0] = drop->obj.prop->pos.x;
	pos[1] = drop->floory + ZOMBIE_POWERUP_FLOAT + sinf(drop->age60 * 0.07f) * 7;
	pos[2] = drop->obj.prop->pos.z;
}

/**
 * Black Ops II's glow about a power-up (fx_zombie_powerup_on): green light
 * added round its model, drawn with the Ray Gun's sprites (zombiesRenderRayFx()).
 * Asked with draw false, whether there is any (non-NULL for some)
 */
static Gfx *zombiesRenderPowerupGlows(Gfx *gdl, Mtxf *cam, struct coord *campos, const f32 right[3], const f32 up[3], bool draw)
{
	s32 i;
	s32 k;

	for (i = 0; i < ARRAYCOUNT(g_ZombieDrops); i++) {
		struct zombiedrop *drop = &g_ZombieDrops[i];
		struct coord centre;
		f32 pos[3];
		f32 a[3];
		f32 b[3];
		f32 pulse;

		if (!drop->active || drop->obj.prop == NULL || (drop->obj.flags2 & OBJFLAG2_INVISIBLE)
				|| zombiesPowerupMesh(drop->kind) == NULL) {
			continue;
		}

		if (!draw) {
			return (Gfx *)1;
		}

		zombiesPowerupCentre(drop, pos);

		// in front of the model, towards the eye, so it is about it all round
		// and not hidden behind it
		{
			f32 to[3] = { campos->x - pos[0], campos->y - pos[1], campos->z - pos[2] };
			f32 len = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
			f32 pull = len > ZOMBIE_POWERUP_MESHSIZE ? ZOMBIE_POWERUP_MESHSIZE * 0.6f / len : 0;

			centre.x = pos[0] + to[0] * pull;
			centre.y = pos[1] + to[1] * pull;
			centre.z = pos[2] + to[2] * pull;
		}

		pulse = 0.85f + 0.15f * sinf(drop->age60 * 0.1f);

		// a wide soft green, and a smaller brighter one in it
		for (k = 0; k < 3; k++) {
			a[k] = right[k] * ZOMBIE_POWERUP_GLOW * pulse;
			b[k] = up[k] * ZOMBIE_POWERUP_GLOW * pulse;
		}

		gDPSetPrimColor(gdl++, 0, 0, 0x30, 0xff, 0x40, 0x80);
		gdl = zombiesRayQuad(gdl, cam, &centre, a, b);

		for (k = 0; k < 3; k++) {
			a[k] *= 0.5f;
			b[k] *= 0.5f;
		}

		gDPSetPrimColor(gdl++, 0, 0, 0x60, 0xff, 0x60, 0x38);
		gdl = zombiesRayQuad(gdl, cam, &centre, a, b);
	}

	return draw ? gdl : NULL;
}

static bool zombiesRenderPowerup(struct modelrenderdata *renderdata, struct defaultobj *obj)
{
	struct zombiedrop *drop = NULL;
	struct zombiemesh *zm;
	f32 centre[3];
	f32 pos[3];
	f32 longest;
	f32 k;
	Mtxf world;
	Mtxf *mtx;
	s32 i;
	s32 j;

	for (i = 0; i < ARRAYCOUNT(g_ZombieDrops); i++) {
		if (obj == &g_ZombieDrops[i].obj) {
			drop = &g_ZombieDrops[i];
			break;
		}
	}

	if (drop == NULL || obj->prop == NULL || (zm = zombiesPowerupMesh(drop->kind)) == NULL) {
		return false;
	}

	if (!(renderdata->flags & MODELRENDERFLAG_OPA)) {
		return true;
	}

	longest = zm->width > zm->height ? zm->width : zm->height;
	longest = zm->depth > longest ? zm->depth : longest;
	k = ZOMBIE_POWERUP_MESHSIZE / (longest * ZOMBIE_MESH_FIXED);

	centre[0] = zm->midx * ZOMBIE_MESH_FIXED;
	centre[1] = (zm->ymin + zm->height / 2) * ZOMBIE_MESH_FIXED;
	centre[2] = (zm->zmin + zm->depth / 2) * ZOMBIE_MESH_FIXED;

	zombiesPowerupCentre(drop, pos);

	mtx4LoadYRotation(drop->age60 * 0.04f, &world);

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			world.m[i][j] *= k;
		}
	}

	for (i = 0; i < 3; i++) {
		world.m[3][i] = pos[i] - (world.m[0][i] * centre[0] + world.m[1][i] * centre[1] + world.m[2][i] * centre[2]);
	}

	mtx = gfxAllocateMatrix();
	mtx4MultMtx4(camGetWorldToScreenMtxf(), &world, mtx);

	// its own gold, whatever light it is in, as Black Ops II's shine
	renderdata->gdl = zombiesBottleDraw(renderdata->gdl, zm, mtx, 0xff, 0xff, 0xff);

	return true;
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
	if (!zombiesDropShown(drop->age60)) {
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

	// watch_for_drop: the team's points past the mark make the next kill's
	if (g_ZombieTeamEarned > g_ZombieScoreToDrop) {
		g_ZombieDropIncrement *= 1.14f;
		g_ZombieScoreToDrop = g_ZombieTeamEarned + (s32)g_ZombieDropIncrement;
		g_ZombieDropDue = true;
	}

	if (victim->prop == NULL || !g_ZombieCfgPowerups || g_ZombieDropsThisRound >= ZOMBIE_DROPS_PER_ROUND) {
		return;
	}

	if ((s32)(rngRandom() % 100) >= ZOMBIE_DROP_CHANCE && !g_ZombieDropDue) {
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
		g_ZombieDropDue = false;
		zombiesCodOr(ZMSFX_PU_SPAWN, NULL, SFX_PICKUP_SHIELD, 1.5f);
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
			if (g_ZombiePowerups[drop->kind].stand) {
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

		g_ZombieGrenades[p] = ZOMBIE_GRENADES_MAX;
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
			g_ZombieTeamEarned += ZOMBIE_NUKE_POINTS;
			zombiesAddPopup(i, ZOMBIE_NUKE_POINTS);
		}
	}

	g_ZombieNukeFlash60 = 0;
}

static void zombiesBarrierSet(s32 b, s32 boards);

// Every barrier's boards back up at once, and each player standing gets 200
static void zombiesCarpenter(void)
{
	s32 i;

	for (i = 0; i < g_ZombieNumBarriers; i++) {
		zombiesBarrierSet(i, 0x7fff); // a full set: it holds to its own count
	}

	for (i = 0; i < PLAYERCOUNT(); i++) {
		if (zombiesPlayerIsUp(i)) {
			g_ZombiePoints[i] += ZOMBIE_CARPENTER_POINTS;
			g_ZombieTeamEarned += ZOMBIE_CARPENTER_POINTS;
			zombiesAddPopup(i, ZOMBIE_CARPENTER_POINTS);
		}
	}
}

// What the Mystery Box costs now: Fire Sale's 10 while it lasts
static s32 zombiesBoxCost(void)
{
	return g_ZombieFireSale60 > 0 ? ZOMBIE_FIRESALE_COST : ZOMBIE_BOX_COST;
}

// A power-up picked up: what it does
static void zombiesPowerupTaken(s32 kind)
{
	switch (kind) {
	case ZOMBIEPOWERUP_MAXAMMO:
		zombiesMaxAmmo();
		zombiesCodOr(ZMSFX_PU_MAXAMMO, NULL, SFX_PICKUP_AMMO, 0.8f);
		break;
	case ZOMBIEPOWERUP_INSTAKILL:
		g_ZombieInstaKill60 = ZOMBIE_POWERUP_TIME60;
		zombiesPlayCodSound(ZMSFX_PU_INSTAKILL, NULL);
		break;
	case ZOMBIEPOWERUP_DOUBLEPOINTS:
		g_ZombieDoublePoints60 = ZOMBIE_POWERUP_TIME60;
		break;
	case ZOMBIEPOWERUP_NUKE:
		zombiesNuke();
		zombiesCodOr(ZMSFX_PU_NUKE, NULL, SFX_EXPLOSION_8098, 1.0f);
		break;
	case ZOMBIEPOWERUP_CARPENTER:
		zombiesCarpenter();
		zombiesPlayCodSound(ZMSFX_PU_CARPENTER, NULL);
		break;
	case ZOMBIEPOWERUP_FIRESALE:
		g_ZombieFireSale60 = ZOMBIE_POWERUP_TIME60;
		break;
	}

	// Black Ops II's grab, under the power-up's own, and the announcer calling
	// it: Richtofen, as Tranzit, Die Rise and Buried have him
	zombiesCodOr(ZMSFX_PU_GRAB, NULL, SFX_PICKUP_SHIELD, 1.0f);

	if (kind >= 0 && kind < ZOMBIEPOWERUP_COUNT) {
		zombiesPlayCodSound(ZMSFX_ANNOUNCER + kind, NULL);
	}
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

// Insta-Kill's, Double Points' and Fire Sale's time left, low in the middle of the view,
// blinking for the last five seconds
static Gfx *zombiesRenderPowerupTimers(Gfx *gdl)
{
	struct fontchar *chars = g_CharsHandelGothicSm;
	struct font *font = g_FontHandelGothicSm;
	char text[48];
	s32 line = 0;
	s32 k;

	for (k = 0; k < 3; k++) {
		s32 left = k == 0 ? g_ZombieInstaKill60 : k == 1 ? g_ZombieDoublePoints60 : g_ZombieFireSale60;
		u32 colour = k == 0 ? 0xff6060e0 : k == 1 ? 0xffe040e0 : 0xff8040e0;
		s32 textwidth;
		s32 textheight;

		if (left <= 0) {
			continue;
		}

		if (left < 300 && (left / 15) % 2) {
			line++;
			continue;
		}

		sprintf(text, "%s %d", k == 0 ? "INSTA-KILL" : k == 1 ? "DOUBLE POINTS" : "FIRE SALE", (left + 59) / 60);

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
	// Double Points running out: Black Ops II's sound for it
	if (g_ZombieDoublePoints60 > 0 && g_ZombieDoublePoints60 <= g_Vars.lvupdate60 && !NET_CLIENT) {
		zombiesPlayCodSound(ZMSFX_PU_DBLPOINTS_OFF, NULL);
	}

	g_ZombieDoublePoints60 = g_ZombieDoublePoints60 > g_Vars.lvupdate60 ? g_ZombieDoublePoints60 - g_Vars.lvupdate60 : 0;
	g_ZombieFireSale60 = g_ZombieFireSale60 > g_Vars.lvupdate60 ? g_ZombieFireSale60 - g_Vars.lvupdate60 : 0;

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
		zombiesCodOr(ZMSFX_BUY_DENY, NULL, SFX_MENU_ERROR, 0.4f);
		return;
	}

	g_ZombiePoints[playernum] -= cost;
	zombiesAddPopup(playernum, -cost);
	zombiesCodOr(ZMSFX_PERK_BUY, NULL, SFX_MENU_SELECT, 1.0f);

	// the first buy puts the gun up in its outline, for everyone, for good
	zombiesWallBuyShowGun(buy);

	if (owned) {
		setCurrentPlayerNum(playernum);
		zombiesFillGun(weaponnum, ZOMBIE_AMMO_CLIPS, false);
		setCurrentPlayerNum(prevplayernum);
		zombiesCodOr(ZMSFX_WALL_BUY, NULL, SFX_PICKUP_AMMO, 1.0f);
	} else {
		zombiesBoxGive(playernum, buy->mpweaponnum);
		zombiesCodOr(ZMSFX_WALL_BUY, NULL, SFX_PICKUP_GUN, 1.0f);
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

/**
 * Buyable doors and barriers, as Call of Duty's (CLAUDE-notes/netplay.md,
 * "Zombies: barriers and doors").
 *
 * Zones. The arena's waypoints are joined into zones with every link through
 * a doorway taken out: a door whose links are the only way between two zones
 * is a door that can be locked. A door with a way round it, or with a zone of
 * fewer than ZOMBIE_ZONE_MINPADS spawn pads on a side, is left a door, its
 * zones one (again and again until nothing changes), and no more than
 * ZOMBIE_DOOR_MAX doorways are kept, the smallest zones merged first. The
 * players start in the zone with the most spawn pads; a door's price is how
 * many doors out from there it is (750, 1000, 1250). Nothing random: every
 * machine online makes the same doors itself, as it places the machines.
 *
 * A locked door wants a key nobody has (ZOMBIE_DOOR_KEY: the player's own
 * and automatic doors ask doorIsUnlocked()), cannot be activated, and
 * chrOpenDoor() leaves it shut for the simulants (zombiesDoorBarsChrs());
 * its waypoint links are taken out (navDisableSegment()), so a zombie's
 * route never runs through it. The players spawn, and the zombies come in,
 * only in the zones opened so far (zombiesChooseSpawnLocation()), which are
 * joined by the doors bought, so a zombie never has a locked door between it
 * and a player. Modern simulant movement (Mod.SimBrain) walks a navmesh that
 * has no doors: while any door is shut the simulants walk stock's waypoints
 * (simbrainOwns(), zombiesDoorsShut()).
 *
 * Barriers. Up to ZOMBIE_BARRIER_MAX boarded windows go on walls beside spawn
 * pads, one a zone first, then the furthest from everything else. Most
 * zombies come in at one in an open zone (zombiesBarrierPick()) and stand at
 * its boards, held, tearing one off every ZOMBIE_BARRIER_TEAR60 until none
 * is left; a player holding Interact at one puts them back one at a time, ten
 * points a board, ZOMBIE_BARRIER_CAP a round.
 */
#define ZOMBIE_DOOR_MAX       12      // doorways locked at the most (a u16's bits online)
#define ZOMBIE_DOOR_LEAVES    4       // doors in one doorway (a double door's two)
#define ZOMBIE_DOOR_SEGS      12      // waypoint links through one doorway
#define ZOMBIE_DOOR_CANDS     48      // doorways looked at
#define ZOMBIE_DOOR_RANGE     160.0f  // how near the doorway's middle buying works
#define ZOMBIE_DOOR_KEY       0x40000000 // a key nobody carries
#define ZOMBIE_ZONE_MINPADS   2       // spawn pads a zone has, at the least, to be shut off
#define ZOMBIE_WP_MAX         1024    // waypoints an arena may have to have doors

#define ZOMBIE_BARRIER_MAX    8
#define ZOMBIE_BARRIER_BOARDS 6
#define ZOMBIE_BARRIER_REACH  400.0f  // how far from its pad the barrier's wall may be
#define ZOMBIE_BARRIER_APART  350.0f  // from every machine, wall buy, doorway and other barrier
#define ZOMBIE_BARRIER_STAND  30.0f   // a zombie tearing at it stands this far out from the wall
#define ZOMBIE_BARRIER_RANGE  130.0f  // how near (of the front, ZOMBIE_BARRIER_FRONT) rebuilding works

// Call of Duty's barrier at its own size (zmcore_basicwoodbarrier, its board
// tears aligned to it): its origin is the window's sill, 31 inches over the
// floor, a zombie waiting 23 inches behind it and the boards nailed 28 to 34
// in front of it, on the room's side of a wall that thick. Perfect Dark's
// walls have nothing behind them, so the window stands out from the wall: a
// pen between, where the zombie waits
#define ZOMBIE_COD_INCH       2.54f   // our units an inch
#define ZOMBIE_BARRIER_ORIGIN (ZOMBIE_BARRIER_STAND + 23 * ZOMBIE_COD_INCH) // the origin out from the wall
#define ZOMBIE_BARRIER_SILL   (31 * ZOMBIE_COD_INCH)
#define ZOMBIE_BARRIER_INSIDE (ZOMBIE_BARRIER_ORIGIN + 28 * ZOMBIE_COD_INCH) // the boards' face of the wall
#define ZOMBIE_BARRIER_FRONT  (ZOMBIE_BARRIER_ORIGIN + 52 * ZOMBIE_COD_INCH) // where a player rebuilds it
#define ZOMBIE_BARRIER_OPEN   (32 * ZOMBIE_COD_INCH) // the opening, half its width
#define ZOMBIE_BARRIER_TOP    (ZOMBIE_BARRIER_SILL + 60 * ZOMBIE_COD_INCH) // its top over the floor
#define ZOMBIE_BARRIER_TEAR60 54      // a board torn off this often
#define ZOMBIE_BARRIER_FIX60  33      // and put back this often, Interact held
#define ZOMBIE_BARRIER_POINTS 10
#define ZOMBIE_BARRIER_CAP    500     // Call of Duty's rebuild_barrier_cap_per_round
#define ZOMBIE_BARRIER_NEAR   3000.0f // a zombie goes to a barrier this near a player, when there is one
#define ZOMBIE_BOARD_FALL60   36      // a torn board falling
#define ZOMBIE_BOARD_FIX60    14      // a board coming back on

struct zombiedoor {
	s16 portal;
	s16 numleaves;
	struct doorobj *leaves[ZOMBIE_DOOR_LEAVES];
	s32 numsegs;
	s16 segs[ZOMBIE_DOOR_SEGS][2];
	s32 comps[2];      // the zones either side (waypoint roots)
	s32 cost;
	struct coord pos;  // the doorway's middle
	bool open;
};

struct zombiebarrier {
	struct defaultobj obj; // drawn as its boards (zombiesRenderBarrier())
	struct coord pos;      // the boards' foot, against the wall
	struct coord wall;     // the way to the wall
	struct coord stand;    // where a zombie at it stands
	RoomNum rooms[8];
	RoomNum standrooms[8];
	s32 comp;
	s32 boards;
	s32 zombie;            // the bot tearing it, -1 none
	s32 tear60;            // its zombie through a board's grab, hold and pull
	s32 pulling;           // the board being pulled off, -1 none
	s32 enter60;           // its zombie climbing in over the sill, -1 not
	s32 reach60;           // its zombie swiping through the boards at a player, -1 not
	s32 reachplayer;       // ... at whom
	bool reachleft;        // ... with which arm, each in turn
	bool reachhit;         // ... and whether the blow has landed
	s16 anim60[ZOMBIE_BARRIER_BOARDS]; // since each board last went or came
	bool torn[ZOMBIE_BARRIER_BOARDS];  // ... and which
	bool placed;
};

static struct zombiedoor g_ZombieDoors[ZOMBIE_DOOR_MAX];
static s32 g_ZombieNumDoors = 0;
static bool g_ZombieDoorsMade = false;
static s16 g_ZombieWpComp[ZOMBIE_WP_MAX];  // each waypoint's zone (its root)
static u8 g_ZombieCompOpen[ZOMBIE_WP_MAX]; // by root
static s16 g_ZombiePadCompPad[MAX_SPAWNPOINTS];
static s16 g_ZombiePadCompOf[MAX_SPAWNPOINTS];
static s32 g_ZombieNumPadComps = 0;
static s32 g_ZombieNearDoor[MAX_PLAYERS];
static s32 g_ZombieDoorHeld60[MAX_PLAYERS];
static bool g_ZombieDoorNeedsRelease[MAX_PLAYERS];
static u32 g_ZombieNetDoorMask = 0; // client: the doors the host has bought

static struct zombiebarrier g_ZombieBarriers[ZOMBIE_BARRIER_MAX];
static s32 g_ZombieNumBarriers = 0;
static s32 g_ZombieNearBarrier[MAX_PLAYERS];
static s32 g_ZombieFix60[MAX_PLAYERS];
static s32 g_ZombieRebuilt[MAX_PLAYERS];   // the points rebuilding has given this round
static s32 g_ZombieRebuiltRound = -1;

u32 setupGetCmdLength(u32 *cmd);
void navDisableSegment(struct waypoint *a, struct waypoint *b);
void navEnableSegment(struct waypoint *a, struct waypoint *b);

/**
 * scenarios.c's scenarioReset(), each stage's setup freshly loaded: its
 * waypoints are whole again, and the doors are made afresh at the first
 * playerReset() (zombiesReserveJuggPad())
 */
void zombiesStageReset(void)
{
	g_ZombieDoorsMade = false;
	g_ZombieNumDoors = 0;
	g_ZombieNumPadComps = 0;
	g_ZombieStartComp = -1;
	g_ZombieNumBarriers = 0;
	g_ZombieNetDoorMask = 0;
}

static s32 zombiesWpRoot(s16 *parent, s32 i)
{
	while (parent[i] != i) {
		parent[i] = parent[parent[i]];
		i = parent[i];
	}

	return i;
}

// The doorway's line in plan (its two furthest corners), its heights and its middle
static bool zombiesPortalLine(s32 portalnum, f32 line[4], f32 *ymin, f32 *ymax, struct coord *mid)
{
	struct portalvertices *pv = (struct portalvertices *)((uintptr_t)g_BgPortals + g_BgPortals[portalnum].verticesoffset);
	f32 best = -1;
	s32 i;
	s32 j;

	if (pv->count < 3) {
		return false;
	}

	mid->x = mid->y = mid->z = 0;
	*ymin = 1e20f;
	*ymax = -1e20f;

	for (i = 0; i < pv->count; i++) {
		mid->x += pv->vertices[i].x / pv->count;
		mid->y += pv->vertices[i].y / pv->count;
		mid->z += pv->vertices[i].z / pv->count;

		if (pv->vertices[i].y < *ymin) {
			*ymin = pv->vertices[i].y;
		}

		if (pv->vertices[i].y > *ymax) {
			*ymax = pv->vertices[i].y;
		}

		for (j = i + 1; j < pv->count; j++) {
			f32 dx = pv->vertices[i].x - pv->vertices[j].x;
			f32 dz = pv->vertices[i].z - pv->vertices[j].z;

			if (dx * dx + dz * dz > best) {
				best = dx * dx + dz * dz;
				line[0] = pv->vertices[i].x;
				line[1] = pv->vertices[i].z;
				line[2] = pv->vertices[j].x;
				line[3] = pv->vertices[j].z;
			}
		}
	}

	return best > 40 * 40;
}

/**
 * A door in no portal (Temple's, Ruins', Grid's): its own slab, closed, as
 * its model's box stands - the box's widest way across in plan, its heights
 * and its middle
 */
static bool zombiesDoorSlab(struct doorobj *door, f32 line[4], f32 *ymin, f32 *ymax, struct coord *mid)
{
	struct modelrodata_bbox *bbox = door->base.model ? objFindBboxRodata(&door->base) : NULL;
	f32 lo[3];
	f32 hi[3];
	f32 c[3];
	f32 best = -1;
	f32 half = 0;
	s32 i;
	s32 j;

	if (bbox == NULL || door->base.prop == NULL) {
		return false;
	}

	lo[0] = objGetLocalXMin(bbox);
	hi[0] = objGetLocalXMax(bbox);
	lo[1] = objGetLocalYMin(bbox);
	hi[1] = objGetLocalYMax(bbox);
	lo[2] = objGetLocalZMin(bbox);
	hi[2] = objGetLocalZMax(bbox);

	for (j = 0; j < 3; j++) {
		c[j] = (&door->base.prop->pos.x)[j];

		for (i = 0; i < 3; i++) {
			c[j] += door->base.realrot[i][j] * (lo[i] + hi[i]) / 2;
		}
	}

	mid->x = c[0];
	mid->y = c[1];
	mid->z = c[2];

	for (i = 0; i < 3; i++) {
		f32 ex = door->base.realrot[i][0] * (hi[i] - lo[i]);
		f32 ey = door->base.realrot[i][1] * (hi[i] - lo[i]);
		f32 ez = door->base.realrot[i][2] * (hi[i] - lo[i]);

		half += (ey < 0 ? -ey : ey) / 2;

		if (ex * ex + ez * ez > best) {
			best = ex * ex + ez * ez;
			line[0] = c[0] - ex / 2;
			line[1] = c[2] - ez / 2;
			line[2] = c[0] + ex / 2;
			line[3] = c[2] + ez / 2;
		}
	}

	*ymin = c[1] - half;
	*ymax = c[1] + half;

	return best > 40 * 40;
}

/**
 * Where the link a-b crosses the doorway's line in plan: 0 none, else the
 * side a is on (1 or 2)
 */
static s32 zombiesLinkCrosses(const f32 line[4], f32 ymin, f32 ymax, const struct coord *a, const struct coord *b)
{
	f32 rx = b->x - a->x;
	f32 rz = b->z - a->z;
	f32 sx = line[2] - line[0];
	f32 sz = line[3] - line[1];
	f32 denom = rx * sz - rz * sx;
	f32 qx = line[0] - a->x;
	f32 qz = line[1] - a->z;
	f32 t;
	f32 u;
	f32 y;

	if (denom > -0.001f && denom < 0.001f) {
		return 0;
	}

	t = (qx * sz - qz * sx) / denom;
	u = (qx * rz - qz * rx) / denom;

	if (t <= 0 || t >= 1 || u < 0 || u > 1) {
		return 0;
	}

	y = a->y + (b->y - a->y) * t;

	if (y < ymin - 120 || y > ymax + 60) {
		return 0;
	}

	// a's side of the line
	return (sx * (a->z - line[1]) - sz * (a->x - line[0])) > 0 ? 1 : 2;
}

// The zone a place is in: its room's nearest waypoint's, else the nearest's
static s32 zombiesPosComp(struct coord *pos, RoomNum room, s32 numwp, const struct coord *wppos, const RoomNum *wproom)
{
	f32 best = 1e30f;
	f32 bestany = 1e30f;
	s32 found = -1;
	s32 foundany = -1;
	s32 i;

	for (i = 0; i < numwp; i++) {
		f32 dx = wppos[i].x - pos->x;
		f32 dy = wppos[i].y - pos->y;
		f32 dz = wppos[i].z - pos->z;
		f32 d = dx * dx + dy * dy * 4 + dz * dz;

		if (wproom[i] == room && d < best) {
			best = d;
			found = i;
		}

		if (d < bestany) {
			bestany = d;
			foundany = i;
		}
	}

	if (found < 0) {
		found = foundany;
	}

	return found >= 0 ? g_ZombieWpComp[found] : -1;
}

static s32 zombiesPadComp(s32 padnum)
{
	s32 i;

	for (i = 0; i < g_ZombieNumPadComps; i++) {
		if (g_ZombiePadCompPad[i] == padnum) {
			return g_ZombiePadCompOf[i];
		}
	}

	return -1;
}

static bool zombiesCompOpen(s32 comp)
{
	return g_ZombieNumDoors == 0 || comp < 0 || comp >= ZOMBIE_WP_MAX || g_ZombieCompOpen[comp];
}

// The spawn pads in the zones open so far; the whole list when none are
static s32 zombiesOpenSpawnPads(s16 *out)
{
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	s32 count = 0;
	s32 i;

	for (i = 0; i < n; i++) {
		if (zombiesCompOpen(zombiesPadComp(g_SpawnPoints[i]))) {
			out[count++] = g_SpawnPoints[i];
		}
	}

	if (count == 0) {
		for (i = 0; i < n; i++) {
			out[i] = g_SpawnPoints[i];
		}

		count = n;
	}

	return count;
}

/**
 * The arena's zones and the doors between them, locked (the comment above).
 * Once a stage; zombiesStageReset() clears it.
 */
static void zombiesMakeDoors(void)
{
	static struct coord wppos[ZOMBIE_WP_MAX];
	static RoomNum wproom[ZOMBIE_WP_MAX];
	static s16 parent[ZOMBIE_WP_MAX];
	static struct zombiedoor cands[ZOMBIE_DOOR_CANDS];
	static bool alive[ZOMBIE_DOOR_CANDS];
	static s16 padcount[ZOMBIE_WP_MAX];
	static s16 depth[ZOMBIE_WP_MAX];
	struct waypoint *waypoints = g_StageSetup.waypoints;
	struct liftobj *lifts[32];
	s32 numlifts = 0;
	s32 numcands = 0;
	s32 numwp = 0;
	s32 numportals = 0;
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	s32 numalive;
	s32 pass;
	s32 i;
	s32 j;
	s32 k;
	struct defaultobj *obj;

	if (g_ZombieDoorsMade) {
		return;
	}

	g_ZombieDoorsMade = true;
	g_ZombieNumDoors = 0;
	g_ZombieNumPadComps = 0;
	g_ZombieStartComp = -1;

	if (waypoints == NULL || g_StageSetup.props == NULL || g_BgPortals == NULL || n <= 0) {
		return;
	}

	while (waypoints[numwp].padnum >= 0) {
		if (++numwp >= ZOMBIE_WP_MAX) {
			sysLogPrintf(LOG_WARNING, "zombies: stage 0x%02x has over %d waypoints; no doors", g_Vars.stagenum, ZOMBIE_WP_MAX);
			return;
		}
	}

	while (g_BgPortals[numportals].verticesoffset != 0) {
		numportals++;
	}

	for (i = 0; i < numwp; i++) {
		struct pad pad;

		padUnpack(waypoints[i].padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);
		wppos[i] = pad.pos;
		wproom[i] = pad.room;
	}

	// the lifts' doors are the lifts'
	obj = (struct defaultobj *)g_StageSetup.props;

	while (obj->type != OBJTYPE_END) {
		if (obj->type == OBJTYPE_LIFT && obj->prop && numlifts < ARRAYCOUNT(lifts)) {
			lifts[numlifts++] = (struct liftobj *)obj;
		}

		obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj));
	}

	// every doorway with a plain door in it, its doors together
	obj = (struct defaultobj *)g_StageSetup.props;

	while (obj->type != OBJTYPE_END) {
		if (obj->type == OBJTYPE_DOOR && obj->prop) {
			struct doorobj *door = (struct doorobj *)obj;
			bool ok = door->portalnum < numportals
				&& door->keyflags == 0
				&& door->frac <= 0
				&& (door->base.flags & (OBJFLAG_DOOR_KEEPOPEN | OBJFLAG_CANNOT_ACTIVATE)) == 0
				&& (door->doorflags & DOORFLAG_DAMAGEONCONTACT) == 0
				&& (door->doortype == DOORTYPE_SLIDING || door->doortype == DOORTYPE_VERTICAL
						|| door->doortype == DOORTYPE_SWINGING || door->doortype == DOORTYPE_FLEXI1
						|| door->doortype == DOORTYPE_FLEXI2 || door->doortype == DOORTYPE_FLEXI3);
			s32 l;

			for (l = 0; ok && l < numlifts; l++) {
				for (k = 0; k < ARRAYCOUNT(lifts[l]->doors); k++) {
					if (lifts[l]->doors[k] == door) {
						ok = false;
					}
				}
			}

			if (ok) {
				// its doorway: the one in its portal; with no portal, the one
				// its sibling (a double door's other half) is in
				for (k = 0; k < numcands; k++) {
					if (door->portalnum >= 0 && cands[k].portal == door->portalnum) {
						break;
					}

					if (door->portalnum < 0 && cands[k].portal < 0) {
						struct doorobj *sib = door->sibling;
						s32 m;

						for (m = 0; m < cands[k].numleaves; m++) {
							for (sib = door->sibling; sib && sib != door && sib != cands[k].leaves[m]; sib = sib->sibling);

							if (sib == cands[k].leaves[m]) {
								break;
							}
						}

						if (m < cands[k].numleaves) {
							break;
						}
					}
				}

				if (k == numcands && numcands < ZOMBIE_DOOR_CANDS) {
					bzero(&cands[k], sizeof(cands[k]));
					cands[k].portal = door->portalnum;
					numcands++;
				}

				if (k < numcands && cands[k].numleaves < ZOMBIE_DOOR_LEAVES) {
					cands[k].leaves[cands[k].numleaves++] = door;
				}
			}
		}

		obj = (struct defaultobj *)((u32 *)obj + setupGetCmdLength((u32 *)obj));
	}

	// a double door is one doorway: two found apart (one half in a portal,
	// the other in none) go together, the portal's kept
	for (k = 0; k < numcands; k++) {
		for (j = k + 1; j < numcands; j++) {
			bool same = false;
			s32 a;
			s32 b;

			for (a = 0; a < cands[k].numleaves && !same; a++) {
				for (b = 0; b < cands[j].numleaves && !same; b++) {
					struct doorobj *sib = cands[k].leaves[a]->sibling;

					while (sib && sib != cands[k].leaves[a] && !same) {
						same = sib == cands[j].leaves[b];
						sib = sib->sibling;
					}
				}
			}

			if (!same) {
				continue;
			}

			for (b = 0; b < cands[j].numleaves && cands[k].numleaves < ZOMBIE_DOOR_LEAVES; b++) {
				cands[k].leaves[cands[k].numleaves++] = cands[j].leaves[b];
			}

			if (cands[k].portal < 0) {
				cands[k].portal = cands[j].portal;
			}

			cands[j] = cands[--numcands];
			j = k;
		}
	}

	// the waypoint links through each doorway
	for (k = 0; k < numcands; k++) {
		f32 line[4];
		f32 ymin;
		f32 ymax;

		alive[k] = false;

		if (cands[k].portal >= 0) {
			if (!zombiesPortalLine(cands[k].portal, line, &ymin, &ymax, &cands[k].pos)) {
				continue;
			}
		} else {
			// in no portal: the slabs of its doors together, their two
			// furthest ends in plan
			f32 ends[ZOMBIE_DOOR_LEAVES * 2][2];
			s32 numends = 0;
			f32 best = -1;
			s32 m;

			ymin = 1e20f;
			ymax = -1e20f;
			cands[k].pos.x = cands[k].pos.y = cands[k].pos.z = 0;

			for (m = 0; m < cands[k].numleaves; m++) {
				f32 l[4];
				f32 lo;
				f32 hi;
				struct coord c;

				if (!zombiesDoorSlab(cands[k].leaves[m], l, &lo, &hi, &c)) {
					continue;
				}

				ends[numends][0] = l[0];
				ends[numends][1] = l[1];
				ends[numends + 1][0] = l[2];
				ends[numends + 1][1] = l[3];
				numends += 2;
				ymin = lo < ymin ? lo : ymin;
				ymax = hi > ymax ? hi : ymax;
				cands[k].pos.x += c.x;
				cands[k].pos.y += c.y;
				cands[k].pos.z += c.z;
			}

			if (numends == 0) {
				continue;
			}

			cands[k].pos.x /= numends / 2;
			cands[k].pos.y /= numends / 2;
			cands[k].pos.z /= numends / 2;

			for (i = 0; i < numends; i++) {
				for (j = i + 1; j < numends; j++) {
					f32 dx = ends[i][0] - ends[j][0];
					f32 dz = ends[i][1] - ends[j][1];

					if (dx * dx + dz * dz > best) {
						best = dx * dx + dz * dz;
						line[0] = ends[i][0];
						line[1] = ends[i][1];
						line[2] = ends[j][0];
						line[3] = ends[j][1];
					}
				}
			}
		}

		for (i = 0; i < numwp; i++) {
			for (j = 0; waypoints[i].neighbours[j] >= 0; j++) {
				s32 other = waypoints[i].neighbours[j] & 0x3fff;
				s32 m;

				if (other <= i || other >= numwp
						|| !zombiesLinkCrosses(line, ymin, ymax, &wppos[i], &wppos[other])) {
					continue;
				}

				for (m = 0; m < cands[k].numsegs; m++) {
					if (cands[k].segs[m][0] == i && cands[k].segs[m][1] == other) {
						break;
					}
				}

				if (m == cands[k].numsegs && cands[k].numsegs < ZOMBIE_DOOR_SEGS) {
					cands[k].segs[m][0] = i;
					cands[k].segs[m][1] = other;
					cands[k].numsegs++;
				}
			}
		}

		alive[k] = cands[k].numsegs > 0;
	}

	// the zones, again and again until every doorway left is the only way
	// between two zones big enough to be worth shutting off
	for (pass = 0; pass < ZOMBIE_DOOR_CANDS * 2 + 4; pass++) {
		s32 worst = -1;
		s32 worstpads = 0x7fff;
		bool changed = false;

		for (i = 0; i < numwp; i++) {
			parent[i] = i;
		}

		for (i = 0; i < numwp; i++) {
			for (j = 0; waypoints[i].neighbours[j] >= 0; j++) {
				s32 other = waypoints[i].neighbours[j] & 0x3fff;
				bool cut = false;

				if (other >= numwp) {
					continue;
				}

				for (k = 0; k < numcands && !cut; k++) {
					s32 m;

					if (!alive[k]) {
						continue;
					}

					for (m = 0; m < cands[k].numsegs; m++) {
						if ((cands[k].segs[m][0] == i && cands[k].segs[m][1] == other)
								|| (cands[k].segs[m][1] == i && cands[k].segs[m][0] == other)) {
							cut = true;
							break;
						}
					}
				}

				if (!cut) {
					s32 ra = zombiesWpRoot(parent, i);
					s32 rb = zombiesWpRoot(parent, other);

					if (ra != rb) {
						// the lower one the root, so the zones' numbers are the arena's alone
						if (ra < rb) {
							parent[rb] = ra;
						} else {
							parent[ra] = rb;
						}
					}
				}
			}
		}

		for (i = 0; i < numwp; i++) {
			g_ZombieWpComp[i] = zombiesWpRoot(parent, i);
			padcount[i] = 0;
		}

		for (i = 0; i < n; i++) {
			struct pad pad;
			s32 comp;

			padUnpack(g_SpawnPoints[i], PADFIELD_POS | PADFIELD_ROOM, &pad);
			comp = zombiesPosComp(&pad.pos, pad.room, numwp, wppos, wproom);

			if (comp >= 0) {
				padcount[comp]++;
			}
		}

		numalive = 0;

		for (k = 0; k < numcands; k++) {
			s32 m;
			s32 pads;

			if (!alive[k]) {
				continue;
			}

			cands[k].comps[0] = g_ZombieWpComp[cands[k].segs[0][0]];
			cands[k].comps[1] = g_ZombieWpComp[cands[k].segs[0][1]];

			// a way round it: not a door that shuts anything off
			for (m = 0; m < cands[k].numsegs; m++) {
				if (g_ZombieWpComp[cands[k].segs[m][0]] == g_ZombieWpComp[cands[k].segs[m][1]]) {
					break;
				}
			}

			if (m < cands[k].numsegs || cands[k].comps[0] == cands[k].comps[1]) {
				alive[k] = false;
				changed = true;
				break;
			}

			pads = padcount[cands[k].comps[0]] < padcount[cands[k].comps[1]]
				? padcount[cands[k].comps[0]] : padcount[cands[k].comps[1]];

			if (pads < worstpads) {
				worstpads = pads;
				worst = k;
			}

			numalive++;
		}

		if (changed) {
			continue;
		}

		// a zone too small to be shut off, or too many doors: the smallest
		// zone goes in with its neighbour, one at a time
		if (worst >= 0 && (worstpads < ZOMBIE_ZONE_MINPADS || numalive > ZOMBIE_DOOR_MAX)) {
			alive[worst] = false;
			continue;
		}

		break;
	}

	// the spawn pads' zones, the whole list as it is before the machines
	// take theirs
	for (i = 0; i < n; i++) {
		struct pad pad;

		padUnpack(g_SpawnPoints[i], PADFIELD_POS | PADFIELD_ROOM, &pad);
		g_ZombiePadCompPad[g_ZombieNumPadComps] = g_SpawnPoints[i];
		g_ZombiePadCompOf[g_ZombieNumPadComps] = zombiesPosComp(&pad.pos, pad.room, numwp, wppos, wproom);
		g_ZombieNumPadComps++;
	}

	numalive = 0;

	for (k = 0; k < numcands; k++) {
		if (alive[k]) {
			numalive++;
		}
	}

	if (numalive == 0) {
		sysLogPrintf(LOG_NOTE, "zombies: stage 0x%02x: %d doorways, none locked (%d waypoints)", g_Vars.stagenum, numcands, numwp);
		return;
	}

	// the players' zone: the most spawn pads, ties to the lower root
	for (i = 0; i < numwp; i++) {
		if (g_ZombieWpComp[i] == i && (g_ZombieStartComp < 0 || padcount[i] > padcount[g_ZombieStartComp])) {
			g_ZombieStartComp = i;
		}
	}

	// how many doors out each zone is
	for (i = 0; i < numwp; i++) {
		depth[i] = -1;
		g_ZombieCompOpen[i] = 0;
	}

	depth[g_ZombieStartComp] = 0;

	for (pass = 0; pass < ZOMBIE_DOOR_MAX + 1; pass++) {
		for (k = 0; k < numcands; k++) {
			s32 a = cands[k].comps[0];
			s32 b = cands[k].comps[1];

			if (!alive[k]) {
				continue;
			}

			if (depth[a] >= 0 && (depth[b] < 0 || depth[b] > depth[a] + 1)) {
				depth[b] = depth[a] + 1;
			}

			if (depth[b] >= 0 && (depth[a] < 0 || depth[a] > depth[b] + 1)) {
				depth[a] = depth[b] + 1;
			}
		}
	}

	g_ZombieCompOpen[g_ZombieStartComp] = 1;

	// the doors kept, nearest the start first, locked
	for (pass = 0; pass <= ZOMBIE_DOOR_MAX; pass++) {
		for (k = 0; k < numcands && g_ZombieNumDoors < ZOMBIE_DOOR_MAX; k++) {
			struct zombiedoor *door = &g_ZombieDoors[g_ZombieNumDoors];
			s32 da = depth[cands[k].comps[0]];
			s32 db = depth[cands[k].comps[1]];
			s32 near;

			if (!alive[k] || da < 0 || db < 0) {
				continue;
			}

			near = da < db ? da : db;

			if (near != pass) {
				continue;
			}

			*door = cands[k];
			door->cost = 750 + 250 * (near < 2 ? near : 2);
			door->open = false;

			for (j = 0; j < door->numleaves; j++) {
				door->leaves[j]->keyflags |= ZOMBIE_DOOR_KEY;
				door->leaves[j]->base.flags |= OBJFLAG_CANNOT_ACTIVATE;
				door->leaves[j]->base.flags2 |= OBJFLAG2_SKIPDOORLOCKEDMSG;
			}

			for (j = 0; j < door->numsegs; j++) {
				navDisableSegment(&waypoints[door->segs[j][0]], &waypoints[door->segs[j][1]]);
			}

			sysLogPrintf(LOG_NOTE, "zombies: stage 0x%02x: door %d (portal %d, %d door%s, %d link%s) at %.0f %.0f %.0f, zones %d|%d (%d|%d pads), %d points",
					g_Vars.stagenum, g_ZombieNumDoors, door->portal, door->numleaves, door->numleaves == 1 ? "" : "s",
					door->numsegs, door->numsegs == 1 ? "" : "s", door->pos.x, door->pos.y, door->pos.z,
					door->comps[0], door->comps[1], padcount[door->comps[0]], padcount[door->comps[1]], door->cost);

			g_ZombieNumDoors++;
		}
	}

	sysLogPrintf(LOG_NOTE, "zombies: stage 0x%02x: %d doorways, %d locked, the players' zone %d (%d of %d pads)",
			g_Vars.stagenum, numcands, g_ZombieNumDoors, g_ZombieStartComp, padcount[g_ZombieStartComp], n);
}

// Whether any door is still shut (simbrain.c: stock's waypoints till none is)
s32 zombiesDoorsShut(void)
{
	s32 d;

	if (!zombiesIsActive()) {
		return 0;
	}

	for (d = 0; d < g_ZombieNumDoors; d++) {
		if (!g_ZombieDoors[d].open) {
			return 1;
		}
	}

	return 0;
}

// chraction.c's chrOpenDoor(): a door still to be bought stays shut to them
s32 zombiesDoorBarsChrs(struct prop *doorprop)
{
	s32 d;
	s32 l;

	if (!zombiesIsActive() || doorprop == NULL) {
		return 0;
	}

	for (d = 0; d < g_ZombieNumDoors; d++) {
		for (l = 0; l < g_ZombieDoors[d].numleaves; l++) {
			if (g_ZombieDoors[d].leaves[l]->base.prop == doorprop) {
				return !g_ZombieDoors[d].open;
			}
		}
	}

	return 0;
}

// A door bought: unlocked, its links back, its zones open, and on its way open
static void zombiesDoorOpen(s32 d)
{
	struct zombiedoor *door = &g_ZombieDoors[d];
	s32 j;

	if (door->open) {
		return;
	}

	door->open = true;

	for (j = 0; j < door->numleaves; j++) {
		door->leaves[j]->keyflags &= ~ZOMBIE_DOOR_KEY;
		door->leaves[j]->base.flags2 &= ~OBJFLAG2_SKIPDOORLOCKEDMSG;
	}

	for (j = 0; j < door->numsegs; j++) {
		navEnableSegment(&g_StageSetup.waypoints[door->segs[j][0]], &g_StageSetup.waypoints[door->segs[j][1]]);
	}

	g_ZombieCompOpen[door->comps[0]] = 1;
	g_ZombieCompOpen[door->comps[1]] = 1;

	sysLogPrintf(LOG_NOTE, "zombies: door %d open (zones %d and %d)", d, door->comps[0], door->comps[1]);
}

// A bought door stays open: opened, then kept so, and never closed by a use
static void zombiesDoorKeepOpen(struct zombiedoor *door)
{
	s32 j;

	for (j = 0; j < door->numleaves; j++) {
		struct doorobj *leaf = door->leaves[j];

		if (leaf->frac >= leaf->maxfrac) {
			leaf->base.flags |= OBJFLAG_DOOR_KEEPOPEN;
		} else if (leaf->mode == DOORMODE_IDLE || leaf->mode == DOORMODE_CLOSING) {
			doorsRequestMode(leaf, DOORMODE_OPENING);
		}
	}
}

static void zombiesTickDoors(void)
{
	s32 i;
	s32 d;

	for (d = 0; d < g_ZombieNumDoors; d++) {
		if (g_ZombieDoors[d].open) {
			zombiesDoorKeepOpen(&g_ZombieDoors[d]);
		}
	}

	for (i = 0; i < PLAYERCOUNT(); i++) {
		struct player *player = g_Vars.players[i];
		f32 bestdist = ZOMBIE_DOOR_RANGE * ZOMBIE_DOOR_RANGE;
		bool held;

		g_ZombieNearDoor[i] = -1;

		if (!zombiesPlayerIsUp(i) || g_ZombieDrink60[i] >= 0 || g_ZombieNearDowned[i] >= 0
				|| g_ZombieNearWall[i] >= 0 || g_ZombieNearBox[i] || g_ZombieNearJugg[i]
				|| g_ZombieNearPerk[i] >= 0 || g_ZombieNearPap[i]) {
			g_ZombieDoorHeld60[i] = 0;
			continue;
		}

		for (d = 0; d < g_ZombieNumDoors; d++) {
			struct coord *pos = &g_ZombieDoors[d].pos;
			f32 dx = player->prop->pos.x - pos->x;
			f32 dy = player->prop->pos.y - pos->y;
			f32 dz = player->prop->pos.z - pos->z;

			if (!g_ZombieDoors[d].open && dx * dx + dz * dz < bestdist && dy < 200 && dy > -200) {
				bestdist = dx * dx + dz * dz;
				g_ZombieNearDoor[i] = d;
			}
		}

		held = g_ZombieNearDoor[i] >= 0 && zombiesHoldingUse(i);

		if (!held) {
			g_ZombieDoorHeld60[i] = 0;
			g_ZombieDoorNeedsRelease[i] = false;
			continue;
		}

		if (g_ZombieDoorNeedsRelease[i]) {
			continue;
		}

		g_ZombieDoorHeld60[i] += g_Vars.lvupdate60;

		if (g_ZombieDoorHeld60[i] < ZOMBIE_BOX_HOLD60) {
			continue;
		}

		g_ZombieDoorNeedsRelease[i] = true;
		g_ZombieDoorHeld60[i] = 0;
		g_ZombieSoundFor = i;

		d = g_ZombieNearDoor[i];

		if (g_ZombiePoints[i] < g_ZombieDoors[d].cost) {
			zombiesCodOr(ZMSFX_BUY_DENY, NULL, SFX_MENU_ERROR, 0.4f);
		} else {
			g_ZombiePoints[i] -= g_ZombieDoors[d].cost;
			zombiesAddPopup(i, -g_ZombieDoors[d].cost);
			zombiesCodOr(ZMSFX_PERK_BUY, NULL, SFX_MENU_SELECT, 1.0f);
			zombiesDoorOpen(d);
			zombiesDoorKeepOpen(&g_ZombieDoors[d]);
			g_ZombieNearDoor[i] = -1;
		}

		g_ZombieSoundFor = -1;
	}
}

static bool zombiesDoorPrompt(s32 playernum, char *text)
{
	s32 d;

	if (playernum < 0 || playernum >= MAX_PLAYERS || (d = g_ZombieNearDoor[playernum]) < 0
			|| d >= g_ZombieNumDoors || g_ZombieDoors[d].open) {
		return false;
	}

	snprintf(text, 64, "Hold %s to open Door [Cost: %d]", zombiesUseName(playernum), g_ZombieDoors[d].cost);

	return true;
}

/**
 * scenarios.c's spawn hook: a zombie sent to a barrier stands at its boards;
 * anyone else on a spawn pad in the zones open so far
 */
bool zombiesChooseSpawnLocation(f32 chrradius, struct coord *pos, RoomNum *rooms, struct prop *prop, f32 *angle)
{
	s16 pads[MAX_SPAWNPOINTS];
	s32 n;

	if (g_ZombieSpawnAt >= 0 && g_ZombieSpawnAt < g_ZombieNumBarriers && prop && prop->type == PROPTYPE_CHR) {
		struct zombiebarrier *bar = &g_ZombieBarriers[g_ZombieSpawnAt];
		s32 i;

		*pos = bar->stand;

		for (i = 0; i < 7 && bar->standrooms[i] != -1; i++) {
			rooms[i] = bar->standrooms[i];
		}

		rooms[i] = -1;
		*angle = atan2f(bar->wall.x, bar->wall.z);

		return true;
	}

	if (g_ZombieNumDoors == 0) {
		return false;
	}

	n = zombiesOpenSpawnPads(pads);

	if (n <= 0) {
		return false;
	}

	*angle = playerChooseSpawnLocation(chrradius, pos, rooms, prop, pads, n);

	return true;
}

/**
 * Barriers
 */
static void zombiesResetBarriers(void)
{
	s32 i;

	bzero(g_ZombieBarriers, sizeof(g_ZombieBarriers));
	g_ZombieNumBarriers = 0;
	g_ZombieRebuiltRound = -1;

	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ZombieNearBarrier[i] = -1;
		g_ZombieFix60[i] = 0;
		g_ZombieRebuilt[i] = 0;
		g_ZombieNearDoor[i] = -1;
		g_ZombieDoorHeld60[i] = 0;
		g_ZombieDoorNeedsRelease[i] = false;
	}

	for (i = 0; i < MAX_BOTS; i++) {
		g_ZombieAtBarrier[i] = -1;
	}
}

// A barrier's boards up: the object it is drawn as, along its wall
static bool zombiesBarrierMakeObj(struct zombiebarrier *bar)
{
	struct defaultobj *obj = &bar->obj;
	struct prop *prop;
	struct coord mid;
	Mtxf mtx;

	bzero(obj, sizeof(*obj));
	obj->extrascale = 256;
	obj->type = OBJTYPE_BASIC;
	obj->modelnum = ZOMBIE_BOX_MODEL;
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
			return false;
		}

		setupLoadModeldef(obj->modelnum);

		if (g_ModelStates[obj->modelnum].modeldef == NULL) {
			return false;
		}
	}

	prop = objInitWithAutoModel(obj);

	if (prop == NULL || obj->model == NULL) {
		return false;
	}

	// the box's crate, long along the wall, the boards' middle; never drawn
	mtx4LoadYRotation(atan2f(bar->wall.z, -bar->wall.x), &mtx);
	mtx00015f04(obj->model->scale, &mtx);

	mid = bar->pos;
	mid.y += 100;
	mid.x -= bar->wall.x * 10;
	mid.z -= bar->wall.z * 10;

	func0f06a580(obj, &mid, &mtx, bar->rooms);

	propActivate(prop);
	propEnable(prop);

	obj->hidden2 &= ~OBJH2FLAG_CANREGEN;

	return true;
}

/**
 * The barriers on their walls beside the spawn pads: one in each zone the
 * pads are in first, then the furthest from everything else. The pads, the
 * walls and the machines are the arena's alone, so a client puts them where
 * the host does.
 */
/**
 * Whether Call of Duty's window, at its own size, fits on the wall walldist
 * from pad along wall: from the wall the room must run on at least
 * ZOMBIE_BARRIER_FRONT and a player's room beyond, and at the pen's back and
 * at the boards the wall must run ZOMBIE_BARRIER_OPEN and the jambs' width
 * either side. Complex's corridors had it standing across the way.
 */
#define ZOMBIE_BARRIER_CLEAR 80.0f // beyond where a player rebuilds it

static bool zombiesBarrierFits(struct coord *pad, RoomNum *padrooms, struct coord *wall, f32 walldist)
{
	static const f32 outs[2] = { 40.0f, ZOMBIE_BARRIER_INSIDE };
	const f32 need = ZOMBIE_BARRIER_FRONT + ZOMBIE_BARRIER_CLEAR;
	const f32 side = ZOMBIE_BARRIER_OPEN + 28;
	f32 back = zombiesRayToWall(pad, padrooms, -wall->x, -wall->z, need);
	s32 k;

	// away from the wall: the pad is walldist out, and the room on past it
	if (back >= 0 && walldist + back < need) {
		return false;
	}

	for (k = 0; k < 2; k++) {
		struct coord at;
		RoomNum atrooms[8];
		f32 left;
		f32 right;

		at.x = pad->x + wall->x * (walldist - outs[k]);
		at.y = pad->y;
		at.z = pad->z + wall->z * (walldist - outs[k]);
		func0f065e74(pad, padrooms, &at, atrooms);

		if (atrooms[0] <= 0) {
			return false;
		}

		left = zombiesRayToWall(&at, atrooms, wall->z, -wall->x, side);
		right = zombiesRayToWall(&at, atrooms, -wall->z, wall->x, side);

		if ((left >= 0 && left < side) || (right >= 0 && right < side)) {
			return false;
		}
	}

	return true;
}

static void zombiesPlaceBarriers(void)
{
	s32 n = g_NumSpawnPoints < MAX_SPAWNPOINTS ? g_NumSpawnPoints : MAX_SPAWNPOINTS;
	struct coord taken[ZOMBIE_MACHINE_IDS + ZOMBIE_WALLBUY_MAX + ZOMBIE_DOOR_MAX + ZOMBIE_BARRIER_MAX + 4];
	struct coord walldir[MAX_SPAWNPOINTS];
	f32 walldist[MAX_SPAWNPOINTS];
	bool usable[MAX_SPAWNPOINTS];
	s32 ntaken = 0;
	s32 pass;
	s32 i;
	s32 k;

	g_ZombieNumBarriers = 0;

	// an arena's own layout: its windows, each out from its pad
	if (zombiesLayout() && zombiesLayout()->numbarriers > 0) {
		const struct zombielayout *l = zombiesLayout();

		for (i = 0; i < l->numbarriers && g_ZombieNumBarriers < ZOMBIE_BARRIER_MAX; i++) {
			struct zombiebarrier *bar = &g_ZombieBarriers[g_ZombieNumBarriers];
			struct pad pad;
			RoomNum padrooms[2];
			f32 out;

			padUnpack(l->barpad[i], PADFIELD_POS | PADFIELD_ROOM, &pad);
			padrooms[0] = pad.room;
			padrooms[1] = -1;
			bzero(bar, sizeof(*bar));

			for (k = 0; k < ZOMBIE_BARRIER_BOARDS; k++) {
				bar->anim60[k] = 1000;
			}

			bar->wall.x = l->barwall[i][0];
			bar->wall.y = 0;
			bar->wall.z = l->barwall[i][1];
			bar->comp = -1;
			bar->boards = ZOMBIE_BARRIER_BOARDS;
			bar->zombie = -1;
			bar->pulling = -1;
			bar->enter60 = -1;
			bar->reach60 = -1;
			bar->pos.x = pad.pos.x + bar->wall.x * (l->bardist[i] - 2);
			bar->pos.y = pad.pos.y;
			bar->pos.z = pad.pos.z + bar->wall.z * (l->bardist[i] - 2);
			func0f065e74(&pad.pos, padrooms, &bar->pos, bar->rooms);
			bar->pos.y = pad.pos.y - 1;
			out = l->bardist[i] - ZOMBIE_BARRIER_STAND;
			out = out < 0 ? 0 : out;
			bar->stand.x = pad.pos.x + bar->wall.x * out;
			bar->stand.y = pad.pos.y;
			bar->stand.z = pad.pos.z + bar->wall.z * out;
			func0f065e74(&pad.pos, padrooms, &bar->stand, bar->standrooms);

			if (bar->rooms[0] <= 0 || bar->standrooms[0] <= 0 || !zombiesBarrierMakeObj(bar)) {
				continue;
			}

			bar->placed = true;
			g_ZombieNumBarriers++;
		}

		sysLogPrintf(LOG_NOTE, "zombies: %d of the arena's %d windows boarded", g_ZombieNumBarriers, l->numbarriers);
		return;
	}

	for (k = 0; k < ZOMBIE_MACHINE_IDS; k++) {
		struct defaultobj *obj = zombiesMachineObjAt(k);

		if (obj && obj->prop) {
			taken[ntaken++] = obj->prop->pos;
		}
	}

	for (k = 0; k < g_ZombieNumWallBuys; k++) {
		if (g_ZombieWallBuys[k].placed) {
			taken[ntaken++] = g_ZombieWallBuys[k].pos;
		}
	}

	for (k = 0; k < g_ZombieNumDoors; k++) {
		taken[ntaken++] = g_ZombieDoors[k].pos;
	}

	for (i = 0; i < n; i++) {
		struct pad pad;
		RoomNum rooms[2];

		padUnpack(g_SpawnPoints[i], PADFIELD_POS | PADFIELD_ROOM, &pad);
		rooms[0] = pad.room;
		rooms[1] = -1;

		usable[i] = pad.room > 0 && zombiesFindWall(&pad.pos, rooms, ZOMBIE_BARRIER_REACH, &walldist[i], &walldir[i]);
	}

	// first one a zone (pass 0), then any (pass 1)
	for (pass = 0; pass < 2; pass++) {
		while (g_ZombieNumBarriers < ZOMBIE_BARRIER_MAX) {
			struct zombiebarrier *bar = &g_ZombieBarriers[g_ZombieNumBarriers];
			f32 bestscore = -1;
			s32 best = -1;
			struct pad pad;
			RoomNum padrooms[2];
			f32 out;

			for (i = 0; i < n; i++) {
				f32 score = 1e20f;
				s32 comp;
				s32 t;

				if (!usable[i]) {
					continue;
				}

				padUnpack(g_SpawnPoints[i], PADFIELD_POS, &pad);
				comp = zombiesPadComp(g_SpawnPoints[i]);

				if (pass == 0) {
					for (k = 0; k < g_ZombieNumBarriers; k++) {
						if (g_ZombieBarriers[k].comp == comp) {
							break;
						}
					}

					if (k < g_ZombieNumBarriers) {
						continue;
					}
				}

				for (t = 0; t < ntaken; t++) {
					f32 dx = pad.pos.x - taken[t].x;
					f32 dy = pad.pos.y - taken[t].y;
					f32 dz = pad.pos.z - taken[t].z;
					f32 d = dx * dx + dy * dy * 4 + dz * dz;

					if (d < score) {
						score = d;
					}
				}

				if (score < ZOMBIE_BARRIER_APART * ZOMBIE_BARRIER_APART) {
					continue;
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
			padrooms[0] = pad.room;
			padrooms[1] = -1;

			bzero(bar, sizeof(*bar));

			for (k = 0; k < ZOMBIE_BARRIER_BOARDS; k++) {
				bar->anim60[k] = 1000;
			}

			bar->wall = walldir[best];

			// Room for it: Call of Duty's window at its own size stands
			// ZOMBIE_BARRIER_INSIDE out from the wall, and a player rebuilds
			// it from in front. A corridor too narrow for that, or a wall
			// too short for its width, is passed over for the next pad
			if (!zombiesBarrierFits(&pad.pos, padrooms, &bar->wall, walldist[best])) {
				continue;
			}

			bar->comp = zombiesPadComp(g_SpawnPoints[best]);
			bar->boards = ZOMBIE_BARRIER_BOARDS;
			bar->zombie = -1;
			bar->pulling = -1;
			bar->enter60 = -1;
			bar->reach60 = -1;

			// the boards against the wall, on the floor under it
			bar->pos.x = pad.pos.x + bar->wall.x * (walldist[best] - 2);
			bar->pos.y = pad.pos.y;
			bar->pos.z = pad.pos.z + bar->wall.z * (walldist[best] - 2);
			func0f065e74(&pad.pos, padrooms, &bar->pos, bar->rooms);
			bar->pos.y = zombiesGroundAt(&pad.pos, padrooms);

			if (bar->pos.y < pad.pos.y - 400 || bar->pos.y > pad.pos.y) {
				bar->pos.y = pad.pos.y - 100;
			}

			// where a zombie at it stands
			out = walldist[best] - ZOMBIE_BARRIER_STAND;
			out = out < 0 ? 0 : out;
			bar->stand.x = pad.pos.x + bar->wall.x * out;
			bar->stand.y = pad.pos.y;
			bar->stand.z = pad.pos.z + bar->wall.z * out;
			func0f065e74(&pad.pos, padrooms, &bar->stand, bar->standrooms);

			if (bar->rooms[0] <= 0 || bar->standrooms[0] <= 0 || !zombiesBarrierMakeObj(bar)) {
				continue;
			}

			bar->placed = true;
			taken[ntaken++] = pad.pos;
			sysLogPrintf(LOG_NOTE, "zombies: barrier %d on stage 0x%02x at %.0f %.0f %.0f (zone %d)",
					g_ZombieNumBarriers, g_Vars.stagenum, bar->pos.x, bar->pos.y, bar->pos.z, bar->comp);
			g_ZombieNumBarriers++;
		}
	}
}

// Its boards to count, the one that went or came set going
static void zombiesBarrierSet(s32 b, s32 boards)
{
	struct zombiebarrier *bar = &g_ZombieBarriers[b];
	s32 k;

	boards = boards < 0 ? 0 : boards > ZOMBIE_BARRIER_BOARDS ? ZOMBIE_BARRIER_BOARDS : boards;

	if (boards == bar->boards) {
		return;
	}

	if (boards < bar->boards) {
		for (k = boards; k < bar->boards; k++) {
			bar->anim60[k] = 0;
			bar->torn[k] = true;
		}

		if (bar->obj.prop) {
			psCreate(NULL, bar->obj.prop, SFX_WOOD_BREAK, -1, -1, 0, 0, PSTYPE_NONE, NULL, 0.9f + (bar->boards & 1) * 0.15f,
					NULL, -1, -1, -1, -1);
		}
	} else {
		for (k = bar->boards; k < boards; k++) {
			bar->anim60[k] = 0;
			bar->torn[k] = false;
		}

		if (bar->obj.prop) {
			psCreate(NULL, bar->obj.prop, SFX_HIT_WOOD_807E, -1, -1, 0, 0, PSTYPE_NONE, NULL, 1.0f,
					NULL, -1, -1, -1, -1);
		}
	}

	bar->boards = boards;
}

static void zombiesBarrierAnimate(void)
{
	s32 b;
	s32 k;

	for (b = 0; b < g_ZombieNumBarriers; b++) {
		for (k = 0; k < ZOMBIE_BARRIER_BOARDS; k++) {
			if (g_ZombieBarriers[b].anim60[k] < 1000) {
				g_ZombieBarriers[b].anim60[k] += g_Vars.lvupdate60;
			}
		}
	}
}

/**
 * Where the zombie being sent in comes in: a barrier in a zone open so far
 * with no zombie at it, near a player, three in four of them; -1 a pad
 */
static s32 zombiesBarrierPick(void)
{
	s32 cands[ZOMBIE_BARRIER_MAX];
	s32 near[ZOMBIE_BARRIER_MAX];
	s32 numcands = 0;
	s32 numnear = 0;
	s32 b;
	s32 p;

	if (g_ZombieNumBarriers == 0 || rngRandom() % 4 == 0) {
		return -1;
	}

	for (b = 0; b < g_ZombieNumBarriers; b++) {
		struct zombiebarrier *bar = &g_ZombieBarriers[b];
		bool isnear = false;

		if (!bar->placed || bar->zombie >= 0 || !zombiesCompOpen(bar->comp)) {
			continue;
		}

		cands[numcands++] = b;

		for (p = 0; p < PLAYERCOUNT(); p++) {
			if (zombiesPlayerIsUp(p)) {
				struct coord *pos = &g_Vars.players[p]->prop->pos;
				f32 dx = pos->x - bar->stand.x;
				f32 dz = pos->z - bar->stand.z;

				if (dx * dx + dz * dz < ZOMBIE_BARRIER_NEAR * ZOMBIE_BARRIER_NEAR) {
					isnear = true;
				}
			}
		}

		if (isnear) {
			near[numnear++] = b;
		}
	}

	if (numnear > 0) {
		return near[rngRandom() % numnear];
	}

	return numcands > 0 ? cands[rngRandom() % numcands] : -1;
}

static void zombiesBarrierTake(s32 b, s32 botnum)
{
	struct zombiebarrier *bar = &g_ZombieBarriers[b];

	if (bar->boards <= 0) {
		return;
	}

	bar->zombie = botnum;
	bar->tear60 = 0;
	g_ZombieAtBarrier[botnum] = b;
}

// zombiesTick(): a zombie at a barrier's boards stays there, its target known
static bool zombiesBarrierHolds(s32 botnum, struct chrdata *zchr, struct prop *target)
{
	s32 b = g_ZombieAtBarrier[botnum];
	struct zombiebarrier *bar;

	if (b < 0 || b >= g_ZombieNumBarriers) {
		g_ZombieAtBarrier[botnum] = -1;
		return false;
	}

	bar = &g_ZombieBarriers[b];

	// held while it pulls the last board off and climbs in, too
	if (bar->zombie != botnum || (bar->boards <= 0 && bar->pulling < 0 && bar->enter60 < 0)) {
		if (bar->zombie == botnum) {
			bar->zombie = -1;
		}

		g_ZombieAtBarrier[botnum] = -1;
		return false;
	}

	if (zchr->aibot->command != AIBOTCMD_HOLD) {
		botApplyHold(zchr, &bar->stand, bar->standrooms, atan2f(-bar->wall.x, -bar->wall.z));
	}

	if (target) {
		botSetTarget(zchr, target - g_Vars.props);
	}

	return true;
}

/**
 * How long, in 60ths, a zombie takes over board k's grab, hold or pull
 * (ZMCHAR_TEAR_*: Call of Duty's own clips, 2.5 to 3 seconds a board in
 * all), or over its climb in when k is -1. Without Black Ops II's zombie, the
 * old ZOMBIE_BARRIER_TEAR60 a board and no climb.
 */
#define ZOMBIE_BARRIER_CLIMB (57.8f * ZOMBIE_COD_INCH) // how far the climb carries it from the origin

// Whether a barrier's zombie is a crawler: its tear and climb are a crawler's
static bool zombiesBarrierCrawler(const struct zombiebarrier *bar)
{
	return bar->zombie >= 0 && bar->zombie < MAX_BOTS && g_ZombieCrawler[bar->zombie];
}

static s32 zombiesBarrierAnim(const struct zombiebarrier *bar, s32 k, s32 part)
{
	const bool crawler = zombiesBarrierCrawler(bar);

	return k < 0 ? (crawler ? ZMCHAR_CRAWL_ENTER : ZMCHAR_ENTER) : (crawler ? ZMCHAR_CRAWL_TEAR : ZMCHAR_TEAR) + k * 3 + part;
}

static s32 zombiesBarrierPhase60(const struct zombiebarrier *bar, s32 k, s32 part)
{
	const s32 anim = zombiesBarrierAnim(bar, k, part);
	const f32 frames = zmcharAnimFrames(anim);

	if (frames <= 0) {
		return k < 0 ? 0 : ZOMBIE_BARRIER_TEAR60 / 3;
	}

	return (s32)(frames * 60 / zmcharAnimRate(anim));
}

/**
 * A swipe through a barrier's boards (ZMCHAR_WINDOW_L/R): a player is in its
 * reach standing up close to the boards on the room's side, across the
 * opening; the blow lands ZOMBIE_REACH_HITAT through the clip, a zombie's
 * blow's half a player's health (chrPunchInflictDamage()'s Zombies 8.5)
 */
#define ZOMBIE_REACH_HITAT 0.45f
#define ZOMBIE_REACH_DEPTH 75.0f   // how far out from the boards' face a player is in reach
#define ZOMBIE_REACH_BLOW  0.51f   // of a player's health
#define ZOMBIE_REACH_REST60 120    // between one swipe through the boards and the next, tearing at them meanwhile

static s32 g_ZombieReachRest60[ZOMBIE_BARRIER_MAX];

static bool zombiesBarrierReaches(const struct zombiebarrier *bar, s32 playernum)
{
	struct prop *pp;
	f32 rx;
	f32 rz;
	f32 out;
	f32 along;

	// --zombies-test-reach 1: player 1 counts as at every barrier's boards
	{
		static s32 testreach = -1;

		if (testreach < 0) {
			testreach = sysArgGetInt("--zombies-test-reach", 0);
		}

		if (testreach && playernum == 0) {
			return true;
		}
	}

	if (playernum < 0 || playernum >= PLAYERCOUNT() || !zombiesPlayerIsUp(playernum)) {
		return false;
	}

	pp = g_Vars.players[playernum]->prop;
	rx = pp->pos.x - bar->pos.x;
	rz = pp->pos.z - bar->pos.z;
	out = -(rx * bar->wall.x + rz * bar->wall.z);
	along = rx * bar->wall.z - rz * bar->wall.x;

	return out > ZOMBIE_BARRIER_INSIDE && out < ZOMBIE_BARRIER_INSIDE + ZOMBIE_REACH_DEPTH
		&& along > -ZOMBIE_BARRIER_OPEN - 30 && along < ZOMBIE_BARRIER_OPEN + 30
		&& pp->pos.y - bar->pos.y < 250 && pp->pos.y - bar->pos.y > -100;
}

static void zombiesReachHit(struct chrdata *zchr, s32 playernum)
{
	struct player *player = g_Vars.players[playernum];
	struct gset gset = { WEAPON_UNARMED, 0, 0, FUNC_PRIMARY };
	struct coord dir;

	if (!player || !player->prop || !player->prop->chr) {
		return;
	}

	dir.x = player->prop->pos.x - zchr->prop->pos.x;
	dir.y = 0;
	dir.z = player->prop->pos.z - zchr->prop->pos.z;
	guNormalize(&dir.x, &dir.y, &dir.z);

	// chrDamage() takes a player's at an eighth, over its health scale, which
	// is Juggernog's (2.5): a blow is half of a player's health without it
	// and a fifth with it, two hits down or five, as Black Ops II's. It was
	// sent times the health scale, which chrDamage() took back off, and
	// Juggernog did nothing against a blow. And on its way chrDamage() turned
	// a fist's hit by its side (0.4 from the front, 0.7 the side), the torso
	// (twice), the player's handicap and the simulant's damage slider: a
	// blow came out at up to twice itself, the whole of a player's health,
	// one hit down. Call of Duty's zombie hits the same however it lands, so
	// chrDamage() is handed the blow as it is to take (zombiesBlowDamage()).
	g_ZombieReachThrough = true;
	g_ZombieBlowDamage = ZOMBIE_REACH_BLOW * 8.0f;
	chrDamageByImpact(player->prop->chr, ZOMBIE_REACH_BLOW * 8.0f, &dir, &gset,
			zchr->prop, HITPART_TORSO);
	g_ZombieBlowDamage = -1;

	// Black Ops II's swipe taken, for the one hit alone; the blow that downs
	// them has its own (zombiesTickLastStandSound())
	if (!g_ZombieDowned[playernum]) {
		const s32 prevfor = g_ZombieSoundFor;

		g_ZombieSoundFor = playernum;
		zombiesPlayCodSound(ZMSFX_SWIPED + rngRandom() % 3, NULL);
		g_ZombieSoundFor = prevfor;
	}
	g_ZombieReachThrough = false;
}

/**
 * Lava, as Tranzit's burns: a player standing on it (a floor codmap.c flags
 * GEOFLAG_0400, under rock_lava_molten) is burnt for ZOMBIE_LAVA_BURN of their
 * health every ZOMBIE_LAVA_EVERY60, from the moment they step on, a zombie's
 * blow's way (so Juggernog holds out longer, and going down is going down).
 * The screen's own hurt flash shows it. Zombies walk it unburnt for now:
 * Town's streets cross its cracks.
 */
#define ZOMBIE_LAVA_BURN    0.2f
#define ZOMBIE_LAVA_EVERY60 30
#define ZOMBIE_GEOFLAG_LAVA 0x0400

static s32 g_ZombieLava60[MAX_PLAYERS]; // till the next burn, 0 off the lava

static void zombiesTickLava(s32 playernum)
{
	struct player *player = g_Vars.players[playernum];
	struct gset gset = { WEAPON_UNARMED, 0, 0, FUNC_PRIMARY };
	struct coord dir = { 0, 1, 0 };

	if (!player || !player->prop || !player->prop->chr || !zombiesPlayerIsUp(playernum)
			|| !(player->floorflags & ZOMBIE_GEOFLAG_LAVA)
			|| player->isfalling) {
		g_ZombieLava60[playernum] = 0;
		return;
	}

	g_ZombieLava60[playernum] -= g_Vars.lvupdate60;

	if (g_ZombieLava60[playernum] > 0) {
		return;
	}

	g_ZombieLava60[playernum] = ZOMBIE_LAVA_EVERY60;
	g_ZombieReachThrough = true;
	g_ZombieBlowDamage = ZOMBIE_LAVA_BURN * 8.0f;
	chrDamageByImpact(player->prop->chr, ZOMBIE_LAVA_BURN * 8.0f, &dir, &gset, NULL, HITPART_GENERAL);
	g_ZombieBlowDamage = -1;
	g_ZombieReachThrough = false;
}

static void zombiesTickBarriers(void)
{
	s32 i;
	s32 b;

	zombiesBarrierAnimate();

	if (g_ZombieRebuiltRound != g_ZombieRound) {
		g_ZombieRebuiltRound = g_ZombieRound;

		for (i = 0; i < MAX_PLAYERS; i++) {
			g_ZombieRebuilt[i] = 0;
		}
	}

	// the zombies at them tearing the boards off
	for (b = 0; b < g_ZombieNumBarriers; b++) {
		struct zombiebarrier *bar = &g_ZombieBarriers[b];
		struct chrdata *zchr;
		f32 dx;
		f32 dz;

		if (bar->zombie < 0) {
			continue;
		}

		zchr = bar->zombie < g_BotCount ? g_MpBotChrPtrs[bar->zombie] : NULL;

		if (zchr == NULL || zchr->prop == NULL || chrIsDead(zchr) || g_ZombieAtBarrier[bar->zombie] != b) {
			if (bar->zombie < MAX_BOTS && g_ZombieAtBarrier[bar->zombie] == b) {
				g_ZombieAtBarrier[bar->zombie] = -1;
			}

			bar->zombie = -1;
			continue;
		}

		// once it is at the boards
		dx = zchr->prop->pos.x - bar->stand.x;
		dz = zchr->prop->pos.z - bar->stand.z;

		if (dx * dx + dz * dz > 80 * 80) {
			continue;
		}

		// over the sill: where Call of Duty's climb ends, then after the players
		if (bar->enter60 >= 0) {
			bar->enter60 += g_Vars.lvupdate60;

			if (bar->enter60 >= zombiesBarrierPhase60(bar, -1, 0)) {
				struct coord to;
				RoomNum torooms[8];
				const s32 anim = zombiesBarrierAnim(bar, -1, 0);
				f32 out = ZOMBIE_BARRIER_ORIGIN + ZOMBIE_BARRIER_CLIMB;
				f32 root[3];
				f32 q[4];

				// where the climb's own root motion ends, a crawler's or not
				if (zmcharAnimRoot(anim, zmcharAnimFrames(anim) - 1, root, q)) {
					out = ZOMBIE_BARRIER_ORIGIN + root[0] * ZOMBIE_COD_INCH;
				}

				to.x = bar->pos.x - bar->wall.x * out;
				to.y = zchr->prop->pos.y;
				to.z = bar->pos.z - bar->wall.z * out;
				func0f065e74(&zchr->prop->pos, zchr->prop->rooms, &to, torooms);
				chrMoveToPos(zchr, &to, torooms, atan2f(-bar->wall.x, -bar->wall.z), false);

				bar->enter60 = -1;
				g_ZombieAtBarrier[bar->zombie] = -1;
				bar->zombie = -1;
			}

			continue;
		}

		// a player at the boards: a swipe through them (Call of Duty's
		// reachThroughAttacks), the boards left alone meanwhile
		if (bar->reach60 >= 0) {
			const s32 anim = bar->reachleft ? ZMCHAR_WINDOW_L : ZMCHAR_WINDOW_R;
			const s32 len = zmcharAnimFrames(anim) > 0 ? (s32)(zmcharAnimFrames(anim) * 60 / zmcharAnimRate(anim)) : 90;

			bar->reach60 += g_Vars.lvupdate60;

			if (!bar->reachhit && bar->reach60 >= len * ZOMBIE_REACH_HITAT) {
				bar->reachhit = true;

				if (zombiesBarrierReaches(bar, bar->reachplayer)) {
					zombiesReachHit(zchr, bar->reachplayer);
				}
			}

			if (bar->reach60 >= len) {
				bar->reach60 = -1;
				g_ZombieReachRest60[b] = ZOMBIE_REACH_REST60;
			}

			continue;
		}

		if (g_ZombieReachRest60[b] > 0) {
			g_ZombieReachRest60[b] -= g_Vars.lvupdate60;
		}

		if (bar->pulling < 0 && bar->boards > 0 && g_ZombieReachRest60[b] <= 0) {
			for (i = 0; i < PLAYERCOUNT(); i++) {
				if (zombiesBarrierReaches(bar, i)) {
					bar->reach60 = 0;
					bar->reachplayer = i;
					bar->reachleft = !bar->reachleft;
					bar->reachhit = false;
					break;
				}
			}

			if (bar->reach60 >= 0) {
				continue;
			}
		}

		bar->tear60 += g_Vars.lvupdate60;

		// a board's grab and hold, then its pull: the board goes as it begins
		if (bar->pulling < 0 && bar->boards > 0) {
			const s32 k = bar->boards - 1;

			if (bar->tear60 >= zombiesBarrierPhase60(bar, k, ZMCHAR_TEAR_GRAB) + zombiesBarrierPhase60(bar, k, ZMCHAR_TEAR_HOLD)) {
				bar->pulling = k;
				bar->tear60 = 0;
				zombiesBarrierSet(b, k);
			}
		} else if (bar->pulling >= 0 && bar->tear60 >= zombiesBarrierPhase60(bar, bar->pulling, ZMCHAR_TEAR_PULL)) {
			bar->pulling = -1;
			bar->tear60 = 0;

			// through: in over the sill
			if (bar->boards <= 0) {
				bar->enter60 = 0;
			}
		}
	}

	// the players putting them back
	for (i = 0; i < PLAYERCOUNT(); i++) {
		struct player *player = g_Vars.players[i];
		f32 bestdist = ZOMBIE_BARRIER_RANGE * ZOMBIE_BARRIER_RANGE;

		g_ZombieNearBarrier[i] = -1;

		if (!zombiesPlayerIsUp(i) || g_ZombieDrink60[i] >= 0 || g_ZombieNearDowned[i] >= 0
				|| g_ZombieNearWall[i] >= 0 || g_ZombieNearBox[i] || g_ZombieNearJugg[i]
				|| g_ZombieNearPerk[i] >= 0 || g_ZombieNearPap[i] || g_ZombieNearDoor[i] >= 0) {
			g_ZombieFix60[i] = 0;
			continue;
		}

		for (b = 0; b < g_ZombieNumBarriers; b++) {
			struct zombiebarrier *bar = &g_ZombieBarriers[b];
			// before the boards, on the room's side
			f32 dx = player->prop->pos.x - (bar->pos.x - bar->wall.x * ZOMBIE_BARRIER_FRONT);
			f32 dy = player->prop->pos.y - bar->stand.y;
			f32 dz = player->prop->pos.z - (bar->pos.z - bar->wall.z * ZOMBIE_BARRIER_FRONT);

			if (bar->placed && bar->boards < ZOMBIE_BARRIER_BOARDS
					&& dx * dx + dz * dz < bestdist && dy < 200 && dy > -200) {
				bestdist = dx * dx + dz * dz;
				g_ZombieNearBarrier[i] = b;
			}
		}

		if (g_ZombieNearBarrier[i] < 0 || !zombiesHoldingUse(i)) {
			g_ZombieFix60[i] = 0;
			continue;
		}

		// a board at a time while it is held; with Speed Cola twice as fast,
		// as Call of Duty's rebuilds
		g_ZombieFix60[i] += g_Vars.lvupdate60 * (zombiesPlayerHasPerk(i, ZOMBIEPERK_SPEEDCOLA) ? 2 : 1);

		if (g_ZombieFix60[i] >= ZOMBIE_BARRIER_FIX60) {
			struct zombiebarrier *bar = &g_ZombieBarriers[g_ZombieNearBarrier[i]];

			g_ZombieFix60[i] = 0;
			zombiesBarrierSet(g_ZombieNearBarrier[i], bar->boards + 1);

			if (g_ZombieRebuilt[i] < ZOMBIE_BARRIER_CAP) {
				g_ZombieRebuilt[i] += ZOMBIE_BARRIER_POINTS;
				g_ZombiePoints[i] += ZOMBIE_BARRIER_POINTS;
				g_ZombieTeamEarned += ZOMBIE_BARRIER_POINTS;
				zombiesAddPopup(i, ZOMBIE_BARRIER_POINTS);
			}

			// a zombie at it starts on the new board afresh
			bar->tear60 = 0;
		}
	}
}

static bool zombiesBarrierPrompt(s32 playernum, char *text)
{
	if (playernum < 0 || playernum >= MAX_PLAYERS || g_ZombieNearBarrier[playernum] < 0
			|| g_ZombieNearBarrier[playernum] >= g_ZombieNumBarriers) {
		return false;
	}

	snprintf(text, 64, "Hold %s to rebuild Barrier", zombiesUseName(playernum));

	return true;
}

/**
 * The boards as plain shaded boxes (no texture), in the room's light, in
 * the opaque pass: a dark window behind, a post either side, and the boards
 * across it at slants of their own. A torn board falls out into the room; a
 * board put back comes in from it.
 */
struct zombieboard {
	s16 y;     // its middle, over the floor
	s16 len;
	s8 tilt;   // in degrees
	s8 out;    // off the wall
};

static const struct zombieboard g_ZombieBoards[ZOMBIE_BARRIER_BOARDS] = {
	{ 58, 118, 7, 4 }, { 80, 112, -9, 7 }, { 102, 122, 4, 4 },
	{ 124, 114, -6, 7 }, { 146, 120, 10, 4 }, { 166, 110, -4, 7 },
};

// One face, its corners already about the barrier's foot
static Gfx *zombiesBoardFace(Gfx *gdl, const f32 c[4][3], u8 r, u8 g, u8 b)
{
	Vtx *vtx = gfxAllocate(sizeof(Vtx) * 4);
	Col *col = gfxAllocate(sizeof(Col) * 4);
	s32 i;

	if (!vtx || !col) {
		return gdl;
	}

	for (i = 0; i < 4; i++) {
		vtx[i].x = (s16)c[i][0];
		vtx[i].y = (s16)c[i][1];
		vtx[i].z = (s16)c[i][2];
		vtx[i].flags = 0;
		vtx[i].colour = (u8)(i * 4);
		vtx[i].s = 0;
		vtx[i].t = 0;
		col[i].r = r;
		col[i].g = g;
		col[i].b = b;
		col[i].a = 0xff;
	}

	gSPColor(gdl++, col, 4);
	gSPVertex(gdl++, vtx, 4, 0);
	gSPTri4(gdl++, 0, 1, 2, 0, 2, 3, 0, 0, 0, 0, 0, 0);

	return gdl;
}

/**
 * A box: middle (x along the wall, y up, z out of it), half sizes, turned by
 * tilt about the way out; its five faces but the back, shaded each its own
 */
static Gfx *zombiesBoardBox(Gfx *gdl, const struct zombiebarrier *bar, f32 x, f32 y, f32 z,
		f32 hl, f32 hh, f32 ht, f32 tilt, const u8 rgb[3], f32 light)
{
	static const s8 faces[5][4][3] = {
		{ { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } },     // front
		{ { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 } },     // top
		{ { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } }, // bottom
		{ { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 } }, // left
		{ { 1, -1, 1 }, { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 } },     // right
	};
	static const f32 shade[5] = { 1.0f, 1.15f, 0.5f, 0.72f, 0.72f };
	const f32 ax = bar->wall.z;  // along the wall
	const f32 az = -bar->wall.x;
	const f32 ox = -bar->wall.x; // out of it
	const f32 oz = -bar->wall.z;
	const f32 cs = cosf(tilt);
	const f32 sn = sinf(tilt);
	s32 f;
	s32 i;

	for (f = 0; f < 5; f++) {
		f32 c[4][3];
		f32 s = shade[f] * light;
		s32 r = (s32)(rgb[0] * s);
		s32 g = (s32)(rgb[1] * s);
		s32 b = (s32)(rgb[2] * s);

		for (i = 0; i < 4; i++) {
			f32 lx = faces[f][i][0] * hl;
			f32 ly = faces[f][i][1] * hh;
			f32 px = x + lx * cs - ly * sn;
			f32 py = y + lx * sn + ly * cs;
			f32 pz = z + faces[f][i][2] * ht;

			c[i][0] = ax * px + ox * pz;
			c[i][1] = py;
			c[i][2] = az * px + oz * pz;
		}

		gdl = zombiesBoardFace(gdl, c, r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
	}

	return gdl;
}

/**
 * Black Ops II's own boards (p6_anim_zm_barricade_board_01 and 02, Origins'
 * zone, tools/zombies/glb2obj.py into zombies/models/board01 and board02): a
 * plank each, long along x, up y and thick z, every board one of the two in
 * turn. Call of Duty places its six by animation; here each takes the place
 * and tilt of the board it stands for, fitted to its length, 14 high and 4
 * thick. Without them the boards are boxes (zombiesBoardBox()).
 */
static struct zombiemesh g_ZombieBoardMeshes[2];

static struct zombiemesh *zombiesBoardMesh(s32 k)
{
	struct zombiemesh *zm = &g_ZombieBoardMeshes[k & 1];

	if (!zm->tried) {
		zombiesMeshBuildFrom(zm, k & 1 ? "$E/zombies/models/board02/board02.obj" : "$E/zombies/models/board01/board01.obj",
				k & 1 ? "a barrier's board (02)" : "a barrier's board (01)");
	}

	return zm->gdl && zm->width > 0 && zm->height > 0 && zm->depth > 0 ? zm : NULL;
}

/**
 * Call of Duty's own layout: where zmcore_basicwoodbarrier's board N comes
 * to rest, the last frame of o_zombie_board_N_repair (Origins' zone): its
 * place the animation's root motion, in inches (x into the room from the
 * barrier's origin, y across, z up), and its turn tag_animate's, as the
 * board's length, width and thickness in the barrier's axes. Four boards
 * across and two upright; the uprights are the longer plank (_02, _03).
 */
struct zombiecodboard {
	f32 pos[3];
	f32 len[3];
	f32 wid[3];
	f32 thk[3];
	u8 mesh;
};

static const struct zombiecodboard g_ZombieCodBoards[ZOMBIE_BARRIER_BOARDS] = {
	{ { 27.9f,   0.0f, 50.4f }, { 0, 0.95f, -0.31f }, { 0, 0.31f, 0.95f },  { -1, 0, 0 }, 0 },
	{ { 29.8f,   9.1f, 30.4f }, { 0, -0.13f, 0.99f }, { 0, -0.99f, -0.13f }, { -1, 0, 0 }, 1 },
	{ { 29.9f, -17.3f, 18.1f }, { 0, 0.06f, 1.00f },  { 0, -1.00f, 0.06f },  { -1, 0, 0 }, 1 },
	{ { 33.8f,   1.4f, 49.5f }, { 0, 1.00f, 0.04f },  { 0, -0.04f, 1.00f },  { -1, 0, 0 }, 0 },
	{ { 31.7f,   0.0f, 37.8f }, { 0, 1.00f, -0.05f }, { 0, 0.05f, 1.00f },  { -1, 0, 0 }, 0 },
	{ { 33.8f,   0.0f, 10.2f }, { 0, 0.98f, 0.19f },  { 0, -0.19f, 0.98f },  { -1, 0, 0 }, 0 },
};

/**
 * A direction (point false) or a place (point true) in Call of Duty's barrier
 * space - inches from its origin, the sill: x into the room, y the left of a
 * zombie looking in, z up - into the world
 */
static void zombiesBarrierToWorld(const struct zombiebarrier *bar, const f32 v[3], bool point, f32 out[3])
{
	// into the room is away from the wall; a zombie looking in has its left
	// along (-wall.z, wall.x)
	out[0] = (-bar->wall.x * v[0] - bar->wall.z * v[1]) * ZOMBIE_COD_INCH;
	out[1] = v[2] * ZOMBIE_COD_INCH;
	out[2] = (-bar->wall.z * v[0] + bar->wall.x * v[1]) * ZOMBIE_COD_INCH;

	if (point) {
		out[0] += bar->pos.x - bar->wall.x * ZOMBIE_BARRIER_ORIGIN;
		out[1] += bar->pos.y + ZOMBIE_BARRIER_SILL;
		out[2] += bar->pos.z - bar->wall.z * ZOMBIE_BARRIER_ORIGIN;
	}
}

// A quaternion's turn of an axis (x y z w, Call of Duty's)
static void zombiesQuatAxis(const f32 q[4], const f32 v[3], f32 out[3])
{
	const f32 tx = 2 * (q[1] * v[2] - q[2] * v[1]);
	const f32 ty = 2 * (q[2] * v[0] - q[0] * v[2]);
	const f32 tz = 2 * (q[0] * v[1] - q[1] * v[0]);

	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

/**
 * One of Black Ops II's boards at its own size, where Call of Duty has it:
 * its place (inches) and its length, width and thickness in the barrier's
 * space. The plank's mesh is long along x, up y and thick z, in centimetres.
 */
static Gfx *zombiesBoardMeshDraw(Gfx *gdl, const struct zombiebarrier *bar, s32 k,
		const f32 pos[3], const f32 len[3], const f32 wid[3], const f32 thk[3], f32 light)
{
	struct zombiemesh *zm = zombiesBoardMesh(g_ZombieCodBoards[k].mesh);
	const f32 s = 1.0f / ZOMBIE_COD_INCH / ZOMBIE_MESH_FIXED;
	const f32 *axes[3] = { len, wid, thk };
	f32 w[3];
	Mtxf local;
	Mtxf *mtx = gfxAllocateMatrix();
	s32 shade = (s32)(0xff * light) - (k % 3) * 10;
	s32 i;

	mtx4LoadIdentity(&local);

	for (i = 0; i < 3; i++) {
		zombiesBarrierToWorld(bar, axes[i], false, w);
		local.m[i][0] = w[0] * s;
		local.m[i][1] = w[1] * s;
		local.m[i][2] = w[2] * s;
	}

	zombiesBarrierToWorld(bar, pos, true, w);
	local.m[3][0] = w[0];
	local.m[3][1] = w[1];
	local.m[3][2] = w[2];

	if (zm == NULL) {
		return gdl;
	}

	mtx4MultMtx4(camGetWorldToScreenMtxf(), &local, mtx);

	shade = shade < 0x40 ? 0x40 : shade > 0xff ? 0xff : shade;

	return zombiesBottleDraw(gdl, zm, mtx, shade, shade, shade);
}

static bool zombiesRenderBarrier(struct modelrenderdata *renderdata, struct defaultobj *obj)
{
	static const u8 wood[3] = { 0x8c, 0x5e, 0x34 };
	static const u8 post[3] = { 0x5a, 0x3c, 0x22 };
	static const u8 hole[3] = { 0x0c, 0x0a, 0x08 };
	struct zombiebarrier *bar = NULL;
	Gfx *gdl;
	Mtxf world;
	Mtxf *mtx;
	f32 light;
	s32 b;
	s32 k;

	for (b = 0; b < g_ZombieNumBarriers; b++) {
		if (obj == &g_ZombieBarriers[b].obj) {
			bar = &g_ZombieBarriers[b];
			break;
		}
	}

	if (bar == NULL) {
		return false;
	}

	// nothing in the translucent pass
	if (!(renderdata->flags & MODELRENDERFLAG_OPA)) {
		return true;
	}

	// the room's light, half of it, as the machines take it
	{
		u32 room = (renderdata->fogcolour & 0xffffff00) ? renderdata->fogcolour : 0xffffffff;
		f32 lum = (((room >> 24) & 0xff) + ((room >> 16) & 0xff) + ((room >> 8) & 0xff)) / (3 * 255.0f);

		light = 0.55f + lum * 0.45f;
	}

	gdl = renderdata->gdl;
	mtx = gfxAllocateMatrix();
	mtx4LoadIdentity(&world);
	world.m[3][0] = bar->pos.x;
	world.m[3][1] = bar->pos.y;
	world.m[3][2] = bar->pos.z;
	mtx4MultMtx4(camGetWorldToScreenMtxf(), &world, mtx);

	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetRenderMode(gdl++, renderdata->zbufferenabled ? G_RM_AA_ZB_OPA_SURF : G_RM_AA_OPA_SURF,
			renderdata->zbufferenabled ? G_RM_AA_ZB_OPA_SURF2 : G_RM_AA_OPA_SURF2);
	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
	gSPClearGeometryMode(gdl++, G_LIGHTING | G_TEXTURE_GEN | G_FOG | G_CULL_BOTH);
	gSPSetGeometryMode(gdl++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
	gSPMatrix(gdl++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW | G_MTX_FLOATS);

	// the window: dark, a hair off the wall, and its posts
	if (zombiesBoardMesh(0)) {
		// Call of Duty's window at its own size, standing out from the wall:
		// the pen behind it dark at the back and walled at the sides, then
		// the wall's thickness about the opening - the sill under it, a jamb
		// either side, the lintel over it - and the boards on its room side
		static const u8 wall[3] = { 0x5c, 0x56, 0x4e };
		const f32 hw = ZOMBIE_BARRIER_OPEN;
		const f32 jamb = 14;
		const f32 depth = (ZOMBIE_BARRIER_INSIDE - ZOMBIE_BARRIER_ORIGIN) / 2;
		const f32 thick = ZOMBIE_BARRIER_ORIGIN + depth;
		const f32 top = ZOMBIE_BARRIER_TOP + 18;

		gdl = zombiesBoardBox(gdl, bar, 0, top / 2, 0.8f, hw + jamb * 2, top / 2, 0.6f, 0, hole, 1.0f);
		gdl = zombiesBoardBox(gdl, bar, -hw - jamb, top / 2, ZOMBIE_BARRIER_ORIGIN / 2,
				jamb, top / 2, ZOMBIE_BARRIER_ORIGIN / 2, 0, wall, light * 0.8f);
		gdl = zombiesBoardBox(gdl, bar, hw + jamb, top / 2, ZOMBIE_BARRIER_ORIGIN / 2,
				jamb, top / 2, ZOMBIE_BARRIER_ORIGIN / 2, 0, wall, light * 0.8f);
		gdl = zombiesBoardBox(gdl, bar, 0, ZOMBIE_BARRIER_SILL / 2, thick,
				hw, ZOMBIE_BARRIER_SILL / 2, depth, 0, wall, light);
		gdl = zombiesBoardBox(gdl, bar, -hw - jamb, top / 2, thick, jamb, top / 2, depth, 0, post, light);
		gdl = zombiesBoardBox(gdl, bar, hw + jamb, top / 2, thick, jamb, top / 2, depth, 0, post, light);
		gdl = zombiesBoardBox(gdl, bar, 0, (ZOMBIE_BARRIER_TOP + top) / 2, thick,
				hw, (top - ZOMBIE_BARRIER_TOP) / 2, depth, 0, post, light);
	} else {
		gdl = zombiesBoardBox(gdl, bar, 0, 108, 0.8f, 52, 70, 0.6f, 0, hole, 1.0f);
		gdl = zombiesBoardBox(gdl, bar, -58, 108, 3, 5, 78, 3, 0, post, light);
		gdl = zombiesBoardBox(gdl, bar, 58, 108, 3, 5, 78, 3, 0, post, light);
	}

	for (k = 0; k < ZOMBIE_BARRIER_BOARDS; k++) {
		const struct zombieboard *board = &g_ZombieBoards[k];
		f32 y = board->y;
		f32 z = board->out + 2;
		f32 tilt = board->tilt * (M_BADPI / 180.0f);
		u8 rgb[3];
		s32 t = bar->anim60[k];

		if (zombiesBoardMesh(g_ZombieCodBoards[k].mesh)) {
			const struct zombiecodboard *cb = &g_ZombieCodBoards[k];
			static const f32 xaxis[3] = { 1, 0, 0 };
			static const f32 yaxis[3] = { 0, 1, 0 };
			static const f32 zaxis[3] = { 0, 0, 1 };
			const s32 anim = k >= bar->boards ? ZMCHAR_BOARD_PULL + k : ZMCHAR_BOARD_REPAIR + k;
			const f32 frame = bar->anim60[k] * zmcharAnimRate(anim) / 60.0f;
			f32 pos[3];
			f32 q[4];
			f32 len[3];
			f32 wid[3];
			f32 thk[3];

			// torn, or coming back: Call of Duty's own flight for the board,
			// its pull off to the zombie or its repair back on; at rest where
			// the repair leaves it (g_ZombieCodBoards)
			if (((k >= bar->boards && bar->torn[k]) || (k < bar->boards && frame < zmcharAnimFrames(anim)))
					&& zmcharAnimRoot(anim, frame, pos, q)) {
				if (k >= bar->boards && frame >= zmcharAnimFrames(anim) - 1) {
					continue;
				}

				zombiesQuatAxis(q, xaxis, len);
				zombiesQuatAxis(q, zaxis, wid);
				zombiesQuatAxis(q, yaxis, thk);
			} else if (k >= bar->boards) {
				continue;
			} else {
				memcpy(pos, cb->pos, sizeof(pos));
				memcpy(len, cb->len, sizeof(len));
				memcpy(wid, cb->wid, sizeof(wid));
				memcpy(thk, cb->thk, sizeof(thk));
			}

			gdl = zombiesBoardMeshDraw(gdl, bar, k, pos, len, wid, thk, light);

			// the boxes' state again for the next
			gDPPipeSync(gdl++);
			gDPSetRenderMode(gdl++, renderdata->zbufferenabled ? G_RM_AA_ZB_OPA_SURF : G_RM_AA_OPA_SURF,
					renderdata->zbufferenabled ? G_RM_AA_ZB_OPA_SURF2 : G_RM_AA_OPA_SURF2);
			gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
			gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
			gSPMatrix(gdl++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW | G_MTX_FLOATS);

			continue;
		}

		if (k >= bar->boards) {
			// torn: falling out and down into the room, turning
			f32 f;

			if (!bar->torn[k] || t >= ZOMBIE_BOARD_FALL60) {
				continue;
			}

			f = t / (f32)ZOMBIE_BOARD_FALL60;
			z += f * 45;
			y -= f * f * (board->y - 6);
			tilt += f * (k & 1 ? 1.2f : -1.2f);
		} else if (!bar->torn[k] && t < ZOMBIE_BOARD_FIX60) {
			// put back: in from the room
			f32 f = 1 - t / (f32)ZOMBIE_BOARD_FIX60;

			z += f * f * 40;
		}

		// each board a shade of its own
		rgb[0] = wood[0] - (k % 3) * 9;
		rgb[1] = wood[1] - (k % 3) * 6;
		rgb[2] = wood[2] - (k % 3) * 4;

		gdl = zombiesBoardBox(gdl, bar, 0, y, z, board->len / 2, 7, 2, tilt, rgb, light);
	}

	gDPPipeSync(gdl++);
	gSPClearGeometryMode(gdl++, G_CULL_BOTH);

	renderdata->gdl = gdl;

	return true;
}

/**
 * zombiesRenderCodZombie()'s, for a zombie held at a barrier and come to its
 * place there: the clip it plays and how far through, from the barrier's own
 * clock (zombiesTickBarriers()) - a board's grab, hold and pull, Call of
 * Duty's for that board, or the climb in. False when it is not at one, or
 * the clips are not there.
 */
static bool zombiesCodBarrier(s32 i, struct chrdata *chr, s32 *anim, f32 *frame)
{
	const s32 b = i >= 0 && i < MAX_BOTS ? g_ZombieAtBarrier[i] : -1;
	struct zombiebarrier *bar;
	f32 dx;
	f32 dz;
	s32 t;

	if (b < 0 || b >= g_ZombieNumBarriers || zmcharAnimFrames(ZMCHAR_TEAR) <= 0) {
		return false;
	}

	bar = &g_ZombieBarriers[b];

	if (bar->zombie != i) {
		return false;
	}

	dx = chr->prop->pos.x - bar->stand.x;
	dz = chr->prop->pos.z - bar->stand.z;

	if (dx * dx + dz * dz > 80 * 80) {
		return false;
	}

	if (bar->reach60 >= 0) {
		*anim = bar->reachleft ? ZMCHAR_WINDOW_L : ZMCHAR_WINDOW_R;
		t = bar->reach60;
	} else if (bar->enter60 >= 0) {
		*anim = zombiesBarrierAnim(bar, -1, 0);
		t = bar->enter60;
	} else if (bar->pulling >= 0) {
		*anim = zombiesBarrierAnim(bar, bar->pulling, ZMCHAR_TEAR_PULL);
		t = bar->tear60;
	} else if (bar->boards > 0) {
		const s32 k = bar->boards - 1;
		const s32 grab = zombiesBarrierPhase60(bar, k, ZMCHAR_TEAR_GRAB);

		if (bar->tear60 < grab) {
			*anim = zombiesBarrierAnim(bar, k, ZMCHAR_TEAR_GRAB);
			t = bar->tear60;
		} else {
			*anim = zombiesBarrierAnim(bar, k, ZMCHAR_TEAR_HOLD);
			t = bar->tear60 - grab;
		}
	} else {
		*anim = zombiesBarrierAnim(bar, -1, 0);
		t = 0;
	}

	*frame = t * zmcharAnimRate(*anim) / 60.0f;

	if (*frame > zmcharAnimFrames(*anim) - 1) {
		*frame = zmcharAnimFrames(*anim) - 1;
	}

	return true;
}

// The zombie's place and facing in its barrier's frame: its clip's root
// motion from the barrier's origin, the sill
static void zombiesCodBarrierPlace(s32 i, Mtxf *world)
{
	static const f32 axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	const struct zombiebarrier *bar = &g_ZombieBarriers[g_ZombieAtBarrier[i]];
	f32 pos[3];
	f32 q[4];
	f32 w[3];
	s32 a;

	for (a = 0; a < 3; a++) {
		zombiesBarrierToWorld(bar, axes[a], false, w);
		world->m[a][0] = w[0] / 4.0f;
		world->m[a][1] = w[1] / 4.0f;
		world->m[a][2] = w[2] / 4.0f;
	}

	// a swipe through the boards carries no root motion: its lunge is its
	// hips' own (11 to 35 inches), from where a zombie waits at the window
	if (!zmcharAnimRoot(g_ZombieCod[i].anim, g_ZombieCod[i].frame, pos, q)) {
		pos[0] = -23;
		pos[1] = 0;
		pos[2] = -31;
	}

	zombiesBarrierToWorld(bar, pos, true, w);
	world->m[3][0] = w[0];
	world->m[3][1] = w[1];
	world->m[3][2] = w[2];
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
		sprintf(text, "Hold %s for Mystery Box [Cost: %d]", zombiesUseName(playernum), zombiesBoxCost());
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
 * The round's end and start are Black Ops II's own (zombiesRoundMusic()). Without
 * their files Perfect Dark's are borrowed: the
 * opening of Skedar Mystery for the end of a round (the death stings tried
 * there and at a round's start sounded like a death), and the failed
 * mission's end screen music for the game over (the solo mission's death
 * music was too much a death, and not the end of a game). The groans are
 * its pained men and its death gurgles, pitched down (no Skedar).
 */
#define ZOMBIE_STING240        TICKS(1200) // a sting's length: the round break's five seconds
/**
 * The game's end, as Black Ops II's: the view falls to the floor and the
 * hands reach out (zombiesGameOverCameraTick(), zombiesReachViewShift()),
 * GAME OVER and the rounds survived over the HUD as it was; then the picture
 * goes black and comes back on a camera rising and turning over the arena,
 * the scoreboard over it (zombiesRenderGameOver()); then the end screens.
 * Alone, fire or accept ends the scoreboard early.
 */
#define ZOMBIE_GO_FALL60       75          // the fall to the floor
#define ZOMBIE_GO_TEXT60       40          // GAME OVER fades in from here
#define ZOMBIE_GO_TEXTIN60     45          // ... over this long
#define ZOMBIE_GO_PAN60        330         // black, and the camera over the arena
#define ZOMBIE_GO_BLACK60      30          // the fade to black and the fade back each
#define ZOMBIE_GO_SKIP60       (ZOMBIE_GO_PAN60 + 150) // alone, fire or accept ends it from here
// how long until the end screens: the whole of Tranzit's game over song (21
// seconds from the game's end, zombiesGameOverMusic()) and a breath after it.
// At 17.5 seconds the end screens cut its last few off.
#define ZOMBIE_GAMEOVER60      (ZOMBIE_GO_PAN60 + 990)
#define ZOMBIE_GAMEOVER_FADE60 60          // the old GAME OVER's fade in
#define ZOMBIE_GROAN_MIN60     70          // a groan from somewhere this often at the most,
#define ZOMBIE_GROAN_MAX60     200         // and at the least, while any zombie is near
#define ZOMBIE_GROAN_REACH     2500.0f     // how near a zombie is to be heard
#define ZOMBIE_RECORDS_FILE    "$S/zombies-records.txt"

s32 g_ZombieGameOver60;            // since the game ended (zombiesGunLowerFrac() reads it earlier)
static bool g_ZombieGameOverEnded; // mainEndStage() asked for
static s32 g_ZombieBestRound;      // the arena's best before this match, 0 for none
static bool g_ZombieNewBest;
static bool g_ZombieGameOverTest;  // --zombies-gameover-at's: no record written
static bool g_ZombieGameOverQuit;  // ended from the pause's End Game: nobody falls, the view stays as it is
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

/**
 * A player's Zombies over every match, kept under the agent's name (the
 * player's name is the agent's, mainmenu.c) rather than in a player file,
 * which nothing makes any more (no Save Player, no prompt at a match's end):
 * one line a name in the save folder, "<name>\t<kills> <downs> <headshots>
 * <shots> <hits>". Added to at each match's end (mpCalculateAwards()) for
 * this machine's players; read by Player Setup's Statistics.
 */
#define ZOMBIE_STATS_FILE "$S/zombies-stats.txt"

struct zombiecareer {
	u32 kills;
	u32 downs;
	u32 headshots;
	u32 shots;
	u32 hits;
};

static char g_ZombieCareerName[32];        // whose zombiesCareerOf() has read
static struct zombiecareer g_ZombieCareer; // ... and theirs
static bool g_ZombieCareerHave = false;

static void zombiesCareerKey(char *out, s32 size, const char *name)
{
	s32 i;

	for (i = 0; i < size - 1 && name[i] != '\0' && name[i] != '\n' && name[i] != '\t'; i++) {
		out[i] = name[i];
	}

	out[i] = '\0';
}

// The career of name, or nothing, out of the file
static void zombiesCareerRead(const char *name, struct zombiecareer *out)
{
	u32 size = 0;
	char *data = fsFileLoad(ZOMBIE_STATS_FILE, &size);
	char *p;
	s32 len = strlen(name);

	memset(out, 0, sizeof(*out));

	if (data == NULL) {
		return;
	}

	for (p = data; p < data + size; ) {
		char *end = p;

		while (end < data + size && *end != '\n') {
			end++;
		}

		if (end - p > len && strncmp(p, name, len) == 0 && p[len] == '\t') {
			sscanf(p + len + 1, "%u %u %u %u %u", &out->kills, &out->downs, &out->headshots, &out->shots, &out->hits);
		}

		p = end + 1;
	}

	sysMemFree(data);
}

// The Statistics page's: the career of the player whose menu it is
static struct zombiecareer *zombiesCareerOf(s32 mpindex)
{
	char key[32];

	zombiesCareerKey(key, sizeof(key), g_PlayerConfigsArray[mpindex].base.name);

	if (!g_ZombieCareerHave || strcmp(key, g_ZombieCareerName) != 0) {
		strcpy(g_ZombieCareerName, key);
		zombiesCareerRead(key, &g_ZombieCareer);
		g_ZombieCareerHave = true;
	}

	return &g_ZombieCareer;
}

/**
 * mpCalculateAwards(), each player of this machine as the match ends: its
 * kills, downs and headshots this match (g_ZombieStats), and its shots and
 * hits (mpstats), added to the agent's line.
 */
void zombiesCareerRecord(s32 playernum, s32 shots, s32 hits)
{
	u32 size = 0;
	char *data;
	char key[32];
	struct zombiecareer c;
	FILE *f;
	char *p;
	s32 len;

	if (playernum < 0 || playernum >= MAX_PLAYERS || !netIsLocalSlot(playernum) || g_ZombieGameOverTest) {
		return;
	}

	zombiesCareerKey(key, sizeof(key), g_PlayerConfigsArray[g_Vars.playerstats[playernum].mpindex].base.name);

	if (key[0] == '\0') {
		return;
	}

	zombiesCareerRead(key, &c);
	c.kills += g_ZombieStats[playernum].kills;
	c.downs += g_ZombieStats[playernum].downs;
	c.headshots += g_ZombieStats[playernum].headshots;
	c.shots += shots > 0 ? shots : 0;
	c.hits += hits > 0 ? hits : 0;

	data = fsFileLoad(ZOMBIE_STATS_FILE, &size);
	f = fsFileOpenWrite(ZOMBIE_STATS_FILE);

	if (f == NULL) {
		if (data) {
			sysMemFree(data);
		}

		return;
	}

	// everyone else's line kept
	len = strlen(key);

	if (data) {
		for (p = data; p < data + size; ) {
			char *end = p;

			while (end < data + size && *end != '\n') {
				end++;
			}

			if (end > p && !(end - p > len && strncmp(p, key, len) == 0 && p[len] == '\t')) {
				fprintf(f, "%.*s\n", (s32)(end - p), p);
			}

			p = end + 1;
		}

		sysMemFree(data);
	}

	fprintf(f, "%s\t%u %u %u %u %u\n", key, c.kills, c.downs, c.headshots, c.shots, c.hits);
	fsFileFree(f);

	g_ZombieCareerHave = false;
}

// Statistics' values (setup.c), item->param which: 0 kills, 1 downs, 2
// accuracy, 3 headshots
char *zombiesMenuTextCareer(struct menuitem *item)
{
	static char text[24];
	struct zombiecareer *c = zombiesCareerOf(g_MpPlayerNum);

	switch (item->param) {
	case 0:
		sprintf(text, "%u\n", c->kills);
		break;
	case 1:
		sprintf(text, "%u\n", c->downs);
		break;
	case 2:
		sprintf(text, "%.1f%%\n", c->shots > 0 ? c->hits * 100.0f / c->shots : 0.0f);
		break;
	default:
		sprintf(text, "%u\n", c->headshots);
		break;
	}

	return text;
}

// The music under a round being cleared, while the counter blinks. Not
// MUSIC_DEATH_BETA: that is the solo game over's tune. A round's start has
// none (MUSIC_DEATH_MP was there, and was not liked): the level's own comes
// back.
// Black Ops II's own (Tranzit's mus_zombie_round_over and _round_start,
// zmsfx.c), each machine's own, with the level's music paused for as long
// The level's music held off for the round's end (9.2 seconds) and the rest of
// the break until the next round's start takes over: held only as long as the
// piece, it came back for the break's last second before the round started
#define ZOMBIE_ROUND_OVER240  TICKS(240 * (ZOMBIE_ROUND_BREAK / 60 + 2))
#define ZOMBIE_ROUND_START240 TICKS(240 * 141 / 10) // and the next's start, 14 seconds

static bool zombiesRoundMusic(s32 slot, s32 duration240)
{
	const s32 id = zmSfxSound(slot);

	if (id <= 0) {
		return false;
	}

	musicPauseFor(duration240);
	zombiesPlaySoundHere(id, 1.0f);

	return true;
}

static void zombiesRoundSting(void)
{
	if (!g_ZombieCfgRoundMusic) {
		return;
	}

	if (!zombiesRoundMusic(ZMSFX_ROUND_OVER, ZOMBIE_ROUND_OVER240)) {
		musicStartSting(MUSIC_SKEDAR_MYSTERY, ZOMBIE_STING240);
	}
}

// The game over's: Black Ops II's, Tranzit's (21 seconds, in the patch's bank,
// zmb_patch.all.sabs: no alias names it), or Perfect Dark's failed mission's
// when its file is not there
// held past the song (21 seconds) to the end screens, or it came back for the
// game over's last second
#define ZOMBIE_GAME_OVER240 TICKS(240 * (ZOMBIE_GAMEOVER60 / 60 + 3))

static void zombiesGameOverMusic(void)
{
	if (!zombiesRoundMusic(ZMSFX_GAME_OVER, ZOMBIE_GAME_OVER240)) {
		musicStartSting(MUSIC_MISSION_FAILED, TICKS(240 * 20));
	}
}

// A round begun: its music, Black Ops II's (Perfect Dark had none for it)
static void zombiesRoundStartSting(void)
{
	if (g_ZombieCfgRoundMusic) {
		zombiesRoundMusic(ZMSFX_ROUND_START, ZOMBIE_ROUND_START240);
	}
}

static bool zombiesScoreboardHeld(void);

// The game's end, once the camera has left the player: the scoreboard alone
static bool zombiesGameOverHidesHud(void)
{
	return g_ZombieGameOver && g_ZombieGameOver60 >= ZOMBIE_GO_PAN60;
}

// bondmove.c's bmoveTick(): the game over is watched, not played - from the
// fall to the scoreboard no looking round, aiming or firing. Aiming down the
// sights is let go of with it.
// bondmove.c, before a move is applied: --zombies-test-gap's walk, player 1
// crouched and going straight on
s32 bmoveGetCrouchPos(void);

void zombiesTestDrive(struct movedata *data)
{
	if (g_ZombieTestDrive60 <= 0 || g_Vars.currentplayernum != 0) {
		return;
	}

	g_ZombieTestDrive60 -= g_Vars.lvupdate60;

	if (g_ZombieTestDrive60 % 60 < g_Vars.lvupdate60) {
		sysLogPrintf(LOG_NOTE, "zombies: --zombies-test-gap: player at %.0f %.0f %.0f, crouch %d of %d",
				g_Vars.currentplayer->prop->pos.x, g_Vars.currentplayer->vv_manground, g_Vars.currentplayer->prop->pos.z,
				bmoveGetCrouchPos(), g_Vars.currentplayer->crouchpos);
	}
	g_Vars.currentplayer->crouchpos = CROUCHPOS_DUCK;
	data->digitalstepforward = true;
	data->digitalstepback = false;
	data->analogwalk = 0;
	data->analogstrafe = 0;
	data->analogturn = 0;
}

bool zombiesLocksControls(void)
{
	if (!zombiesIsActive() || !g_ZombieGameOver) {
		return false;
	}

	g_Vars.currentplayer->insightaimmode = false;

	return true;
}

// bondgun.c and radar.c: the game's end over the arena and the scoreboard
// have the screen to themselves, no ammo or radar. Falling, the HUD stays.
bool zombiesHidesHud(void)
{
	return zombiesIsActive() && (zombiesGameOverHidesHud() || zombiesScoreboardHeld());
}

static f32 zombiesSmooth(f32 f)
{
	f = f < 0 ? 0 : f > 1 ? 1 : f;

	return f * f * (3 - 2 * f);
}

// How far into the fall to the floor, 0 to 1, eased
static f32 zombiesGameOverFall(void)
{
	return zombiesSmooth((f32)g_ZombieGameOver60 / ZOMBIE_GO_FALL60);
}

/**
 * zombiesGunLowerFrac() at the game's end. Falling, the hands stay in view,
 * reaching (zombiesReachViewShift()); a lowering of nothing, which stops the
 * trigger all the same (bondgun.c). Over the arena they are gone.
 */
static f32 zombiesGameOverGunLower(void)
{
	if (g_ZombieGameOver60 < ZOMBIE_GO_PAN60 - ZOMBIE_GO_BLACK60) {
		return 0.0001f;
	}

	return 1;
}

/**
 * bondgun.c, each hand's place in the view at the game's end: as the view
 * falls the hands go out in front and in toward the middle, reaching along
 * the floor, the way Black Ops II's last fall has them. True when it moved.
 */
bool zombiesReachViewShift(s32 handnum, struct coord *shift)
{
	s32 me = g_Vars.currentplayernum;
	f32 e;

	if (!zombiesIsActive() || !g_ZombieGameOver || me < 0 || me >= MAX_PLAYERS
			|| g_ZombieGameOver60 >= ZOMBIE_GO_PAN60 || g_ZombieGameOverQuit) {
		return false;
	}

	e = zombiesGameOverFall();

	shift->x = (handnum == HAND_LEFT ? 3.0f : -3.0f) * e;
	shift->y = 3.5f * e;
	shift->z = -9.0f * e;

	return true;
}

/**
 * player.c, each view at the game's end; 1 when it placed the camera.
 *
 * Falling (still on their feet, or down): the eye drops to the floor and the
 * view rolls onto its side, looking where it looked when the game ended. The
 * dead keep Perfect Dark's death view until the arena's camera. Over the
 * arena, after the black: a camera out along whichever of sixteen ways from
 * where the player lies has the longest clear sight (a corridor's length,
 * not its wall), backing away up it and rising as far as it is clear,
 * looking back across the arena past them and panning slowly from side to
 * side. Clients' own (the arena's walls are the same everywhere).
 */
#define ZOMBIE_GO_ROLL      0.5f   // the fall's roll, radians
#define ZOMBIE_GO_FLOOR     20.0f  // the eye above the floor, fallen
#define ZOMBIE_GO_ORBIT_R1  640.0f // the arena camera's distance out at the most,
#define ZOMBIE_GO_ORBIT_H1  360.0f // and its height over the player there,
#define ZOMBIE_GO_ORBIT_T   12.0f  // the seconds it takes to back away,
#define ZOMBIE_GO_ORBIT_W   0.10f  // its pan's pace,
#define ZOMBIE_GO_ORBIT_DIP 0.34f  // and how far down it looks at the most (the sine)

static bool g_ZombieGoHave[MAX_PLAYERS];        // the look at the end is kept
static struct coord g_ZombieGoLook[MAX_PLAYERS];
static f32 g_ZombieGoAngle[MAX_PLAYERS];        // the way out the arena camera goes (behind them, first)
static bool g_ZombieGoVantage[MAX_PLAYERS];     // ... found
static f32 g_ZombieGoReach[MAX_PLAYERS];        // ... and the share of it clear

s32 zombiesGameOverCameraTick(struct coord *pos, struct coord *up, struct coord *look)
{
	s32 me = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;
	struct prop *prop = player->prop;

	if (!zombiesIsActive() || me < 0 || me >= MAX_PLAYERS || prop == NULL) {
		return 0;
	}

	// --zombies-test-barrier N: the camera on barrier N, for a run nobody is
	// playing (its pictures: --screenshot-frame)
	{
		static s32 testbar = -2;

		if (testbar == -2) {
			testbar = sysArgGetInt("--zombies-test-barrier", -1);
		}

		// --zombies-test-zombie D: the camera D in front of the first
		// zombie walking (one at a barrier with --zombies-test-reach), the
		// player not to be killed meanwhile
		// --zombies-cherry-cam D: with --zombies-test-cherry, the camera D
		// behind and above player 1, looking past them, to see their burst
		// from outside
		if (sysArgCheck("--zombies-cherry-cam") && g_Vars.players[0] && g_Vars.players[0]->prop) {
			struct player *pl = g_Vars.players[0];
			const f32 d = sysArgGetInt("--zombies-cherry-cam", 250);

			pos->x = pl->prop->pos.x - pl->cam_look.x * d;
			pos->y = pl->prop->pos.y + 120;
			pos->z = pl->prop->pos.z - pl->cam_look.z * d;
			look->x = pl->prop->pos.x + pl->cam_look.x * 150 - pos->x;
			look->y = pl->prop->pos.y - 60 - pos->y;
			look->z = pl->prop->pos.z + pl->cam_look.z * 150 - pos->z;

			// D below 0: in front of them, looking back at their chest
			if (d < 0) {
				pos->y = pl->prop->pos.y + 20;
				look->x = pl->prop->pos.x - pos->x;
				look->y = pl->prop->pos.y - pos->y;
				look->z = pl->prop->pos.z - pos->z;
			}
			guNormalize(&look->x, &look->y, &look->z);
			up->x = 0;
			up->y = 1;
			up->z = 0;
			player0f0c1840(pos, up, look, &pl->prop->pos, pl->prop->rooms);

			return 1;
		}

		// --zombies-test-perk N: the camera round machine N (a perk's id,
		// Pack-a-Punch's after them), an eighth of the way round every 40
		// frames, --zombies-perk-dist away (200); --zombies-shot-step as above
		if (sysArgCheck("--zombies-test-perk")) {
			extern s32 g_ShotFrame;
			struct defaultobj *mobj = zombiesMachineObjAt(sysArgGetInt("--zombies-test-perk", 0));

			g_PlayerInvincible = true;

			if (g_ShotFrame > 0 && g_Vars.lvframenum > g_ShotFrame) {
				g_ShotFrame += sysArgGetInt("--zombies-shot-step", 10);
			}

			if (mobj && mobj->prop) {
				const f32 a = (g_Vars.lvframenum / 40) * 0.785398f;
				const f32 r = sysArgGetInt("--zombies-perk-dist", 200);

				pos->x = mobj->prop->pos.x + sinf(a) * r;
				pos->y = mobj->prop->pos.y + 60;
				pos->z = mobj->prop->pos.z + cosf(a) * r;
				look->x = mobj->prop->pos.x - pos->x;
				look->y = mobj->prop->pos.y + 60 - pos->y;
				look->z = mobj->prop->pos.z - pos->z;
				guNormalize(&look->x, &look->y, &look->z);
				up->x = 0;
				up->y = 1;
				up->z = 0;
				player0f0c1840(pos, up, look, &mobj->prop->pos, mobj->prop->rooms);

				return 1;
			}
		}

		// --zombies-test-follow N: the camera beside zombie N at its feet'
		// height, wherever it goes (down a ledge), the player not to be killed;
		// --zombies-shot-step S takes --screenshot-frame's picture every S
		// frames from there on
		if (sysArgCheck("--zombies-test-follow")) {
			extern s32 g_ShotFrame;
			static s32 climber = -1;
			s32 b = sysArgGetInt("--zombies-test-follow", 0);
			struct chrdata *zchr;

			// --zombies-follow-climb: the first zombie to jump up a ledge
			// instead, the pictures from its jump on
			if (sysArgCheck("--zombies-follow-climb")) {
				s32 k;

				for (k = 0; climber < 0 && k < g_BotCount && k < MAX_BOTS; k++) {
					if (g_ZombieClimb[k].on) {
						climber = k;
						g_ShotFrame = g_Vars.lvframenum + 2;
					}
				}

				if (climber >= 0) {
					b = climber;
				}
			}

			zchr = b < g_BotCount ? g_MpBotChrPtrs[b] : NULL;
			g_PlayerInvincible = true;

			if (g_ShotFrame > 0 && g_Vars.lvframenum > g_ShotFrame) {
				g_ShotFrame += sysArgGetInt("--zombies-shot-step", 10);
			}

			if (zchr && zchr->prop) {
				const f32 th = chrGetInverseTheta(zchr) + 1.5708f;

				pos->x = zchr->prop->pos.x + sinf(th) * 320;
				pos->y = zchr->manground + 120;
				pos->z = zchr->prop->pos.z + cosf(th) * 320;
				look->x = zchr->prop->pos.x - pos->x;
				look->y = zchr->manground + 60 - pos->y;
				look->z = zchr->prop->pos.z - pos->z;
				guNormalize(&look->x, &look->y, &look->z);
				up->x = 0;
				up->y = 1;
				up->z = 0;
				player0f0c1840(pos, up, look, &zchr->prop->pos, zchr->prop->rooms);

				return 1;
			}
		}

		{
			static s32 testzombie = -2;

			if (testzombie == -2) {
				testzombie = sysArgGetInt("--zombies-test-zombie", 0);
			}

			if (testzombie > 0) {
				s32 b;

				g_PlayerInvincible = true;

				for (b = 0; b < g_BotCount; b++) {
					struct chrdata *zchr = g_MpBotChrPtrs[b];

					if (zchr && zchr->prop && zchr->actiontype != ACT_DIE && zchr->actiontype != ACT_DEAD
							&& !chrIsDead(zchr) && zchr->prop->type == PROPTYPE_CHR
							&& (g_ZombieAtBarrier[b] >= 0) == (sysArgGetInt("--zombies-test-reach", 0) != 0 || sysArgGetInt("--zombies-test-atbarrier", 0) != 0)) {
						const f32 th = chrGetInverseTheta(zchr);

						pos->x = zchr->prop->pos.x + sinf(th) * testzombie;
						pos->y = zchr->ground + (g_ZombieCrawler[b] ? 55 : 110);
						pos->z = zchr->prop->pos.z + cosf(th) * testzombie;
						look->x = zchr->prop->pos.x - pos->x;
						look->y = zchr->ground + (g_ZombieCrawler[b] ? 20 : 90) - pos->y;
						look->z = zchr->prop->pos.z - pos->z;
						guNormalize(&look->x, &look->y, &look->z);
						up->x = 0;
						up->y = 1;
						up->z = 0;
						player0f0c1840(pos, up, look, &zchr->prop->pos, zchr->prop->rooms);

						return 1;
					}
				}
			}
		}

		if (testbar >= 0 && testbar < g_ZombieNumBarriers && g_ZombieBarriers[testbar].placed) {
			struct zombiebarrier *bar = &g_ZombieBarriers[testbar];

			pos->x = bar->pos.x - bar->wall.x * 560 + bar->wall.z * 120;
			pos->y = bar->pos.y + 170;
			pos->z = bar->pos.z - bar->wall.z * 560 - bar->wall.x * 120;
			look->x = bar->pos.x - bar->wall.x * 120 - pos->x;
			look->y = bar->pos.y + 120 - pos->y;
			look->z = bar->pos.z - bar->wall.z * 120 - pos->z;
			guNormalize(&look->x, &look->y, &look->z);
			up->x = 0;
			up->y = 1;
			up->z = 0;
			player0f0c1840(pos, up, look, &bar->stand, bar->standrooms);

			return 1;
		}
	}

	if (!g_ZombieGameOver) {
		g_ZombieGoHave[me] = false;
		g_ZombieGoVantage[me] = false;
		return 0;
	}

	if (!g_ZombieGoHave[me]) {
		f32 len;

		g_ZombieGoLook[me] = *look;
		g_ZombieGoLook[me].y = 0;
		len = sqrtf(g_ZombieGoLook[me].x * g_ZombieGoLook[me].x + g_ZombieGoLook[me].z * g_ZombieGoLook[me].z);

		if (len < 0.0001f) {
			f32 inv = prop->chr ? chrGetInverseTheta(prop->chr) : 0;

			g_ZombieGoLook[me].x = sinf(inv);
			g_ZombieGoLook[me].z = cosf(inv);
		} else {
			g_ZombieGoLook[me].x /= len;
			g_ZombieGoLook[me].z /= len;
		}

		// the arena camera starts behind them
		g_ZombieGoAngle[me] = atan2f(-g_ZombieGoLook[me].x, -g_ZombieGoLook[me].z);
		g_ZombieGoHave[me] = true;
	}

	if (g_ZombieGameOver60 < ZOMBIE_GO_PAN60) {
		const struct coord *fwd = &g_ZombieGoLook[me];
		f32 e = zombiesGameOverFall();
		f32 floor;
		struct coord side;
		struct coord lup;
		f32 c;
		f32 s;

		// the dead keep the death view; End Game keeps everyone's own, on
		// their feet, under GAME OVER
		if (player->isdead || g_ZombieGameOverQuit) {
			return 0;
		}

		floor = (prop->chr ? prop->chr->ground : prop->pos.y - 30) + ZOMBIE_GO_FLOOR;

		if (pos->y > floor) {
			pos->y += (floor - pos->y) * e;
		}

		// level ahead, rolled onto the right side
		look->x = fwd->x;
		look->y = -0.08f * e;
		look->z = fwd->z;

		side.x = -fwd->z;
		side.y = 0;
		side.z = fwd->x;
		c = cosf(ZOMBIE_GO_ROLL * e);
		s = sinf(ZOMBIE_GO_ROLL * e);
		lup.x = side.x * s;
		lup.y = c;
		lup.z = side.z * s;
		*up = lup;

		player0f0c1840(pos, up, look, &prop->pos, prop->rooms);

		return 1;
	}

	{
		f32 secs = (g_ZombieGameOver60 - ZOMBIE_GO_PAN60) / 60.0f;
		f32 f = zombiesSmooth(secs / ZOMBIE_GO_ORBIT_T);
		struct coord centre = prop->pos;
		struct coord cam;
		struct coord dir;
		f32 dist;
		f32 yaw;
		f32 len;

		centre.y = (prop->chr ? prop->chr->ground : prop->pos.y) + 40;

		// Once: the way out from them with the longest clear sight, a little
		// up, of sixteen; a corridor's length rather than its wall
		if (!g_ZombieGoVantage[me]) {
			f32 best = -1;
			s32 i;
			s32 k;

			for (i = 0; i < 16; i++) {
				f32 a = g_ZombieGoAngle[me] + i * (M_PI / 8);
				f32 frac = 1;
				struct coord t;

				for (k = 0; k < 8; k++) {
					t.x = centre.x + sinf(a) * ZOMBIE_GO_ORBIT_R1 * frac;
					t.y = centre.y + ZOMBIE_GO_ORBIT_H1 * frac;
					t.z = centre.z + cosf(a) * ZOMBIE_GO_ORBIT_R1 * frac;

					// clear to them, and inside a room: a sight line can
					// pass out of the level through a gap no wall closes
					if (cdTestLos04(&centre, prop->rooms, &t, CDTYPE_BG)) {
						RoomNum inrooms[9];
						RoomNum aboverooms[9];

						bgFindRoomsByPos(&t, inrooms, aboverooms, 8, NULL);

						if (inrooms[0] != -1) {
							break;
						}
					}

					frac *= 0.75f;
				}

				if (k == 8) {
					frac = 0;
				}

				// behind them first, all else being equal
				if (frac > best + 0.05f) {
					best = frac;
					g_ZombieGoAngle[me] = a;
					g_ZombieGoReach[me] = frac * 0.8f; // short of whatever stopped it
				}
			}

			g_ZombieGoVantage[me] = true;
		}

		// backing away up that line and rising, as far as it is clear,
		// looking back past them and turning slowly one way and the other
		dist = g_ZombieGoReach[me] * (0.55f + 0.45f * f);
		cam.x = centre.x + sinf(g_ZombieGoAngle[me]) * ZOMBIE_GO_ORBIT_R1 * dist;
		cam.y = centre.y + ZOMBIE_GO_ORBIT_H1 * dist;
		cam.z = centre.z + cosf(g_ZombieGoAngle[me]) * ZOMBIE_GO_ORBIT_R1 * dist;

		yaw = sinf(secs * ZOMBIE_GO_ORBIT_W * 2.5f) * 0.35f;
		dir.x = centre.x - cam.x;
		dir.y = centre.y - cam.y;
		dir.z = centre.z - cam.z;
		len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);

		if (len < 1) {
			dir = g_ZombieGoLook[me];
		} else {
			f32 c = cosf(yaw);
			f32 s = sinf(yaw);
			f32 x = dir.x / len;
			f32 z = dir.z / len;

			dir.x = x * c + z * s;
			dir.y /= len;
			dir.z = z * c - x * s;
		}

		// across the arena past them, not down at the floor
		if (dir.y < -ZOMBIE_GO_ORBIT_DIP) {
			f32 flat = sqrtf(dir.x * dir.x + dir.z * dir.z);
			f32 keep = sqrtf(1 - ZOMBIE_GO_ORBIT_DIP * ZOMBIE_GO_ORBIT_DIP);

			if (flat > 0.0001f) {
				dir.x = dir.x / flat * keep;
				dir.z = dir.z / flat * keep;
				dir.y = -ZOMBIE_GO_ORBIT_DIP;
			}
		}

		*pos = cam;
		*look = dir;
		zombiesUpFromLook(look, up);

		player0f0c1840(pos, up, look, &prop->pos, prop->rooms);
	}

	return 1;
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

	if (g_ZombieNewBest && !g_ZombieGameOverTest) {
		zombiesWriteBest(g_Vars.stagenum, g_ZombieRound);
	}

	sysLogPrintf(LOG_NOTE, "zombies: game over on stage 0x%02x at round %d (best before %d)",
			g_Vars.stagenum, g_ZombieRound, g_ZombieBestRound);

	zombiesGameOverMusic();
}

// The game over held, then the match's end screens
static void zombiesTickGameOver(void)
{
	g_ZombieGameOver60 += g_Vars.lvupdate60;

	// alone (offline), fire or accept is done with the scoreboard
	if (g_NetMode == NETMODE_NONE && g_ZombieGameOver60 >= ZOMBIE_GO_SKIP60) {
		s32 i;

		for (i = 0; i < PLAYERCOUNT(); i++) {
			s32 pad = optionsGetContpadNum1(g_Vars.playerstats[i].mpindex);

			if (joyGetButtonsPressedThisFrame(pad, Z_TRIG | A_BUTTON | BUTTON_UI_ACCEPT)) {
				g_ZombieGameOver60 = ZOMBIE_GAMEOVER60;
			}
		}
	}

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
/**
 * A perk machine's jingle now and then, as Black Ops II's machines play
 * theirs (mus_perks_*_jingle): one near a player, at random, from the
 * machine, every one and a half to four minutes. The host's, heard online
 * from the same machine everywhere.
 */
#define ZOMBIE_JINGLE_MIN60 (60 * 90)
#define ZOMBIE_JINGLE_MAX60 (60 * 240)
#define ZOMBIE_JINGLE_REACH 1500.0f

static s32 g_ZombieJingle60 = -1;

static void zombiesTickJingles(void)
{
	s32 near[ZOMBIEPERK_COUNT];
	s32 numnear = 0;
	s32 perk;

	if (NET_CLIENT) {
		return;
	}

	if (g_ZombieJingle60 < 0) {
		g_ZombieJingle60 = ZOMBIE_JINGLE_MIN60 + rngRandom() % (ZOMBIE_JINGLE_MAX60 - ZOMBIE_JINGLE_MIN60);
	}

	g_ZombieJingle60 -= g_Vars.lvupdate60;

	if (g_ZombieJingle60 > 0) {
		return;
	}

	g_ZombieJingle60 = -1;

	for (perk = 0; perk < ZOMBIEPERK_COUNT; perk++) {
		struct defaultobj *obj = zombiesMachineObjAt(perk);
		s32 p;

		if (!obj || !obj->prop) {
			continue;
		}

		for (p = 0; p < PLAYERCOUNT(); p++) {
			struct prop *pp = g_Vars.players[p]->prop;

			if (pp) {
				const f32 dx = pp->pos.x - obj->prop->pos.x;
				const f32 dz = pp->pos.z - obj->prop->pos.z;

				if (dx * dx + dz * dz < ZOMBIE_JINGLE_REACH * ZOMBIE_JINGLE_REACH) {
					near[numnear++] = perk;
					break;
				}
			}
		}
	}

	if (numnear > 0) {
		perk = near[rngRandom() % numnear];
		zombiesPlayCodSound(ZMSFX_PERK_JINGLE + perk, zombiesMachineObjAt(perk)->prop);
	}
}

/**
 * Each machine's hum, Black Ops II's (zmb_perks_machine_loop, Pack-a-Punch's
 * zmb_perks_packa_loop), looped from the machine and heard only close by.
 * Each machine's own (no word online): started as a player comes near and
 * stopped once they are well away, so the hums hold no channels across the
 * arena.
 */
#define ZOMBIE_HUM_START 650.0f
#define ZOMBIE_HUM_STOP  850.0f

static s16 g_ZombieHumChannel[ZOMBIE_PAP_ID + 1];
static struct prop *g_ZombieHumProp[ZOMBIE_PAP_ID + 1];
static bool g_ZombieHumInit = false;

static void zombiesTickHums(void)
{
	s32 id;

	if (!g_ZombieHumInit) {
		for (id = 0; id <= ZOMBIE_PAP_ID; id++) {
			g_ZombieHumChannel[id] = -1;
			g_ZombieHumProp[id] = NULL;
		}

		g_ZombieHumInit = true;
	}

	for (id = 0; id <= ZOMBIE_PAP_ID; id++) {
		struct defaultobj *obj = zombiesMachineObjAt(id);
		struct prop *prop = obj ? obj->prop : NULL;
		f32 nearest = 1e10f;
		s32 p;

		// a machine gone or moved (a new match), or its channel taken back
		// (a level's sounds stopped): its hum with it. The table is read
		// itself: psIsChannelFree() knows only the AI's channels.
		if (g_ZombieHumChannel[id] >= 0) {
			struct pschannel *ch = &g_PsChannels[g_ZombieHumChannel[id]];
			const bool ours = (ch->flags & PSFLAG_FREE) == 0 && ch->prop == g_ZombieHumProp[id];

			if (!ours || prop != g_ZombieHumProp[id]) {
				if (ours) {
					psStopChannel(g_ZombieHumChannel[id]);
				}

				g_ZombieHumChannel[id] = -1;
			}
		}

		if (!prop) {
			continue;
		}

		for (p = 0; p < PLAYERCOUNT(); p++) {
			struct prop *pp = g_Vars.players[p]->prop;

			if (pp) {
				const f32 dx = pp->pos.x - prop->pos.x;
				const f32 dz = pp->pos.z - prop->pos.z;

				nearest = MIN(nearest, dx * dx + dz * dz);
			}
		}

		if (g_ZombieHumChannel[id] < 0 && nearest < ZOMBIE_HUM_START * ZOMBIE_HUM_START) {
			const s32 sound = zmSfxSound(id == ZOMBIE_PAP_ID ? ZMSFX_PAP_HUM : ZMSFX_PERK_HUM);

			if (sound > 0) {
				g_ZombieHumChannel[id] = psCreate(NULL, prop, sound, -1, -1, 0, 0, PSTYPE_NONE, NULL, 1.0f,
						NULL, -1, 60, 300, 600);
				g_ZombieHumProp[id] = prop;
			}
		} else if (g_ZombieHumChannel[id] >= 0 && nearest > ZOMBIE_HUM_STOP * ZOMBIE_HUM_STOP) {
			psStopChannel(g_ZombieHumChannel[id]);
			g_ZombieHumChannel[id] = -1;
		}
	}
}

/**
 * Going down, as Black Ops II sounds it, for this machine's own players: the
 * final blow (evt_player_final_hit) as they drop, then the last stand's loop
 * (evt_laststand_loop) under everything until they are up again, bled out or
 * the game is over. Every machine's own (zombiesTick() and
 * zombiesClientTick()), heard by the one down alone.
 */
static struct sndstate *g_ZombieLastStandSnd[MAX_PLAYERS];
static bool g_ZombieWasDownSnd[MAX_PLAYERS];

static void zombiesTickLastStandSound(void)
{
	s32 p;

	for (p = 0; p < MAX_PLAYERS; p++) {
		const bool mine = p < PLAYERCOUNT() && (g_NetMode == NETMODE_NONE || netIsLocalSlot(p));
		const bool down = mine && g_ZombieDowned[p];
		// the loop not over the game's end (alone without Quick Revive, the
		// first down is it)
		const bool bleeding = down && !g_ZombieGameOver;

		// the blow that downs, whatever comes of it
		if (down && !g_ZombieWasDownSnd[p]) {
			const s32 hit = zmSfxSound(ZMSFX_FINAL_HIT);

			if (hit > 0) {
				zombiesPlaySoundHere(hit, 1.0f);
			}
		}

		if (bleeding && g_ZombieLastStandSnd[p] == NULL) {
			const s32 loop = zmSfxSound(ZMSFX_LASTSTAND);

			if (loop > 0) {
				sndStart(var80095200, loop, &g_ZombieLastStandSnd[p], -1, -1, -1, -1, -1);
			}
		} else if (!bleeding && g_ZombieLastStandSnd[p]) {
			audioStop(g_ZombieLastStandSnd[p]);
			g_ZombieLastStandSnd[p] = NULL;
		}

		g_ZombieWasDownSnd[p] = down;
	}
}

/**
 * A power-up lying there hums, Black Ops II's zmb_spawn_powerup_loop, looped
 * from it until it is taken or goes. Every machine's own, as the hums of the
 * machines (zombiesTickHums()).
 */
static s16 g_ZombieDropHumChannel[4] = { -1, -1, -1, -1 };
static struct prop *g_ZombieDropHumProp[4];

static void zombiesTickDropHums(void)
{
	s32 i;

	for (i = 0; i < 4; i++) {
		struct prop *prop = g_ZombieDrops[i].active ? g_ZombieDrops[i].obj.prop : NULL;

		if (g_ZombieDropHumChannel[i] >= 0) {
			struct pschannel *ch = &g_PsChannels[g_ZombieDropHumChannel[i]];
			const bool ours = (ch->flags & PSFLAG_FREE) == 0 && ch->prop == g_ZombieDropHumProp[i];

			if (!ours || prop != g_ZombieDropHumProp[i]) {
				if (ours) {
					psStopChannel(g_ZombieDropHumChannel[i]);
				}

				g_ZombieDropHumChannel[i] = -1;
			}
		}

		if (prop && g_ZombieDropHumChannel[i] < 0) {
			const s32 sound = zmSfxSound(ZMSFX_PU_LOOP);

			if (sound > 0) {
				g_ZombieDropHumChannel[i] = psCreate(NULL, prop, sound, -1, -1, 0, 0, PSTYPE_NONE, NULL, 1.0f,
						NULL, -1, 100, 500, 900);
				g_ZombieDropHumProp[i] = prop;
			}
		}
	}
}

/**
 * Quick Revive alone, as Black Ops II's (_zm_perks' revive_solo_fx): once its
 * last bottle (the third) is drunk, the machine goes, as the Mystery Box goes.
 * Two seconds on, zmb_box_move, and it rises 40 inches over three seconds,
 * rocking (vibrate: ten degrees, half a second a swing; zombiesQrLeavePose()),
 * then a puff of smoke (the poltergeist effect's place) and zmb_box_poof, and
 * it is out of the world, its prompt, hum and jingle with it (a machine no
 * longer placed, to everything that looks for one). The life it gave is kept.
 * Machines are made anew with the next stage.
 */
static void zombiesTickQrMachine(void)
{
	struct defaultobj *obj = &g_ZombiePerkObjs[ZOMBIEPERK_QUICKREVIVE];
	s32 before;
	s32 p;

	if (!g_ZombiePerkPlaced[ZOMBIEPERK_QUICKREVIVE] || !obj->prop
			|| zombiesPlayersInPlay() >= 2 || g_ZombieQrSoloBuys < ZOMBIE_QR_SOLO_BUYS) {
		return;
	}

	// the last bottle still in the hand: not yet
	if (g_ZombieQrLeave60 < 0) {
		for (p = 0; p < PLAYERCOUNT(); p++) {
			if (g_ZombieDrink60[p] >= 0 && g_ZombieDrinkPerk[p] == ZOMBIEPERK_QUICKREVIVE) {
				return;
			}
		}

		g_ZombieQrLeave60 = 0;
		return;
	}

	before = g_ZombieQrLeave60;
	g_ZombieQrLeave60 += g_Vars.lvupdate60;

	if (before < ZOMBIE_QR_LEAVE_WAIT60 && g_ZombieQrLeave60 >= ZOMBIE_QR_LEAVE_WAIT60) {
		zombiesPlayCodSound(ZMSFX_BOX_MOVE, obj->prop);
	}

	if (g_ZombieQrLeave60 < ZOMBIE_QR_LEAVE_WAIT60 + ZOMBIE_QR_LEAVE_RISE60) {
		return;
	}

	// the puff where it was, as high as it went
	{
		struct coord pos = obj->prop->pos;

		pos.y += ZOMBIE_QR_LEAVE_RISE + 90;
		smokeCreateSimple(&pos, obj->prop->rooms, SMOKETYPE_MEDIUM);
		zombiesCodOr(ZMSFX_BOX_POOF, obj->prop, SFX_EXPLOSION_8098, 1.4f);
	}

	g_ZombieQrLeave60 = -1;
	propDeregisterRooms(obj->prop);
	propDisable(obj->prop);
	g_ZombiePerkPlaced[ZOMBIEPERK_QUICKREVIVE] = false;

	sysLogPrintf(LOG_NOTE, "zombies: Quick Revive's machine is gone, its %d bottles spent", ZOMBIE_QR_SOLO_BUYS);
}

static void zombiesTickGroans(void)
{
	static const s16 groans[] = {
		SFX_ARGH_MALE_0086, SFX_ARGH_MALE_0088, SFX_ARGH_MALE_008B, SFX_ARGH_MALE_008E,
		SFX_ARGH_MALE_0091, SFX_ARGH_MALE_0094, SFX_ARGH_MALE_0097, SFX_ARGH_MALE_009A,
		SFX_GURGLE_05B1, SFX_GURGLE_05B2,
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
		s32 vox;

		// Black Ops II's own voices where they are there (zmsfx.c): a
		// running zombie's sprint, now and then a taunt, else its groan; at
		// their own pitch, give or take a little
		vox = zmSfxPickVox(zombiesChrIsCrawler(zchr) ? ZMVOX_CRAWL : rngRandom() % 6 == 0 ? ZMVOX_TAUNT
				: zombiesChrSpeedScale(zchr) > 0.8f ? ZMVOX_SPRINT : ZMVOX_AMB, rngRandom());

		if (vox >= 0) {
			sound = ZMSFX_NET(vox);
			pitch = 0.94f + RANDOMFRAC() * 0.12f;
		}

		psCreate(NULL, zchr->prop, zmSfxResolve(sound), -1, -1, 0, 0, PSTYPE_NONE, NULL, pitch, NULL, -1, -1, -1, -1);

		// online, from the same zombie on every machine (its config slot)
		zombiesNetKeepSound(sound, pitch, func0f18d074(mpPlayerGetIndex(zchr)));
	}
}

static void zombiesNetResumeTake(void);

/**
 * The Ray Gun's shot is Black Ops II's (zmsfx.c) once its file is there: the
 * weapon's own shoot sound, the Phoenix's until then, so every machine's gun
 * plays it as Perfect Dark plays any shot. Its burst and reload are played
 * by Zombies itself (zombiesShellBlast(), zombiesVmGunTick()).
 */
static void zombiesRayGunSounds(void)
{
	extern struct weaponfunc_shootauto invfunc_zm_raygun_shot;
	extern struct weaponfunc_shootsingle invfunc_zm_raygun_burst;
	static bool done = false;

	if (!done) {
		const s32 fire = zmSfxSound(ZMSFX_RAYGUN_FIRE);

		done = true;

		if (fire > 0) {
			invfunc_zm_raygun_shot.base.shootsound = fire;
			invfunc_zm_raygun_burst.base.shootsound = fire;
		}
	}
}

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

		if (g_ZombieGrenadeWait60[i] > 0) {
			g_ZombieGrenadeWait60[i] -= g_Vars.lvupdate60;
		}
	}

	zombiesTickPopups();

	// --zombies-test-throw F: player 1 throws a grenade at level frame F and
	// every 120 frames after (the button let go at once: a quick throw), with
	// grenades to spare; with --zombies-test-hold, seen in third person
	if (sysArgCheck("--zombies-test-throw") && !NET_CLIENT && g_Vars.players[0]) {
		extern s32 g_ShotFrame;
		const s32 at = sysArgGetInt("--zombies-test-throw", 300);

		g_PlayerInvincible = true;

		if (g_ShotFrame > 0 && g_Vars.lvframenum > g_ShotFrame) {
			g_ShotFrame += sysArgGetInt("--zombies-shot-step", 10);
		}

		if (g_Vars.lvframenum >= at && (g_Vars.lvframenum - at) % 120 == 0) {
			const s32 prevplayernum = g_Vars.currentplayernum;

			g_ZombieGrenades[0] = ZOMBIE_GRENADES_MAX;
			setCurrentPlayerNum(0);
			zombiesGrenadePress();
			setCurrentPlayerNum(prevplayernum);
		}
	}

	// --zombies-scan-gaps: at level frame 120, the spots of the arena under a
	// ceiling a crouch does not fit and the full crouch does (bondwalk.c's
	// bwalkZombiesLowGap()), logged a 25 unit cell at a time. With
	// --zombies-test-gap N, player 1 is put at frame 150 beside the Nth one
	// found, facing it, crouched, and walked forward (zombiesTestDrive());
	// --zombies-shot-step S takes --screenshot-frame's picture every S frames
	if (sysArgCheck("--zombies-scan-gaps") && !NET_CLIENT && g_Vars.players[0] && g_Vars.players[0]->prop) {
		static struct coord gapat[64];
		static RoomNum gaproom[64];
		static s32 numgaps = 0;
		extern s32 g_ShotFrame;
		struct player *pl = g_Vars.players[0];
		const f32 head = pl->vv_headheight;
		const f32 duck = head - 45.0f * pl->vv_eyeheight / 159.0f;
		const f32 squat = MAX(80.0f, head - 90.0f * pl->vv_eyeheight / 159.0f);

		if (sysArgCheck("--zombies-test-gap")) {
			g_PlayerInvincible = true;

			if (g_ShotFrame > 0 && g_Vars.lvframenum > g_ShotFrame) {
				g_ShotFrame += sysArgGetInt("--zombies-shot-step", 10);
			}
		}

		if (g_Vars.lvframenum == 120) {
			s32 r;

			for (r = 0; r < g_Vars.roomcount && numgaps < ARRAYCOUNT(gapat); r++) {
				RoomNum rooms[2] = { r, -1 };
				f32 x;
				f32 z;

				for (x = g_Rooms[r].bbmin[0]; x <= g_Rooms[r].bbmax[0] && numgaps < ARRAYCOUNT(gapat); x += 25) {
					for (z = g_Rooms[r].bbmin[2]; z <= g_Rooms[r].bbmax[2] && numgaps < ARRAYCOUNT(gapat); z += 25) {
						struct coord pos;
						f32 top = g_Rooms[r].bbmax[1] + 10;
						s32 k;

						// each floor in the room at this spot, from the top down
						for (k = 0; k < 4; k++) {
							f32 g;

							pos.x = x;
							pos.y = top;
							pos.z = z;
							g = cdFindGroundAtCyl(&pos, 20, rooms, NULL, NULL);

							if (g < g_Rooms[r].bbmin[1] - 10 || g < -30000) {
								break;
							}

							pos.y = g + 60;

							if (cdTestVolume(&pos, 20, rooms, CDTYPE_BG, CHECKVERTICAL_YES, duck - 60, -29.9f) != CDRESULT_NOCOLLISION
									&& cdTestVolume(&pos, 20, rooms, CDTYPE_BG, CHECKVERTICAL_YES, squat - 60, -29.9f) == CDRESULT_NOCOLLISION) {
								pos.y = g;
								gapat[numgaps] = pos;
								gaproom[numgaps] = r;
								sysLogPrintf(LOG_NOTE, "zombies: gap %d at %.0f %.0f %.0f (room %d)", numgaps, pos.x, pos.y, pos.z, r);
								numgaps++;
								break;
							}

							top = g - 10;
						}
					}
				}
			}

			sysLogPrintf(LOG_NOTE, "zombies: %d gap cells found", numgaps);
		}

		extern struct waypoint *waypointFindClosestToPos(struct coord *pos, RoomNum *rooms);

		// beside it: the waypoint nearest it, which is in the arena, facing it
		if (g_Vars.lvframenum == 150 && sysArgCheck("--zombies-test-gap")) {
			const s32 n = sysArgGetInt("--zombies-test-gap", 0);

			if (n >= 0 && n < numgaps) {
				RoomNum rooms[2] = { gaproom[n], -1 };
				struct waypoint *wp = waypointFindClosestToPos(&gapat[n], rooms);
				struct pad pad;

				if (wp && wp->padnum >= 0) {
					RoomNum atrooms[2];

					padUnpack(wp->padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);
					atrooms[0] = pad.room;
					atrooms[1] = -1;
					chrMoveToPos(pl->prop->chr, &pad.pos, atrooms,
							atan2f(gapat[n].x - pad.pos.x, gapat[n].z - pad.pos.z), true);
					g_ZombieTestDrive60 = 600;
					sysLogPrintf(LOG_NOTE, "zombies: --zombies-test-gap: player at pad %d (%.0f %.0f %.0f), walking to gap %d",
							wp->padnum, pad.pos.x, pad.pos.y, pad.pos.z, n);
				}
			}
		}
	}
	// --zombies-test-at X Z: player 1 put at X, Z (the floor there) at level
	// frame 150, and not to be killed by zombies
	if (sysArgCheck("--zombies-test-at") && !NET_CLIENT && g_Vars.players[0] && g_Vars.players[0]->prop
			&& g_Vars.lvframenum == 150) {
		struct coord at;
		RoomNum rooms[21];
		RoomNum above[21];
		const char *s = sysArgGetString("--zombies-test-at");

		at.x = at.z = 0;

		if (s && sscanf(s, "%f,%f", &at.x, &at.z) == 2) {
			at.y = g_Vars.players[0]->prop->pos.y + 300;
			func0f065e74(&g_Vars.players[0]->prop->pos, g_Vars.players[0]->prop->rooms, &at, rooms);
			bgFindRoomsByPos(&at, rooms, above, 20, NULL);
			at.y = cdFindGroundAtCyl(&at, 20, rooms, NULL, NULL) + 1;
			chrMoveToPos(g_Vars.players[0]->prop->chr, &at, rooms, 0, true);
			sysLogPrintf(LOG_NOTE, "zombies: --zombies-test-at: player at %.0f %.0f %.0f", at.x, at.y, at.z);
		}
	}

	// --zombies-test-qrleave F: Quick Revive's three solo bottles counted as
	// bought at level frame F, so its machine goes (zombiesTickQrMachine());
	// watch it with --zombies-test-perk 1
	if (sysArgCheck("--zombies-test-qrleave") && !NET_CLIENT
			&& g_Vars.lvframenum == sysArgGetInt("--zombies-test-qrleave", 200)) {
		g_ZombieQrSoloBuys = ZOMBIE_QR_SOLO_BUYS;
	}

	// --zombies-test-powerups K: at level frame 200, power-ups K to K+3 (of
	// ZOMBIEPOWERUP_*, round from the last to the first) in a row in front of
	// player 1, who is not to be killed; --zombies-shot-step S takes
	// --screenshot-frame's picture every S frames from there on
	if (sysArgCheck("--zombies-test-powerups") && !NET_CLIENT && g_Vars.players[0] && g_Vars.players[0]->prop) {
		extern s32 g_ShotFrame;
		struct prop *pprop = g_Vars.players[0]->prop;

		g_PlayerInvincible = true;

		if (g_ShotFrame > 0 && g_Vars.lvframenum > g_ShotFrame) {
			g_ShotFrame += sysArgGetInt("--zombies-shot-step", 10);
		}

		if (g_Vars.lvframenum == 200) {
			const struct coord *look = &g_Vars.players[0]->cam_look;
			const f32 len = sqrtf(look->x * look->x + look->z * look->z) + 0.001f;
			const f32 fx = look->x / len;
			const f32 fz = look->z / len;
			const s32 first = sysArgGetInt("--zombies-test-powerups", 0);
			s32 k;

			for (k = 0; k < ARRAYCOUNT(g_ZombieDrops); k++) {
				const f32 side = (k - 1.5f) * 70.0f;
				struct coord pos;
				bool made;

				pos.x = pprop->pos.x + fx * 200.0f - fz * side;
				pos.y = pprop->pos.y;
				pos.z = pprop->pos.z + fz * 200.0f + fx * side;

				made = zombiesDropMake(&g_ZombieDrops[k], (first + k) % ZOMBIEPOWERUP_COUNT, &pos,
						zombiesGroundAt(&pos, pprop->rooms), pprop->rooms);

				if (made) {
					g_ZombieDropSerial[k]++;
				}

				sysLogPrintf(LOG_NOTE, "zombies: --zombies-test-powerups: %d at %.0f %.0f %.0f, %s",
						(first + k) % ZOMBIEPOWERUP_COUNT, pos.x, pos.y, pos.z, made ? "made" : "not made");
			}
		}
	}

	// --zombies-test-drink P: player 1 buys perk P at level frame 200 (its
	// sounds and its drink), the player not to be killed; --zombies-shot-step
	// S takes --screenshot-frame's picture every S frames from there on
	if (sysArgCheck("--zombies-test-drink") && g_Vars.lvframenum == 200) {
		g_ZombieDrinkPerk[0] = sysArgGetInt("--zombies-test-drink", 0);
		g_ZombieDrink60[0] = 0;
		zombiesPerkBought(g_ZombieDrinkPerk[0], NULL);
	}

	if (sysArgCheck("--zombies-test-drink")) {
		extern s32 g_ShotFrame;

		g_PlayerInvincible = true;

		if (g_ShotFrame > 0 && g_Vars.lvframenum > g_ShotFrame) {
			g_ShotFrame += sysArgGetInt("--zombies-shot-step", 10);
		}
	}

	// --zombies-test-cherry: Electric Cherry's burst round player 1 every 90
	// frames (from frame 30), the player not to be killed; --zombies-shot-step
	// S takes --screenshot-frame's picture every S frames from there on
	if (sysArgCheck("--zombies-test-cherry")) {
		extern s32 g_ShotFrame;

		g_PlayerInvincible = true;

		if (g_Vars.lvframenum % 90 == 30) {
			zombiesCherryBurst(0, sysArgGetInt("--zombies-cherry-reach", 300));
		}

		if (g_ShotFrame > 0 && g_Vars.lvframenum > g_ShotFrame) {
			g_ShotFrame += sysArgGetInt("--zombies-shot-step", 10);
		}
	}

	// a new match: the frame count has gone back (the block below runs for
	// a second of frames, so it is no good for a once-only reset)
	if (g_Vars.lvframenum < g_ZombieLastFrame) {
		zombiesResetJuggernog();
		zombiesResetBox();
		zombiesResetDrops();
		zombiesResetWallBuys();
		zombiesResetPerks();
		zombiesResetPap();
		zombiesResetBarriers();

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
		zombiesPlaceBarriers();
	}

	zombiesRayGunSounds();

	if (g_Vars.lvframenum < 60) {
		zombiesResetPoints();

		// new match: reset state and set up teams (you vs. all bots)
		g_ZombieRound = 0;
		g_ZombieNextWaveFrame = -1;
		g_ZombieGraceUntil = 0;
		g_ZombieGameOver = false;
		g_ZombieGameOver60 = 0;
		g_ZombieGameOverEnded = false;
		g_ZombieGameOverQuit = false;
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

	// --zombies-gameover-at N: the game's end at level frame N, for a run
	// nobody is playing (its pictures: --screenshot-frame)
	{
		static s32 gameoverat = -2;

		if (gameoverat == -2) {
			gameoverat = sysArgGetInt("--zombies-gameover-at", 0);
		}

		if (gameoverat > 0 && g_Vars.lvframenum >= gameoverat && !g_ZombieGameOver && !NET_CLIENT) {
			gameoverat = 0;

			if (g_ZombieRound < 1) {
				g_ZombieRound = 1;
			}

			g_ZombieGameOverTest = true; // its records are not written
			sysLogPrintf(LOG_NOTE, "zombies: --zombies-gameover-at: the game's end at frame %d", g_Vars.lvframenum);
			zombiesStartGameOver();
		}
	}

	zombiesTestShoot();
	zombiesTickSwings();

	// --zombies-test-gibs 1: every zombie's left arm off at level frame 600,
	// for a run nobody is playing
	if (g_Vars.lvframenum == 600 && sysArgGetInt("--zombies-test-gibs", 0)) {
		for (i = 0; i < MAX_BOTS; i++) {
			g_ZombieGib[i] |= ZGIB_LARM;
		}
	}

	// --zombies-test-crawlers 1: every zombie a crawler, for a run nobody is playing
	{
		static s32 testcrawlers = -1;

		if (testcrawlers < 0) {
			testcrawlers = sysArgGetInt("--zombies-test-crawlers", 0);
		}

		if (testcrawlers) {
			for (i = 0; i < MAX_BOTS; i++) {
				g_ZombieCrawler[i] = 1;
			}
		}
	}

	// --zombies-test-hold W: player 1 given weapon W at level frame 60 and
	// seen in third person (--zombies-test-third 0: first), for a run nobody
	// is playing
	{
		static s32 testhold = -2;

		if (testhold == -2) {
			testhold = sysArgGetInt("--zombies-test-hold", 0);
		}

		if (testhold > 0 && g_Vars.lvframenum >= 30 && !NET_CLIENT && g_Vars.players[0]) {
			const s32 prevplayernum = g_Vars.currentplayernum;

			setCurrentPlayerNum(0);
			invGiveSingleWeapon(testhold);
			zombiesSyncGuns(0);
			zombiesFillGun(testhold, ZOMBIE_AMMO_CLIPS, true);
			bgunEquipWeapon(testhold);
			g_Vars.currentplayer->thirdperson = sysArgGetInt("--zombies-test-third", 1) != 0;
			g_Vars.currentplayer->vv_theta += sysArgGetInt("--zombies-test-turn", 0);
			g_ModOptions.camside = sysArgGetInt("--zombies-test-side", (s32)g_ModOptions.camside);

			// --zombies-test-func 1: its second function chosen
			if (sysArgGetInt("--zombies-test-func", 0) == 1 && weaponHost(testhold) >= WEAPON_UNARMED) {
				const s32 host = weaponHost(testhold) - 1;

				g_PlayerConfigsArray[g_Vars.currentplayerstats->mpindex].gunfuncs[host >> 3] |= 1 << (host & 7);
			}
			setCurrentPlayerNum(prevplayernum);

			sysLogPrintf(LOG_NOTE, "zombies: --zombies-test-hold: weapon %d", testhold);
			testhold = 0;
		}
	}

	// nobody bleeds out or gets up once the game is over: they lie where
	// they fell for its camera
	if (!g_ZombieGameOver) {
		zombiesTickDowned();
	}

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
		zombiesTickLava(i);
		zombiesTickJuggernog(i);
		zombiesTickPerkMachines(i);
		zombiesTickPap(i);
		zombiesTickWhere(i);
		zombiesTickKnife(i);
		zombiesTickThrow(i);
	}

	g_ZombieSoundFor = -1;

	zombiesTintPerkMachines();
	zombiesTintPap();

	zombiesTickBox();
	zombiesTickWallBuys();
	zombiesTickDoors();
	zombiesTickBarriers();
	zombiesTickDrops();

	if (g_ZombieRound > 0) {
		zombiesTickGroans();
		zombiesTickJingles();
		zombiesTickQrMachine();
		zombiesTickHums();
	}

	zombiesTickLastStandSound();
	zombiesTickDropHums();



	if (g_ZombieRound == 0) {
		if (g_Vars.lvframenum >= 120) {
			// the Zombies Options page's Starting Round
			g_ZombieRound = g_ZombieCfgStartRound > 1 ? g_ZombieCfgStartRound : 1;
			zombiesRoundStartSting();

			// round 1's grenades
			for (i = 0; i < MAX_PLAYERS; i++) {
				g_ZombieGrenades[i] = ZOMBIE_GRENADES_ROUND;
			}

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

			// at a barrier's boards: held there until they are torn off
			if (zombiesBarrierHolds(i, zchr, target)) {
				continue;
			}

			// at a ladder: Black Ops II's jump up it instead
			if (zombiesTickClimb(i, zchr, target)) {
				continue;
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
		zombiesRoundStartSting();
		zombiesRespawnTheDead();

		for (i = 0; i < MAX_PLAYERS; i++) {
			g_ZombieGrenades[i] += ZOMBIE_GRENADES_ROUND;

			if (g_ZombieGrenades[i] > ZOMBIE_GRENADES_MAX) {
				g_ZombieGrenades[i] = ZOMBIE_GRENADES_MAX;
			}
		}

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

static Gfx *zombiesFillRect(Gfx *gdl, f32 x1, f32 y1, f32 x2, f32 y2, u32 colour);
static u32 zombiesAlphaOf(u32 colour, f32 alpha);

/**
 * The gun's corner of the HUD goes quiet, as Black Ops II's does: a while
 * with no shot fired, no grenade thrown, no reload and no change of gun or
 * function, and the function's square, the ammo and the grenades fade away
 * (the gun's and the function's names fade by themselves already). The
 * round and the points stay. Any of those brings it all back at once, the
 * gun's name with it (zombiesHudWoke()).
 */
#define ZOMBIE_HUD_IDLE60 480 // eight seconds
#define ZOMBIE_HUD_FADE60 30

static s32 g_ZombieHudIdle60[MAX_PLAYERS];
static bool g_ZombieHudWoke[MAX_PLAYERS];
static f32 g_ZombieHudAlphaNow = 1;

// bondgun.c, as it draws the current player's corner of the HUD: its alpha, 0 to 1
f32 zombiesHudIdleAlpha(void)
{
	s32 me = g_Vars.currentplayernum;
	struct player *player = g_Vars.currentplayer;
	bool busy;
	f32 t;

	if (!zombiesIsActive() || me < 0 || me >= MAX_PLAYERS) {
		return 1;
	}

	busy = player->hands[HAND_RIGHT].firing || player->hands[HAND_LEFT].firing
		|| player->hands[HAND_RIGHT].state == HANDSTATE_RELOAD
		|| player->hands[HAND_RIGHT].state == HANDSTATE_CHANGEGUN
		|| player->hands[HAND_RIGHT].state == HANDSTATE_CHANGEFUNC
		|| player->gunctrl.switchtoweaponnum >= 0
		|| g_ZombieThrowStage[me] != ZTHROW_NONE
		|| g_ZombieGameOver;

	if (busy) {
		if (g_ZombieHudIdle60[me] >= ZOMBIE_HUD_IDLE60) {
			g_ZombieHudWoke[me] = true;
		}

		g_ZombieHudIdle60[me] = 0;
	} else if (g_ZombieHudIdle60[me] < ZOMBIE_HUD_IDLE60 + ZOMBIE_HUD_FADE60) {
		g_ZombieHudIdle60[me] += g_Vars.lvupdate60;
	}

	t = (f32)(g_ZombieHudIdle60[me] - ZOMBIE_HUD_IDLE60) / ZOMBIE_HUD_FADE60;
	g_ZombieHudAlphaNow = t <= 0 ? 1 : t >= 1 ? 0 : 1 - t;

	return g_ZombieHudAlphaNow;
}

// zombiesHudIdleAlpha()'s, as it was worked out for the view being drawn
static f32 zombiesHudIdleAlphaNow(void)
{
	return zombiesIsActive() ? g_ZombieHudAlphaNow : 1;
}

// bondgun.c: the corner has just come back, and the gun's name with it
bool zombiesHudWoke(void)
{
	s32 me = g_Vars.currentplayernum;
	bool woke;

	if (!zombiesIsActive() || me < 0 || me >= MAX_PLAYERS) {
		return false;
	}

	woke = g_ZombieHudWoke[me];
	g_ZombieHudWoke[me] = false;

	return woke;
}

// Compact ammo readout, e.g. "35/200", or "8|8/80" when dual wielding.
// Called from the game's own ammo HUD code in bondgun.c.
Gfx *zombiesDrawAmmo(Gfx *gdl, s32 clipl, s32 clipr, s32 reserve, s32 bottom, s32 scalex)
{
	const f32 idle = zombiesHudIdleAlphaNow();
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
		text[0] = '\0';
	}

	// over the function's name, under the gun's (zombiesHudAmmoBottom())
	ammobottom = zombiesHudAmmoBottom(bottom);
	zombiesHudNameBottom(bottom);

	// the grenades left, in a row under the function's square (bondgun.c's,
	// whose right edge is the stack's): one upright pip each, right to left
	if (g_Vars.currentplayernum >= 0 && g_Vars.currentplayernum < MAX_PLAYERS) {
		s32 x = rightx;
		s32 k;

		for (k = 0; k < g_ZombieGrenades[g_Vars.currentplayernum] && k < ZOMBIE_GRENADES_MAX; k++) {
			gdl = zombiesFillRect(gdl, x - 4, bottom + 4, x, bottom + 11, zombiesAlphaOf(0xd0d8c0d0, idle));
			gdl = zombiesFillRect(gdl, x - 3, bottom + 2, x - 1, bottom + 4, zombiesAlphaOf(0xd0d8c0d0, idle));
			x -= 7;
		}
	}

	if (!text[0] || idle <= 0) {
		return gdl;
	}

	return zombiesDrawTextBR(gdl, text, rightx, ammobottom, zombiesAlphaOf(0xffffffc0, idle), g_CharsHandelGothicSm, g_FontHandelGothicSm);
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

	// nothing to buy at the game's end
	if (playernum < 0 || playernum >= MAX_PLAYERS || g_ZombieGameOver) {
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
	} else if (zombiesPapPrompt(playernum, text) || zombiesBoxPrompt(playernum, text) || zombiesWallBuyPrompt(playernum, text)
			|| zombiesDoorPrompt(playernum, text) || zombiesBarrierPrompt(playernum, text)) {
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

// The alignment a portrait is drawn under: the points' (the right edge's),
// or the scoreboard's, which is centred (zombiesRenderScoreboard())
static u32 g_ZombiePortraitCentred = false;

static u32 zombiesPortraitAlign(void)
{
	return g_ZombiePortraitCentred ? G_ASPECT_CENTER_EXT : g_HudAlignModeR;
}

/**
 * A player's head in a box boxh tall whose right edge is at right and bottom
 * at bottom, in the HUD's coordinates as the points are drawn (aligned to the
 * right edge, or centred for the scoreboard), backed in backing. Returns the box's left edge, or right when
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

	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, zombiesPortraitAlign());

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
			(s32)zombiesHudAlignedX(left, zombiesPortraitAlign()), top,
			(s32)(zombiesHudAlignedX(right, zombiesPortraitAlign()) + 0.999f), bottom);
	gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, zombiesPortraitAlign());

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

		// Black Ops II's own model glows instead (zombiesRenderPowerupGlows())
		if (!drop->active || prop == NULL || (drop->obj.flags2 & OBJFLAG2_INVISIBLE)
				|| zombiesPowerupMesh(drop->kind)) {
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
		// only while somebody is still up to finish the round: dead alone
		// (solo, or the last one standing) the game is over, and this read
		// over GAME OVER
		if (anyoneup && !g_ZombieGameOver) {
			gdl = zombiesDrawWords(gdl, "YOU WILL RESPAWN NEXT ROUND", centrex, viGetViewTop() + 80, 0xffffffa0);
		}

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

static Gfx *zombiesRenderScoreboard(Gfx *gdl, f32 alpha, s32 top);

/**
 * The game's end on the HUD (the timing is ZOMBIE_GO_*'s, above).
 *
 * Falling: GAME OVER and the rounds survived fade in over the middle of the
 * view, the HUD as it was around them (zombiesRenderHud()), and the view
 * goes to black. Over the arena: back from black with the scoreboard, and
 * under it the arena's best or that this was it (zombiesStartGameOver()).
 */
static Gfx *zombiesRenderGameOver(Gfx *gdl)
{
	s32 left = viGetViewLeft();
	s32 top = viGetViewTop();
	s32 width = viGetViewWidth();
	s32 height = viGetViewHeight();
	s32 centrex = left + width / 2;
	s32 t = g_ZombieGameOver60;
	struct fontchar *bigchars = g_CharsHandelGothicLg;
	struct font *bigfont = g_FontHandelGothicLg;
	f32 black = 0;
	s32 y;
	char text[64];

	if (bigfont == NULL) {
		bigchars = g_CharsHandelGothicMd;
		bigfont = g_FontHandelGothicMd;
	}

	if (t < ZOMBIE_GO_PAN60) {
		f32 f = (f32)(t - ZOMBIE_GO_TEXT60) / ZOMBIE_GO_TEXTIN60;
		u32 alpha;

		f = f < 0 ? 0 : f > 1 ? 1 : f;
		alpha = (u32)(0xff * f);

		if (alpha > 0) {
#ifndef PLATFORM_N64
			gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
#endif

			gdl = text0f153628(gdl);

			y = top + height * 34 / 100;
			gdl = zombiesDrawCentred(gdl, "GAME OVER", centrex, y, 0xc8141400 | alpha, bigchars, bigfont, 2);
			y += zombiesLineHeight(bigchars, bigfont) * 2 + 6;

			sprintf(text, "You Survived %d Round%s", g_ZombieRound, g_ZombieRound == 1 ? "" : "s");
			gdl = zombiesDrawCentred(gdl, text, centrex, y, 0xffffff00 | alpha, g_CharsHandelGothicMd, g_FontHandelGothicMd, 1);

			gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
			gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif
		}

		if (t >= ZOMBIE_GO_PAN60 - ZOMBIE_GO_BLACK60) {
			black = (f32)(t - (ZOMBIE_GO_PAN60 - ZOMBIE_GO_BLACK60)) / ZOMBIE_GO_BLACK60;
		}
	} else {
		f32 f = (f32)(t - ZOMBIE_GO_PAN60) / ZOMBIE_GO_BLACK60;
		s32 boardtop = top + height * 22 / 100;

		f = f > 1 ? 1 : f;
		black = 1 - f;

		// the arena's darkened a little under the board, as Black Ops II's
		gdl = text0f153628(gdl);
		gdl = zombiesFillRect(gdl, left, top, left + width, top + height, 0x00000050);
		gdl = text0f153780(gdl);

		gdl = zombiesRenderScoreboard(gdl, f, boardtop);

#ifndef PLATFORM_N64
		gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
#endif

		gdl = text0f153628(gdl);

		y = top + height * 84 / 100;

		if (g_ZombieNewBest) {
			strcpy(text, g_ZombieBestRound > 0 ? "NEW BEST ON THIS ARENA!" : "FIRST RECORD ON THIS ARENA");
			gdl = zombiesDrawCentred(gdl, text, centrex, y, 0x60ff6000 | (u32)(0xff * f), g_CharsHandelGothicSm, g_FontHandelGothicSm, 1);
		} else if (g_ZombieBestRound > 0) {
			sprintf(text, "Best on this arena: Round %d", g_ZombieBestRound);
			gdl = zombiesDrawCentred(gdl, text, centrex, y, 0xc0c0c000 | (u32)(0xff * f), g_CharsHandelGothicSm, g_FontHandelGothicSm, 1);
		}

		gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
		gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif
	}

	if (black > 0) {
		gdl = text0f153628(gdl);
		gdl = zombiesFillRect(gdl, left, top, left + width, top + height, (u32)(0xff * (black > 1 ? 1 : black)));
		gdl = text0f153780(gdl);
	}

	return gdl;
}

/**
 * The scoreboard, as Black Ops II's has it: a bar with the arena's name and
 * the columns' names, then a row a player, best score first: their head,
 * their name on a bar (gold for the player looking at it), and their score,
 * kills, downs, revives and headshots over the columns' blue. Centred on the
 * view, alpha from 0 to 1. Held up by the scoreboard button in play
 * (zombiesScoreboardHeld()), and over the arena at the game's end
 * (zombiesRenderGameOver()).
 */
#define ZOMBIE_SB_COLUMNS 5

static void zombiesTextOf(char *out, s32 size, const char *in)
{
	s32 i;

	for (i = 0; i < size - 1 && in[i] != '\0' && in[i] != '\n'; i++) {
		out[i] = in[i];
	}

	out[i] = '\0';
}

static u32 zombiesAlphaOf(u32 colour, f32 alpha)
{
	return (colour & 0xffffff00) | (u32)((colour & 0xff) * alpha);
}

static Gfx *zombiesRenderScoreboard(Gfx *gdl, f32 alpha, s32 top)
{
	static const char *headings[ZOMBIE_SB_COLUMNS] = { "Score", "Kills", "Downs", "Revives", "Headshots" };
	// each column's share of the columns' width, by its heading's and its
	// number's width
	static const u8 weights[ZOMBIE_SB_COLUMNS] = { 24, 18, 20, 23, 29 };
	struct fontchar *chars = g_CharsHandelGothicSm;
	struct font *font = g_FontHandelGothicSm;
	struct fontchar *xschars = g_CharsHandelGothicXs;
	struct font *xsfont = g_FontHandelGothicXs;
	f32 s = viGetViewHeight() / 240.0f;
	s32 width = viGetViewWidth() * 94 / 100;
	s32 left = viGetViewLeft() + (viGetViewWidth() - width) / 2;
	s32 right = left + width;
	s32 line = zombiesLineHeight(chars, font);
	s32 headh = line + 6 * s;
	s32 rowh = line * 3 / 2 + 6 * s;
	s32 colleft = right - width * 64 / 100;
	s32 colx[ZOMBIE_SB_COLUMNS + 1];
	s32 order[MAX_PLAYERS];
	s32 count = 0;
	s32 i;
	s32 j;
	s32 y;
	char text[32];

	if (alpha <= 0) {
		return gdl;
	}

	if (alpha > 1) {
		alpha = 1;
	}

	colx[0] = colleft;

	for (i = 0; i < ZOMBIE_SB_COLUMNS; i++) {
		colx[i + 1] = colx[i] + (right - colleft) * weights[i] / 114;
	}

	colx[ZOMBIE_SB_COLUMNS] = right;

	// the players in play, best score first
	for (i = 0; i < PLAYERCOUNT(); i++) {
		if (!g_ZombieOutOfPlay[i]) {
			order[count++] = i;
		}
	}

	for (i = 1; i < count; i++) {
		for (j = i; j > 0 && zombiesGetPoints(NULL, order[j]) > zombiesGetPoints(NULL, order[j - 1]); j--) {
			s32 tmp = order[j];
			order[j] = order[j - 1];
			order[j - 1] = tmp;
		}
	}

	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);

	gdl = text0f153628(gdl);

	// the heading bar: the arena's name, and each column's
	gdl = zombiesFillRect(gdl, left, top, colleft - 2 * s, top + headh, zombiesAlphaOf(0x202428d0, alpha));
	gdl = zombiesFillRect(gdl, colleft, top, right, top + headh, zombiesAlphaOf(0x202428d0, alpha));
	gdl = zombiesFillRect(gdl, left, top + headh - s, right, top + headh, zombiesAlphaOf(0x8090a0c0, alpha));

	zombiesTextOf(text, sizeof(text), mpMenuTextArenaName(NULL));
	gdl = zombiesDrawTextBL(gdl, text, left + 6 * s, top + headh - 2 * s, zombiesAlphaOf(0xffffffff, alpha), chars, font);

	for (i = 0; i < ZOMBIE_SB_COLUMNS; i++) {
		s32 textwidth;
		s32 textheight;

		textMeasure(&textheight, &textwidth, (char *)headings[i], xschars, xsfont, 0);
		gdl = zombiesDrawTextBL(gdl, (char *)headings[i], colx[i] + (colx[i + 1] - colx[i] - textwidth) / 2,
				top + headh - 3 * s, zombiesAlphaOf(0xd0d8e0ff, alpha), xschars, xsfont);
	}

	y = top + headh + 3 * s;

	for (i = 0; i < count; i++) {
		s32 p = order[i];
		bool me = p == g_Vars.currentplayernum;
		s32 namebottom = y + rowh - (rowh - line) / 2 - s;
		s32 nameleft = left + rowh + 4 * s;
		s32 values[ZOMBIE_SB_COLUMNS];

		values[0] = zombiesGetPoints(NULL, p);
		values[1] = g_ZombieStats[p].kills;
		values[2] = g_ZombieStats[p].downs;
		values[3] = g_ZombieStats[p].revives;
		values[4] = g_ZombieStats[p].headshots;

		// the name's bar, gold edged for your own
		if (me) {
			gdl = zombiesFillRect(gdl, nameleft - s, y - s, colleft - 2 * s + s, y + rowh + s, zombiesAlphaOf(0xe0a020ff, alpha));
			gdl = zombiesFillRect(gdl, nameleft, y, colleft - 2 * s, y + rowh, zombiesAlphaOf(0x60380ce0, alpha));
		} else {
			gdl = zombiesFillRect(gdl, nameleft, y, colleft - 2 * s, y + rowh, zombiesAlphaOf(0x181c20c0, alpha));
		}

		zombiesTextOf(text, sizeof(text), g_PlayerConfigsArray[g_Vars.playerstats[p].mpindex].base.name);
		gdl = zombiesDrawTextBL(gdl, text, nameleft + 6 * s, namebottom,
				zombiesAlphaOf(me ? 0xffe080ff : 0xffffffff, alpha), chars, font);

		// the columns, in Black Ops' blue, a shade apart each
		for (j = 0; j < ZOMBIE_SB_COLUMNS; j++) {
			s32 textwidth;
			s32 textheight;
			s32 cl = colx[j];
			s32 colw = colx[j + 1] - colx[j];

			gdl = zombiesFillRect(gdl, cl + s, y, cl + colw - s, y + rowh,
					zombiesAlphaOf((j & 1) ? 0x1c4c8cc0 : 0x245aa0c0, alpha));

			sprintf(text, "%d", values[j]);
			textMeasure(&textheight, &textwidth, text, chars, font, 0);
			gdl = zombiesDrawTextBL(gdl, text, cl + (colw - textwidth) / 2, namebottom,
					zombiesAlphaOf(0xffffffff, alpha), chars, font);
		}

		y += rowh + 3 * s;
	}

	gdl = text0f153780(gdl);

	// each row's head, left of its name (zombiesRenderPortrait(), centred)
#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);

	if (alpha >= 1) {
		y = top + headh + 3 * s;
		g_ZombiePortraitCentred = true;

		for (i = 0; i < count; i++) {
			s32 p = order[i];

			if (!g_Vars.players[p]->isdead) {
				zombiesRenderPortrait(&gdl, p, left + rowh, y + rowh, rowh,
						g_ZombieDowned[p] ? 0xa00000b0 : 0x00000080);
			}

			y += rowh + 3 * s;
		}

		g_ZombiePortraitCentred = false;
	}
#endif

	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);

	return gdl;
}

// The scoreboard button held, by the current player, in play: the scoreboard
// over the view and nothing else of the HUD; the game goes on, alone or not
static bool zombiesScoreboardHeld(void)
{
	static s32 always = -1; // --zombies-scoreboard: held all the time, for its pictures
	s32 me = g_Vars.currentplayernum;

	if (always < 0) {
		always = sysArgGetInt("--zombies-scoreboard", 0) ? 1 : 0;
	}

	if (!zombiesIsActive() || me < 0 || me >= MAX_PLAYERS || g_ZombieGameOver
			|| g_Menus[g_Vars.currentplayerstats->mpindex].curdialog) {
		return false;
	}

	return always || joyGetButtons(optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex), BUTTON_SCOREBOARD) != 0;
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

	// over the arena, the board alone; falling, GAME OVER goes over the HUD
	// at the end of this
	if (zombiesGameOverHidesHud()) {
		return zombiesRenderGameOver(gdl);
	}

	// the scoreboard button: the board, and none of the rest
	if (zombiesScoreboardHeld()) {
		return zombiesRenderScoreboard(gdl, 1, viGetViewTop() + viGetViewHeight() * 26 / 100);
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
	if (g_ZombieGameOver) {
		return zombiesRenderGameOver(gdl);
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
#define ZOMBIE_NET_VERSION 7 // 7 (protocol 40): each zombie's gibs (an arm, the head); 6 (protocol 39): the crawlers, and each barrier's zombie at work (its tear, pull, climb and reach); 5 (protocol 38): each player's scoreboard (kills, downs, revives, headshots); 4 (protocol 36): Fire Sale's time, each player's grenades and throw; 3 (protocol 35): nine perks (a two-byte mask), three more machines; 2 (34): the doors, the barriers' boards

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
#define ZOMBIE_NET_PLAYERS_AT 297 // 285 + 4 a machine for Deadshot, PhD and Electric Cherry
#define ZOMBIE_NET_PLAYER_BYTES 42 // zombiesNetReadPlayer()'s: 38 with six perks, a byte of mask
#define ZOMBIE_NET_DOORS_AT   (ZOMBIE_NET_PLAYERS_AT + ZOMBIE_NET_PLAYER_BYTES * MAX_PLAYERS) // past the players

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
	pl->perks = znGet(b, 2);
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
static u32 g_ZombieResumeDoors = 0;
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

	// the doors bought: the same doors here, made the same way
	b.at = ZOMBIE_NET_DOORS_AT;
	g_ZombieResumeDoors = znGet(&b, 2);

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

	for (k = 0; k < g_ZombieNumDoors; k++) {
		if (g_ZombieResumeDoors & (1 << k)) {
			zombiesDoorOpen(k);
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

	for (i = 0; i < ARRAYCOUNT(g_ZombieBarriers); i++) {
		if (obj == &g_ZombieBarriers[i].obj) {
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
	znPut(&b, znClamp(g_ZombieFireSale60, 0, 0xffff), 2); // (version 4)
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
		znPut(&b, perks, 2);
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

	// (version 2) the doors bought, the barriers' boards, and who is at a
	// door or a barrier (their prompts)
	if (b.at != ZOMBIE_NET_DOORS_AT) {
		static bool warned = false;

		if (!warned) {
			warned = true;
			sysLogPrintf(LOG_WARNING, "zombies: the online block's doors start at %d, not ZOMBIE_NET_DOORS_AT %d", b.at, ZOMBIE_NET_DOORS_AT);
		}
	}

	mask = 0;

	for (k = 0; k < g_ZombieNumDoors; k++) {
		if (g_ZombieDoors[k].open) {
			mask |= 1 << k;
		}
	}

	znPut(&b, mask, 2);
	znPut(&b, g_ZombieNumBarriers, 1);

	for (k = 0; k < ZOMBIE_BARRIER_MAX; k++) {
		znPut(&b, k < g_ZombieNumBarriers ? g_ZombieBarriers[k].boards : 0, 1);
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		const bool in = i < PLAYERCOUNT() && g_Vars.players[i] != NULL;

		znPut(&b, (u8)(s8)(in ? znClamp(g_ZombieNearDoor[i], -1, 0x7f) : -1), 1);
		znPut(&b, (u8)(s8)(in ? znClamp(g_ZombieNearBarrier[i], -1, 0x7f) : -1), 1);
	}

	// (version 4) each player's grenades, and its throw's stage and 60ths
	for (i = 0; i < MAX_PLAYERS; i++) {
		znPut(&b, znClamp(g_ZombieGrenades[i], 0, ZOMBIE_GRENADES_MAX), 1);
		znPut(&b, znClamp(g_ZombieThrowStage[i], 0, ZTHROW_UP), 1);
		znPut(&b, znClamp(g_ZombieThrow60[i], 0, 0xff), 1);
	}

	// (version 5) each player's scoreboard: kills, downs, revives, headshots
	for (i = 0; i < MAX_PLAYERS; i++) {
		znPut(&b, g_ZombieStats[i].kills, 2);
		znPut(&b, g_ZombieStats[i].downs, 2);
		znPut(&b, g_ZombieStats[i].revives, 2);
		znPut(&b, g_ZombieStats[i].headshots, 2);
	}

	// (version 6) the crawlers, a bit a zombie, and each barrier's zombie at
	// work: which, its tear's clock, the board it pulls, its climb, its reach
	for (i = 0; i < MAX_BOTS; i += 8) {
		u32 bits = 0;

		for (k = 0; k < 8 && i + k < MAX_BOTS; k++) {
			bits |= g_ZombieCrawler[i + k] ? 1u << k : 0;
		}

		znPut(&b, bits, 1);
	}

	for (k = 0; k < ZOMBIE_BARRIER_MAX; k++) {
		const struct zombiebarrier *bar = &g_ZombieBarriers[k];
		const bool in = k < g_ZombieNumBarriers;

		znPut(&b, (u8)(s8)(in ? znClamp(bar->zombie, -1, 0x7f) : -1), 1);
		znPut(&b, in ? znClamp(bar->tear60, 0, 0xffff) : 0, 2);
		znPut(&b, (u8)(s8)(in ? znClamp(bar->pulling, -1, 0x7f) : -1), 1);
		znPut(&b, (u16)(s16)(in ? znClamp(bar->enter60, -1, 0x7fff) : -1), 2);
		znPut(&b, (u16)(s16)(in ? znClamp(bar->reach60, -1, 0x7fff) : -1), 2);
		znPut(&b, in && bar->reachleft ? 1 : 0, 1);
	}

	// (version 7) what has been shot off each zombie, a byte each (ZGIB_*)
	for (i = 0; i < MAX_BOTS; i++) {
		znPut(&b, g_ZombieGib[i], 1);
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
	// one of Black Ops II's (zmsfx.c) travels as its slot: this machine's number for it
	const s32 sound = zmSfxResolve(s->sound);

	if (sound == 0 || (s->who >= 0 && (s->who != g_NetLocalSlot || s->mine))) {
		return;
	}

	g_ZombieNetSoundsTaken++;

	if (s->chr != 0xff) {
		const s32 index = func0f18d0e8(s->chr);

		if (index >= 0 && index < g_MpNumChrs && g_MpAllChrPtrs[index] && g_MpAllChrPtrs[index]->prop) {
			psCreate(NULL, g_MpAllChrPtrs[index]->prop, sound, -1, -1, 0, 0, PSTYPE_NONE, NULL, pitch, NULL, -1, -1, -1, -1);
		}

		return;
	}

	zombiesPlaySoundHere(sound, pitch);
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
	const s32 wasround = g_ZombieRound;
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
	g_ZombieFireSale60 = znGet(&b, 2);
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

	// (version 2) the doors bought (zombiesClientTick() opens them here), the
	// barriers' boards, each player's door and barrier
	g_ZombieNetDoorMask = znGet(&b, 2);
	v = znGet(&b, 1);

	for (k = 0; k < ZOMBIE_BARRIER_MAX; k++) {
		const s32 boards = znGet(&b, 1);

		// the boards that went or came, falling and coming in here too
		if (k < v && k < g_ZombieNumBarriers && g_ZombieBarriers[k].placed && !final) {
			zombiesBarrierSet(k, boards);
		}
	}

	for (i = 0; i < MAX_PLAYERS; i++) {
		const s32 neardoor = (s8)znGet(&b, 1);
		const s32 nearbarrier = (s8)znGet(&b, 1);

		g_ZombieNearDoor[i] = neardoor >= 0 && neardoor < g_ZombieNumDoors ? neardoor : -1;
		g_ZombieNearBarrier[i] = nearbarrier >= 0 && nearbarrier < g_ZombieNumBarriers ? nearbarrier : -1;
	}

	// (version 4) each player's grenades, and its throw's stage and 60ths
	for (i = 0; i < MAX_PLAYERS; i++) {
		const s32 grenades = znGet(&b, 1);
		const s32 stage = znClamp(znGet(&b, 1), 0, ZTHROW_UP);
		const s32 t = znGet(&b, 1);

		g_ZombieGrenades[i] = znClamp(grenades, 0, ZOMBIE_GRENADES_MAX);

		// the others' throws, for their bodies; this machine's is its own
		if (i != g_NetLocalSlot) {
			g_ZombieThrowStage[i] = stage;
			g_ZombieThrow60[i] = t;
		}
	}

	// (version 5) the scoreboard's, the host's
	for (i = 0; i < MAX_PLAYERS; i++) {
		g_ZombieStats[i].kills = znGet(&b, 2);
		g_ZombieStats[i].downs = znGet(&b, 2);
		g_ZombieStats[i].revives = znGet(&b, 2);
		g_ZombieStats[i].headshots = znGet(&b, 2);
	}

	// (version 6) the crawlers, and the barriers' zombies at work: drawn here
	// as the host's are (zombiesCodBarrier())
	for (i = 0; i < MAX_BOTS; i += 8) {
		const u32 bits = znGet(&b, 1);
		s32 k2;

		for (k2 = 0; k2 < 8 && i + k2 < MAX_BOTS; k2++) {
			g_ZombieCrawler[i + k2] = (bits >> k2) & 1;
		}
	}

	for (i = 0; i < MAX_BOTS; i++) {
		g_ZombieAtBarrier[i] = -1;
	}

	for (i = 0; i < ZOMBIE_BARRIER_MAX; i++) {
		const s32 zombie = (s8)znGet(&b, 1);
		const s32 tear60 = znGet(&b, 2);
		const s32 pulling = (s8)znGet(&b, 1);
		const s32 enter60 = (s16)znGet(&b, 2);
		const s32 reach60 = (s16)znGet(&b, 2);
		const bool reachleft = znGet(&b, 1) != 0;

		if (i < g_ZombieNumBarriers) {
			struct zombiebarrier *bar = &g_ZombieBarriers[i];

			bar->zombie = zombie >= 0 && zombie < MAX_BOTS ? zombie : -1;
			bar->tear60 = tear60;
			bar->pulling = pulling >= 0 && pulling < ZOMBIE_BARRIER_BOARDS ? pulling : -1;
			bar->enter60 = enter60;
			bar->reach60 = reach60;
			bar->reachleft = reachleft;

			if (bar->zombie >= 0) {
				g_ZombieAtBarrier[bar->zombie] = i;
			}
		}
	}

	// (version 7) the gibs, the host's: this machine throws its own (zombiesCodGibFresh())
	for (i = 0; i < MAX_BOTS; i++) {
		g_ZombieGib[i] = znGet(&b, 1) & (ZGIB_LARM | ZGIB_RARM | ZGIB_HEAD);
	}

	// the round cleared: its sting, as the host's (zombiesTick())
	if (wasnext < 0 && g_ZombieNextWaveFrame >= 0 && !final && g_ZombieRound > 0) {
		zombiesRoundSting();
	}

	// and a round begun: its music
	if (g_ZombieRound > wasround && !final) {
		zombiesRoundStartSting();
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
			zombiesGameOverMusic();
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
		zombiesResetBarriers();

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
		g_ZombieNetDoorMask = 0;
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
		zombiesPlaceBarriers();
	}

	// the sides, as the host's (the radar's and the names' colours)
	zombiesRayGunSounds();

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

	// each machine's own sounds (zombiesTick() has the host's)
	zombiesTickHums();
	zombiesTickLastStandSound();
	zombiesTickDropHums();

	// between the host's blocks its clocks run on here as they do there
	if (g_ZombieMaxAmmoBanner60 >= 0) {
		g_ZombieMaxAmmoBanner60 += adv;

		if (g_ZombieMaxAmmoBanner60 > ZOMBIE_DROP_BANNER60) {
			g_ZombieMaxAmmoBanner60 = -1;
		}
	}

	g_ZombieInstaKill60 = g_ZombieInstaKill60 > adv ? g_ZombieInstaKill60 - adv : 0;
	g_ZombieDoublePoints60 = g_ZombieDoublePoints60 > adv ? g_ZombieDoublePoints60 - adv : 0;
	g_ZombieFireSale60 = g_ZombieFireSale60 > adv ? g_ZombieFireSale60 - adv : 0;

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

	// the doors bought: unlocked here too (the doors themselves are the
	// host's, moved by its records), and the boards falling and coming
	for (k = 0; k < g_ZombieNumDoors; k++) {
		if ((g_ZombieNetDoorMask & (1 << k)) && !g_ZombieDoors[k].open) {
			zombiesDoorOpen(k);
		}
	}

	zombiesBarrierAnimate();

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
		zombiesTickThrow(me);
	}

	// back in at the next round: no longer watching anyone. Offline the
	// round's turn (g_ZombieRespawnDue) ends it while still dead; a client
	// can be stood up by the host before any block of its has said so
	if (me >= 0 && me < PLAYERCOUNT() && g_Vars.players[me] && !g_Vars.players[me]->isdead) {
		g_ZombieSpectating[me] = false;
	}
}

/**
 * The Zombies Options page, Game Setup's Options for Zombies (below), and the
 * Zombies Controls page, on Player Setup.
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

// Perk Limit: 1 to 99 at once (there are nine perks, so nine and over is all of them)
MenuItemHandlerResult menuhandlerZombiesPerkLimit(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (g_ZombieCfgPerkLimit > 1 ? g_ZombieCfgPerkLimit : 1) - 1;
		break;
	case MENUOP_SET:
		g_ZombieCfgPerkLimit = data->slider.value + 1;
		break;
	case MENUOP_GETSLIDERLABEL:
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
 * secondary function, Shift and the left stick to sprint, C and B to crouch,
 * G and the right bumper to throw a grenade, E and the right stick to knife.
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
	ZOMBIE_LAYOUT_ROW("Sprint\n", 7),
	ZOMBIE_LAYOUT_ROW("Crouch\n", 8),
	ZOMBIE_LAYOUT_ROW("Grenade\n", 9),
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

/**
 * Game Setup's Options for Zombies: the two of the Combat Simulator's own
 * rules that mean something here (the radar, auto-aim), then Zombies' own,
 * the host's to set (zombieperks.c keeps them in pd.ini). Each player's own
 * Zombies Controls are on Player Setup.
 */
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
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Perk Limit\n",
		98, // 1 to 99
		menuhandlerZombiesPerkLimit,
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
	(uintptr_t)"Settings",
	g_ZombiesOptionsMenuItems,
	mpOptionsMenuDialog,
	MENUDIALOGFLAG_MPLOCKABLE | MENUDIALOGFLAG_LITERAL_TEXT,
	&g_ExtGameOptionsMenuDialog,
};

/**
 * Start in a Zombies match (mpPushPauseDialog(), ingame.c), as Black Ops II's
 * pause: ZOMBIES over Resume Game, Options and End Game, and nothing of the
 * Combat Simulator's pause pages (its rankings, stats, inventory, control).
 * The match stops while a player alone has a menu up (mpIsPaused()), and
 * goes on under it with anyone else in. The scoreboard is the scoreboard
 * button's, in play (zombiesScoreboardHeld()).
 *
 * Options is OPTIONS over Settings (the Perfect Menu's Settings pages) and
 * Controls (Settings' Control page: the keys, the mouse, the controller).
 */
extern struct menudialogdef g_CiOptionsViaPauseMenuDialog;
extern struct menudialogdef g_CiControlOptionsMenuDialog;
void netClientLeave(void);

// End Game, sure: a client leaves the room; anyone else ends it as Zombies'
// own end does, the fall and the scoreboard (zombiesStartGameOver())
static MenuItemHandlerResult menuhandlerZombiesEndGame(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		if (NET_CLIENT) {
			netClientLeave();
			return 0;
		}

		func0f0f8120();

		if (!g_ZombieGameOver && g_ZombieRound > 0) {
			// on their feet: GAME OVER, then the scoreboard, with no fall
			g_ZombieGameOverQuit = true;
			zombiesStartGameOver();
		} else if (!g_ZombieGameOver) {
			g_Vars.currentplayer->aborted = true;
			mainEndStage();
		}
	}

	return 0;
}

struct menuitem g_ZombiesEndGameMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LESSHEIGHT,
		L_MPMENU_291, // "Are you sure?"
		0,
		NULL,
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
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_SELECTABLE_CENTRE,
		L_MPMENU_292, // "Cancel"
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE,
		L_MPMENU_293, // "End Game"
		0,
		menuhandlerZombiesEndGame,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ZombiesEndGameMenuDialog = {
	MENUDIALOGTYPE_DANGER,
	L_MPMENU_287, // "End Game"
	g_ZombiesEndGameMenuItems,
	NULL,
	0,
	NULL,
};

struct menuitem g_ZombiesPauseOptionsMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Settings",
		0,
		(void *)&g_CiOptionsViaPauseMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Controls",
		0,
		(void *)&g_CiControlOptionsMenuDialog,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ZombiesPauseOptionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"OPTIONS",
	g_ZombiesPauseOptionsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_ZombiesPauseMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Resume Game",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Options",
		0,
		(void *)&g_ZombiesPauseOptionsMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_BIGFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"End Game",
		0,
		(void *)&g_ZombiesEndGameMenuDialog,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ZombiesPauseMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"ZOMBIES",
	g_ZombiesPauseMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
