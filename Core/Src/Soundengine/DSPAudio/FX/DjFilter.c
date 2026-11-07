/*
 * DjFilter.c
 *
 * Bipolar DJ filter for the FX bus. See DjFilter.h.
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#include "Soundengine/DSPAudio/FX/DjFilter.h"
#include "Soundengine/config.h"
#include <math.h>
#include <string.h>

#ifdef DJF_HOST_TEST
int djf_guardTrips = 0;			// host test only: counts state-guard restarts
#endif


// dead zone edges on the continuous 0..127 scale. Integer CUT 60..67 is
// bypass; the .5 keeps modulated (non-integer) values consistent with that.
#define DJF_ZONE_LO		((float)DJF_ZONE_FIRST - 0.5f)	// 59.5
#define DJF_ZONE_HI		((float)DJF_ZONE_LAST + 0.5f)	// 67.5

// mode/slope/enabled packed into one byte, to detect changes between blocks
#define DJF_CONFIG(mode, slope24, enabled)	((uint8_t)((mode) | ((slope24)<<4) | ((enabled)<<5)))

//------------------------------------------------------------------------------
// true for inf/NaN or a state beyond DJF_STATE_LIMIT. Works on the raw bits:
// with -ffast-math the compiler assumes floats are finite and would delete an
// isnan()/x!=x test (that is what happens to removeMaliciousSample).
static inline uint8_t DjFilter_stateBroken(float x)
{
	uint32_t u;
	memcpy(&u, &x, sizeof(u));
	if((u & 0x7f800000u) == 0x7f800000u)	return 1;
	return (x > DJF_STATE_LIMIT) || (x < -DJF_STATE_LIMIT);
}
//------------------------------------------------------------------------------
// one SVF stage over a block, in sub blocks with the state guard in between.
// Safety net only: the damping floor (DjFilter_applyQ) keeps the filter in its
// stable region. A run-away stage is restarted from zero (a short click) rather
// than clamped: a clamped state can latch at the limit and hold the filter in a
// silent Nyquist oscillation.
static void DjFilter_runStage(ResonantFilter* flt, uint8_t mode, int16_t* buf, uint16_t size)
{
	for(uint16_t off=0; off<size; off+=DJF_SUBBLOCK)
	{
		const uint16_t n = (size-off < DJF_SUBBLOCK) ? (uint16_t)(size-off) : DJF_SUBBLOCK;
		SVF_calcBlockZDF(flt, mode, buf+off, (uint8_t)n);
		if(DjFilter_stateBroken(flt->s1) || DjFilter_stateBroken(flt->s2))
		{
			flt->s1 = 0.f;
			flt->s2 = 0.f;
#ifdef DJF_HOST_TEST
			djf_guardTrips++;
#endif
		}
	}
}
//------------------------------------------------------------------------------
static void DjFilter_resetStates(DjFilter* djf)
{
	for(uint8_t c=0;c<2;c++)
		for(uint8_t s=0;s<2;s++)
			SVF_reset(&djf->stage[c][s]);
}
//------------------------------------------------------------------------------
// damping actually used: the RES setting, but never below the stability floor
// for the current cutoff (see DJF_QFLOOR_* in DjFilter.h)
static void DjFilter_applyQ(DjFilter* djf)
{
	float q = 1.f - djf->reso*(1.f - DJF_Q_MIN);	// RES 0..127 -> q 1.0..0.02, continuous
	const float floorQ = (djf->hz - DJF_QFLOOR_START_HZ) * DJF_QFLOOR_SLOPE;
	if(q < floorQ)	q = floorQ;
	if(q > 1.f)		q = 1.f;

	for(uint8_t c=0;c<2;c++)
		for(uint8_t s=0;s<2;s++)
			djf->stage[c][s].q = q;
}
//------------------------------------------------------------------------------
// target cutoff in Hz -> normalised f + integrator gain
static void DjFilter_setHz(DjFilter* djf, float hz)
{
	float fn = hz / REAL_FS;			// 44140 Hz
	if(fn > 0.45f) 		fn = 0.45f;		// SVF stability clamp
	if(fn < 0.0001f) 	fn = 0.0001f;		// 4.4 Hz, lets the HP start below the sub range

	for(uint8_t c=0;c<2;c++)
		for(uint8_t s=0;s<2;s++)
		{
			djf->stage[c][s].f = fn;
			SVF_recalcFreq(&djf->stage[c][s]);
		}
}
//------------------------------------------------------------------------------
void DjFilter_init(DjFilter* djf)
{
	memset(djf, 0, sizeof(DjFilter));

	for(uint8_t c=0;c<2;c++)
		for(uint8_t s=0;s<2;s++)
		{
			SVF_init(&djf->stage[c][s]);
			djf->stage[c][s].drive = 1.0f;	// unity input level into the SVF
		}
	DjFilter_setReso(djf, 0.f);

	djf->enabled 	= 1;
	djf->slope24 	= 0;
	djf->stateDirty = 1;
	DjFilter_setCut(djf, DJF_CENTER/127.f);
}
//------------------------------------------------------------------------------
/* Bipolar CUT:
 *   0 .. 59   low-pass,  DJF_LP_CLOSED_HZ .. DJF_LP_OPEN_HZ, exponential
 *  60 .. 67   bypass (dead zone)
 *  68 .. 127  high-pass, DJF_HP_OPEN_HZ .. DJF_HP_THIN_HZ, exponential
 * Each half is normalised to the edge of the dead zone, so there is no jump
 * in cutoff when the knob enters or leaves the zone. */
