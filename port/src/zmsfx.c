/**
 * Black Ops II's sounds in Zombies (zmsfx.h).
 *
 * A .zsn (tools/zombies/sndbake.py) is mono 16-bit samples at the synth's
 * own rate (22020, audio.c), little endian. A raw wave is read big endian, as
 * the cartridge's are (n_load.c, n_alRaw16Pull()), so they are swapped as they
 * load: read as they are, a voice came out as hiss, like a television between
 * channels. Its pitch is 1 at key base 60 (n_sndplayer.c:
 * alCents2Ratio(keyBase * 100 - 6000)).
 *
 * Each is made a sound of the game's bank the way an appended GoldenEye one
 * is (gesfx.c): its ALSound, envelope, key map and wave table in a buffer of
 * ours, every pointer in them an offset from the sfx ctl segment's start
 * (the wave's base from the tbl's), and sndAppendSound() given the ALSound's.
 * Perfect Dark's key map is more than a key map: its keyMin's low bits pick a
 * volume group and its high ones a chained sound, velocityMin the chain's
 * number, keyMax a flag nibble and an effects send. So the key map and the
 * envelope are a gun's own, the Phoenix shot's (SFX_8072), with no chain,
 * the key base 60 and the envelope as long as the sample.
 */
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include <PR/libaudio.h>
#include "constants.h"
#include "types.h"
#include "fs.h"
#include "system.h"
#include "zmsfx.h"
#include "lib/audiodma.h"
#include "lib/snd.h"

static const char *const g_ZmSfxFixed[ZMSFX_FIXED_COUNT] = {
	"raygun_fire",
	"raygun_fire_npc",
	"raygun_exp",
	"raygun_reload_open",
	"raygun_reload_out",
	"raygun_reload_in",
	"raygun_reload_close",
	"cherry_explode",
	"perk_buy",
	"perk_dispense",
	"perk_deny",
	"perk_open",
	"perk_swallow",
	"perk_break",
	"perk_belch",
	"perk_hum",
	"pap_hum",
	"box_open",
	"box_close",
	"box_music",
	"box_spin",
	"box_poof",
	"box_land",
	"box_move",
	"buy_deny",
	"wall_buy",
	"pap_upgrade",
	"pap_ready",
	"pap_deny",
	"pap_crunch",
	"pap_knuckle",
	"pu_spawn",
	"pu_grab",
	"pu_maxammo",
	"pu_instakill",
	"pu_nuke",
	"pu_carpenter",
	"pu_dblpoints_off",
	// zombies.inc's power-ups' order
	"ann_maxammo",
	"ann_instakill",
	"ann_doublepoints",
	"ann_nuke",
	"ann_carpenter",
	"ann_firesale",
	"round_start",
	"round_over",
	"laststand",
	"final_hit",
	"swiped_0",
	"swiped_1",
	"swiped_2",
	"pu_loop",
	"game_over",
	"tinnitus",
#define ZMSFX_PERKS(kind) \
	kind "_juggernog", kind "_quickrevive", kind "_speedcola", kind "_doubletap", kind "_staminup", \
	kind "_mulekick", kind "_deadshot", kind "_phdflopper", kind "_electriccherry"
	ZMSFX_PERKS("sting"),
	ZMSFX_PERKS("jingle"),
#undef ZMSFX_PERKS
};

static const char *const g_ZmVoxKinds[ZMVOX_COUNT] = {
	"amb", "attack", "behind", "crawl", "death", "sprint", "taunt",
};

// one sound's bank data, each part on its own 16 bytes and room to spare:
// the loaders read 0x40 of an ALSound and 0x80 of a wave table
struct zmsfxbank {
	ALSound sound;
	u8 pad0[0x40];
	ALEnvelope envelope;
	u8 pad1[0x40];
	ALKeyMap keymap;
	u8 pad2[0x40];
	ALWaveTable wave;
	u8 pad3[0x80];
	ALRawLoop loop;   // a hum's: the whole sample, for ever
	u8 pad4[0x40];
};

// A machine's hum goes round and round until it is stopped
static bool zmSfxLoops(s32 slot)
{
	return slot == ZMSFX_PERK_HUM || slot == ZMSFX_PAP_HUM || slot == ZMSFX_LASTSTAND || slot == ZMSFX_PU_LOOP
		|| slot == ZMSFX_TINNITUS;
}

static s16 g_ZmSfxSound[ZMSFX_COUNT];   // the game's number, 0 not yet, -1 not there
static s8 g_ZmVoxCount[ZMVOX_COUNT];    // how many of each voice there are, -1 not counted

static void zmSfxName(s32 slot, char *out, s32 size)
{
	if (slot < ZMSFX_FIXED_COUNT) {
		snprintf(out, size, "%s", g_ZmSfxFixed[slot]);
	} else {
		const s32 v = slot - ZMSFX_FIXED_COUNT;

		snprintf(out, size, "zmb_%s_%d", g_ZmVoxKinds[v / ZMSFX_VARIANTS], v % ZMSFX_VARIANTS);
	}
}

