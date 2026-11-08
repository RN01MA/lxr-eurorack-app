/*
 * DjFilter.h
 *
 * Bipolar DJ filter for the FX bus (FX type "DJf").
 * CUT below centre = low-pass sweep, centre dead zone = bypass,
 * CUT above centre = high-pass sweep. 12 dB (1 SVF stage) or
 * 24 dB (2 cascaded stages). Reuses the voice SVF (SVF_calcBlockZDF),
 * no new DSP.
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#ifndef INC_SOUNDENGINE_DSPAUDIO_FX_DJFILTER_H_
#define INC_SOUNDENGINE_DSPAUDIO_FX_DJFILTER_H_

#include <stdint.h>
#include "Soundengine/DSPAudio/ResonantFilter.h"

//---- tuning ------------------------------------------------------------
// Sweep end points. The two "open" values sit at the edges of the dead zone
// and are chosen so entering/leaving bypass is (nearly) inaudible: the SVF at
// RES 0 is heavily damped, its -3 dB point is well away from the set cutoff.
// #ifndef so they can be tuned from the build (-D) while listening.
#ifndef DJF_LP_CLOSED_HZ
#define DJF_LP_CLOSED_HZ	20.0f		// CUT 0   : low-pass fully closed
#endif
#ifndef DJF_LP_OPEN_HZ
#define DJF_LP_OPEN_HZ		19860.0f	// CUT 59  : = SVF stability clamp 0.45*FS
#endif
#ifndef DJF_HP_OPEN_HZ
#define DJF_HP_OPEN_HZ		3.0f		// CUT +1 (with the curve): 11 Hz, 40 Hz loses ~1 dB at 24 dB
#endif
#ifndef DJF_HP_THIN_HZ
#define DJF_HP_THIN_HZ		18000.0f	// CUT 127 : high-pass, only top end left (spec open question: 8-18k)
#endif

// Knob curve. Plain exponential (1.0) spends the first ~8 steps next to the
// zone in the inaudible top octave (LP) / sub-sonic range (HP). An exponent < 1
// moves the cutoff faster right after the zone and finer towards the far end.
// Tuned on the host: the change is audible from step 3-4 next to the zone
// (LP: top end -3 dB at -4, HP: lows -4.5 dB at +4), step 1 stays gentle.
#ifndef DJF_LP_CURVE
#define DJF_LP_CURVE		0.65f		// -4: 6.6 kHz, -30: 250 Hz, -50: 44 Hz
#endif
#ifndef DJF_HP_CURVE
#define DJF_HP_CURVE		0.4f		// +4: 79 Hz, +20: 1 kHz, +50: 10 kHz
#endif

// The SVF low-pass output goes through fastTanh, whose small-signal gain is
// 4.15/4.29 (-0.29 dB). Compensated per LP stage so bypass <-> LP is level matched.
#define DJF_LP_MAKEUP		(4.29f/4.15f)

// State guard (safety net behind the damping floor below). With loud input and
// low damping at high cutoff the SVF integrators run away (softClipTwo grows
// ~x/15, it does not saturate) into inf/NaN or a Nyquist limit cycle, and the
// output goes silent. The NaN check in ResonantFilter.c is removed by
// -ffast-math. The DJ filter runs the SVF in short sub blocks and restarts a
// stage whose state is non-finite or beyond the limit (bit-level test that
// -ffast-math cannot optimise away).
#ifndef DJF_SUBBLOCK
#define DJF_SUBBLOCK		8			// samples per SVF call
#endif
#ifndef DJF_STATE_LIMIT
#define DJF_STATE_LIMIT		8.0f		// |s1|,|s2| beyond this = run-away (normal < ~3)
#endif

// RES 0..127 -> SVF damping q 1.0..0.02, continuous. (SVF_setReso jumps from
// q 0.1 to 0.02 at RES 115, leaving RES 115..127 without effect.)
#define DJF_Q_MIN			0.02f

// High-cutoff damping floor. Above ~11 kHz the nonlinear SVF falls into a
// full-scale limit cycle at Nyquist (inaudible = output goes silent) once the
// damping q is low and the bus is loud. Mapped on hardware-equivalent host
// tests (drums / noise at 0 dBFS, 12 and 24 dB, LP and HP):
//   unstable when q < 0.15 @12.4k, < 0.25 @13.2k, < 0.35 @14-15k,
//                 < 0.5 @15.9-17k, < 0.7 @17.7-19.4k
// The floor rises linearly from DJF_QFLOOR_START_HZ and sits well above that
// boundary: resonance fades out in the top octave, below it RES is untouched.
#ifndef DJF_QFLOOR_START_HZ
#define DJF_QFLOOR_START_HZ	10000.0f
#endif
#ifndef DJF_QFLOOR_SLOPE
#define DJF_QFLOOR_SLOPE	(0.9f/9000.0f)	// +0.1 q per kHz
#endif
// CUT knob layout, symmetric over the 128 values:
//   0 .. 59   low-pass  (60 values, display -60 .. -1)
//  60 .. 67   bypass    ( 8 values, display  0)
//  68 .. 127  high-pass (60 values, display   1 .. 60)
#define DJF_ZONE_FIRST	60			// first bypass value
#define DJF_ZONE_LAST	67			// last bypass value
#define DJF_CENTER		64			// CUT set when DJf is selected (inside the zone)

// defaults set when DJf is selected on the TYP knob (not on kit load, so
// kits saved with DJf keep their own values)
#define DJF_RES_DEFAULT	70			// ~+3 dB at the cutoff with 24 dB: audible, no whistle

// stored menu values (kit file)
#define DJF_STATE_ON	0			// "on"  - default for kits that never used DJf
#define DJF_STATE_OFF	1			// "off" - bus passes through dry
#define DJF_SLOPE_12	0
#define DJF_SLOPE_24	1

typedef struct DjFilterStruct
{
	ResonantFilter stage[2][2];	// [channel L/R][cascade 1/2]

	float cut;					// 0..1 (CUT/127, may be modulated)
	float reso;					// 0..1 (RES/127)
	float hz;					// current cutoff [Hz] (LP or HP)

	uint8_t mode;				// FILTER_LP, FILTER_HP or FILTER_OFF (dead zone)
	uint8_t enabled;			// 1 = filter active, 0 = bypass (DJF param)
	uint8_t slope24;			// 0 = 12 dB/oct, 1 = 24 dB/oct

	uint8_t lastConfig;			// mode/slope/enabled used for the previous block
	uint8_t stateDirty;			// set when the filter states must be cleared
} DjFilter;

void DjFilter_init(DjFilter* djf);
void DjFilter_setCut(DjFilter* djf, float cut01);
void DjFilter_setReso(DjFilter* djf, float reso01);
void DjFilter_setState(DjFilter* djf, uint8_t menuValue);	// DJF_STATE_ON / DJF_STATE_OFF
void DjFilter_setSlope(DjFilter* djf, uint8_t menuValue);	// DJF_SLOPE_12 / DJF_SLOPE_24
// mark filter states stale (FX type switched away, etc.)
void DjFilter_markDirty(DjFilter* djf);

// true if a CUT value (0..127) is inside the bypass dead zone
static inline uint8_t DjFilter_isCutBypass(uint8_t cut127)
{
	return (cut127 >= DJF_ZONE_FIRST) && (cut127 <= DJF_ZONE_LAST);
}

// CUT as shown on the display: -60..-1 (LP), 0 (bypass), 1..60 (HP)
static inline int8_t DjFilter_cutDisplayValue(uint8_t cut127)
{
	if(cut127 < DJF_ZONE_FIRST)	return (int8_t)((int16_t)cut127 - DJF_ZONE_FIRST);	// 0 -> -60, 59 -> -1
	if(cut127 > DJF_ZONE_LAST)	return (int8_t)((int16_t)cut127 - DJF_ZONE_LAST);	// 68 -> 1, 127 -> 60
	return 0;
}

/* fxBuffer: interleaved stereo FX bus (L,R,L,R...), size frames.
 * out: interleaved output (stereo when channels == 2, otherwise every 2nd
 * sample, like the other FX). The result is added (saturating) to out. */
void DjFilter_process(DjFilter* djf, int16_t* fxBuffer, int16_t* out, uint16_t size, uint8_t channels);

#endif /* INC_SOUNDENGINE_DSPAUDIO_FX_DJFILTER_H_ */
