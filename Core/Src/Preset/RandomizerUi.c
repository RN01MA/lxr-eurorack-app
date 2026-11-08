/*
 * RandomizerUi.c
 *
 * Front panel side of the kit randomizer, see RandomizerUi.h.
 * Runs in the main loop context (button handler, menu), like kit loading.
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#include "Preset/RandomizerUi.h"
#include "Preset/Randomizer.h"
#include "Preset/PresetManager.h"
#include "menu/menu.h"
#include "ledHandler.h"
#include "buttonHandler.h"
#include "main.h"
#include "Soundengine/globals.h"
#include "Soundengine/DSPAudio/random.h"
#include "Soundengine/MIDI/MidiVoiceControl.h"
#include <Soundengine/TriggerProcessor.h>
#include <string.h>

// systick_ticks: 0.25 ms
#define TICKS_PER_MS		4u
#define BAR_STEP_TICKS		(40u * TICKS_PER_MS)	// bar over the 7 voice buttons: 7 x 40 ms
#define BAR_HOLD_TICKS		(120u * TICKS_PER_MS)	// full bar stays a moment
#define PERF_HOLD_TICKS		(450u * TICKS_PER_MS)	// PERF held this long -> recipe view

#define ALL_VOICES			0x3f
#define BAR_STEPS			7						// buttons 2..8 (V1..V6 + open hihat)

static uint8_t  rnd_latched;		// randomizer mode switched on (SHIFT+PERF)
static uint8_t  rnd_active;			// randomizer page shown
static uint8_t  rnd_voiceDown;		// voice buttons held, bit = button 1..7

static uint8_t  rnd_barStep;		// 0 = idle, 1..7 = bar up to button n, 8 = hold
static uint32_t rnd_barTime;
static uint8_t  rnd_barKeep;		// voices kept by this click (not lit by the bar)

static uint8_t  rnd_perfDown;
static uint32_t rnd_perfTime;
static uint8_t  rnd_showRecipes;	// PERF held: recipe per voice on the display

static uint8_t  rnd_haveResult;
static RandomizerResult rnd_result;
static RandomizerConfig rnd_lastCfg;
static uint32_t rnd_counter;

static const char rnd_keyText[12][3] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

//--------------------------------------------------------------------------
// held voice buttons -> voices to keep (button 7 = open hihat = V6)
static uint8_t heldVoices()
{
	uint8_t m = 0;
	for(uint8_t b=1; b<=7; b++)
		if(rnd_voiceDown & (1u << b))
			m |= (uint8_t)(1u << (b >= 7 ? 5 : b - 1));
	return m;
}

// voice LEDs back to "button held" state
static void showHeld()
{
	for(uint8_t b=1; b<=7; b++)
		led_setValue((rnd_voiceDown >> b) & 1, (uint8_t)(LED_1 + b));
}

static void barLight(uint8_t step)	// step 1..7 = button 2..8
{
	const uint8_t voice = step >= 7 ? 5 : (uint8_t)(step - 1);
	if(!(rnd_barKeep & (1u << voice)))
		led_setValue(1, (uint8_t)(LED_1 + step));
}

static void generate()
{
	RandomizerConfig c;
	c.type     = activeParameters[PAR_RND_TYPE] ? RND_TYPE_EXP : RND_TYPE_CLS;
	c.key      = activeParameters[PAR_RND_KEY] ? (int8_t)(activeParameters[PAR_RND_KEY] - 1) : RND_KEY_OFF;
	c.mod      = activeParameters[PAR_RND_MOD] ? 1 : 0;
	c.hz       = activeParameters[PAR_RND_HZ];
	c.lockMask = heldVoices();		// voices held while DRUM is pressed stay as they are

	if((c.lockMask & ALL_VOICES) == ALL_VOICES) return;	// nothing to do

	// TRNG + time of the press + click counter
	const uint32_t seed = GetRngValue() ^ (systick_ticks * 2654435761u) ^ (++rnd_counter << 20);

	Randomizer_generate(activeParameters, &c, seed, &rnd_result);
	rnd_lastCfg = c;
	rnd_haveResult = 1;

	// apply like a loaded kit; the generated kit is the current edit state
	// (autosaved to the temp kit like any other edit)
	preset_sendDrumsetParameters();
	preset_touchTimer(PRESET_KIT_TIMER);

	rnd_barKeep = c.lockMask;
	rnd_barStep = 1;
	rnd_barTime = systick_ticks;
	barLight(1);
}

//--------------------------------------------------------------------------
void RndUi_setLatched(uint8_t on)
{
	rnd_latched = on ? 1 : 0;
	if(!on)
	{
		rnd_perfDown = 0;
		rnd_showRecipes = 0;
	}
}

uint8_t RndUi_isLatched()
{
	return rnd_latched;
}

void RndUi_enter()
{
	rnd_active = 1;
	rnd_voiceDown = 0;
	rnd_barStep = 0;
	led_clearLeds_1To8();
}

void RndUi_leave()
{
	rnd_active = 0;
	rnd_barStep = 0;
	rnd_showRecipes = 0;
	led_clearLeds_1To8();
}

void RndUi_perfPressed()
{
	rnd_perfDown = 1;
	rnd_perfTime = systick_ticks;
}

void RndUi_perfReleased()
{
	rnd_perfDown = 0;
	if(rnd_showRecipes)
	{
		rnd_showRecipes = 0;
		menu_repaintAll();
	}
}

void RndUi_drumPressed()
{
	generate();		// new kit right on the press
}

void RndUi_drumReleased()
{
}

void RndUi_voicePressed(uint8_t voiceNr)
{
	if(voiceNr < 1 || voiceNr > 7) return;
	rnd_voiceDown |= (uint8_t)(1u << voiceNr);

	// audition the voice (as on the PERF page)
	const uint8_t vel = Trigger_getAccent((uint8_t)(voiceNr - 1));
	voiceControl_noteOn((uint8_t)(voiceNr - 1), SEQ_DEFAULT_NOTE, vel);
	led_setValue(1, (uint8_t)(LED_1 + voiceNr));
}

void RndUi_voiceReleased(uint8_t voiceNr)
{
	if(voiceNr < 1 || voiceNr > 7) return;
	rnd_voiceDown &= (uint8_t)~(1u << voiceNr);
	if(!rnd_barStep) led_setValue(0, (uint8_t)(LED_1 + voiceNr));
}

void RndUi_tick()
{
	if(!rnd_active) return;

	const uint32_t now = systick_ticks;

	if(rnd_barStep)
	{
		if(rnd_barStep <= BAR_STEPS)
		{
			if(now - rnd_barTime >= BAR_STEP_TICKS)
			{
				rnd_barTime = now;
				rnd_barStep++;
				if(rnd_barStep <= BAR_STEPS) barLight(rnd_barStep);
			}
		}
		else if(now - rnd_barTime >= BAR_HOLD_TICKS)
		{
			rnd_barStep = 0;
			showHeld();
		}
	}

	if(rnd_perfDown && !rnd_showRecipes && (now - rnd_perfTime >= PERF_HOLD_TICKS))
	{
		rnd_showRecipes = 1;
		menu_repaintAll();
	}
}

//--------------------------------------------------------------------------
static void put3(char* dst, const char* s)
{
	for(uint8_t i=0; i<3; i++)
	{
		if(!s[i]) break;
		dst[i] = s[i];
	}
}

uint8_t RndUi_paint(char buf[2][17])
{
	if(!rnd_active || !rnd_showRecipes) return 0;

	memset(buf, ' ', 2*17);
	if(!rnd_haveResult)
	{
		memcpy(&buf[0][0], "No new kit yet", 14);
		memcpy(&buf[1][0], "press DRUM", 10);
	}
	else
	{
		// recipe per voice of the last new kit, kept voices = "---"
		// "KIK REE FLD  EXP"
		// "SNR CYM HAT   A#"
		for(uint8_t v=0; v<6; v++)
		{
			const uint8_t a = rnd_result.arch[v];
			put3(&buf[v/3][(v%3)*4], a == RND_A_LOCKED ? "---" : Randomizer_archName(a));
		}
		put3(&buf[0][13], rnd_lastCfg.type == RND_TYPE_EXP ? "EXP" : "CLS");
		if(rnd_lastCfg.key >= 0)
		{
			const char* k = rnd_keyText[(uint8_t)rnd_lastCfg.key];
			put3(&buf[1][k[1] ? 14 : 15], k);
		}
	}
	buf[0][16] = buf[1][16] = 0;
	return 1;
}
