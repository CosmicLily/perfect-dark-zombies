#ifndef _IN_ZMSFX_H
#define _IN_ZMSFX_H

#include <PR/ultratypes.h>

/**
 * Black Ops II's own sounds in Zombies (zmsfx.c): the Ray Gun's and the
 * zombies' voices, baked by tools/zombies/sndbake.py into the build's
 * zombies/sound/, each played through the game's own sound player as a raw
 * wave appended to its bank.
 *
 * A sound is named by its slot in a fixed table, the same on every machine;
 * online it travels as ZMSFX_NET(slot) in Zombies' sound ring, since an
 * appended sound's number is the order it was appended in.
 */
enum {
	ZMSFX_RAYGUN_FIRE,
	ZMSFX_RAYGUN_FIRE_NPC,
	ZMSFX_RAYGUN_EXP,
	ZMSFX_RAYGUN_RELOAD_OPEN,
	ZMSFX_RAYGUN_RELOAD_OUT,
	ZMSFX_RAYGUN_RELOAD_IN,
	ZMSFX_RAYGUN_RELOAD_CLOSE,
	ZMSFX_CHERRY_EXPLODE,      // Electric Cherry's shock (zmb_cherry_explode: the taser mine's)
	ZMSFX_PERK_BUY,            // zmb_cha_ching, a machine's purchase
	ZMSFX_PERK_DISPENSE,       // the bottle out of the machine
	ZMSFX_PERK_DENY,           // too few points
	ZMSFX_PERK_OPEN,           // the drink's: evt_perk_bottle_open,
	ZMSFX_PERK_SWALLOW,        // evt_perk_swallow,
	ZMSFX_PERK_BREAK,          // evt_bottle_break,
	ZMSFX_PERK_BELCH,          // evt_belch
	ZMSFX_PERK_HUM,            // a perk machine's hum (zmb_perks_machine_loop), looped
	ZMSFX_PAP_HUM,             // Pack-a-Punch's (zmb_perks_packa_loop), looped
	ZMSFX_BOX_OPEN,            // the Mystery Box: zmb_lid_open,
	ZMSFX_BOX_CLOSE,           // zmb_lid_close,
	ZMSFX_BOX_MUSIC,           // zmb_music_box,
	ZMSFX_BOX_SPIN,            // zmb_box_spin,
	ZMSFX_BOX_POOF,            // zmb_box_poof, zmb_box_poof_land, zmb_box_move: its going
	ZMSFX_BOX_LAND,
	ZMSFX_BOX_MOVE,
	ZMSFX_BUY_DENY,            // zmb_no_cha_ching: too few points
	ZMSFX_WALL_BUY,            // zmb_weap_wall: a gun off the wall
	ZMSFX_PAP_UPGRADE,         // Pack-a-Punch: zmb_perks_packa_upgrade,
	ZMSFX_PAP_READY,           // _ready,
	ZMSFX_PAP_DENY,            // _deny,
	ZMSFX_PAP_CRUNCH,          // _upgrade_crnch,
	ZMSFX_PAP_KNUCKLE,         // _knuckle_0
	ZMSFX_PU_SPAWN,            // power-ups: zmb_spawn_powerup,
	ZMSFX_PU_GRAB,             // zmb_powerup_grabbed,
	ZMSFX_PU_MAXAMMO,          // zmb_full_ammo,
	ZMSFX_PU_INSTAKILL,        // zmb_insta_kill,
	ZMSFX_PU_NUKE,             // evt_nuked,
	ZMSFX_PU_CARPENTER,        // evt_carpenter_end,
	ZMSFX_PU_DBLPOINTS_OFF,    // Double Points running out
	ZMSFX_ANNOUNCER,           // + a power-up (zombies.inc's ZOMBIEPOWERUP_*): the announcer's call,
	                           // Richtofen's (Tranzit's vox_zmba_powerup_*)
	ZMSFX_ANNOUNCER_END = ZMSFX_ANNOUNCER + 6 - 1,
	ZMSFX_ROUND_START,         // mus_zombie_round_start, Tranzit's (the global one)
	ZMSFX_ROUND_OVER,          // mus_zombie_round_over
	ZMSFX_LASTSTAND,           // evt_laststand_loop: down, bleeding out, looped
	ZMSFX_FINAL_HIT,           // evt_player_final_hit: the blow that downs
	ZMSFX_SWIPED,              // evt_player_swiped, one of three: a blow taken
	ZMSFX_SWIPED_END = ZMSFX_SWIPED + 3 - 1,
	ZMSFX_PU_LOOP,             // zmb_spawn_powerup_loop: a power-up lying there, looped
	ZMSFX_GAME_OVER,           // mus_zombie_game_over, Buried's "Dead or Undead" (Tranzit has none)
	ZMSFX_PERK_STING,          // + a perk (zombies.inc's ZOMBIEPERK_*): its sting as it is bought (mus_perks_*_sting)
	ZMSFX_PERK_JINGLE = ZMSFX_PERK_STING + 9, // + a perk: its machine's jingle (mus_perks_*_jingle)
	ZMSFX_FIXED_COUNT = ZMSFX_PERK_JINGLE + 9
};

// the zombies' voices, each kind ZMSFX_VARIANTS slots from ZMSFX_FIXED_COUNT
enum {
	ZMVOX_AMB,
	ZMVOX_ATTACK,
	ZMVOX_BEHIND,
	ZMVOX_CRAWL,
	ZMVOX_DEATH,
	ZMVOX_SPRINT,
	ZMVOX_TAUNT,
	ZMVOX_COUNT
};

#define ZMSFX_VARIANTS 16
#define ZMSFX_COUNT    (ZMSFX_FIXED_COUNT + ZMVOX_COUNT * ZMSFX_VARIANTS)

#define ZMSFX_NET_FLAG 0xf000
#define ZMSFX_NET(slot) (ZMSFX_NET_FLAG | (slot))
#define ZMSFX_IS_NET(sound) (((sound) & 0xf000) == ZMSFX_NET_FLAG)

// The game's sound number for a slot, loading it the first time; 0 when its
// file is not there (or there is no sound bank)
s32 zmSfxSound(s32 slot);

// One of a kind of voice that is there, at random (r any random number);
// -1 when there is none of that kind
s32 zmSfxPickVox(s32 kind, u32 r);

// A sound number as Zombies' ring carries it: ZMSFX_NET(slot) made the game's own
s32 zmSfxResolve(s32 sound);

// Whether the game's sound number is one of these (n_sndplayer.c: they play to
// their own length, Black Ops II's music running past the bank's 5.5 seconds)
bool zmSfxIsOurs(s32 sound);

#endif
