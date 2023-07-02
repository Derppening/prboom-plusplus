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

#include "flplayer.h"

#ifndef HAVE_LIBFLUIDSYNTH

namespace {
auto fl_name() -> const char* {
  return "fluidsynth midi player (DISABLED)";
}

auto fl_init([[maybe_unused]] const int samplerate) -> int {
  return 0;
}
}  // namespace

const music_player_t fl_player = {fl_name, fl_init, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr};

#else  // HAVE_LIBFLUIDSYNTH

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include <fluidsynth.h>

#include "i_sound.h"   // for snd_soundfont, mus_fluidsynth_gain
#include "i_system.h"  // for I_FindFile()
#include "lprintf.h"
#include "midifile.h"

namespace {
std::unique_ptr<fluid_settings_t, decltype(&delete_fluid_settings)> f_set{nullptr, delete_fluid_settings};
std::unique_ptr<fluid_synth_t, decltype(&delete_fluid_synth)> f_syn{nullptr, delete_fluid_synth};
int f_font;
std::unique_ptr<midi_event_t* [], decltype(&MIDI_DestroyFlatList)> events {
  nullptr, MIDI_DestroyFlatList
};
std::size_t eventpos;
std::unique_ptr<midi_file_t, decltype(&MIDI_FreeFile)> midifile{nullptr, MIDI_FreeFile};

bool f_playing;
bool f_paused;
bool f_looping;
int f_volume;
double spmc;
double f_delta;
int f_soundrate;

constexpr std::size_t SYSEX_BUFF_SIZE = 1024;
std::array<std::byte, SYSEX_BUFF_SIZE> sysexbuff;
std::size_t sysexbufflen;

auto fl_name() -> const char* {
  return "fluidsynth midi player";
}

#ifdef _MSC_VER
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <delayimp.h>
#include <windows.h>
#endif

auto fl_init(const int samplerate) -> int {
  TESTDLLLOAD("libfluidsynth.dll", TRUE)

  f_soundrate = samplerate;
  // fluidsynth 1.1.4 supports sample rates as low as 8000hz.  earlier versions only go down to 22050hz
  // since the versions are ABI compatible, detect at runtime, not compile time
  int major;
  int minor;
  int micro;
  fluid_version(&major, &minor, &micro);
  lprintf(LO_INFO, "Fluidplayer: Fluidsynth version %i.%i.%i\n", major, minor, micro);

  int sratemin;
  if (major >= 2 || (minor >= 1 && micro >= 4)) {
    sratemin = 8000;
  } else {
    sratemin = 22050;
  }

  if (f_soundrate < sratemin) {
    lprintf(LO_INFO, "Fluidplayer: samplerates under %i are not supported\n", sratemin);
    return 0;
  }

  f_set.reset(new_fluid_settings());

#if FLUIDSYNTH_VERSION_MAJOR == 1
#define FSET(a, b, c)                                  \
  if (!fluid_settings_set##a(f_set.get(), b, c)) {     \
    lprintf(LO_INFO, "fl_init: Couldn't set " b "\n"); \
  }
#else
#define FSET(a, b, c)                                             \
  if (fluid_settings_set##a(f_set.get(), b, c) == FLUID_FAILED) { \
    lprintf(LO_INFO, "fl_init: Couldn't set " b "\n");            \
  }
#endif

  FSET(num, "synth.sample-rate", f_soundrate);

  FSET(int, "synth.chorus.active", mus_fluidsynth_chorus);
  FSET(int, "synth.reverb.active", mus_fluidsynth_reverb);

  if (mus_fluidsynth_chorus) {
    FSET(num, "synth.chorus.depth", (double)5);
    FSET(num, "synth.chorus.level", (double)0.35);
  }

  if (mus_fluidsynth_reverb) {
    FSET(num, "synth.reverb.damp", (double)0.4);
    FSET(num, "synth.reverb.level", (double)0.15);
    FSET(num, "synth.reverb.width", (double)4);
    FSET(num, "synth.reverb.room-size", (double)0.6);
  }

  // gain control
  FSET(num, "synth.gain", mus_fluidsynth_gain / 100.0);  // 0.0 - 0.2 - 10.0
  // behavior wrt bank select messages
  FSET(str, "synth.midi-bank-select", "gs");  // fluidsynth default
  // general midi spec says 24 voices, but modern midi songs use more
  FSET(int, "synth.polyphony", 256);  // fluidsynth default

  // we're not using the builtin shell or builtin midiplayer,
  // and our own access to the synth is protected by mutex in i_sound.c
  FSET(int, "synth.threadsafe-api", 0);
#if FLUIDSYNTH_VERSION_MAJOR == 1
  FSET(int, "synth.parallel-render", 0);
#endif

  // prints debugging information to STDOUT
  // FSET (int, "synth.verbose", 1);

#undef FSET

  f_syn.reset(new_fluid_synth(f_set.get()));
  if (!f_syn) {
    lprintf(LO_WARN, "fl_init: error creating fluidsynth object\n");
    f_set.reset();
    return 0;
  }

  const char* filename = I_FindFile2(snd_soundfont, ".sf2");
  f_font = fluid_synth_sfload(f_syn.get(), filename, 1);

  if (f_font == FLUID_FAILED) {
    lprintf(LO_WARN, "fl_init: error loading soundfont %s\n", snd_soundfont);
    f_syn.reset();
    f_set.reset();
    return 0;
  }

  return 1;
}

void fl_shutdown() {
  if (f_syn) {
    fluid_synth_sfunload(f_syn.get(), f_font, 1);
    f_syn.reset();
    f_font = 0;
  }

  f_set.reset();
}

const void* fl_registersong(const void* const data, const unsigned len) {
  midimem_t mf{.data = static_cast<const byte*>(data), .len = len, .pos = 0};

  midifile.reset(MIDI_LoadFile(&mf));

  if (!midifile) {
    lprintf(LO_WARN, "fl_registersong: Failed to load MIDI.\n");
    return nullptr;
  }

  events.reset(MIDI_GenerateFlatList(midifile.get()));
  if (!events) {
    midifile.reset();
    return NULL;
  }
  eventpos = 0;

  // implicit 120BPM (this is correct to spec)
  // spmc = compute_spmc (MIDI_GetFileTimeDivision (midifile), 500000, f_soundrate);
  spmc = MIDI_spmc(midifile.get(), nullptr, f_soundrate);

  // handle not used
  return data;
}

void fl_unregistersong([[maybe_unused]] const void* const handle) {
  events.reset();
  midifile.reset();
}

void fl_pause() {
  // int i;
  f_paused = true;
  // instead of cutting notes, pause the synth so they can resume seamlessly
  // for (i = 0; i < 16; i++)
  //  fluid_synth_cc (f_syn, i, 123, 0); // ALL NOTES OFF
}

static void fl_resume() {
  f_paused = false;
}

void fl_play([[maybe_unused]] const void* const handle, int looping) {
  eventpos = 0;
  f_looping = static_cast<bool>(looping);
  f_playing = true;
  // f_paused = 0;
  f_delta = 0.0;
  fluid_synth_program_reset(f_syn.get());
  fluid_synth_system_reset(f_syn.get());
}

void fl_stop() {
  f_playing = false;

  for (int i = 0; i < 16; i++) {
    fluid_synth_cc(f_syn.get(), i, 123, 0);  // ALL NOTES OFF
    fluid_synth_cc(f_syn.get(), i, 121, 0);  // RESET ALL CONTROLLERS
  }
}

void fl_setvolume(int v) {
  f_volume = v;
}

static void fl_writesamples_ex(short* const dest, const int nsamp) {  // does volume conversion and then writes samples
  const float multiplier = 16384.0f / 15.0f * f_volume;

  static std::vector<float> fbuff;
  //  static float* fbuff = nullptr;
  //  static int fbuff_siz = 0;

  if (nsamp * 2 > fbuff.size()) {
    fbuff.resize(nsamp * 2);
  }

  fluid_synth_write_float(f_syn.get(), nsamp, fbuff.data(), 0, 2, fbuff.data(), 1, 2);

  for (std::size_t i = 0; i < nsamp * 2; i++) {
    // data is NOT already clipped
    const float f = std::clamp(fbuff[i], -1.0f, 1.0f);
    dest[i] = static_cast<short>(f * multiplier);
  }
}

void writesysex(unsigned char* const data, const int len) {
  // sysex code is untested
  // it's possible to use an auto-resizing buffer here, but a malformed
  // midi file could make it grow arbitrarily large (since it must grow
  // until it hits an 0xf7 terminator)

  if (len + sysexbufflen > SYSEX_BUFF_SIZE) {
    lprintf(LO_WARN, "fluidplayer: ignoring large or malformed sysex message\n");
    sysexbufflen = 0;
    return;
  }

  std::copy_n(reinterpret_cast<const std::byte*>(data), len, sysexbuff.data() + sysexbufflen);
  sysexbufflen += len;

  int didrespond = 0;
  if (sysexbuff[sysexbufflen - 1] == std::byte{0xf7}) {  // terminator
                                                         // pass len-1 because fluidsynth does NOT want the final F7
    fluid_synth_sysex(f_syn.get(), reinterpret_cast<const char*>(sysexbuff.data()), sysexbufflen - 1, nullptr, nullptr,
                      &didrespond, 0);
    sysexbufflen = 0;
  }

  if (didrespond == 0) {
    lprintf(LO_WARN, "fluidplayer: SYSEX message received but not understood\n");
  }
}

void fl_render(void* const vdest, const unsigned length) {
  auto* dest = static_cast<short*>(vdest);

  unsigned samples;
  unsigned sampleswritten = 0;

  if (!f_playing || f_paused) {
    // save CPU time and allow for seamless resume after pause
    std::fill_n(static_cast<std::byte*>(vdest), length * 4, std::byte{0});
    // fl_writesamples_ex (vdest, length);
    return;
  }

  while (true) {
    midi_event_t* const currevent = events[eventpos];

    // how many samples away event is
    double eventdelta = currevent->delta_time * spmc;

    // how many we will render (rounding down); include delta offset
    samples = static_cast<unsigned>(eventdelta + f_delta);

    if (samples + sampleswritten > length) {  // overshoot; render some samples without processing an event
      break;
    }

    if (samples != 0) {
      fl_writesamples_ex(dest, samples);
      sampleswritten += samples;
      f_delta -= samples;
      dest += samples * 2;
    }

    // process event
    switch (currevent->event_type) {
      case MIDI_EVENT_NOTE_OFF:
        fluid_synth_noteoff(f_syn.get(), currevent->data.channel.channel, currevent->data.channel.param1);
        break;

      case MIDI_EVENT_NOTE_ON:
        fluid_synth_noteon(f_syn.get(), currevent->data.channel.channel, currevent->data.channel.param1,
                           currevent->data.channel.param2);
        break;

      case MIDI_EVENT_AFTERTOUCH:
        // not suipported?
        break;

      case MIDI_EVENT_CONTROLLER:
        fluid_synth_cc(f_syn.get(), currevent->data.channel.channel, currevent->data.channel.param1,
                       currevent->data.channel.param2);
        break;

      case MIDI_EVENT_PROGRAM_CHANGE:
        fluid_synth_program_change(f_syn.get(), currevent->data.channel.channel, currevent->data.channel.param1);
        break;

      case MIDI_EVENT_CHAN_AFTERTOUCH:
        fluid_synth_channel_pressure(f_syn.get(), currevent->data.channel.channel, currevent->data.channel.param1);
        break;

      case MIDI_EVENT_PITCH_BEND:
        fluid_synth_pitch_bend(f_syn.get(), currevent->data.channel.channel,
                               currevent->data.channel.param1 | currevent->data.channel.param2 << 7);
        break;

      case MIDI_EVENT_SYSEX:
      case MIDI_EVENT_SYSEX_SPLIT:
        writesysex(currevent->data.sysex.data, currevent->data.sysex.length);
        break;

      case MIDI_EVENT_META:
        if (currevent->data.meta.type == MIDI_META_SET_TEMPO) {
          spmc = MIDI_spmc(midifile.get(), currevent, f_soundrate);
        } else if (currevent->data.meta.type == MIDI_META_END_OF_TRACK) {
          if (f_looping) {
            eventpos = 0;
            f_delta += eventdelta;
            // fix buggy songs that forget to terminate notes held over loop point
            // sdl_mixer does this as well
            for (int i = 0; i < 16; i++) {
              fluid_synth_cc(f_syn.get(), i, 123, 0);  // ALL NOTES OFF
              fluid_synth_cc(f_syn.get(), i, 121, 0);  // RESET ALL CONTROLLERS
            }
            continue;
          }

          // stop, write leadout
          fl_stop();
          samples = length - sampleswritten;
          if (samples != 0) {
            fl_writesamples_ex(dest, samples);
            sampleswritten += samples;
            // timecodes no longer relevant
            dest += samples * 2;
          }

          return;
        }
        break;  // not interested in most metas

      default:  // uhh
        break;
    }

    // event processed so advance midiclock
    f_delta += eventdelta;
    eventpos++;
  }

  if (samples + sampleswritten > length) {  // broke due to next event being past the end of current render buffer
    // finish buffer, return
    samples = length - sampleswritten;
    if (samples != 0) {
      fl_writesamples_ex(dest, samples);
      sampleswritten += samples;
      f_delta -= samples;  // save offset
      dest += samples * 2;
    }
  } else {  // huh?
    return;
  }
}
}  // namespace

const music_player_t fl_player = {fl_name,         fl_init,           fl_shutdown, fl_setvolume, fl_pause, fl_resume,
                                  fl_registersong, fl_unregistersong, fl_play,     fl_stop,      fl_render};

#endif  // HAVE_LIBFLUIDSYNTH
