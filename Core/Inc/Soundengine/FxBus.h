/*
 * FxBus.h
 *
 *  Created on: 26.08.2020
 *      Author: julian
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#ifndef INC_SOUNDENGINE_FXBUS_H_
#define INC_SOUNDENGINE_FXBUS_H_

#include "Soundengine/globals.h"
#include "Soundengine/config.h"
#include "Soundengine/DSPAudio/FX/Distortion.h"
#include "Soundengine/DSPAudio/FX/Ringmod.h"
#include "Soundengine/DSPAudio/FX/Compressor.h"
#include "Soundengine/DSPAudio/FX/Delay.h"
#include "Soundengine/DSPAudio/FX/Reverb.h"
#include "Soundengine/DSPAudio/FX/DjFilter.h"

#define USE_REVERB 0

#if USE_REVERB
/* The DJ filter reuses the reverb's menu/kit slot (PAR_FX_REVERB_TYPE) for its
 * 12/24 dB switch, and the FX sub page index 5. Re-enabling the reverb needs
 * those to be split up again. */
#error "USE_REVERB conflicts with FX_DJFILTER (shared PAR_FX_REVERB_TYPE slot)"
#endif

enum FxTypeEnum
{
	FX_OFF,
	FX_DISTORTION,
	FX_RING,
	FX_COMPRESSOR,
	FX_DELAY,
	FX_DJFILTER,	// bipolar DJ filter. index 5 = FX sub page 5 in menuPages.h
#if USE_REVERB
	FX_REVERB
#endif
};

typedef struct FxStruct
{

	uint8_t fxType;
	uint8_t audioRouting;
	float dryWet;

	float parameter[4];


	MasterDistortion distortion;
	Ringmod ringmod;
	Compressor compressor;
	Delay delay;
	DjFilter djFilter;



#if USE_REVERB
	Reverb reverb;
#endif

} FxBus;

extern INDTCMZ FxBus fxBus;

void FxBus_init();
void FxBus_process(int16_t *fxBuffer, int16_t* outL,int16_t* outR,int16_t* outL2, int16_t* outR2, uint32_t size);
void FxBus_setParameter(uint8_t idx, float value);
void FxBus_updateParameter();
//to prevent NaN pollution from delay
void FxBus_prepareFxChange();


#endif /* INC_SOUNDENGINE_FXBUS_H_ */
