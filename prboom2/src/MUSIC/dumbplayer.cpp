/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *
 *  Copyright (C) 2011 by
 *  Nicholai Main
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
 *
 *---------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "dumbplayer.h"

#ifndef HAVE_LIBDUMB

namespace {
auto db_name() -> const char* {
  return "dumb tracker player (DISABLED)";
}

auto db_init([[maybe_unused]] const int samplerate) -> int {
  return 0;
}
}  // namespace

const music_player_t db_player = {db_name, db_init, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr};

#else  // HAVE_DUMB

#if !defined(_FILE_OFFSET_BITS) || (_FILE_OFFSET_BITS < 64)
#ifdef _MSC_VER
#define DUMB_OFF_T_CUSTOM __int64
#else

#include <cstdint>

#define DUMB_OFF_T_CUSTOM int64_t
#endif
#endif

#include <memory>

#include <dumb.h>

#include "lprintf.h"

namespace {
float db_delta;
float db_volume;
bool db_looping;
bool db_playing = false;
bool db_paused = false;

std::unique_ptr<DUH_SIGRENDERER, decltype(&duh_end_sigrenderer)> dsren{nullptr, duh_end_sigrenderer};
std::unique_ptr<DUH, decltype(&unload_duh)> duh{nullptr, unload_duh};
std::unique_ptr<DUMBFILE, decltype(&dumbfile_close)> dfil{nullptr, dumbfile_close};

auto db_name() -> const char* {
  return "dumb tracker player";
}

auto db_init(const int samplerate) -> int {
  db_delta = 65536.0f / samplerate;

  return 1;
}

void db_shutdown() {
  dumb_exit();
}

void db_setvolume(const int v) {
  db_volume = static_cast<float>(v) / 15.0f;
}

auto db_registersong(const void* const data, const unsigned len) -> const void* {
  // because dumbfiles don't have any concept of backward seek or
  // rewind, you have to reopen if any loader fails

  dfil.reset(dumbfile_open_memory(static_cast<const char*>(data), len));
  duh.reset(read_duh(dfil.get()));

  if (!duh) {
    dfil.reset(dumbfile_open_memory(static_cast<const char*>(data), len));
    duh.reset(dumb_read_it_quick(dfil.get()));
  }

  if (!duh) {
    dfil.reset(dumbfile_open_memory(static_cast<const char*>(data), len));
    duh.reset(dumb_read_xm_quick(dfil.get()));
  }

  if (!duh) {
    dfil.reset(dumbfile_open_memory(static_cast<const char*>(data), len));
    duh.reset(dumb_read_s3m_quick(dfil.get()));
  }

  if (!duh) {
    dfil.reset(dumbfile_open_memory(static_cast<const char*>(data), len));
#if (DUMB_MAJOR_VERSION >= 1)
    duh.reset(dumb_read_mod_quick(dfil.get(), 0));
#else
    duh.reset(dumb_read_mod_quick(dfil.get()));
#endif
    // No way to get the filename, so we can't check for a .mod extension, and
    // therefore, trying to load an old 15-instrument SoundTracker module is not
    // safe. We'll restrict MOD loading to 31-instrument modules with known
    // signatures and let the sound system worry about 15-instrument ones.
    // (Assuming it even supports them)
    {
      DUMB_IT_SIGDATA* const sigdata = duh_get_it_sigdata(duh.get());
      if (sigdata != nullptr) {
        const int n_samples = dumb_it_sd_get_n_samples(sigdata);
        if (n_samples == 15) {
          duh.reset();
        }
      }
    }
  }

  if (!duh) {
    dfil.reset();
    return nullptr;
  }
  // handle not used
  return data;
}

void db_unregistersong([[maybe_unused]] const void* const handle) {
  duh.reset();
  dfil.reset();
}

void db_play([[maybe_unused]] const void* const handle, const int looping) {
  dsren.reset(duh_start_sigrenderer(duh.get(), 0, 2, 0));

  if (!dsren) {  // fail?
    db_playing = false;
    return;
  }

  db_looping = static_cast<bool>(looping);
  db_playing = true;
}

void db_stop() {
  dsren.reset();
  db_playing = false;
}

void db_pause() {
  db_paused = true;
}

void db_resume() {
  db_paused = false;
}

void db_render(void* const dest, const unsigned nsamp) {
  if (db_playing && !db_paused) {
    auto* cdest = static_cast<unsigned char*>(dest);
#if (DUMB_MAJOR_VERSION >= 2)
    sample_t** sig_samples = nullptr;
    long sig_samples_size = 0;

    unsigned nsampwrit =
        duh_render_int(dsren.get(), &sig_samples, &sig_samples_size, 16, 0, db_volume, db_delta, nsamp, dest);
    destroy_sample_buffer(sig_samples);
#else
    unsigned nsampwrit = duh_render(dsren.get(), 16, 0, db_volume, db_delta, nsamp, dest);
#endif
    if (nsampwrit != nsamp) {  // end of file
      // tracker formats can have looping imbedded in them, in which case
      // we'll never reach this (even if db_looping is 0!!)

      cdest += nsampwrit * 4;

      if (db_looping) {  // but if the tracker doesn't loop, and we want loop anyway, restart
        // from beginning

        if (nsampwrit == 0) {  // special case: avoid infinite recursion
          db_stop();
          lprintf(LO_WARN, "db_render: problem (0 length tracker file on loop?\n");
          return;
        }

        // im not sure if this is the best way to seek, but there isn't
        // a sigrenderer_rewind type function
        db_stop();
        db_play(nullptr, 1);
        db_render(cdest, nsamp - nsampwrit);
      } else {  // halt
        db_stop();
        std::fill_n(reinterpret_cast<std::byte*>(cdest), (nsamp - nsampwrit) * 4, std::byte{0});
      }
    }
  } else {
    std::fill_n(static_cast<std::byte*>(dest), nsamp * 4, std::byte{0});
  }
}
}  // namespace

const music_player_t db_player = {db_name,         db_init,           db_shutdown, db_setvolume, db_pause, db_resume,
                                  db_registersong, db_unregistersong, db_play,     db_stop,      db_render};

#endif  // HAVE_DUMB
