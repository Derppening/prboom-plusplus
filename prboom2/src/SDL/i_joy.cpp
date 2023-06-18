/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
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
 *   Joystick handling for Linux
 *
 *-----------------------------------------------------------------------------
 */

#ifndef lint
#endif /* lint */

#include <cstdlib>

#include "d_event.h"
#include "d_main.h"
#include "doomdef.h"
#include "doomtype.h"
#include "i_joy.h"
#include "i_system.h"
#include "lprintf.h"
#include "m_argv.h"
#include <SDL.h>

int joyleft;
int joyright;
int joyup;
int joydown;

int usejoystick;

#ifdef HAVE_SDL_JOYSTICKGETAXIS
namespace {
SDL_Joystick* joystick;
}  // namespace
#endif

namespace {
static void I_EndJoystick() {
  lprintf(LO_DEBUG, "I_EndJoystick : closing joystick\n");
}
}  // namespace

void I_PollJoystick(void) {
#ifdef HAVE_SDL_JOYSTICKGETAXIS
  if (usejoystick == 0 || joystick == nullptr) {
    return;
  }

  event_t ev{
      .type = ev_joystick,
      .data1 = {},
      .data2 = {},
      .data3 = {}
  };

  for (int i = 0; i < 7; ++i) {
    ev.data1 |= SDL_JoystickGetButton(joystick, i) << i;
  }

  Sint16 axis_value = SDL_JoystickGetAxis(joystick, 0) / 3000;
  if (std::abs(axis_value) < 7) {
    axis_value = 0;
  }
  ev.data2 = axis_value;

  axis_value = SDL_JoystickGetAxis(joystick, 1) / 3000;
  if (std::abs(axis_value) < 7) {
    axis_value = 0;
  }
  ev.data3 = axis_value;

  D_PostEvent(&ev);
#endif
}

void I_InitJoystick() {
#ifdef HAVE_SDL_JOYSTICKGETAXIS
  const char* fname = "I_InitJoystick : ";

  if (usejoystick == 0) {
    return;
  }

  SDL_InitSubSystem(SDL_INIT_JOYSTICK);
  const int num_joysticks = SDL_NumJoysticks();

  if (M_CheckParm("-nojoy") || (usejoystick > num_joysticks) || (usejoystick < 0)) {
    if ((usejoystick > num_joysticks) || (usejoystick < 0)) {
      lprintf(LO_WARN, "%sinvalid joystick %d\n", fname, usejoystick);
    } else {
      lprintf(LO_INFO, "%suser disabled\n", fname);
    }

    return;
  }

  joystick = SDL_JoystickOpen(usejoystick - 1);
  if (joystick == nullptr) {
    lprintf(LO_ERROR, "%serror opening joystick %d\n", fname, usejoystick);
  } else {
    I_AtExit(I_EndJoystick, true);
    lprintf(LO_INFO, "%sopened %s\n", fname, SDL_JoystickName(joystick));
    joyup = 32767;
    joydown = -32768;
    joyright = 32767;
    joyleft = -32768;
  }
#endif
}
