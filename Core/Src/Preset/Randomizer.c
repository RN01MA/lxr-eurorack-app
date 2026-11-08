/*
 * Randomizer.c
 *
 * Built-in kit randomizer, generator part (phase 1).
 *
 * How a kit is made, per unlocked voice:
 *   1. pick an archetype for the voice (TYP pool, weights shift with VAR)
 *   2. reset the voice to neutral engine defaults (init kit), so nothing of
 *      the previous sound leaks into the new one
 *   3. archetype recipe: every value is centre +- range * spread
 *   4. register / Hz band / key (COA) placement of the pitch
 *   5. musical clamps (kick protection, audible levels, filter not closed)
 * then the cross-voice logic (bass vs. kick) and, with MOD on, one LFO route
 * per voice in a CV slot that is free or already LFO driven.
 *
 * The recipes of CLS are the "techno" recipes of the desktop kit tool, byte
 * for byte (they were tuned by ear). The desktop tool computed pitch 3
 * semitones off and the filter linear; bands and key here use the real
 * firmware formulas, kept at the pitches heard with the tool.
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#include "Preset/Randomizer.h"
#include "Parameters.h"
#include "Preset/RandomizerLoudness.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

// menu/Cc2Text.c: sound parameter -> index into modTargets (CV_DEST values)
extern uint8_t paramToModTarget[END_OF_SOUND_PARAMETERS];

//---- random ---------------------------------------------------------------
typedef struct { uint32_t s; } Rng;

static uint32_t rng_next(Rng* r)
{
	uint32_t x = r->s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	r->s = x;
	return x;
}
static float rng_u(Rng* r)						// [0,1)
{
	return (float)(rng_next(r) >> 8) * (1.f/16777216.f);
}
static float rng_bi(Rng* r)						// [-1,1)
{
	return rng_u(r)*2.f - 1.f;
}
static int rng_int(Rng* r, int lo, int hi)		// lo..hi inclusive
{
	return lo + (int)(rng_u(r) * (float)(hi - lo + 1));
}
static void rng_seed(Rng* r, uint32_t seed)
{
	// murmur3 finaliser, spreads TRNG / counter seeds over all bits
	seed ^= seed >> 16; seed *= 0x85ebca6bu;
	seed ^= seed >> 13; seed *= 0xc2b2ae35u;
	seed ^= seed >> 16;
	r->s = seed ? seed : 0x6d2b79f5u;
	for(int i=0;i<4;i++) rng_next(r);
}

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

//---- engine constants -----------------------------------------------------
#define W_SIN	0
#define W_TRI	1
#define W_SAW	2
#define W_REC	3
#define W_NOI	4
#define W_PWM	5

#define FT_LP	0
#define FT_HP	1
#define FT_BP	2

// LFO waves (MENU_LFO_WAVES)
#define LW_SIN	0
#define LW_TRI	1
#define LW_SUP	2
#define LW_SDN	3
#define LW_RND	5
#define LW_XDN	7

#define CV_SRC_LFO1		5		// lf1..lf6 = 5..10, cv1..cv5 = 0..4, ac1..ac6 = 11..16
#define CV_SLOTS_PER_VOICE 3	// voice n owns CV slots 3(n-1) .. 3(n-1)+2 (its MOD page)
#define CV_AMT_MAX		100		// DTYPE_0B100

// spread (fraction of each recipe range used). CLS = the desktop tool default,
// EXP wider; EXP also widens the ranges themselves (x1.5) and picks the synth
// recipes more often
#define RND_SPREAD_CLS	0.55f
#define RND_TOO_QUIET_DB	4.0f	// re-roll a voice predicted > 4 dB under its target at VOL 127
#define RND_SPREAD_EXP	0.75f

//---- voice roles ----------------------------------------------------------
enum RndRoles
{
	R_WAVE, R_MODWAVE, R_MODWAVE2,
	R_COARSE, R_FINE,
	R_AEGA, R_AEGD, R_AEGDOPEN, R_VOLSLOPE,
	R_MODAMT, R_PITCHSLOPE, R_MODEG,
	R_FMAMT, R_FMFREQ, R_MIXMOD,
	R_MODF1, R_MODF2, R_MODG1, R_MODG2,
	R_FILTTYPE, R_FILTFREQ, R_RESO, R_FILTDRIVE,
	R_DRIVE,
	R_TRANSWAVE, R_TRANSVOL, R_TRANSFREQ,
	R_NOISEMIX, R_NOISEFREQ, R_REPEAT,
	R_PWM, R_VOL, R_DECIM,
	R_NUM
};

#define NA (-1)
// parameter of each role per voice (NA = engine has no such parameter)
static const int16_t rolePar[6][R_NUM] =
{
	{ PAR_OSC_WAVE_DRUM1, PAR_MOD_WAVE_DRUM1, NA,
	  PAR_COARSE1, PAR_FINE1,
	  PAR_VELOA1, PAR_VELOD1, NA, PAR_VOL_SLOPE1,
	  PAR_MODAMNT1, PAR_PITCH_SLOPE1, PAR_MOD_EG1,
	  PAR_FMAMNT1, PAR_FM_FREQ1, PAR_MIX_MOD_1,
	  NA, NA, NA, NA,
	  PAR_FILTER_TYPE_1, PAR_FILTER_FREQ_1, PAR_RESO_1, PAR_FILTER_DRIVE_1,
	  PAR_DRIVE1,
	  PAR_TRANS1_WAVE, PAR_TRANS1_VOL, PAR_TRANS1_FREQ,
	  NA, NA, NA,
	  PAR_PWM_1, PAR_VOL1, PAR_VOICE_DECIMATION1 },

	{ PAR_OSC_WAVE_DRUM2, PAR_MOD_WAVE_DRUM2, NA,
	  PAR_COARSE2, PAR_FINE2,
	  PAR_VELOA2, PAR_VELOD2, NA, PAR_VOL_SLOPE2,
	  PAR_MODAMNT2, PAR_PITCH_SLOPE2, PAR_MOD_EG2,
	  PAR_FMAMNT2, PAR_FM_FREQ2, PAR_MIX_MOD_2,
	  NA, NA, NA, NA,
	  PAR_FILTER_TYPE_2, PAR_FILTER_FREQ_2, PAR_RESO_2, PAR_FILTER_DRIVE_2,
	  PAR_DRIVE2,
	  PAR_TRANS2_WAVE, PAR_TRANS2_VOL, PAR_TRANS2_FREQ,
	  NA, NA, NA,
	  PAR_PWM_2, PAR_VOL2, PAR_VOICE_DECIMATION2 },

	{ PAR_OSC_WAVE_DRUM3, PAR_MOD_WAVE_DRUM3, NA,
	  PAR_COARSE3, PAR_FINE3,
	  PAR_VELOA3, PAR_VELOD3, NA, PAR_VOL_SLOPE3,
	  PAR_MODAMNT3, PAR_PITCH_SLOPE3, PAR_MOD_EG3,
	  PAR_FMAMNT3, PAR_FM_FREQ3, PAR_MIX_MOD_3,
	  NA, NA, NA, NA,
	  PAR_FILTER_TYPE_3, PAR_FILTER_FREQ_3, PAR_RESO_3, PAR_FILTER_DRIVE_3,
	  PAR_DRIVE3,
	  PAR_TRANS3_WAVE, PAR_TRANS3_VOL, PAR_TRANS3_FREQ,
	  NA, NA, NA,
	  PAR_PWM_3, PAR_VOL3, PAR_VOICE_DECIMATION3 },

	{ PAR_OSC_WAVE_SNARE, NA, NA,
	  PAR_COARSE4, PAR_FINE4,
	  PAR_VELOA4, PAR_VELOD4, NA, PAR_VOL_SLOPE4,
	  PAR_MODAMNT4, PAR_PITCH_SLOPE4, PAR_MOD_EG4,
	  NA, NA, NA,
	  NA, NA, NA, NA,
	  PAR_FILTER_TYPE_4, PAR_FILTER_FREQ_4, PAR_RESO_4, PAR_FILTER_DRIVE_4,
	  PAR_SNARE_DISTORTION,
	  PAR_TRANS4_WAVE, PAR_TRANS4_VOL, PAR_TRANS4_FREQ,
	  PAR_MIX1, PAR_NOISE_FREQ1, PAR_REPEAT4,
	  PAR_PWM_4, PAR_VOL4, PAR_VOICE_DECIMATION4 },

	{ PAR_WAVE1_CYM, PAR_WAVE2_CYM, PAR_WAVE3_CYM,
	  PAR_COARSE5, PAR_FINE5,
	  PAR_VELOA5, PAR_VELOD5, NA, PAR_VOL_SLOPE5,
	  NA, NA, NA,
	  NA, NA, NA,
	  PAR_MOD_OSC_F1_CYM, PAR_MOD_OSC_F2_CYM, PAR_MOD_OSC_GAIN1_CYM, PAR_MOD_OSC_GAIN2_CYM,
	  PAR_FILTER_TYPE_5, PAR_FILTER_FREQ_5, PAR_RESO_5, PAR_FILTER_DRIVE_5,
	  PAR_CYMBAL_DISTORTION,
	  PAR_TRANS5_WAVE, PAR_TRANS5_VOL, PAR_TRANS5_FREQ,
	  NA, NA, PAR_REPEAT5,
	  PAR_PWM_5, PAR_VOL5, PAR_VOICE_DECIMATION5 },

	{ PAR_WAVE1_HH, PAR_WAVE2_HH, PAR_WAVE3_HH,
	  PAR_COARSE6, PAR_FINE6,
	  PAR_VELOA6, PAR_VELOD6_CLOSED, PAR_VELOD6_OPEN, PAR_VOL_SLOPE6,
	  NA, NA, NA,
	  NA, NA, NA,
	  PAR_MOD_OSC_F1, PAR_MOD_OSC_F2, PAR_MOD_OSC_GAIN1, PAR_MOD_OSC_GAIN2,
	  PAR_FILTER_TYPE_6, PAR_FILTER_FREQ_6, PAR_RESO_6, PAR_FILTER_DRIVE_6,
	  PAR_HAT_DISTORTION,
	  PAR_TRANS6_WAVE, PAR_TRANS6_VOL, PAR_TRANS6_FREQ,
	  NA, NA, NA,
	  PAR_PWM_6, PAR_VOL6, PAR_VOICE_DECIMATION6 },
};

// neutral engine defaults = init kit (what the desktop tool started from).
// Every role of a voice is reset to these before its recipe is applied.
static const uint8_t roleDefault[6][R_NUM] =
{
	//wav mw mw2 crs fin  aA  aD aDo vSl mAm pSl mEG fmA fmF mix mF1 mF2 mG1 mG2 fTy  fF rso fDr drv tWv tVo tFr nMx nFr rep pwm vol dec
	{  0,  0,  0, 31, 63,  0, 21,  0, 46, 44, 22, 26,  0,  0,  0,  0,  0,  0,  0,  0,127,  0,  0, 63,  0,  0,  0,  0,  0,  0,  0,127,127 },
	{  0,  0,  0, 41, 63,  0, 13,  0, 11, 14, 10, 93,  0,  0,  0,  0,  0,  0,  0,  0,127,  0,  0, 63,  0,  0,  0,  0,  0,  0,  0,127,127 },
	{  0,  0,  0, 58, 63,  0,  3,  0,  4,127, 14,  7,  0,  0,  0,  0,  0,  0,  0,  0,127,  0,  0, 63,  0,  0,  0,  0,  0,  0,  0,127,127 },
	{  1,  0,  0, 45, 64,  0, 10,  0, 10, 81, 15, 14,  0,  0,  0,  0,  0,  0,  0,  0,127,  0,  0, 63,  0,  0,  0, 61,127,  0,  0,127,127 },
	{  4,  0,  0,127, 63,  0,107,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1, 68,  0,  0, 63,  0,  0,  0,  0,  0,  0,  0,127,127 },
	{  0,  0,  0, 50, 63,  0,  5, 17,  6,  0,  0,  0,  0,  0,  0, 56,127,127,127,  1,104,  0,  0, 63,  0,  0,  0,  0,  0,  0,  0,127,127 },
};

//---- archetypes -----------------------------------------------------------
typedef struct { uint8_t c1; uint8_t r; } RndRange;	// c1 = centre+1, 0 = role not in recipe

#define RG(c,r)		{ (uint8_t)((c)+1), (uint8_t)(r) }
#define C1(x)		((uint8_t)((x)+1))			// choice lists: value+1, 0 terminates
#define IV(x)		((uint8_t)((x)+64))			// interval lists: semitones+64, 0 terminates
#define TR(x)		((uint16_t)(1u<<(x)))

#define MM_FM		1		// mixMod field: 0 = engine default
#define MM_MIX		2

// flags
#define F_KICK		0x01	// protected: narrow spread, own clamps, no LFO, no exotic picks
#define F_TONAL		0x02	// pitch follows COA (key)
#define F_BASSREG	0x04	// sits in the bass register: kick spacing, octave up on V3
#define F_NOTIGHT	0x08	// Hz slotting does not shorten the decay
#define F_NOFLIP	0x10	// mix/FM mode is part of the sound, never flipped
#define F_FIXWAVE	0x20	// no exotic osc wave substitution in EXP
#define F_FMHARM	0x40	// with COA: FM modulator snapped to a harmonic interval
#define F_KEYTRACK	0x80	// band-pass follows the note (else a fixed BP can miss every harmonic = silence)

// LFO profiles (MOD on)
#define LP_NONE		0
#define LP_DRIFT	1		// slow free running sin/tri: evolving over bars
#define LP_GROOVE	2		// beat locked: restarted by the kick
#define LP_ENV		3		// per hit envelope: falling saw restarted by the own voice
#define LP_SH		4		// sample & hold, free running

typedef struct RndArchStruct
{
	char name[4];
	uint8_t wave[4];		// osc wave choices (C1), weighted by repetition
	uint8_t modWave[3];		// mod osc / metal osc 2 wave choices (C1)
	uint8_t filt[3];		// filter type choices (C1)
	uint8_t mixMod;			// MM_FM / MM_MIX / 0
	uint8_t fmInt[4];		// FM modulator = coarse + interval (IV); empty = absolute FM_FREQ range
	uint8_t detMin, detMax;	// fine detune magnitude around 64 (reese)
	uint16_t trans;			// allowed transient waves, 0 = any of 0..11
	uint8_t flags;
	uint8_t bandLo, bandHi;	// pitch band (MIDI notes), 0 = none
	uint8_t hpSoft;			// Hz Sft: HP cutoff byte (desktop tool slotting)
	uint16_t hpHardHz;		// Hz Hrd: HP cutoff [Hz]; 0 = no HP slotting
	uint8_t lfoProf[2];		// [CLS, EXP]
	uint8_t lfoTgt[2];		// role modulated
	RndRange r[R_NUM];		// continuous roles: centre, half range (bytes)
} RndArch;

static const RndArch arch[RND_A_COUNT] =
{
	//------------------------------------------------ desktop tool recipes
	[RND_A_KICK] = { .name="KIK", .wave={C1(W_SIN)}, .filt={C1(FT_LP)}, .mixMod=MM_FM,
		.trans=TR(0)|TR(2)|TR(3)|TR(4)|TR(5)|TR(10),
		.flags=F_KICK|F_TONAL|F_NOTIGHT|F_NOFLIP|F_FIXWAVE, .bandLo=29, .bandHi=36,
		.lfoProf={LP_NONE,LP_NONE},
		.r={ [R_COARSE]=RG(32,7), [R_AEGA]=RG(0,3), [R_AEGD]=RG(28,16), [R_VOLSLOPE]=RG(25,14),
			 [R_MODAMT]=RG(85,30), [R_PITCHSLOPE]=RG(25,16), [R_MODEG]=RG(32,22),
			 [R_FILTFREQ]=RG(112,16), [R_RESO]=RG(10,16), [R_TRANSVOL]=RG(58,45), [R_TRANSFREQ]=RG(60,45),
			 [R_FMAMT]=RG(0,22), [R_VOL]=RG(115,12), [R_DECIM]=RG(122,6) } },

	[RND_A_TOM] = { .name="TOM", .wave={C1(W_SIN),C1(W_TRI)}, .filt={C1(FT_LP)},
		.flags=F_TONAL, .bandLo=50, .bandHi=63,
		.lfoProf={LP_DRIFT,LP_SH}, .lfoTgt={R_FILTFREQ,R_MODEG},
		.r={ [R_COARSE]=RG(52,16), [R_AEGA]=RG(0,3), [R_AEGD]=RG(28,8), [R_VOLSLOPE]=RG(16,10),
			 [R_MODAMT]=RG(55,28), [R_PITCHSLOPE]=RG(20,14), [R_MODEG]=RG(26,18),
			 [R_FILTFREQ]=RG(110,16), [R_RESO]=RG(12,18), [R_TRANSVOL]=RG(26,25),
			 [R_VOL]=RG(102,14), [R_DECIM]=RG(120,8) } },

	[RND_A_BASS] = { .name="BAS", .wave={C1(W_SIN),C1(W_SIN),C1(W_TRI),C1(W_SAW)}, .filt={C1(FT_LP)},
		.flags=F_TONAL|F_BASSREG|F_NOTIGHT|F_FMHARM, .bandLo=41, .bandHi=49,
		.lfoProf={LP_DRIFT,LP_GROOVE}, .lfoTgt={R_FILTFREQ,R_FILTFREQ},
		.r={ [R_COARSE]=RG(43,3), [R_AEGA]=RG(0,3), [R_AEGD]=RG(20,8), [R_VOLSLOPE]=RG(16,9),
			 [R_MODAMT]=RG(10,8), [R_FMAMT]=RG(0,22), [R_FMFREQ]=RG(58,22), [R_PITCHSLOPE]=RG(18,12),
			 [R_FILTFREQ]=RG(40,14), [R_RESO]=RG(8,10), [R_TRANSVOL]=RG(4,6),
			 [R_VOL]=RG(112,10), [R_DECIM]=RG(127,0) } },

	[RND_A_PERC] = { .name="PRC", .wave={C1(W_REC)}, .modWave={C1(W_REC)}, .filt={C1(FT_BP)}, .mixMod=MM_MIX,
		.flags=F_TONAL|F_KEYTRACK, .bandLo=75, .bandHi=84,
		.lfoProf={LP_DRIFT,LP_SH}, .lfoTgt={R_FILTFREQ,R_FMFREQ},	// S&H modulator: other metal per hit
		// decay and drive up from the tool (18 / 42): the keytracked BP perc was thin
		.r={ [R_COARSE]=RG(71,12), [R_AEGA]=RG(0,3), [R_AEGD]=RG(22,8), [R_VOLSLOPE]=RG(5,5),
			 [R_MODAMT]=RG(12,16), [R_FMAMT]=RG(80,22), [R_FMFREQ]=RG(86,26),
			 [R_FILTFREQ]=RG(88,18), [R_RESO]=RG(72,20), [R_TRANSVOL]=RG(12,16), [R_DRIVE]=RG(58,26),
			 [R_VOL]=RG(98,14), [R_DECIM]=RG(120,8) } },

	[RND_A_BELL] = { .name="BEL", .wave={C1(W_REC)}, .modWave={C1(W_SAW),C1(W_REC),C1(W_NOI)}, .filt={C1(FT_BP)}, .mixMod=MM_FM,
		.flags=F_TONAL|F_KEYTRACK, .bandLo=69, .bandHi=84,
		.lfoProf={LP_DRIFT,LP_DRIFT}, .lfoTgt={R_FILTFREQ,R_FMAMT},
		.r={ [R_COARSE]=RG(70,20), [R_AEGA]=RG(0,3), [R_AEGD]=RG(40,14), [R_VOLSLOPE]=RG(8,6),
			 [R_FMAMT]=RG(104,20), [R_FMFREQ]=RG(94,34),
			 [R_MODF1]=RG(82,30), [R_MODF2]=RG(108,26), [R_MODG1]=RG(74,26), [R_MODG2]=RG(68,26),
			 [R_FILTFREQ]=RG(96,24), [R_RESO]=RG(64,24), [R_TRANSVOL]=RG(8,14), [R_DRIVE]=RG(54,30),
			 [R_DECIM]=RG(80,34), [R_VOL]=RG(92,14) } },

	[RND_A_SNARE] = { .name="SNR", .wave={C1(W_SIN),C1(W_TRI)}, .filt={C1(FT_HP),C1(FT_BP)},
		.flags=F_TONAL,
		.lfoProf={LP_DRIFT,LP_GROOVE}, .lfoTgt={R_FILTFREQ,R_NOISEMIX},
		.r={ [R_COARSE]=RG(56,18), [R_AEGA]=RG(0,3), [R_AEGD]=RG(12,4), [R_VOLSLOPE]=RG(34,10),
			 [R_MODAMT]=RG(40,26), [R_NOISEMIX]=RG(74,18),
			 [R_FILTFREQ]=RG(72,24), [R_RESO]=RG(42,24), [R_TRANSVOL]=RG(20,22),
			 [R_VOL]=RG(100,14), [R_DECIM]=RG(120,8) } },

	[RND_A_CLAP] = { .name="CLP", .wave={C1(W_NOI)}, .filt={C1(FT_BP)},
		.hpSoft=2, .hpHardHz=900,
		.lfoProf={LP_DRIFT,LP_SH}, .lfoTgt={R_FILTFREQ,R_FILTFREQ},
		.r={ [R_COARSE]=RG(120,8), [R_AEGA]=RG(6,6), [R_AEGD]=RG(7,6), [R_VOLSLOPE]=RG(6,5),
			 [R_REPEAT]=RG(4,1), [R_FILTFREQ]=RG(60,16), [R_RESO]=RG(96,20), [R_TRANSVOL]=RG(6,12),
			 [R_VOL]=RG(100,14), [R_DECIM]=RG(122,6) } },

	[RND_A_HIHAT] = { .name="HAT", .wave={C1(W_REC),C1(W_SAW)}, .filt={C1(FT_HP)},
		.hpSoft=10, .hpHardHz=5000,
		.lfoProf={LP_DRIFT,LP_GROOVE}, .lfoTgt={R_AEGD,R_AEGD},
		.r={ [R_COARSE]=RG(104,24), [R_AEGA]=RG(0,3), [R_AEGD]=RG(9,6), [R_AEGDOPEN]=RG(22,9), [R_VOLSLOPE]=RG(7,5),
			 [R_MODF1]=RG(78,30), [R_MODF2]=RG(110,24), [R_MODG1]=RG(82,26), [R_MODG2]=RG(76,26),
			 [R_FILTFREQ]=RG(118,10), [R_RESO]=RG(30,26), [R_TRANSVOL]=RG(10,16), [R_DRIVE]=RG(30,26),
			 [R_VOL]=RG(90,18), [R_DECIM]=RG(120,8) } },

	[RND_A_CYMBAL] = { .name="CYM", .wave={C1(W_REC),C1(W_SAW)}, .filt={C1(FT_BP)},
		.hpSoft=9, .hpHardHz=4500,
		.lfoProf={LP_DRIFT,LP_DRIFT}, .lfoTgt={R_FILTFREQ,R_FILTFREQ},
		.r={ [R_COARSE]=RG(104,24), [R_AEGA]=RG(0,3), [R_AEGD]=RG(26,10), [R_AEGDOPEN]=RG(36,12), [R_VOLSLOPE]=RG(5,5),
			 [R_MODF1]=RG(72,34), [R_MODF2]=RG(112,26), [R_MODG1]=RG(86,28), [R_MODG2]=RG(80,28),
			 [R_FILTFREQ]=RG(116,10), [R_RESO]=RG(80,24), [R_TRANSVOL]=RG(8,14), [R_DRIVE]=RG(36,28),
			 [R_VOL]=RG(74,20), [R_DECIM]=RG(120,8) } },

	[RND_A_CONGA] = { .name="CON", .wave={C1(W_SIN),C1(W_TRI)}, .filt={C1(FT_LP)},
		.flags=F_TONAL, .bandLo=55, .bandHi=67,
		.lfoProf={LP_DRIFT,LP_SH}, .lfoTgt={R_FILTFREQ,R_MODEG},
		.r={ [R_COARSE]=RG(66,16), [R_AEGA]=RG(0,3), [R_AEGD]=RG(24,8), [R_VOLSLOPE]=RG(14,10),
			 [R_MODAMT]=RG(45,22), [R_PITCHSLOPE]=RG(18,12), [R_MODEG]=RG(22,14),
			 [R_FILTFREQ]=RG(106,18), [R_RESO]=RG(16,16), [R_TRANSVOL]=RG(18,20),
			 [R_VOL]=RG(104,14), [R_DECIM]=RG(122,6) } },

	[RND_A_RIM] = { .name="RIM", .wave={C1(W_SIN),C1(W_TRI),C1(W_REC)}, .filt={C1(FT_LP)},
		.flags=F_TONAL, .hpSoft=2, .hpHardHz=800,
		.lfoProf={LP_DRIFT,LP_SH}, .lfoTgt={R_FILTFREQ,R_FILTFREQ},
		.r={ [R_COARSE]=RG(80,20), [R_AEGA]=RG(0,2), [R_AEGD]=RG(8,6), [R_VOLSLOPE]=RG(8,6),
			 [R_MODAMT]=RG(15,16), [R_FILTFREQ]=RG(100,25), [R_RESO]=RG(30,24),
			 [R_TRANSVOL]=RG(72,38), [R_TRANSFREQ]=RG(82,34),
			 [R_VOL]=RG(100,14), [R_DECIM]=RG(122,6) } },

	[RND_A_SHAKER] = { .name="SHK", .wave={C1(W_NOI)}, .filt={C1(FT_HP)},
		.hpSoft=6, .hpHardHz=3000,
		.lfoProf={LP_DRIFT,LP_GROOVE}, .lfoTgt={R_FILTFREQ,R_AEGD},
		.r={ [R_COARSE]=RG(110,16), [R_AEGA]=RG(0,4), [R_AEGD]=RG(10,8), [R_VOLSLOPE]=RG(8,6),
			 [R_FILTFREQ]=RG(116,10), [R_RESO]=RG(26,22), [R_TRANSVOL]=RG(8,14),
			 [R_VOL]=RG(90,16), [R_DECIM]=RG(120,8) } },

	[RND_A_RIDE] = { .name="RID", .wave={C1(W_REC),C1(W_SAW)}, .filt={C1(FT_BP)},
		.hpSoft=8, .hpHardHz=4000,
		.lfoProf={LP_DRIFT,LP_DRIFT}, .lfoTgt={R_FILTFREQ,R_FILTFREQ},
		.r={ [R_COARSE]=RG(108,20), [R_AEGA]=RG(0,3), [R_AEGD]=RG(24,10), [R_AEGDOPEN]=RG(34,12), [R_VOLSLOPE]=RG(6,5),
			 [R_MODF1]=RG(84,28), [R_MODF2]=RG(114,22), [R_MODG1]=RG(84,24), [R_MODG2]=RG(78,24),
			 [R_FILTFREQ]=RG(120,8), [R_RESO]=RG(72,22), [R_TRANSVOL]=RG(8,12), [R_DRIVE]=RG(28,24),
			 [R_VOL]=RG(78,18), [R_DECIM]=RG(120,8) } },

	[RND_A_STAB] = { .name="STB", .wave={C1(W_TRI),C1(W_SAW)}, .filt={C1(FT_LP)}, .mixMod=MM_FM,
		.flags=F_TONAL|F_FMHARM, .bandLo=65, .bandHi=79,
		.lfoProf={LP_DRIFT,LP_GROOVE}, .lfoTgt={R_FILTFREQ,R_FILTFREQ},
		.r={ [R_COARSE]=RG(48,18), [R_AEGA]=RG(0,4), [R_AEGD]=RG(22,10), [R_VOLSLOPE]=RG(24,14),
			 [R_MODAMT]=RG(20,14), [R_PITCHSLOPE]=RG(16,12), [R_MODEG]=RG(30,18),
			 [R_FMAMT]=RG(64,30), [R_FMFREQ]=RG(66,30),
			 [R_FILTFREQ]=RG(52,20), [R_RESO]=RG(48,26), [R_TRANSVOL]=RG(10,16), [R_DRIVE]=RG(48,28),
			 [R_VOL]=RG(100,16), [R_DECIM]=RG(122,6) } },

	[RND_A_FM] = { .name="FM ", .wave={C1(W_TRI),C1(W_SAW)}, .filt={C1(FT_LP)}, .mixMod=MM_FM,
		.flags=F_TONAL|F_FMHARM, .bandLo=46, .bandHi=65,
		.lfoProf={LP_DRIFT,LP_DRIFT}, .lfoTgt={R_FILTFREQ,R_FMAMT},
		.r={ [R_COARSE]=RG(40,12), [R_AEGA]=RG(0,3), [R_AEGD]=RG(28,10), [R_VOLSLOPE]=RG(20,10),
			 [R_MODAMT]=RG(5,7), [R_PITCHSLOPE]=RG(26,16), [R_MODEG]=RG(70,22),
			 [R_FMAMT]=RG(86,26), [R_FMFREQ]=RG(66,24),
			 [R_FILTFREQ]=RG(70,20), [R_RESO]=RG(48,20), [R_TRANSVOL]=RG(16,22), [R_DRIVE]=RG(58,22),
			 [R_VOL]=RG(104,12), [R_DECIM]=RG(122,6) } },

	[RND_A_NOISE] = { .name="NOI", .wave={C1(W_NOI)}, .filt={C1(FT_LP),C1(FT_HP),C1(FT_BP)},
		.hpSoft=3, .hpHardHz=1500,
		.lfoProf={LP_DRIFT,LP_SH}, .lfoTgt={R_FILTFREQ,R_FILTFREQ},
		// tool: coarse 90+-37, filt 80+-45, reso 50+-40 ... tamed ("not extreme")
		.r={ [R_COARSE]=RG(90,30), [R_AEGA]=RG(0,8), [R_AEGD]=RG(30,14), [R_VOLSLOPE]=RG(26,22),
			 [R_FILTFREQ]=RG(80,32), [R_RESO]=RG(48,30), [R_TRANSVOL]=RG(10,20),
			 [R_VOL]=RG(95,18), [R_DECIM]=RG(116,10) } },

	//------------------------------------------------ EXP synth recipes
	// Reese: two saws crossfaded (mix mode, FM amount 63 = equal), main osc
	// detuned against the modulator, dark LP, saturation.
	[RND_A_REESE] = { .name="REE", .wave={C1(W_SAW)}, .modWave={C1(W_SAW)}, .filt={C1(FT_LP)}, .mixMod=MM_MIX,
		.fmInt={IV(0),IV(0),IV(12),IV(-12)}, .detMin=8, .detMax=20, .trans=TR(1)|TR(2)|TR(4),
		.flags=F_TONAL|F_BASSREG|F_NOTIGHT|F_NOFLIP|F_FIXWAVE, .bandLo=36, .bandHi=47,
		.lfoProf={LP_DRIFT,LP_DRIFT}, .lfoTgt={R_FILTFREQ,R_FILTFREQ},
		.r={ [R_COARSE]=RG(40,4), [R_AEGA]=RG(0,3), [R_AEGD]=RG(44,14), [R_VOLSLOPE]=RG(20,10),
			 [R_MODAMT]=RG(4,4), [R_PITCHSLOPE]=RG(20,10), [R_MODEG]=RG(20,10),
			 [R_FMAMT]=RG(62,8), [R_FILTFREQ]=RG(50,14), [R_RESO]=RG(30,20), [R_FILTDRIVE]=RG(40,30),
			 [R_DRIVE]=RG(44,28), [R_TRANSVOL]=RG(4,6), [R_VOL]=RG(106,10), [R_DECIM]=RG(127,0) } },

	// Fold: the engine has no wavefolder; sine/tri FM at a harmonic ratio with
	// a high index (shaped by the mod EG) + saturation gives the folded,
	// harmonically rich "west coast" tone.
	[RND_A_FOLD] = { .name="FLD", .wave={C1(W_SIN),C1(W_SIN),C1(W_TRI)}, .modWave={C1(W_SIN),C1(W_TRI)}, .filt={C1(FT_LP)}, .mixMod=MM_FM,
		.fmInt={IV(12),IV(19),IV(24),IV(0)}, .trans=TR(1)|TR(2)|TR(4)|TR(11),
		.flags=F_TONAL|F_BASSREG|F_NOFLIP|F_FIXWAVE, .bandLo=38, .bandHi=50,
		.lfoProf={LP_DRIFT,LP_GROOVE}, .lfoTgt={R_FMAMT,R_FMAMT},
		.r={ [R_COARSE]=RG(41,5), [R_AEGA]=RG(0,3), [R_AEGD]=RG(30,12), [R_VOLSLOPE]=RG(18,10),
			 [R_MODAMT]=RG(6,6), [R_PITCHSLOPE]=RG(20,12), [R_MODEG]=RG(52,24),
			 [R_FMAMT]=RG(84,22), [R_FILTFREQ]=RG(78,22), [R_RESO]=RG(24,20), [R_FILTDRIVE]=RG(30,24),
			 [R_DRIVE]=RG(56,26), [R_TRANSVOL]=RG(8,10), [R_VOL]=RG(104,12), [R_DECIM]=RG(124,4) } },

	// Wavetable: crossfade (mix mode) between two different shapes an octave /
	// fifth apart; FM amount = table position, PWM = shape. MOD on scans it.
	[RND_A_WT] = { .name="WT ", .wave={C1(W_SAW),C1(W_REC),C1(W_PWM),C1(W_TRI)}, .modWave={C1(W_SIN),C1(W_TRI),C1(W_REC)},
		.filt={C1(FT_LP),C1(FT_LP),C1(FT_BP)}, .mixMod=MM_MIX,
		.fmInt={IV(12),IV(12),IV(7),IV(19)}, .trans=TR(1)|TR(2)|TR(4),
		.flags=F_TONAL|F_BASSREG|F_NOFLIP|F_KEYTRACK, .bandLo=38, .bandHi=50,
		.lfoProf={LP_DRIFT,LP_DRIFT}, .lfoTgt={R_FMAMT,R_FMAMT},
		.r={ [R_COARSE]=RG(42,6), [R_AEGA]=RG(0,3), [R_AEGD]=RG(30,12), [R_VOLSLOPE]=RG(18,10),
			 [R_MODAMT]=RG(4,5), [R_PITCHSLOPE]=RG(18,10), [R_MODEG]=RG(24,14),
			 [R_FMAMT]=RG(64,36), [R_PWM]=RG(64,28), [R_FILTFREQ]=RG(70,20), [R_RESO]=RG(36,22),
			 [R_FILTDRIVE]=RG(24,20), [R_DRIVE]=RG(36,26), [R_TRANSVOL]=RG(6,8),
			 [R_VOL]=RG(104,12), [R_DECIM]=RG(126,2) } },

	// Acid: saw/square, resonant driven LP, short and plucky. With MOD on a
	// falling saw LFO restarted by every hit sweeps the filter (envelope).
	[RND_A_ACID] = { .name="ACD", .wave={C1(W_SAW),C1(W_SAW),C1(W_REC)}, .filt={C1(FT_LP)}, .mixMod=MM_FM,
		.trans=TR(1)|TR(2)|TR(4),
		.flags=F_TONAL|F_BASSREG|F_NOTIGHT|F_NOFLIP|F_FIXWAVE, .bandLo=36, .bandHi=48,
		.lfoProf={LP_ENV,LP_ENV}, .lfoTgt={R_FILTFREQ,R_FILTFREQ},
		.r={ [R_COARSE]=RG(40,6), [R_AEGA]=RG(0,2), [R_AEGD]=RG(22,8), [R_VOLSLOPE]=RG(26,12),
			 [R_MODAMT]=RG(6,6), [R_PITCHSLOPE]=RG(16,10), [R_MODEG]=RG(18,10),
			 [R_FMAMT]=RG(0,6), [R_FILTFREQ]=RG(50,14), [R_RESO]=RG(96,16), [R_FILTDRIVE]=RG(56,28),
			 [R_DRIVE]=RG(48,26), [R_TRANSVOL]=RG(6,8), [R_VOL]=RG(104,12), [R_DECIM]=RG(127,0) } },

	// Ghost kick (EXP, V2/V3): small, short, a little higher than the kick -
	// syncopated off-beat kicks / fills under the main kick
	[RND_A_GHOST] = { .name="GST", .wave={C1(W_SIN),C1(W_SIN),C1(W_TRI)}, .filt={C1(FT_LP)}, .mixMod=MM_FM,
		.trans=TR(0)|TR(2)|TR(4)|TR(5)|TR(10),
		.flags=F_TONAL|F_NOTIGHT|F_NOFLIP|F_FIXWAVE, .bandLo=36, .bandHi=47,
		.lfoProf={LP_NONE,LP_SH}, .lfoTgt={R_AEGD,R_AEGD},
		.r={ [R_COARSE]=RG(40,6), [R_AEGA]=RG(0,2), [R_AEGD]=RG(14,6), [R_VOLSLOPE]=RG(22,10),
			 [R_MODAMT]=RG(60,25), [R_PITCHSLOPE]=RG(22,12), [R_MODEG]=RG(22,14),
			 [R_FILTFREQ]=RG(100,20), [R_RESO]=RG(10,12), [R_TRANSVOL]=RG(30,30), [R_TRANSFREQ]=RG(60,40),
			 [R_FMAMT]=RG(0,12), [R_VOL]=RG(80,10), [R_DECIM]=RG(122,6) } },
};

//---- pools ----------------------------------------------------------------
typedef struct { uint8_t a; uint8_t w0, w1; } PoolEntry;	// weight at VAR 0 / VAR 127
#define POOL_MAX 12

typedef struct { uint8_t n; PoolEntry e[POOL_MAX]; } Pool;

static const Pool poolCls[6] =
{
	{ 1, { {RND_A_KICK,1,1} } },
	{ 1, { {RND_A_BASS,1,1} } },
	{ 1, { {RND_A_FM,1,1} } },
	{ 3, { {RND_A_SNARE,2,2}, {RND_A_CLAP,1,1}, {RND_A_SHAKER,1,1} } },
	{ 4, { {RND_A_CLAP,1,1}, {RND_A_CYMBAL,1,1}, {RND_A_RIDE,1,1}, {RND_A_SHAKER,1,1} } },
	{ 2, { {RND_A_HIHAT,3,3}, {RND_A_RIDE,1,1} } },
};

static const Pool poolExp[6] =
{
	{ 1, { {RND_A_KICK,1,1} } },
	{ 7, { {RND_A_BASS,4,1}, {RND_A_FM,2,2}, {RND_A_REESE,2,3}, {RND_A_FOLD,1,3}, {RND_A_WT,1,3}, {RND_A_ACID,2,3}, {RND_A_GHOST,1,1} } },
	{ 12,{ {RND_A_FM,3,1}, {RND_A_STAB,3,2}, {RND_A_PERC,2,2}, {RND_A_BELL,1,2}, {RND_A_TOM,2,1}, {RND_A_CONGA,1,1},
		   {RND_A_RIM,1,1}, {RND_A_FOLD,1,2}, {RND_A_WT,1,2}, {RND_A_ACID,1,2}, {RND_A_REESE,0,1}, {RND_A_GHOST,1,1} } },
	{ 4, { {RND_A_SNARE,4,2}, {RND_A_CLAP,2,2}, {RND_A_SHAKER,1,1}, {RND_A_NOISE,0,2} } },
	{ 6, { {RND_A_CLAP,2,1}, {RND_A_CYMBAL,2,2}, {RND_A_RIDE,2,2}, {RND_A_SHAKER,1,1}, {RND_A_BELL,0,2}, {RND_A_NOISE,0,1} } },
	{ 5, { {RND_A_HIHAT,4,2}, {RND_A_CYMBAL,1,1}, {RND_A_RIDE,1,1}, {RND_A_SHAKER,1,1}, {RND_A_BELL,0,1} } },
};

static uint8_t pickFromPool(Rng* r, const Pool* pool, float amt, uint8_t avoid)
{
	float w[POOL_MAX];
	float sum = 0;
	for(uint8_t i=0;i<pool->n;i++)
	{
		const PoolEntry* e = &pool->e[i];
		w[i] = (float)e->w0 + ((float)e->w1 - (float)e->w0) * amt;	// amt = type spread
		if(e->a == avoid && pool->n > 1) w[i] = 0;
		sum += w[i];
	}
	if(sum <= 0) return pool->e[0].a;
	float x = rng_u(r) * sum;
	for(uint8_t i=0;i<pool->n;i++)
	{
		if(x < w[i]) return pool->e[i].a;
		x -= w[i];
	}
	return pool->e[pool->n-1].a;
}

//---- helpers --------------------------------------------------------------
static uint8_t choose(Rng* r, const uint8_t* list, uint8_t maxLen, int* out)	// C1 encoded list
{
	uint8_t n = 0;
	while(n < maxLen && list[n]) n++;
	if(!n) return 0;
	*out = list[rng_int(r, 0, n-1)] - 1;
	return 1;
}

#define FILTER_FS		44140.f
#define FILTER_K		(2.f*(-0.9f)/(1.0001f+0.9f))	// valueShaperF2F, FILTER_SHAPER

float Randomizer_filterHz(uint8_t byte)
{
	const float x = byte / 127.f;
	return 0.45f * FILTER_FS * ((1.f+FILTER_K)*x / (1.f+FILTER_K*x));
}

uint8_t Randomizer_filterByte(float hz)
{
	float y = hz / (0.45f * FILTER_FS);
	if(y <= 0) return 0;
	if(y >= 1) return 127;
	const float x = y / (1.f + FILTER_K - FILTER_K*y);
	return (uint8_t)clampi((int)(x*127.f + 0.5f), 0, 127);
}

// LFO rate byte for a frequency (lfo_setFreq: f = (1/128 + x*127/128)^3 * 200 Hz)
static uint8_t lfoByteFromHz(float hz)
{
	float c = cbrtf(hz / 200.f);
	float x = (c - 0.0078125f) / (1.f - 0.0078125f);
	return (uint8_t)clampi((int)(x*127.f + 0.5f), 1, 127);
}

// AD decay byte -> approximate hit length [ms] (desktop tool estimate)
static float decayMs(uint8_t d)
{
	const float x = d / 127.f;
	return 20.f + x*x*1500.f;
}

// pitch class match against key root / fifth
static uint8_t inKey(int note, int key)
{
	const int pc = ((note % 12) + 12) % 12;
	return pc == key || pc == (key + 7) % 12;
}

/* snap a note to the key (root or fifth).
 * mode 0: nearest to cur (anywhere >= floor)
 * mode 1: random candidate inside [lo,hi]
 * mode 2: middle candidate inside [lo,hi]
 * mode 3: candidate inside [lo,hi] nearest to cur */
