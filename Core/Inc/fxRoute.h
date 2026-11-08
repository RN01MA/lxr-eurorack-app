/*
 * fxRoute.h
 *
 * Hot key "voice -> FX bus": hold SHIFT, hold FX, tap voice buttons.
 * Each tap toggles the voice's audio out (PAR_AUDIO_OUTx, the MIX page "out")
 * between the FX bus and the output it had before. While the combo is held
 * the voice buttons route instead of triggering, the display lists the voices
 * on the bus.
 *
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

#ifndef INC_FXROUTE_H_
#define INC_FXROUTE_H_

#include <stdint.h>

void fxRoute_fxPressed(uint8_t shiftHeld);	// returns via fxRoute_isActive() whether the combo started
void fxRoute_fxReleased();
uint8_t fxRoute_isActive();
void fxRoute_voicePressed(uint8_t voiceNr);	// 1..7 (7 = open hihat button -> V6)

// display while the combo is held; returns 1 if it painted the buffer
uint8_t fxRoute_paint(char buf[2][17]);

#endif /* INC_FXROUTE_H_ */
