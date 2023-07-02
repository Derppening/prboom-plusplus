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

#include "vorbisplayer.h"

#ifndef HAVE_LIBVORBISFILE

namespace {
auto vorb_name() -> const char* {
  return "vorbis player (DISABLED)";
}

auto vorb_init([[maybe_unused]] const int samplerate) -> int {
  return 0;
}
}  // namespace

const music_player_t vorb_player = {vorb_name, vorb_init, nullptr, nullptr, nullptr, nullptr,
                                    nullptr,   nullptr,   nullptr, nullptr, nullptr};

#else  // HAVE_LIBVORBISFILE

#include <cstddef>

#include <algorithm>
#include <limits>
#include <string_view>

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#include "i_sound.h"
#include "lprintf.h"

// uncomment to allow (experiemntal) support for
// zdoom-style audio loops
#define ZDOOM_AUDIO_LOOP

namespace {
bool vorb_looping = false;
int vorb_volume = 0;  // 0-15
int vorb_samplerate_target = 0;
int vorb_samplerate_in = 0;
bool vorb_paused = false;
bool vorb_playing = false;

#ifdef ZDOOM_AUDIO_LOOP
unsigned vorb_loop_from;
unsigned vorb_loop_to;
unsigned vorb_total_pos;
#endif  // ZDOOM_AUDIO_LOOP

const std::byte* vorb_data;
std::size_t vorb_len;
std::size_t vorb_pos;
}  // namespace

OggVorbis_File vf;

namespace {
namespace ov {
auto clear(OggVorbis_File& vf) -> int {
  return ov_clear(&vf);
}

auto comment(OggVorbis_File& vf, const int link) -> vorbis_comment* {
  return ov_comment(&vf, link);
}

auto info(OggVorbis_File& vf, const int link) -> vorbis_info* {
  return ov_info(&vf, link);
}

auto pcm_seek_lap(OggVorbis_File& vf, const ogg_int64_t pos) -> int {
  return ov_pcm_seek_lap(&vf, pos);
}

auto raw_seek_lap(OggVorbis_File& vf, const ogg_int64_t pos) -> int {
  return ov_raw_seek_lap(&vf, pos);
}

auto read_float(OggVorbis_File& vf, float**& pcm_channels, const int samples, int& bitstream) -> long {
  return ov_read_float(&vf, &pcm_channels, samples, &bitstream);
}

auto test_callbacks(std::byte* const datasource,
                    OggVorbis_File& vf,
                    const std::byte* const initial,
                    const std::size_t ibytes,
                    const ov_callbacks callbacks) -> int {
  return ov_test_callbacks(datasource, &vf, reinterpret_cast<const char*>(initial), static_cast<long>(ibytes),
                           callbacks);
}

auto test_open(OggVorbis_File& vf) -> int {
  return ov_test_open(&vf);
}
}  // namespace ov
}  // namespace

// io callbacks

namespace {
auto vread(void* const dst, const std::size_t s, const std::size_t n, [[maybe_unused]] void* const src) -> std::size_t {
  std::size_t size = s * n;

  if (vorb_pos + size >= vorb_len) {
    size = vorb_len - vorb_pos;
  }

  std::copy_n(vorb_data + vorb_pos, size, static_cast<std::byte*>(dst));
  vorb_pos += size;
  return size;
}

auto vseek([[maybe_unused]] void* const src, const ogg_int64_t offset, const int whence) -> int {
  std::size_t desired_pos;

  switch (whence) {
    case SEEK_SET:
      desired_pos = static_cast<std::size_t>(offset);
      break;
    case SEEK_CUR:
      desired_pos = vorb_pos + static_cast<std::size_t>(offset);
      break;
    case SEEK_END:
    default:
      desired_pos = vorb_len + static_cast<std::size_t>(offset);
      break;
  }

  if (desired_pos > vorb_len) {  // placing exactly at the end is allowed)
    return -1;
  }

  vorb_pos = desired_pos;
  return 0;
}

auto vtell([[maybe_unused]] void* const src) -> long {
  // correct to vorbisfile spec, this is a long, not 64 bit
  return static_cast<long>(vorb_pos);
}
}  // namespace

const ov_callbacks vcallback = {vread, vseek, nullptr, vtell};