static int keySnap(Rng* r, int cur, int lo, int hi, int floorNote, int key, uint8_t mode)
{
	if(mode != 0)
	{
		int cand[16];
		int n = 0;
		for(int m=lo; m<=hi && n<16; m++)
			if(m >= floorNote && inKey(m, key)) cand[n++] = m;
		if(n)
		{
			if(mode == 1) return cand[rng_int(r, 0, n-1)];
			if(mode == 2) return cand[(n-1)/2];
			int best = cand[0];
			for(int i=1;i<n;i++)
				if(abs(cand[i] - cur) < abs(best - cur) || (abs(cand[i] - cur) == abs(best - cur) && rng_u(r) < 0.5f))
					best = cand[i];
			return best;
		}
		cur = (lo + hi) / 2;	// no candidate in the band: nearest to its centre
	}
	if(cur < floorNote) cur = floorNote;
	for(int d=0; d<12; d++)
	{
		const uint8_t up = (cur + d <= 127) && inKey(cur + d, key);
		const uint8_t dn = (cur - d >= floorNote) && (cur - d >= 0) && inKey(cur - d, key);
		if(up && dn) return (rng_u(r) < 0.5f) ? cur + d : cur - d;
		if(up) return cur + d;
		if(dn) return cur - d;
	}
	return cur;
}

static int nearestInterval(int d)
{
	static const int8_t harm[] = { -12, 0, 7, 12, 19, 24, 31 };
	int best = harm[0];
	for(uint8_t i=1;i<sizeof(harm);i++)
		if(abs(harm[i] - d) < abs(best - d)) best = harm[i];
	return best;
}

static void setRole(uint8_t* p, uint8_t v, uint8_t role, int value)
{
	const int16_t par = rolePar[v][role];
	if(par != NA) p[par] = (uint8_t)clampi(value, 0, 127);
}

static int getRole(const uint8_t* p, uint8_t v, uint8_t role)
{
	const int16_t par = rolePar[v][role];
	return par != NA ? p[par] : -1;
}

//---- loudness ---------------------------------------------------------------
// Mix balance: every voice gets the VOL that puts its predicted loudness on
// the target of its recipe (K-weighted short-term peak, solo, vel 127, dB).
// The prediction is the regression of RandomizerLoudness.h, fitted on host
// renders of this engine; it follows decay, drive, filter, FM, waveform...
// so a fat reese and a thin perc end up level-matched instead of "VOL 104".
static const float rndLoudTarget[RND_A_COUNT] =
{
	[RND_A_KICK]  =  -8.5f,	// reference: the loudest element
	[RND_A_BASS]  = -11.0f, [RND_A_REESE] = -11.0f, [RND_A_FOLD] = -11.0f,
	[RND_A_WT]    = -11.5f, [RND_A_ACID]  = -11.0f, [RND_A_FM]   = -11.5f,
	[RND_A_GHOST] = -15.0f,	// ghost kick sits under the kick
	[RND_A_TOM]   = -11.0f, [RND_A_CONGA] = -12.0f, [RND_A_RIM]  = -14.0f,
	[RND_A_PERC]  = -14.0f, [RND_A_BELL]  = -15.0f, [RND_A_STAB] = -12.5f,
	[RND_A_SNARE] = -10.0f, [RND_A_CLAP]  = -11.5f, [RND_A_NOISE] = -13.0f,
	[RND_A_SHAKER]= -15.0f, [RND_A_HIHAT] = -20.0f,
	[RND_A_CYMBAL]= -17.0f, [RND_A_RIDE]  = -17.0f,
};

static float clip127(int x) { return (float)(x < 0 ? 0 : x > 127 ? 127 : x); }

// returns the headroom: predicted loudness at VOL 127 minus the target [dB]
static float setLoudness(uint8_t* p, uint8_t v, uint8_t a)
{
	const int8_t m = rndLoudIdx[v][a];
	if(m < 0) return 0;					// no model: recipe VOL stays
	const float* w = rndLoudW[(uint8_t)m];

	const float aD  = clip127(getRole(p, v, R_AEGD));
	const float L1  = logf(20.f + (aD/127.f)*(aD/127.f)*1500.f);
	const float drv = clip127(getRole(p, v, R_DRIVE));
	const float ff  = clip127(getRole(p, v, R_FILTFREQ));
	const float fmA = clip127(getRole(p, v, R_FMAMT));
	const int   mixR = getRole(p, v, R_MIXMOD);
	const float mix = mixR > 0 ? 1.f : 0.f;
	const int   wave = getRole(p, v, R_WAVE);
	const int   ft   = getRole(p, v, R_FILTTYPE);

	// same features, same order as the fit (fit2.py)
	const float x[RND_LOUD_NF-1] =
	{
		1.f, L1, L1*L1, clip127(getRole(p, v, R_AEGA)), clip127(getRole(p, v, R_VOLSLOPE)),
		drv, drv*drv/127.f, clip127(getRole(p, v, R_FILTDRIVE)), clip127(getRole(p, v, R_RESO)),
		ff, ff*ff/127.f, fmA, fmA*mix, mix, clip127(getRole(p, v, R_TRANSVOL)),
		clip127(getRole(p, v, R_COARSE)), clip127(getRole(p, v, R_DECIM)), clip127(getRole(p, v, R_NOISEMIX)),
		clip127(getRole(p, v, R_MODAMT)), clip127(getRole(p, v, R_MODEG)),
		clip127(getRole(p, v, R_MODG1)), clip127(getRole(p, v, R_MODG2)), clip127(getRole(p, v, R_REPEAT)),
		wave == 0, wave == 1, wave == 2, wave == 3, wave == 4, wave == 5,
		ft == 0, ft == 1, ft == 2,
	};
	float L = 0;
	for(uint8_t i=0; i<RND_LOUD_NF-1; i++) L += w[i] * x[i];

	float slope = w[RND_LOUD_NF-1];		// dB per dB of VOL (< 1 where VOL feeds a saturator)
	if(slope < 0.3f) slope = 0.3f;
	const float lv = (rndLoudTarget[a] - L) / slope;		// 20*log10(vol/127)
	const int vol = (int)lrintf(127.f * powf(10.f, lv / 20.f));
	setRole(p, v, R_VOL, clampi(vol, 24, 127));
	return L - rndLoudTarget[a];
}

//---- per voice ------------------------------------------------------------
typedef struct
{
	uint8_t type, hz;
	int8_t key;
	float amt;			// spread: fixed per type (CLS = desktop tool 55 %)
} GenCtx;

// floorNote: lowest note allowed (bass above the kick, V3 above the bass), 0 = none
static float applyVoice(uint8_t* p, uint8_t v, uint8_t a, const GenCtx* g, Rng* r, int floorNote)
{
	const RndArch* A = &arch[a];
	const uint8_t exp = g->type == RND_TYPE_EXP;
	const uint8_t anyKick = (A->flags & F_KICK) != 0;
	const uint8_t isKick = anyKick && !exp;		// CLS: protected kick
	const uint8_t expKick = anyKick && exp;		// EXP: free kick, wider bounds

	// spread: the CLS kick is protected (narrow, never widened)
	const float amt  = isKick ? 0.44f : g->amt;
	const float wmul = (exp && !isKick) ? 1.5f : 1.0f;

	// 1. neutral engine defaults
	for(uint8_t role=0; role<R_NUM; role++)
		setRole(p, v, role, roleDefault[v][role]);

	// 2. continuous roles
	for(uint8_t role=0; role<R_NUM; role++)
	{
		const RndRange* rg = &A->r[role];
		if(!rg->c1) continue;
		const float c = (float)(rg->c1 - 1);
		const float val = c + rng_bi(r) * (float)rg->r * amt * wmul;
		setRole(p, v, role, (int)lrintf(val));
	}

	// 3. discrete roles
	int w;
	if(choose(r, A->wave, 4, &w))
	{
		if(expKick && rng_u(r) < 0.2f)
			w = W_TRI;							// EXP kick: sometimes a triangle body
		else if(exp && !(A->flags & F_FIXWAVE) && (A->flags & F_TONAL) && rng_u(r) < g->amt*0.3f)
		{
			static const uint8_t tonalWaves[] = { W_SIN, W_TRI, W_SAW, W_REC, W_PWM };
			w = tonalWaves[rng_int(r, 0, sizeof(tonalWaves)-1)];
		}
		setRole(p, v, R_WAVE, w);
		if(w == W_PWM && !A->r[R_PWM].c1)
			setRole(p, v, R_PWM, rng_int(r, 32, 96));	// 0/127 = silence
	}

	int mw;
	if(choose(r, A->modWave, 3, &mw))
	{
		// wavetable: the two shapes must differ, otherwise there is nothing to scan
		for(uint8_t i=0; i<4 && (a == RND_A_WT) && mw == getRole(p, v, R_WAVE); i++)
			choose(r, A->modWave, 3, &mw);
		setRole(p, v, R_MODWAVE, mw);
	}
	else if(exp && rolePar[v][R_MODWAVE2] != NA)
	{
		// metal voices: osc 2/3 shapes add colour in EXP (CLS keeps sines, as tuned)
		static const uint8_t metalWaves[] = { W_SIN, W_TRI, W_SAW, W_REC };
		if(rng_u(r) < g->amt*0.5f) setRole(p, v, R_MODWAVE,  metalWaves[rng_int(r, 0, 3)]);
		if(rng_u(r) < g->amt*0.5f) setRole(p, v, R_MODWAVE2, metalWaves[rng_int(r, 0, 3)]);
	}

	int ft;
	if(choose(r, A->filt, 3, &ft)) setRole(p, v, R_FILTTYPE, ft);

	if(A->mixMod)
	{
		int m = A->mixMod - 1;
		if(!(A->flags & F_NOFLIP) && rng_u(r) < g->amt*0.4f) m = 1 - m;
		setRole(p, v, R_MIXMOD, m);
	}

	if(rolePar[v][R_TRANSWAVE] != NA && A->r[R_TRANSVOL].c1 && rng_u(r) < 0.6f)
	{
		uint16_t mask = A->trans ? A->trans : 0x0fff;
		int tw;
		do { tw = rng_int(r, 0, 11); } while(!(mask & (1u << tw)));
		setRole(p, v, R_TRANSWAVE, tw);
	}

	// 4. pitch: register, Hz band, key
	const uint8_t upper = (v == 2) && exp && (A->flags & F_BASSREG);	// synth bass recipes as leads on V3
	const int shift = upper ? 12 : 0;
	int note = getRole(p, v, R_COARSE) + shift;
	const int bandLo = expKick ? 26 : (A->bandLo ? A->bandLo + shift : 0);
	const int bandHi = expKick ? 40 : (A->bandHi ? A->bandHi + shift : 0);


	// not in the extremes: drum voices up to ~2.6 kHz, synth basses stay basses
	int minNote = 0, maxNote = 127;
	if(v < 3 && !isKick) maxNote = ((A->flags & F_BASSREG) && !upper) ? 60 : 100;
	if(a == RND_A_SNARE) { minNote = 44; maxNote = 84; }	// body ~100..1000 Hz
	note = clampi(note, minNote, maxNote);

	if(bandLo && g->hz != RND_HZ_OFF)
	{
		if(g->hz == RND_HZ_SOFT)
		{
			const float t = bandLo + rng_u(r) * (float)(bandHi - bandLo);
			note = (int)lrintf(note + (t - note) * 0.5f);		// meet the band half way
		}
		else
		{
			const float mid = 0.5f * (bandLo + bandHi);
			note = (int)lrintf(mid + rng_bi(r) * 0.12f * (bandHi - bandLo));
		}
	}

	if(g->key >= 0 && (A->flags & F_TONAL))
	{
		if(bandLo && g->hz != RND_HZ_OFF)
			note = keySnap(r, note, bandLo, bandHi, floorNote, g->key, g->hz == RND_HZ_HARD ? 2 : 1);
		else if(anyKick)	// the kick never leaves its band, Hz off: nearest key note inside it
			note = keySnap(r, note, bandLo, bandHi, floorNote, g->key, 3);
		else
			note = keySnap(r, note, 0, 0, floorNote, g->key, 0);
		setRole(p, v, R_FINE, 64);
	}
	else if(note < floorNote)
	{
		note = floorNote;
	}
	if(isKick) note = clampi(note, 28, 37);			// ~41..65 Hz
	else if(expKick) note = clampi(note, 24, 44);	// ~33..104 Hz
	else
	{
		while(note > maxNote) note -= 12;			// octaves keep the key
		while(note < minNote) note += 12;
		if(note < floorNote) note = floorNote;
	}
	setRole(p, v, R_COARSE, note);
	note = getRole(p, v, R_COARSE);

	// reese detune (after the key: the modulator stays exactly on the note)
	if(A->detMax)
	{
		const int det = rng_int(r, A->detMin, A->detMax);
		setRole(p, v, R_FINE, 64 + ((rng_u(r) < 0.5f) ? det : -det));
	}

	// FM modulator relative to the final pitch
	if(rolePar[v][R_FMFREQ] != NA)
	{
		int iv;
		uint8_t n = 0;
		while(n < 4 && A->fmInt[n]) n++;
		if(n)
		{
			iv = A->fmInt[rng_int(r, 0, n-1)] - 64;
			setRole(p, v, R_FMFREQ, note + iv);
		}
		else if(g->key >= 0 && getRole(p, v, R_MIXMOD) == 1)
		{
			// mix mode: the modulator is a second, audible oscillator -> in key
			setRole(p, v, R_FMFREQ, keySnap(r, getRole(p, v, R_FMFREQ), 0, 0, 0, g->key, 0));
		}
		else if(g->key >= 0 && (A->flags & F_FMHARM))
		{
			// FM: harmonic ratio keeps the tone on the note (bells / percs stay metallic)
			iv = nearestInterval(getRole(p, v, R_FMFREQ) - note);
			setRole(p, v, R_FMFREQ, note + iv);
		}
	}

	// band-pass on a pitched drum voice: centre on a harmonic of the note
	if((A->flags & F_KEYTRACK) && v < 3 && getRole(p, v, R_FILTTYPE) == FT_BP)
	{
		static const int8_t ktHarm[] = { 0, 12, 19, 24 };
		int base = note;
		if(getRole(p, v, R_MIXMOD) == 1 && rng_u(r) < 0.5f) base = getRole(p, v, R_FMFREQ);	// mix: either osc
		const int n = base + ktHarm[rng_int(r, 0, 3)];
		const float hz = 220.f * powf(2.f, (n - 57) / 12.f);
		setRole(p, v, R_FILTFREQ, Randomizer_filterByte(hz) + rng_int(r, -2, 2));
	}

	// 5. Hz: HP slotting of the noise / metal voices, tighter decays
	if(A->hpHardHz && g->hz != RND_HZ_OFF)
	{
		setRole(p, v, R_FILTTYPE, FT_HP);
		if(g->hz == RND_HZ_SOFT)
			setRole(p, v, R_FILTFREQ, A->hpSoft);
		else
			setRole(p, v, R_FILTFREQ, Randomizer_filterByte(A->hpHardHz) + rng_int(r, -2, 2));
	}
	if(g->hz != RND_HZ_OFF && !(A->flags & F_NOTIGHT))
	{
		const float k = g->hz == RND_HZ_SOFT ? 0.88f : 0.82f;
		setRole(p, v, R_AEGD,     (int)lrintf(getRole(p, v, R_AEGD) * k));
		if(rolePar[v][R_AEGDOPEN] != NA)
			setRole(p, v, R_AEGDOPEN, (int)lrintf(getRole(p, v, R_AEGDOPEN) * k));
	}

	// 6. musical clamps (VOL: set by the loudness model at the end)
	setRole(p, v, R_DECIM, clampi(getRole(p, v, R_DECIM), exp ? 88 : 95, 127));
	setRole(p, v, R_AEGA,  clampi(getRole(p, v, R_AEGA), 0, 12));
	setRole(p, v, R_AEGD,  clampi(getRole(p, v, R_AEGD), 2, 127));	// decay 0 = no hit (EXP clap + Hz tightening)
	setRole(p, v, R_RESO,  clampi(getRole(p, v, R_RESO), 0, 118));
	{
		const int type = getRole(p, v, R_FILTTYPE);
		const int ff = getRole(p, v, R_FILTFREQ);
		if(type == FT_LP)	setRole(p, v, R_FILTFREQ, clampi(ff, 22, 127));	// closed LP = silence
		if(type == FT_HP)	setRole(p, v, R_FILTFREQ, clampi(ff, 0, 120));	// HP at the top = silence
		if(type == FT_BP)	setRole(p, v, R_FILTFREQ, clampi(ff, 16, 124));
	}
	if(A->mixMod == MM_MIX && (A->flags & F_NOFLIP))
		setRole(p, v, R_FMAMT, clampi(getRole(p, v, R_FMAMT), 16, 112));	// both shapes audible
	if(rolePar[v][R_AEGDOPEN] != NA)
	{
		const int dc = getRole(p, v, R_AEGD);
		setRole(p, v, R_AEGDOPEN, clampi(getRole(p, v, R_AEGDOPEN), dc + 6, 127));
	}
	if(getRole(p, v, R_WAVE) == W_PWM)
		setRole(p, v, R_PWM, clampi(getRole(p, v, R_PWM), 24, 104));

	if(isKick)
	{
		// the kick must stay good at VAR 127: proven bounds
		setRole(p, v, R_MODAMT,   clampi(getRole(p, v, R_MODAMT), 55, 115));
		setRole(p, v, R_AEGD,     clampi(getRole(p, v, R_AEGD), 14, 46));
		setRole(p, v, R_FILTFREQ, clampi(getRole(p, v, R_FILTFREQ), 96, 127));
		setRole(p, v, R_RESO,     clampi(getRole(p, v, R_RESO), 0, 26));
		setRole(p, v, R_FMAMT,    clampi(getRole(p, v, R_FMAMT), 0, 16));
		setRole(p, v, R_DECIM,    clampi(getRole(p, v, R_DECIM), 118, 127));
		setRole(p, v, R_AEGA,     clampi(getRole(p, v, R_AEGA), 0, 3));
	}
	else if(expKick)
	{
		// EXP: freer kick, still a kick (body, punch, no crush)
		setRole(p, v, R_MODAMT,   clampi(getRole(p, v, R_MODAMT), 30, 127));
		setRole(p, v, R_AEGD,     clampi(getRole(p, v, R_AEGD), 8, 70));
		setRole(p, v, R_FILTFREQ, clampi(getRole(p, v, R_FILTFREQ), 70, 127));
		setRole(p, v, R_RESO,     clampi(getRole(p, v, R_RESO), 0, 60));
		setRole(p, v, R_FMAMT,    clampi(getRole(p, v, R_FMAMT), 0, 40));
		setRole(p, v, R_DECIM,    clampi(getRole(p, v, R_DECIM), 100, 127));
		setRole(p, v, R_AEGA,     clampi(getRole(p, v, R_AEGA), 0, 6));
	}

	// 7. level: mix balance from the loudness model
	return setLoudness(p, v, a);
}

//---- LFO (MOD on) ---------------------------------------------------------
static int8_t findLfoSlot(const uint8_t* p, uint8_t v)
{
	const uint8_t first = v * CV_SLOTS_PER_VOICE;
	const uint8_t ownSrc = CV_SRC_LFO1 + v;
	int8_t freeSlot = -1, lfoSlot = -1;

	for(uint8_t i=0;i<CV_SLOTS_PER_VOICE;i++)
	{
		const uint8_t s = first + i;
		const uint8_t src = p[PAR_CV_SRC_1 + s];
		const uint8_t amt = p[PAR_CV_MOD_AMT_1 + s];
		if(amt && src == ownSrc) return (int8_t)s;					// our route from the last click
		if(!amt && freeSlot < 0) freeSlot = (int8_t)s;
		if(amt && src >= CV_SRC_LFO1 && src < CV_SRC_LFO1+6 && lfoSlot < 0) lfoSlot = (int8_t)s;
		// amt && cv1..cv5 or accent: user routing, never touched
	}
	return freeSlot >= 0 ? freeSlot : lfoSlot;
}

static int8_t applyLfo(uint8_t* p, uint8_t v, uint8_t a, const GenCtx* g, Rng* r)
{
	const RndArch* A = &arch[a];
	const uint8_t prof = A->lfoProf[g->type];
	if(prof == LP_NONE) return -1;

	uint8_t role = A->lfoTgt[g->type];
	if(rolePar[v][role] == NA) role = R_FILTFREQ;	// e.g. a bell recipe on the metal engine
	// COA on: no pitch movement (would leave the key) -> filter instead
	if(g->key >= 0 && (role == R_COARSE || role == R_FINE ||
		(role == R_FMFREQ && getRole(p, v, R_MIXMOD) == 1)))
		role = R_FILTFREQ;
	const int16_t par = rolePar[v][role];
	if(par == NA) return -1;
	const uint8_t target = paramToModTarget[par];
	if(!target) return -1;

	const int8_t slot = findLfoSlot(p, v);
	if(slot < 0) return -1;

	const uint8_t exp = g->type == RND_TYPE_EXP;
	int wave, rate, depth, retrig = 0;
	switch(prof)
	{
	default:
	case LP_DRIFT:
		wave  = rng_u(r) < 0.5f ? LW_SIN : LW_TRI;
		rate  = exp ? rng_int(r, 10, 26) : rng_int(r, 8, 20);		// ~0.1 .. 1.5 Hz
		depth = exp ? rng_int(r, 18, 36) : rng_int(r, 10, 22);
		break;
	case LP_GROOVE:
		wave  = (rng_u(r) < 0.5f) ? LW_TRI : ((rng_u(r) < 0.5f) ? LW_SUP : LW_SDN);
		rate  = exp ? rng_int(r, 22, 36) : rng_int(r, 20, 32);		// ~0.9 .. 4.5 Hz
		depth = exp ? rng_int(r, 16, 34) : rng_int(r, 10, 24);
		retrig = 1;													// restarted by the kick
		break;
	case LP_ENV:
	{
		wave  = rng_u(r) < 0.7f ? LW_SDN : LW_XDN;
		// one falling ramp per hit: period ~ 1.5x the amp decay
		const float ms = decayMs((uint8_t)getRole(p, v, R_AEGD));
		rate  = lfoByteFromHz(1000.f / (1.5f * ms)) + rng_int(r, -2, 2);
		depth = exp ? rng_int(r, 26, 44) : rng_int(r, 18, 30);
		retrig = v + 1;												// restarted by the own voice
		break;
	}
	case LP_SH:
		wave  = LW_RND;
		rate  = rng_int(r, 26, 40);									// ~2 .. 7 Hz
		depth = rng_int(r, 14, 30);
		break;
	}

	// depth limits per target: decay, modulator pitch and PWM are sensitive
	int depthMax = CV_AMT_MAX;
	if(role == R_AEGD)   depthMax = 20;
	if(role == R_FMFREQ) depthMax = 10;
	if(role == R_PWM)    depthMax = 30;
	if(role == R_MODEG)  depthMax = 24;
	if(role == R_NOISEMIX) depthMax = 30;
	depth = (int)lrintf(depth * (0.5f + 0.5f * g->amt));
	depth = clampi(depth, 1, depthMax);	// 0 = slot skipped by CV_process

	// the voice's own LFO
	p[PAR_WAVE_LFO1 + v]      = (uint8_t)wave;
	p[PAR_FREQ_LFO1 + v]      = (uint8_t)clampi(rate, 1, 127);
	p[PAR_RETRIGGER_LFO1 + v] = (uint8_t)retrig;
	p[PAR_SYNC_LFO1 + v]      = 0;			// no sync on the eurorack
	p[PAR_OFFSET_LFO1 + v]    = 0;
	p[PAR_VOICE_LFO1 + v]     = v + 1;

	// route: lf(v) -> target
	p[PAR_CV_SRC_1 + slot]     = CV_SRC_LFO1 + v;
	p[PAR_CV_DEST_1 + slot]    = target;
	p[PAR_CV_MOD_AMT_1 + slot] = (uint8_t)depth;
	return slot;
}

