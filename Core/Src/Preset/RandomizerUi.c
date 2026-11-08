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
#define BAR_STEP_TICKS		(45u * TICKS_PER_MS)	// V1->V6 bar: 6 x 45 ms
#define BAR_HOLD_TICKS		(120u * TICKS_PER_MS)	// full bar stays a moment
#define RESULT_TICKS		(2500u * TICKS_PER_MS)	// archetype names on the display
#define ALL_LOCKED_TICKS	(1500u * TICKS_PER_MS)

#define ALL_VOICES			0x3f

static uint8_t  rnd_lockMask;		// bit v = voice v+1 locked (kept while the module runs)
static uint8_t  rnd_voiceDown;		// voice buttons held, bit = button 1..7
static uint8_t  rnd_drumDown;
static uint8_t  rnd_drumUsed;		// DRUM was used for locking while held -> no new kit
static uint8_t  rnd_active;

static uint8_t  rnd_barStep;		// 0 = idle, 1..6 = bar lit up to voice n, 7 = hold
static uint32_t rnd_barTime;

static uint8_t  rnd_showResult;		// 1 = names, 2 = "all locked"
static uint32_t rnd_showTime;
static RandomizerResult rnd_result;
static RandomizerConfig rnd_lastCfg;
static uint32_t rnd_counter;

static const char rnd_keyText[12][3] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

//--------------------------------------------------------------------------
static uint8_t buttonToVoice(uint8_t voiceNr)
{
	return voiceNr >= 7 ? 5 : (uint8_t)(voiceNr - 1);	// open hihat button = V6
}

// voice LEDs: LED_2..LED_7 = V1..V6, LED_8 (open hihat) follows V6
static void showLocks()
{
	for(uint8_t v=0; v<6; v++)
		led_setValue((rnd_lockMask >> v) & 1, (uint8_t)(LED_2 + v));
	led_setValue((rnd_lockMask >> 5) & 1, LED_8);
}

static void toggleLock(uint8_t voiceNr)
{
	rnd_lockMask ^= (uint8_t)(1u << buttonToVoice(voiceNr));
}

static void readConfig(RandomizerConfig* c)
{
	c->type     = activeParameters[PAR_RND_TYPE] ? RND_TYPE_EXP : RND_TYPE_CLS;
	c->key      = activeParameters[PAR_RND_KEY] ? (int8_t)(activeParameters[PAR_RND_KEY] - 1) : RND_KEY_OFF;
	c->mod      = activeParameters[PAR_RND_MOD] ? 1 : 0;
	c->var      = activeParameters[PAR_RND_VAR];
	c->hz       = activeParameters[PAR_RND_HZ];
	c->lockMask = rnd_lockMask;
}

static void generate()
{
	RandomizerConfig c;
	readConfig(&c);

	if((c.lockMask & ALL_VOICES) == ALL_VOICES)
	{
		rnd_showResult = 2;
		rnd_showTime = systick_ticks;
		menu_repaintAll();
		return;
	}

	// TRNG + time of the tap + click counter
	const uint32_t seed = GetRngValue() ^ (systick_ticks * 2654435761u) ^ (++rnd_counter << 20);

	Randomizer_generate(activeParameters, &c, seed, &rnd_result);
	rnd_lastCfg = c;

	// apply like a loaded kit; the generated kit is the current edit state
	// (autosaved to the temp kit like any other edit)
	preset_sendDrumsetParameters();
	preset_touchTimer(PRESET_KIT_TIMER);

	rnd_barStep = 1;
	rnd_barTime = systick_ticks;
	led_setValue(1, LED_2);

	rnd_showResult = 1;
	rnd_showTime = systick_ticks;
	menu_repaintAll();
}

//--------------------------------------------------------------------------
void RndUi_enter()
{
	rnd_active = 1;
	rnd_voiceDown = 0;
	rnd_drumDown = 0;
	rnd_drumUsed = 0;
	rnd_barStep = 0;
	rnd_showResult = 0;
	led_clearLeds_1To8();
	showLocks();
}

void RndUi_leave()
{
	rnd_active = 0;
	rnd_barStep = 0;
	rnd_showResult = 0;
	rnd_drumDown = 0;
	led_clearLeds_1To8();
}

