/*
 * RandomizerUi.h
 *
 * Front panel side of the kit randomizer (SHIFT+PERF):
 *   DRUM tap            -> new kit (on release)
 *   hold DRUM + voice   -> lock / unlock that voice
 *   hold voice + DRUM   -> lock / unlock (spec order), no new kit
 *   voice buttons       -> trigger the voice (audition, like PERF)
 *   voice LEDs          -> locked voices lit; V1->V6 bar after a new kit
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#ifndef INC_PRESET_RANDOMIZERUI_H_
#define INC_PRESET_RANDOMIZERUI_H_

#include <stdint.h>

void RndUi_enter();							// mode entered (SHIFT+PERF)
void RndUi_leave();							// another mode selected

void RndUi_drumPressed();
void RndUi_drumReleased();
void RndUi_voicePressed(uint8_t voiceNr);	// 1..7 (7 = open hihat button -> V6)
void RndUi_voiceReleased(uint8_t voiceNr);

void RndUi_tick();							// main loop: LED bar, result display timeout

// display overlay for the randomizer page; returns 1 if it painted the buffer
uint8_t RndUi_paint(char buf[2][17]);

#endif /* INC_PRESET_RANDOMIZERUI_H_ */
