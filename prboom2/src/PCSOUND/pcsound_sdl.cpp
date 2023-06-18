// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 2007 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
// 02111-1307, USA.
//
// DESCRIPTION:
//    PC speaker interface.
//
//-----------------------------------------------------------------------------

// NSM: reworked to function without sdl_mixer (needs new i_sound.c)

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstdio>
#include <cstdlib>

#include <SDL.h>

#include "pcsound.h"

// e6y
#include "lprintf.h"

#include "i_sound.h"  // snd_samplerate

namespace {
constexpr const Sint16 SQUARE_WAVE_AMP = 0x2000;

pcsound_callback_func callback;

// Output sound format

int mixing_freq;
// static Uint16 mixing_format;
// static int mixing_channels;

// Currently playing sound
// current_remaining is the number of remaining samples that must be played
// before we invoke the callback to get the next frequency.

int current_remaining;
int current_freq;

int phase_offset = 0;

bool pcsound_inited = false;
}  // namespace

// Mixer function that does the PC speaker emulation

extern "C" void PCSound_Mix_Callback(void* udata, Uint8* const stream, const int len) {
  // safe to call even if not active
  if (!pcsound_inited) {
    return;
  }

  // Number of samples is quadrupled, because of 16-bit and stereo

  int nsamples = len / 4;

  auto* leftptr = reinterpret_cast<Sint16*>(stream);
  auto* rightptr = reinterpret_cast<Sint16*>(stream) + 1;

  // Fill the output buffer

  for (int i = 0; i < nsamples; ++i) {
    // Has this sound expired? If so, invoke the callback to get
    // the next frequency.

    while (current_remaining == 0) {
      int oldfreq = current_freq;

      // Get the next frequency to play

      callback(&current_remaining, &current_freq);

      if (current_freq != 0) {
        // Adjust phase to match to the new frequency.
        // This gives us a smooth transition between different tones,
        // with no impulse changes.

        phase_offset = (phase_offset * oldfreq) / current_freq;
      }

      current_remaining = (current_remaining * mixing_freq) / 1000;
    }

    // Set the value for this sample.

    Sint16 this_value;
    if (current_freq == 0) {
      // Silence

      this_value = 0;
    } else {
      // Determine whether we are at a peak or trough in the current
      // sound.  Multiply by 2 so that frac % 2 will give 0 or 1
      // depending on whether we are at a peak or trough.

      int frac = (phase_offset * current_freq * 2) / mixing_freq;

      if ((frac % 2) == 0) {
        this_value = SQUARE_WAVE_AMP;
      } else {
        this_value = -SQUARE_WAVE_AMP;
      }

      ++phase_offset;
    }

    --current_remaining;

    // Use the same value for the left and right channels.

    *leftptr += this_value;
    *rightptr += this_value;

    leftptr += 2;
    rightptr += 2;
  }
}

namespace {
int PCSound_SDL_Init(pcsound_callback_func callback_func) {
  mixing_freq = snd_samplerate;

  callback = callback_func;
  current_freq = 0;
  current_remaining = 0;
  pcsound_inited = true;

  return 1;
}

void PCSound_SDL_Shutdown(void) {
  pcsound_inited = false;
}
}  // namespace

extern "C" pcsound_driver_t pcsound_sdl_driver = {
    "SDL",
    PCSound_SDL_Init,
    PCSound_SDL_Shutdown,
};