void DjFilter_setCut(DjFilter* djf, float cut01)
{
	if(cut01 < 0.f) cut01 = 0.f;
	if(cut01 > 1.f) cut01 = 1.f;
	djf->cut = cut01;

	const float c = cut01 * 127.f;
	float hz;

	if(c >= DJF_ZONE_LO && c <= DJF_ZONE_HI)
	{
		djf->mode = FILTER_OFF;
		return;
	}
	else if(c < DJF_ZONE_LO)
	{
		const float u = c / DJF_ZONE_LO;							// 0..1
		hz = DJF_LP_CLOSED_HZ * powf(DJF_LP_OPEN_HZ/DJF_LP_CLOSED_HZ, u);
		djf->mode = FILTER_LP;
	}
	else
	{
		const float w = (c - DJF_ZONE_HI) / (127.f - DJF_ZONE_HI);	// 0..1
		hz = DJF_HP_OPEN_HZ * powf(DJF_HP_THIN_HZ/DJF_HP_OPEN_HZ, w);
		djf->mode = FILTER_HP;
	}
	djf->hz = hz;
	DjFilter_setHz(djf, hz);
	DjFilter_applyQ(djf);
}
//------------------------------------------------------------------------------
void DjFilter_setReso(DjFilter* djf, float reso01)
{
	if(reso01 < 0.f) reso01 = 0.f;
	if(reso01 > 1.f) reso01 = 1.f;
	djf->reso = reso01;
	DjFilter_applyQ(djf);
}
//------------------------------------------------------------------------------
void DjFilter_setState(DjFilter* djf, uint8_t menuValue)
{
	djf->enabled = (menuValue == DJF_STATE_OFF) ? 0 : 1;
}
//------------------------------------------------------------------------------
void DjFilter_setSlope(DjFilter* djf, uint8_t menuValue)
{
	djf->slope24 = (menuValue == DJF_SLOPE_24) ? 1 : 0;
}
//------------------------------------------------------------------------------
void DjFilter_markDirty(DjFilter* djf)
{
	djf->stateDirty = 1;
}
//------------------------------------------------------------------------------
static void DjFilter_addDry(const int16_t* fxBuffer, int16_t* out, uint16_t size, uint8_t channels)
{
	// identical to the FX_OFF path in FxBus_process
	if(channels == 2)
	{
		const int32_t* stereoIn = (const int32_t*)fxBuffer;
		int32_t* stereoOut = (int32_t*)out;
		for(uint16_t i=0;i<size;i++)
		{
			*stereoOut = __QADD16(*stereoOut, *stereoIn);
			stereoOut++;
			stereoIn++;
		}
	}
	else
	{
		for(uint16_t i=0;i<size;i++)
		{
			*out = (int16_t)__SSAT((int32_t)*out + fxBuffer[i*2], 16);
			out += 2;
		}
	}
}
//------------------------------------------------------------------------------
void DjFilter_process(DjFilter* djf, int16_t* fxBuffer, int16_t* out, uint16_t size, uint8_t channels)
{
	// sample the control state once per block (it may be changed from the parser)
	const uint8_t mode 		= djf->mode;
	const uint8_t slope24 	= djf->slope24;
	const uint8_t enabled 	= djf->enabled;

	const uint8_t config = DJF_CONFIG(mode, slope24, enabled);
	if(config != djf->lastConfig)
	{
		// LP <-> bypass <-> HP, 12 <-> 24 dB, on <-> off: the integrator
		// states belong to a different filter now -> start clean (no thump
		// from stale state when e.g. a resonant LP jumps over to HP)
		djf->stateDirty = 1;
		djf->lastConfig = config;
	}

	if(!enabled || mode == FILTER_OFF || size > OUTPUT_DMA_SIZE)
	{
		DjFilter_addDry(fxBuffer, out, size, channels);
		return;
	}

	if(djf->stateDirty)
	{
		DjFilter_resetStates(djf);
		djf->stateDirty = 0;
	}

	// de-interleave the stereo bus, the SVF works on contiguous blocks
	int16_t bufL[OUTPUT_DMA_SIZE];
	int16_t bufR[OUTPUT_DMA_SIZE];
	for(uint16_t i=0;i<size;i++)
	{
		bufL[i] = fxBuffer[i*2];
		bufR[i] = fxBuffer[i*2+1];
	}

	DjFilter_runStage(&djf->stage[0][0], mode, bufL, (uint16_t)size);
	if(slope24) DjFilter_runStage(&djf->stage[0][1], mode, bufL, (uint16_t)size);
	if(channels == 2)
	{
		DjFilter_runStage(&djf->stage[1][0], mode, bufR, (uint16_t)size);
		if(slope24) DjFilter_runStage(&djf->stage[1][1], mode, bufR, (uint16_t)size);
	}

	// level-match the low-pass to bypass (fastTanh small-signal gain, per stage)
	if(mode == FILTER_LP)
	{
		const float makeup = slope24 ? DJF_LP_MAKEUP*DJF_LP_MAKEUP : DJF_LP_MAKEUP;
		for(uint16_t i=0;i<size;i++)
		{
			bufL[i] = (int16_t)__SSAT((int32_t)(bufL[i]*makeup), 16);
			bufR[i] = (int16_t)__SSAT((int32_t)(bufR[i]*makeup), 16);
		}
	}

	if(channels == 2)
	{
		int32_t* stereoOut = (int32_t*)out;
		for(uint16_t i=0;i<size;i++)
		{
			const int32_t x = __PKHBT(bufL[i], bufR[i], 16);
			*stereoOut = __QADD16(*stereoOut, x);
			stereoOut++;
		}
	}
	else
	{
		// mono destination: left bus channel, same as the other FX types
		for(uint16_t i=0;i<size;i++)
		{
			*out = (int16_t)__SSAT((int32_t)*out + bufL[i], 16);
			out += 2;
		}
	}
}