static s32 zmSfxLoad(s32 slot)
{
	char name[48];
	char rel[96];
	u32 size = 0;
	u8 *data;
	u32 rate;
	u32 count;
	u8 *samples;
	struct zmsfxbank *b;
	ALSound *ref;
	s32 id;

	zmSfxName(slot, name, sizeof(name));
	snprintf(rel, sizeof(rel), "$E/zombies/sound/%s.zsn", name);

	if (fsFileSize(fsFullPath(rel)) <= 12 || (data = fsFileLoad(rel, &size)) == NULL) {
		return -1;
	}

	if (size < 12 || memcmp(data, "ZSN1", 4) != 0) {
		sysMemFree(data);
		return -1;
	}

	memcpy(&rate, data + 4, 4);
	memcpy(&count, data + 8, 4);

	if (count == 0 || size < 12 + count * 2) {
		sysMemFree(data);
		return -1;
	}

	// the gun's own key map and envelope to start from
	ref = sndLoadSound(SFX_8072);

	if (ref == NULL || ref->keyMap == NULL || ref->envelope == NULL) {
		sysMemFree(data);
		return -1;
	}

	b = sysMemZeroAlloc(sizeof(*b));
	// padded: the sound DMA reads a whole item from where a sample starts
	samples = sysMemZeroAlloc(count * 2 + ADMA_ITEM_SIZE);

	if (b == NULL || samples == NULL) {
		sysMemFree(b);
		sysMemFree(samples);
		sysMemFree(data);
		return -1;
	}

	// big endian, as n_alRaw16Pull() reads a raw wave
	for (u32 i = 0; i < count; i++) {
		samples[i * 2] = data[12 + i * 2 + 1];
		samples[i * 2 + 1] = data[12 + i * 2];
	}

	sysMemFree(data);

	b->envelope = *ref->envelope;
	b->envelope.attackTime = 0;
	b->envelope.decayTime = (ALMicroTime)((u64)count * 1000000 / (rate ? rate : 22020));
	b->envelope.attackVolume = 127;
	b->envelope.decayVolume = 127;

	b->keymap = *ref->keyMap;
	b->keymap.velocityMin = 0;    // no chained sound
	b->keymap.keyMin &= ~0xc0;
	b->keymap.keyBase = 60;
	b->keymap.detune = 0;

	b->wave.base = (u8 *)((uintptr_t)samples - sndGetTblStart());
	b->wave.len = count * 2;
	b->wave.type = AL_RAW16_WAVE;
	b->wave.flags = 0;
	b->wave.waveInfo.rawWave.loop = NULL;

	// looped: the raw wave's loop (snd.c reads it in as the ADPCM one), and
	// an envelope that holds (a decay of -1, n_sndplayer.c's sustain)
	if (zmSfxLoops(slot)) {
		b->loop.start = 0;
		b->loop.end = count;
		b->loop.count = -1;
		b->wave.waveInfo.rawWave.loop = (ALRawLoop *)((uintptr_t)&b->loop - sndGetCtlStart());
		b->envelope.decayTime = -1;
	}

	b->sound = *ref;
	b->sound.envelope = (ALEnvelope *)((uintptr_t)&b->envelope - sndGetCtlStart());
	b->sound.keyMap = (ALKeyMap *)((uintptr_t)&b->keymap - sndGetCtlStart());
	b->sound.wavetable = (ALWaveTable *)((uintptr_t)&b->wave - sndGetCtlStart());
	b->sound.samplePan = AL_PAN_CENTER;

	// Call of Duty's voices are mastered loud: a crowd of them over the
	// music and the hits ran the mix into its ceiling
	if (slot >= ZMSFX_FIXED_COUNT) {
		b->sound.sampleVolume = b->sound.sampleVolume * 3 / 4;
	}

	id = sndAppendSound((uintptr_t)&b->sound - sndGetCtlStart());

	if (id <= 0) {
		// no bank (--no-sound), or no numbers left: kept, never played
		return -1;
	}

	sysLogPrintf(LOG_NOTE, "zmsfx: %s is sound %d (%u samples)", name, id, count);

	return id;
}

s32 zmSfxSound(s32 slot)
{
	if (slot < 0 || slot >= ZMSFX_COUNT) {
		return 0;
	}

	if (g_ZmSfxSound[slot] == 0) {
		const s32 id = zmSfxLoad(slot);

		g_ZmSfxSound[slot] = id > 0 ? id : -1;
	}

	return g_ZmSfxSound[slot] > 0 ? g_ZmSfxSound[slot] : 0;
}

bool zmSfxIsOurs(s32 sound)
{
	s32 slot;

	if (sound <= 0) {
		return false;
	}

	for (slot = 0; slot < ZMSFX_COUNT; slot++) {
		if (g_ZmSfxSound[slot] == sound) {
			return true;
		}
	}

	return false;
}

s32 zmSfxPickVox(s32 kind, u32 r)
{
	s32 n;

	if (kind < 0 || kind >= ZMVOX_COUNT) {
		return -1;
	}

	// counted once: the variants there are run from 0 with no gap
	if (g_ZmVoxCount[kind] == 0) {
		for (n = 0; n < ZMSFX_VARIANTS && zmSfxSound(ZMSFX_FIXED_COUNT + kind * ZMSFX_VARIANTS + n); n++);

		g_ZmVoxCount[kind] = n > 0 ? n : -1;
	}

	if (g_ZmVoxCount[kind] <= 0) {
		return -1;
	}

	return ZMSFX_FIXED_COUNT + kind * ZMSFX_VARIANTS + (s32)(r % (u32)g_ZmVoxCount[kind]);
}

s32 zmSfxResolve(s32 sound)
{
	if (ZMSFX_IS_NET(sound)) {
		return zmSfxSound(sound & ~ZMSFX_NET_FLAG);
	}

	return sound;
}