namespace {
auto vorb_name() -> const char* {
  return "vorbis player";
}

// http://zdoom.org/wiki/Audio_loop
// it's hard to accurately implement a grammar with "etc" in the spec,
// so weird edge cases are likely not the same
#ifdef ZDOOM_AUDIO_LOOP
auto parsetag(const std::string_view str, const int samplerate) -> unsigned {
  int ret = 0;
  int seendot = 0;    // number of dots seen so far
  int mult = 1;       // place value of next digit out
  int seencolon = 0;  // number of colons seen so far
  int digincol = 0;   // number of digits in current group
  // (needed to track switch between colon)

  for (auto pos = str.crbegin(); pos != str.crend(); ++pos) {
    if (*pos >= '0' && *pos <= '9') {
      ret += (*pos - '0') * mult;
      mult *= 10;
      digincol++;
    } else if (*pos == '.') {
      if (seencolon != 0 || seendot != 0) {
        return 0;
      }

      seendot = 1;
      // convert decimal to samplerate and move on
      ret *= samplerate;
      ret /= mult;
      mult = samplerate;
      digincol = 0;
    } else if (*pos == ':') {
      if (seencolon == 2) {  // no mention of anything past hours in spec
        return 0;
      }

      seencolon++;
      mult *= 6;

      // the spec is kind of vague and says lots of things can be left out,
      // so constructs like mmm:ss and hh::ss are allowed
      while (digincol > 1) {
        digincol--;
        mult /= 10;
      }

      while (digincol < 1) {
        digincol++;
        mult *= 10;
      }

      digincol = 0;
    } else {
      return 0;
    }
  }

  if (seencolon != 0 && seendot == 0) {  // HH:MM:SS, but we never converted to samples
    return ret * samplerate;
  }

  // either flat pcm or :. in which case everything was converted already
  return ret;
}
#endif  // ZDOOM_AUDIO_LOOP

#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#include <delayimp.h>
#include <windows.h>
#endif

auto vorb_init(const int samplerate) -> int {
  TESTDLLLOAD("libogg-0.dll", FALSE)
  TESTDLLLOAD("libvorbis-0.dll", FALSE)
  TESTDLLLOAD("libvorbisfile-3.dll", TRUE)
  vorb_samplerate_target = samplerate;
  return 1;
}

void vorb_shutdown() {
  // nothing to do
}

auto vorb_registersong(const void* data, unsigned len) -> const void* {
  vorb_data = static_cast<const std::byte*>(data);
  vorb_len = len;
  vorb_pos = 0;

  int i = ov::test_callbacks(const_cast<std::byte*>(vorb_data), vf, nullptr, 0, vcallback);
  if (i != 0) {
    lprintf(LO_WARN, "vorb_registersong: failed\n");
    return nullptr;
  }

  i = ov::test_open(vf);
  if (i != 0) {
    lprintf(LO_WARN, "vorb_registersong: failed\n");
    ov::clear(vf);
    return nullptr;
  }

  const vorbis_info* const vinfo = ov::info(vf, -1);
  vorb_samplerate_in = vinfo->rate;

#ifdef ZDOOM_AUDIO_LOOP
  // parse LOOP_START LOOP_END tags
  vorb_loop_from = 0;
  vorb_loop_to = 0;

  const vorbis_comment* const vcom = ov::comment(vf, -1);
  for (i = 0; i < vcom->comments; i++) {
    using std::string_view_literals::operator""sv;

    std::string_view comment = vcom->user_comments[i];
    if (comment.starts_with("LOOP_START=")) {
      comment.remove_prefix("LOOP_START="sv.length());
      vorb_loop_to = parsetag(comment, vorb_samplerate_in);
    } else if (comment.starts_with("LOOP_END=")) {
      comment.remove_prefix("LOOP_END="sv.length());
      vorb_loop_from = parsetag(comment, vorb_samplerate_in);
    }
  }

  if (vorb_loop_from == 0) {
    vorb_loop_from = std::numeric_limits<unsigned int>::max();
  } else if (vorb_loop_to >= vorb_loop_from) {
    vorb_loop_to = std::numeric_limits<unsigned int>::min();
  }
#endif  // ZDOOM_AUDIO_LOOP

  // handle not used
  return data;
}

void vorb_setvolume(const int v) {
  vorb_volume = v;
}

void vorb_pause() {
  vorb_paused = true;
}

void vorb_resume() {
  vorb_paused = false;
}

void vorb_unregistersong([[maybe_unused]] const void* const handle) {
  vorb_data = nullptr;
  ov::clear(vf);
  vorb_playing = false;
}

void vorb_play([[maybe_unused]] const void* const handle, const int looping) {
  ov::raw_seek_lap(vf, 0);

  vorb_playing = true;
  vorb_looping = static_cast<bool>(looping);
#ifdef ZDOOM_AUDIO_LOOP
  vorb_total_pos = 0;
#endif  // ZDOOM_AUDIO_LOOP
}

void vorb_stop() {
  vorb_playing = false;
}

void vorb_render_ex(void* const dest, unsigned nsamp) {
  // no workie on files that dynamically change sampling rate

  auto* sout = static_cast<short*>(dest);

  int localerrors = 0;

  //  int numread;

  // this call needs to be moved if support for changed number
  // of channels in middle of file is wanted
  const vorbis_info* const vinfo = ov::info(vf, -1);

  if (!vorb_playing || vorb_paused) {
    std::fill_n(static_cast<std::byte*>(dest), nsamp * 4, std::byte{0});
    return;
  }

  while (nsamp > 0) {
    float** pcmdata;
    int bitstreamnum;  // not used
    long numread;

#ifdef ZDOOM_AUDIO_LOOP
    // don't use custom loop end point when not in looping mode
    if (vorb_looping && vorb_total_pos + nsamp > vorb_loop_from) {
      numread = ov::read_float(vf, pcmdata, vorb_loop_from - vorb_total_pos, bitstreamnum);
    } else {
      numread = ov::read_float(vf, pcmdata, nsamp, bitstreamnum);
    }
#else
    numread = ov::read_float(vf, pcmdata, nsamp, bitstreamnum);
#endif  // ZDOOM_AUDIO_LOOP

    if (numread == OV_HOLE) {  // recoverable error, but discontinue if we get too many
      localerrors++;
      if (localerrors == 10) {
        lprintf(LO_WARN, "vorb_render: many errors.  aborting\n");

        vorb_playing = false;
        std::fill_n(reinterpret_cast<std::byte*>(sout), nsamp * 4, std::byte{0});

        return;
      }

      continue;
    }

    if (numread == 0) {  // EOF
      if (vorb_looping) {
#ifdef ZDOOM_AUDIO_LOOP
        ov::pcm_seek_lap(vf, vorb_loop_to);
        vorb_total_pos = vorb_loop_to;
#else   // ZDOOM_AUDIO_LOOP
        ov::raw_seek_lap(vf, 0);
#endif  // ZDOOM_AUDIO_LOOP
        continue;
      }

      vorb_playing = false;
      std::fill_n(reinterpret_cast<std::byte*>(sout), nsamp * 4, std::byte{0});

      return;
    }

    if (numread < 0) {  // unrecoverable errror
      lprintf(LO_WARN, "vorb_render: unrecoverable error\n");

      vorb_playing = false;
      std::fill_n(reinterpret_cast<std::byte*>(sout), nsamp * 4, std::byte{0});

      return;
    }

    const float multiplier = 16384.0f / 15.0f * vorb_volume;
    // volume and downmix
    if (vinfo->channels == 2) {
      for (int i = 0; i < numread; i++, sout += 2) {  // data is preclipped?
        sout[0] = static_cast<short>(pcmdata[0][i] * multiplier);
        sout[1] = static_cast<short>(pcmdata[1][i] * multiplier);
      }
    } else {  // mono
      for (int i = 0; i < numread; i++, sout += 2) {
        sout[0] = sout[1] = static_cast<short>(pcmdata[0][i] * multiplier);
      }
    }
    nsamp -= numread;
#ifdef ZDOOM_AUDIO_LOOP
    vorb_total_pos += numread;
#endif  // ZDOOM_AUDIO_LOOP
  }
}

void vorb_render(void* const dest, const unsigned nsamp) {
  I_ResampleStream(dest, nsamp, vorb_render_ex, vorb_samplerate_in, vorb_samplerate_target);
}
}  // namespace

const music_player_t vorb_player = {vorb_name,  vorb_init,   vorb_shutdown,     vorb_setvolume,
                                    vorb_pause, vorb_resume, vorb_registersong, vorb_unregistersong,
                                    vorb_play,  vorb_stop,   vorb_render};

#endif  // HAVE_LIBVORBISFILE
