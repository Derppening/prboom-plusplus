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

#include "madplayer.h"

#ifndef HAVE_LIBMAD

namespace {
auto mp_name() -> const char* {
  return "mad mp3 player (DISABLED)";
}

auto mp_init([[maybe_unused]] const int samplerate) -> int {
  return 0;
}

const music_player_t mp_player = {mp_name, mp_init, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr};
}  // namespace

#else  // HAVE_LIBMAD

#include <cstring>

#include <algorithm>

#include <mad.h>

#include "i_sound.h"
#include "lprintf.h"

namespace {
struct mad_stream Stream;
struct mad_frame Frame;
struct mad_synth Synth;
struct mad_header Header;

bool mp_looping = 0;
int mp_volume = 0;  // 0-15
int mp_samplerate_target = 0;
bool mp_paused = 0;
bool mp_playing = 0;

const void* mp_data;
unsigned mp_len;

int mp_leftoversamps = 0;  // number of extra samples
                           // left over in mad decoder
int mp_leftoversamppos = 0;

auto mp_name() -> const char* {
  return "mad mp3 player";
}

auto mp_init(const int samplerate) -> int {
  mad_stream_init(&Stream);
  mad_frame_init(&Frame);
  mad_synth_init(&Synth);
  mad_header_init(&Header);
  mp_samplerate_target = samplerate;
  return 1;
}

void mp_shutdown() {

  mad_synth_finish(&Synth);
  mad_frame_finish(&Frame);
  mad_stream_finish(&Stream);
  mad_header_finish(&Header);
}

auto mp_registersong(const void* const data, const unsigned len) -> const void* {
  int success = 0;

  // the MP3 standard doesn't include any global file header.  the only way to tell filetype
  // is to start decoding stuff.  you can't be too strict however because MP3 is resilient to
  // crap in the stream.

  // this routine is a bit slower than it could be, but apparently there are lots of files out
  // there with some dodgy stuff at the beginning.

  // if the stream begins with an ID3v2 magic, search hard and long for our first valid header
  int maxtry;
  if (std::memcmp(data, "ID3", 3) == 0) {
    maxtry = 100;
  } else {
    // otherwise, search for not so long
    maxtry = 20;
  }

  mad_stream_buffer(&Stream, static_cast<const unsigned char*>(data), len);

  for (int i = 0; i < maxtry; i++) {
    if (mad_header_decode(&Header, &Stream) != 0) {
      if (!MAD_RECOVERABLE(Stream.error)) {
        lprintf(LO_WARN, "mad_registersong failed: %s\n", mad_stream_errorstr(&Stream));
        return nullptr;
      }
    } else {
      success++;
    }
  }

  // 80% to pass
  if (success < maxtry * 8 / 10) {
    lprintf(LO_WARN, "mad_registersong failed\n");
    return nullptr;
  }

  lprintf(LO_INFO, "mad_registersong succeed. bitrate %lu samplerate %d\n", Header.bitrate, Header.samplerate);

  mp_data = data;
  mp_len = len;
  // handle not used
  return data;
}

void mp_setvolume(const int v) {
  mp_volume = v;
}

void mp_pause() {
  mp_paused = true;
}

void mp_resume() {
  mp_paused = false;
}

void mp_unregistersong([[maybe_unused]] const void* const handle) {  // nothing to do
  mp_data = nullptr;
  mp_playing = false;
}

void mp_play([[maybe_unused]] const void* const handle, const int looping) {
  mad_stream_buffer(&Stream, static_cast<const unsigned char*>(mp_data), mp_len);

  mp_playing = true;
  mp_looping = static_cast<bool>(looping);
  mp_leftoversamps = 0;
  mp_leftoversamppos = 0;
}

void mp_stop() {
  mp_playing = false;
}

// convert from mad's internal fixed point representation
inline auto mp_fixtoshort(mad_fixed_t f) -> short {
  // clip
  f = std::clamp(f, -MAD_F_ONE, MAD_F_ONE);
  // apply volume before conversion to 16bit
  f /= 15;
  f *= mp_volume;
  f >>= (MAD_F_FRACBITS - 15);

  return static_cast<short>(f);
}

void mp_render_ex(void* const dest, unsigned nsamp) {
  auto* sout = static_cast<short*>(dest);

  int localerrors = 0;

  if (!mp_playing || mp_paused) {
    std::fill_n(static_cast<std::byte*>(dest), nsamp * 4, std::byte{0});
    return;
  }

  while (true) {
    // write any leftover data from last MP3 frame
    while (mp_leftoversamps > 0 && nsamp > 0) {
      short s = mp_fixtoshort(Synth.pcm.samples[0][mp_leftoversamppos]);
      *sout++ = s;
      if (Synth.pcm.channels == 2) {
        s = mp_fixtoshort(Synth.pcm.samples[1][mp_leftoversamppos]);
      }
      // if mono, just duplicate the first channel again
      *sout++ = s;

      mp_leftoversamps -= 1;
      mp_leftoversamppos += 1;
      nsamp -= 1;
    }
    if (nsamp == 0) {
      return;  // done
    }

    // decode next valid MP3 frame
    while (mad_frame_decode(&Frame, &Stream) != 0) {
      if (MAD_RECOVERABLE(Stream.error)) {  // unspecified problem with one frame.
        // try the next frame, but bail if we get a bunch of crap in a row;
        // likely indicates a larger problem (and if we don't bail, we could
        // spend arbitrarily long amounts of time looking for the next good
        // packet)
        localerrors++;

        if (localerrors == 10) {
          lprintf(LO_WARN, "mad_frame_decode: Lots of errors.  Most recent %s\n", mad_stream_errorstr(&Stream));
          mp_playing = false;
          std::fill_n(reinterpret_cast<std::byte*>(sout), nsamp * 4, std::byte{0});
          return;
        }
      } else if (Stream.error == MAD_ERROR_BUFLEN) {  // EOF
        // FIXME: in order to not drop the last frame, there must be at least MAD_BUFFER_GUARD
        // of extra bytes (with value 0) at the end of the file.  current implementation
        // drops last frame
        if (mp_looping) {  // rewind, then go again
          mad_stream_buffer(&Stream, static_cast<const unsigned char*>(mp_data), mp_len);
          continue;
        }

        // stop
        mp_playing = false;
        std::fill_n(reinterpret_cast<std::byte*>(sout), nsamp * 4, std::byte{0});
        return;
      } else {  // oh well.
        lprintf(LO_WARN, "mad_frame_decode: Unrecoverable error %s\n", mad_stream_errorstr(&Stream));
        mp_playing = false;
        std::fill_n(reinterpret_cast<std::byte*>(sout), nsamp * 4, std::byte{0});
        return;
      }
    }

    // got a good frame, so synth it and dispatch it.
    mad_synth_frame(&Synth, &Frame);
    mp_leftoversamps = Synth.pcm.length;
    mp_leftoversamppos = 0;
  }
  // NOT REACHED
}

void mp_render(void* dest, unsigned nsamp) {
  I_ResampleStream(dest, nsamp, mp_render_ex, Header.samplerate, mp_samplerate_target);
}
}  // namespace

const music_player_t mp_player = {mp_name,         mp_init,           mp_shutdown, mp_setvolume, mp_pause, mp_resume,
                                  mp_registersong, mp_unregistersong, mp_play,     mp_stop,      mp_render};

#endif  // HAVE_LIBMAD