//---- main -----------------------------------------------------------------
void Randomizer_generate(uint8_t* p, const RandomizerConfig* cfg, uint32_t seed, RandomizerResult* res)
{
	Rng r;
	rng_seed(&r, seed);

	GenCtx g;
	g.type = cfg->type == RND_TYPE_EXP ? RND_TYPE_EXP : RND_TYPE_CLS;
	g.hz   = cfg->hz <= RND_HZ_HARD ? cfg->hz : RND_HZ_SOFT;
	g.key  = (cfg->key >= 0 && cfg->key < 12) ? cfg->key : RND_KEY_OFF;
	g.amt  = g.type == RND_TYPE_EXP ? RND_SPREAD_EXP : RND_SPREAD_CLS;

	const Pool* pools = g.type == RND_TYPE_EXP ? poolExp : poolCls;

	// archetypes: no doubles between neighbours (V2/V3, V4/V5, V5/V6)
	uint8_t a[6];
	for(uint8_t v=0; v<6; v++)
	{
		uint8_t avoid = 0xff;
		if(v == 2 || v == 4 || v == 5) avoid = a[v-1];
		a[v] = pickFromPool(&r, &pools[v], g.amt, avoid);
	}

	for(uint8_t v=0; v<6; v++)
	{
		if(res) { res->arch[v] = a[v]; res->lfoSlot[v] = -1; }
		if(cfg->lockMask & (1u << v))
		{
			if(res) res->arch[v] = RND_A_LOCKED;
			continue;
		}

		// spacing in the low end, also against locked voices:
		// bass >= kick + 3 semitones, a tonal V3 >= bass + 5 while V2 is a bass
		int floorNote = 0;
		if(v == 1 && (arch[a[v]].flags & F_BASSREG)) floorNote = p[PAR_COARSE1] + 3;
		if(v == 2 && (arch[a[v]].flags & F_TONAL) && p[PAR_COARSE2] <= 52) floorNote = p[PAR_COARSE2] + 5;
		// a variant that stays far below its target even at full VOL (an FM
		// ratio / band-pass / PWM combination that almost cancels) is rolled again
		for(uint8_t tries=0; tries<4; tries++)
			if(applyVoice(p, v, a[v], &g, &r, floorNote) > -RND_TOO_QUIET_DB) break;

		// bass follows the kick's pitch sweep (desktop tool: a long sweep
		// already fills the low end, the bass steps down) - not with COA,
		// there the key keeps the two together
		if(v == 1 && g.key < 0 && a[v] == RND_A_BASS)
		{
			const int drop = clampi((int)lrintf((p[PAR_MODAMNT1] - 26) / 7.f), 0, 7);
			int floorNote = p[PAR_COARSE1] + 3;
			if(g.hz == RND_HZ_HARD && floorNote < arch[a[v]].bandLo)
				floorNote = arch[a[v]].bandLo;		// real band separation wins
			const int c0 = getRole(p, v, R_COARSE);
			int c1 = c0 - drop;
			if(c1 < floorNote) c1 = floorNote;
			if(c1 > c0) c1 = c0;
			setRole(p, v, R_COARSE, c1);
			if(arch[a[v]].fmInt[0])	// a relative modulator moves with the note
				setRole(p, v, R_FMFREQ, getRole(p, v, R_FMFREQ) + (c1 - c0));
		}
	}

	for(uint8_t v=0; v<6; v++)
	{
		if(cfg->lockMask & (1u << v)) continue;
		if(cfg->mod)
		{
			const int8_t s = applyLfo(p, v, a[v], &g, &r);
			if(res) res->lfoSlot[v] = s;
		}
		else
		{
			// MOD off: a regenerated voice keeps no LFO movement from an earlier
			// click (LFO routes in its own slots go); cv1..cv5 / accent stay
			for(uint8_t i=0;i<CV_SLOTS_PER_VOICE;i++)
			{
				const uint8_t s = v * CV_SLOTS_PER_VOICE + i;
				const uint8_t src = p[PAR_CV_SRC_1 + s];
				if(p[PAR_CV_MOD_AMT_1 + s] && src >= CV_SRC_LFO1 && src < CV_SRC_LFO1+6)
				{
					p[PAR_CV_MOD_AMT_1 + s] = 0;
					p[PAR_CV_DEST_1 + s] = 0;
				}
			}
		}
	}
}

const char* Randomizer_archName(uint8_t archId)
{
	if(archId < RND_A_COUNT) return arch[archId].name;
	return "---";
}