void RndUi_drumPressed()
{
	rnd_drumDown = 1;
	rnd_drumUsed = 0;
	led_setValue(1, LED_1);

	// spec order: voice(s) held, then DRUM -> lock / unlock them
	if(rnd_voiceDown)
	{
		for(uint8_t b=1; b<=7; b++)
			if(rnd_voiceDown & (1u << b)) toggleLock(b);
		if((rnd_voiceDown & (1u << 6)) && (rnd_voiceDown & (1u << 7)))
			toggleLock(6);		// both hihat buttons held: one toggle for V6
		rnd_drumUsed = 1;
		if(!rnd_barStep) showLocks();
	}
	rnd_showResult = 0;
	menu_repaintAll();
}

void RndUi_drumReleased()
{
	led_setValue(0, LED_1);
	const uint8_t tap = rnd_drumDown && !rnd_drumUsed;
	rnd_drumDown = 0;
	rnd_drumUsed = 0;

	if(tap)
		generate();
	else
		menu_repaintAll();
}

void RndUi_voicePressed(uint8_t voiceNr)
{
	if(voiceNr < 1 || voiceNr > 7) return;
	rnd_voiceDown |= (uint8_t)(1u << voiceNr);

	if(rnd_drumDown)
	{
		// DRUM held -> lock / unlock, no trigger
		toggleLock(voiceNr);
		rnd_drumUsed = 1;
		if(!rnd_barStep) showLocks();
		menu_repaintAll();
		return;
	}

	// audition the voice (as on the PERF page)
	const uint8_t vel = Trigger_getAccent((uint8_t)(voiceNr - 1));
	voiceControl_noteOn((uint8_t)(voiceNr - 1), SEQ_DEFAULT_NOTE, vel);
	led_setValue(1, (uint8_t)(LED_1 + voiceNr));
}

void RndUi_voiceReleased(uint8_t voiceNr)
{
	if(voiceNr < 1 || voiceNr > 7) return;
	rnd_voiceDown &= (uint8_t)~(1u << voiceNr);
	if(!rnd_barStep) showLocks();
}

void RndUi_tick()
{
	if(!rnd_active) return;

	const uint32_t now = systick_ticks;

	if(rnd_barStep)
	{
		if(rnd_barStep <= 6)
		{
			if(now - rnd_barTime >= BAR_STEP_TICKS)
			{
				rnd_barTime = now;
				rnd_barStep++;
				if(rnd_barStep <= 6) led_setValue(1, (uint8_t)(LED_1 + rnd_barStep));
			}
		}
		else if(now - rnd_barTime >= BAR_HOLD_TICKS)
		{
			rnd_barStep = 0;
			showLocks();
		}
	}

	if(rnd_showResult)
	{
		const uint32_t len = rnd_showResult == 1 ? RESULT_TICKS : ALL_LOCKED_TICKS;
		if(now - rnd_showTime >= len)
		{
			rnd_showResult = 0;
			menu_repaintAll();
		}
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
	if(!rnd_active) return 0;

	if(rnd_drumDown)
	{
		// lock view while DRUM is held
		memset(buf, ' ', 2*17);
		memcpy(&buf[0][0], "Lock: + voice", 13);
		for(uint8_t v=0; v<6; v++)
			buf[1][v*2] = ((rnd_lockMask >> v) & 1) ? (char)('1' + v) : '-';
		memcpy(&buf[1][13], "new", 3);	// release alone = new kit
		buf[0][16] = buf[1][16] = 0;
		return 1;
	}

	if(rnd_showResult == 2)
	{
		memset(buf, ' ', 2*17);
		memcpy(&buf[0][0], "All voices", 10);
		memcpy(&buf[1][0], "locked", 6);
		buf[0][16] = buf[1][16] = 0;
		return 1;
	}

	if(rnd_showResult == 1)
	{
		// "KIK REE FLD  EXP"
		// "SNR CYM HAT   A#"
		memset(buf, ' ', 2*17);
		for(uint8_t v=0; v<6; v++)
		{
			const uint8_t a = rnd_result.arch[v];
			put3(&buf[v/3][(v%3)*4], a == RND_A_LOCKED ? "lck" : Randomizer_archName(a));
		}
		put3(&buf[0][13], rnd_lastCfg.type == RND_TYPE_EXP ? "EXP" : "CLS");
		if(rnd_lastCfg.key >= 0)
		{
			const char* k = rnd_keyText[(uint8_t)rnd_lastCfg.key];
			put3(&buf[1][k[1] ? 14 : 15], k);
		}
		buf[0][16] = buf[1][16] = 0;
		return 1;
	}
	return 0;
}
