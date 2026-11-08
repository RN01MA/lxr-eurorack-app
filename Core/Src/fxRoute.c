/*
 * fxRoute.c
 *
 * Hot key "voice -> FX bus", see fxRoute.h.
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#include "fxRoute.h"
#include "Parameters.h"
#include "menu/menu.h"
#include "frontPanelParser_ui.h"
#include "front.h"
#include "Preset/PresetManager.h"
#include "Soundengine/DSPAudio/mixer.h"
#include <string.h>

#define NO_PREV		0xff

static uint8_t fxr_active;
static uint8_t fxr_prev[6] = { NO_PREV, NO_PREV, NO_PREV, NO_PREV, NO_PREV, NO_PREV };

// set a voice's audio out the way the MIX page encoder does: parameter,
// engine (CC2_AUDIO_OUTx -> mixer_audioRouting), kit autosave
static void setAudioOut(uint8_t v, uint8_t out)
{
	const uint16_t par = (uint16_t)(PAR_AUDIO_OUT1 + v);
	activeParameters[par] = out;
	frontPanel_sendData(CC_2, (uint8_t)(par - 128), out);
	preset_touchTimer(PRESET_KIT_TIMER);
}

void fxRoute_fxPressed(uint8_t shiftHeld)
{
	fxr_active = shiftHeld ? 1 : 0;
	if(fxr_active) menu_repaintAll();
}

void fxRoute_fxReleased()
{
	if(fxr_active)
	{
		fxr_active = 0;
		menu_repaintAll();
	}
}

uint8_t fxRoute_isActive()
{
	return fxr_active;
}

void fxRoute_voicePressed(uint8_t voiceNr)
{
	if(voiceNr < 1 || voiceNr > 7) return;
	const uint8_t v = voiceNr >= 7 ? 5 : (uint8_t)(voiceNr - 1);
	const uint8_t cur = activeParameters[PAR_AUDIO_OUT1 + v];

	if(cur != MIXER_ROUTING_FX_BUS)
	{
		fxr_prev[v] = cur;							// remember where it came from
		setAudioOut(v, MIXER_ROUTING_FX_BUS);
	}
	else
	{
		const uint8_t back = (fxr_prev[v] != NO_PREV && fxr_prev[v] != MIXER_ROUTING_FX_BUS)
							 ? fxr_prev[v] : MIXER_ROUTING_DAC1_STEREO;	// unknown -> St1
		setAudioOut(v, back);
	}
	menu_repaintAll();
}

uint8_t fxRoute_paint(char buf[2][17])
{
	if(!fxr_active) return 0;

	// "FX bus: + voice "
	// "1 2 - - 5 -     "
	memset(buf, ' ', 2*17);
	memcpy(&buf[0][0], "FX bus: + voice", 15);
	for(uint8_t v=0; v<6; v++)
		buf[1][v*2] = (activeParameters[PAR_AUDIO_OUT1 + v] == MIXER_ROUTING_FX_BUS) ? (char)('1' + v) : '-';
	buf[0][16] = buf[1][16] = 0;
	return 1;
}
