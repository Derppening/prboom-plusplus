/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze, Andrey Budko
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *      Uncapped framerate stuff
 *
 *---------------------------------------------------------------------
 */

#ifndef __R_FPS__
#define __R_FPS__

#include "doomstat.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

extern int movement_smooth_default;
extern int movement_smooth;
extern dboolean isExtraDDisplay;

extern int interpolation_maxobjects;

typedef struct {
  unsigned int start;
  unsigned int next;
  unsigned int step;
  fixed_t frac;
  float msec;
} tic_vars_t;

extern tic_vars_t tic_vars;

void M_ChangeUncappedFrameRate(void);

void R_InitInterpolation(void);
void R_InterpolateView(player_t *player, fixed_t frac);

extern dboolean WasRenderedInTryRunTics;

void R_ResetViewInterpolation (void);
void R_UpdateInterpolations(void);
void R_StopAllInterpolations(void);
void R_RestoreInterpolations(void);
void R_ActivateSectorInterpolations(void);
void R_ActivateThinkerInterpolations(thinker_t *th);
void R_StopInterpolationIfNeeded(thinker_t *th);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif
