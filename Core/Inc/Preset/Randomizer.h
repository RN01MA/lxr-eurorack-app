/*
 * Randomizer.h
 *
 * Built-in kit randomizer (SHIFT+PERFORM mode).
 * Generates a whole kit from per-voice sound archetypes ("recipes") into a
 * parameter array (normally activeParameters), which the caller then applies
 * with preset_sendDrumsetParameters().
 *
 * The generator itself is pure: no HAL, no globals except the read-only
 * modTarget lookup, the random seed is passed in (TRNG on the module).
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#ifndef INC_PRESET_RANDOMIZER_H_
#define INC_PRESET_RANDOMIZER_H_

#include <stdint.h>

// TYP
#define RND_TYPE_CLS		0	// classic techno: fixed roles, tight spread
#define RND_TYPE_EXP		1	// experimental: synth archetypes, wider spread
#define RND_TYPE_GLT		2	// glitch / microsound: sine ticks, clicks, crushed noise

// Hz (spectral slotting)
#define RND_HZ_OFF			0	// raw archetypes
#define RND_HZ_SOFT			1	// gentle: voices meet their band half way, light HP on noise voices
#define RND_HZ_HARD			2	// real band separation

// COA
#define RND_KEY_OFF			(-1)	// 0..11 = C..B

typedef struct RandomizerConfigStruct
{
	uint8_t type;		// RND_TYPE_CLS / RND_TYPE_EXP / RND_TYPE_GLT
	int8_t  key;		// RND_KEY_OFF or 0..11 (C..B)
	uint8_t mod;		// 1 = one LFO route per voice (free / LFO slots only), 0 = LFO routes
						// of the regenerated voices removed; cv1..cv5 / accent never touched
	uint8_t hz;			// RND_HZ_OFF / SOFT / HARD
	uint8_t lockMask;	// bit n set = voice n+1 left untouched (not rolled)
} RandomizerConfig;

// archetype ids (result / display)
enum RndArchIds
{
	RND_A_KICK, RND_A_BASS, RND_A_FM, RND_A_TOM, RND_A_CONGA, RND_A_RIM,
	RND_A_PERC, RND_A_BELL, RND_A_STAB, RND_A_SNARE, RND_A_CLAP, RND_A_SHAKER,
	RND_A_HIHAT, RND_A_CYMBAL, RND_A_RIDE, RND_A_NOISE,
	RND_A_REESE, RND_A_FOLD, RND_A_WT, RND_A_ACID, RND_A_GHOST,
	// GLT
	RND_A_TICK, RND_A_DKICK, RND_A_SUB, RND_A_BLIP, RND_A_POP, RND_A_DATA,
	RND_A_CLICK, RND_A_ZAP, RND_A_BIT, RND_A_STUTTER, RND_A_METAL, RND_A_HISS, RND_A_TONE,
	RND_A_SWEEP, RND_A_REV, RND_A_HUM, RND_A_BLASTER,
	RND_A_COUNT,
	RND_A_LOCKED = 0xff
};

typedef struct RandomizerResultStruct
{
	uint8_t arch[6];	// archetype per voice, RND_A_LOCKED for locked voices
	int8_t  lfoSlot[6];	// CV matrix slot written for the voice, -1 = none
} RandomizerResult;

/* Generate a kit into p (indexed by PAR_xxx, at least END_OF_SOUND_PARAMETERS).
 * Only the sound parameters of unlocked voices (and, with mod on, their LFO
 * settings and one CV-matrix slot per voice) are written; GLT also sets the
 * pans of the rolled voices (MOD on: spread, MOD off: middle). FX, routing,
 * pans (CLS / EXP), velocity and every other parameter keep their value in p.
 * seed: any value (0 is remapped), e.g. from the hardware TRNG.
 * res may be NULL. */
void Randomizer_generate(uint8_t* p, const RandomizerConfig* cfg, uint32_t seed, RandomizerResult* res);

// 3 character display name of an archetype id
const char* Randomizer_archName(uint8_t archId);

// filter cutoff byte <-> Hz (voice SVF formula), exposed for tests
float Randomizer_filterHz(uint8_t byte);
uint8_t Randomizer_filterByte(float hz);

#endif /* INC_PRESET_RANDOMIZER_H_ */
