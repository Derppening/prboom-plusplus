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

// TODO: some duplicated code with this and the fluidplayer should be
// split off or something

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portmidiplayer.h"

#ifndef HAVE_LIBPORTMIDI

namespace {
const char* pm_name() {
  return "portmidi midi player (DISABLED)";
}

auto pm_init([[maybe_unused]] const int samplerate) -> int {
  return 0;
}
}  // namespace

const music_player_t pm_player = {pm_name, pm_init, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr};

#else  // HAVE_LIBPORTMIDI

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <array>
#include <chrono>
#include <format>
#include <memory>
#include <span>

#include <portmidi.h>
#include <porttime.h>

#include "i_sound.h"  // for snd_mididev
#include "lprintf.h"
#include "midifile.h"

namespace {
std::unique_ptr<midi_event_t* [], decltype(&MIDI_DestroyFlatList)> events {
  nullptr, MIDI_DestroyFlatList
};
std::size_t eventpos;
std::unique_ptr<midi_file_t, decltype(&MIDI_FreeFile)> midifile{nullptr, MIDI_FreeFile};

bool pm_playing;
bool pm_paused;
bool pm_looping;
int pm_volume = -1;
double spmc;
double pm_delta;

PmTimestamp trackstart;

std::unique_ptr<PortMidiStream, decltype(&Pm_Close)> pm_stream{nullptr, Pm_Close};

constexpr std::size_t SYSEX_BUFF_SIZE = PM_DEFAULT_SYSEX_BUFFER_SIZE;
std::array<std::byte, SYSEX_BUFF_SIZE> sysexbuff;
std::size_t sysexbufflen;

// latency: we're generally writing timestamps slightly in the past (from when the last time
// render was called to this time.  portmidi latency instruction must be larger than that window
// so the messages appear in the future.  ~46-47ms is the nominal length if i_sound.c gets its way
constexpr std::chrono::milliseconds DRIVER_LATENCY = std::chrono::milliseconds{80};
// driver event buffer needs to be big enough to hold however many events occur in latency time
constexpr std::int32_t DRIVER_BUFFER = 1024;  // events

auto pm_name() -> const char* {
  return "portmidi midi player";
}

#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#include <delayimp.h>
#include <windows.h>
#endif

constexpr int DEFAULT_VOLUME = 100;
std::array<int, 16> channel_volume;
float volume_scale;

bool use_reset_delay;
std::byte* sysex_reset;
std::array<std::byte, 11> gs_reset = {std::byte{0xf0}, std::byte{0x41}, std::byte{0x10}, std::byte{0x42},
                                      std::byte{0x12}, std::byte{0x40}, std::byte{0x00}, std::byte{0x7f},
                                      std::byte{0x00}, std::byte{0x41}, std::byte{0xf7}};
std::array<std::byte, 6> gm_system_on = {std::byte{0xf0}, std::byte{0x7e}, std::byte{0x7f},
                                         std::byte{0x09}, std::byte{0x01}, std::byte{0xf7}};
std::array<std::byte, 6> gm2_system_on = {std::byte{0xf0}, std::byte{0x7e}, std::byte{0x7f},
                                          std::byte{0x09}, std::byte{0x03}, std::byte{0xf7}};
std::array<std::byte, 9> xg_system_on = {std::byte{0xf0}, std::byte{0x43}, std::byte{0x10},
                                         std::byte{0x4c}, std::byte{0x00}, std::byte{0x00},
                                         std::byte{0x7e}, std::byte{0x00}, std::byte{0xf7}};
std::array<PmEvent, 16> event_notes_off;
std::array<PmEvent, 16> event_sound_off;
std::array<PmEvent, 16 * 6> event_reset;
std::array<PmEvent, 16 * 6> event_pbs;
std::array<PmEvent, 16> event_reverb;
std::array<PmEvent, 16> event_chorus;

namespace pm {
constexpr auto message(const std::uint8_t status, const std::uint8_t data1 = 0, const std::uint8_t data2 = 0)
    -> PmMessage {
  return Pm_Message(status, data1, data2);
}

auto write(PortMidiStream* const stream, const std::span<PmEvent> buffer) -> PmError {
  return ::Pm_Write(stream, buffer.data(), static_cast<std::int32_t>(buffer.size()));
}
}  // namespace pm

void reset_device() {
  pm::write(pm_stream.get(), event_notes_off);
  pm::write(pm_stream.get(), event_sound_off);

  if (sysex_reset == nullptr) {
    pm::write(pm_stream.get(), event_reset);
  } else {
    Pm_WriteSysEx(pm_stream.get(), 0, reinterpret_cast<byte*>(sysex_reset));
  }

  pm::write(pm_stream.get(), event_pbs);

  if (mus_portmidi_reverb_level > -1 || sysex_reset == nullptr) {
    pm::write(pm_stream.get(), event_reverb);
  }

  if (mus_portmidi_chorus_level > -1 || sysex_reset == nullptr) {
    pm::write(pm_stream.get(), event_chorus);
  }

  use_reset_delay = mus_portmidi_reset_delay > 0;
}

void init_reset_buffer() {
  PmEvent* reset = event_reset.data();
  PmEvent* pbs = event_pbs.data();
  int reverb = mus_portmidi_reverb_level;
  int chorus = mus_portmidi_chorus_level;

  for (std::uint8_t i = 0; i < 16; ++i) {
    event_notes_off[i].message = pm::message(0xB0 | i, 0x7B, 0x00);
    event_sound_off[i].message = pm::message(0xB0 | i, 0x78, 0x00);

    reset[0].message = pm::message(0xB0 | i, 0x79, 0x00);  // reset all controllers
    reset[1].message = pm::message(0xB0 | i, 0x07, 0x64);  // channel volume
    reset[2].message = pm::message(0xB0 | i, 0x0A, 0x40);  // pan
    reset[3].message = pm::message(0xB0 | i, 0x00, 0x00);  // bank select msb
    reset[4].message = pm::message(0xB0 | i, 0x20, 0x00);  // bank select lsb
    reset[5].message = pm::message(0xC0 | i, 0x00, 0x00);  // program change
    reset += 6;

    pbs[0].message = pm::message(0xB0 | i, 0x64, 0x00);  // pitch bend sens RPN LSB
    pbs[1].message = pm::message(0xB0 | i, 0x65, 0x00);  // pitch bend sens RPN MSB
    pbs[2].message = pm::message(0xB0 | i, 0x06, 0x02);  // data entry MSB
    pbs[3].message = pm::message(0xB0 | i, 0x26, 0x00);  // data entry LSB
    pbs[4].message = pm::message(0xB0 | i, 0x64, 0x7F);  // null RPN LSB
    pbs[5].message = pm::message(0xB0 | i, 0x65, 0x7F);  // null RPN MSB
    pbs += 6;
  }

  if (strcasecmp(mus_portmidi_reset_type, "gs") == 0) {
    sysex_reset = gs_reset.data();
  } else if (strcasecmp(mus_portmidi_reset_type, "gm") == 0) {
    sysex_reset = gm_system_on.data();
  } else if (strcasecmp(mus_portmidi_reset_type, "gm2") == 0) {
    sysex_reset = gm2_system_on.data();
  } else if (strcasecmp(mus_portmidi_reset_type, "xg") == 0) {
    sysex_reset = xg_system_on.data();
  } else {
    sysex_reset = nullptr;
  }

  // if no reverb specified and no SysEx reset selected, then use GM default
  if (reverb == -1 && sysex_reset == nullptr) {
    reverb = 40;
  }

  if (reverb > -1) {
    for (std::size_t i = 0; i < event_reverb.size(); ++i) {
      event_reverb[i].message = pm::message(0xB0 | i, 0x5B, reverb);
    }
  }

  // if no chorus specified and no SysEx reset selected, then use GM default
  if (chorus == -1 && sysex_reset == nullptr) {
    chorus = 0;
  }

  if (chorus > -1) {
    for (std::size_t i = 0; i < event_chorus.size(); ++i) {
      event_chorus[i].message = pm::message(0xB0 | i, 0x5D, chorus);
    }
  }
}

auto pm_init([[maybe_unused]] const int samplerate) -> int {
  TESTDLLLOAD("portmidi.dll", TRUE)

  if (Pm_Initialize() != pmNoError) {
    lprintf(LO_WARN, "portmidiplayer: Pm_Initialize () failed\n");
    return 0;
  }

  PmDeviceID outputdevice = Pm_GetDefaultOutputDeviceID();

  if (outputdevice == pmNoDevice) {
    lprintf(LO_WARN, "portmidiplayer: No output devices available\n");
    Pm_Terminate();
    return 0;
  }

  // look for a device that matches the user preference
  lprintf(LO_INFO, "portmidiplayer device list:\n");
  for (int i = 0; i < Pm_CountDevices(); i++) {
    const PmDeviceInfo* const oinfo = Pm_GetDeviceInfo(i);
    if (oinfo == nullptr || oinfo->output == 0) {
      continue;
    }

    const std::string devname = std::format("{}:{}", oinfo->interf, oinfo->name);
    const auto snd_mididev_sv = std::string_view{snd_mididev};
    if (!snd_mididev_sv.empty() && devname.find(snd_mididev_sv) != std::string::npos) {
      outputdevice = i;
      lprintf(LO_INFO, ">>%s\n", devname.c_str());
    } else {
      lprintf(LO_INFO, "  %s\n", devname.c_str());
    }
  }

  const PmDeviceInfo* const oinfo = Pm_GetDeviceInfo(outputdevice);

  lprintf(LO_INFO, "portmidiplayer: Opening device %s:%s for output\n", oinfo->interf, oinfo->name);

  PortMidiStream* pm_stream_tmp;
  const auto open_result = Pm_OpenOutput(&pm_stream_tmp, outputdevice, nullptr, DRIVER_BUFFER, nullptr, nullptr,
                                         std::chrono::milliseconds{DRIVER_LATENCY}.count());
  pm_stream.reset(pm_stream_tmp);
  if (open_result != pmNoError) {
    lprintf(LO_WARN, "portmidiplayer: Pm_OpenOutput () failed\n");
    Pm_Terminate();
    return 0;
  }

  init_reset_buffer();
  reset_device();

  for (int& i : channel_volume) {
    i = DEFAULT_VOLUME;
  }

  return 1;
}

void pm_stop();

void pm_shutdown() {
  if (pm_stream) {
    // stop all sound, in case of hanging notes
    if (pm_playing) {
      pm_stop();
    }

    /* ugly deadlock in portmidi win32 implementation:

    main thread gets stuck in Pm_Close
    midi thread (started by windows) gets stuck in winmm_streamout_callback

    winapi ref says:
    "Applications should not call any multimedia functions from inside the callback function,
     as doing so can cause a deadlock. Other system functions can safely be called from the callback."

    winmm_streamout_callback calls midiOutUnprepareHeader.  oops?


    since timestamps are slightly in the future, it's very possible to have some messages still in
    the windows midi queue when Pm_Close is called.  this is normally no problem, but if one so happens
    to dequeue and call winmm_streamout_callback at the exact right moment...

    fix: at this point, we've stopped generating midi messages.  sleep for more than DRIVER_LATENCY to ensure
    all messages are flushed.

    not a fix: calling Pm_Abort(); then midiStreamStop deadlocks instead of midiStreamClose.
    */
    Pt_Sleep(std::chrono::milliseconds{DRIVER_LATENCY * 2}.count());

    pm_stream.reset();
    Pm_Terminate();
  }
}

auto pm_registersong(const void* const data, const unsigned len) -> const void* {
  midimem_t mf{.data = static_cast<const byte*>(data), .len = len, .pos = 0};

  midifile.reset(MIDI_LoadFile(&mf));

  if (!midifile) {
    lprintf(LO_WARN, "pm_registersong: Failed to load MIDI.\n");
    return nullptr;
  }

  events.reset(MIDI_GenerateFlatList(midifile.get()));
  if (!events) {
    midifile.reset();
    return nullptr;
  }
  eventpos = 0;

  spmc = MIDI_spmc(midifile.get(), nullptr, 1000);

  // handle not used
  return data;
}

void writeevent(const PmTimestamp when,
                const std::uint8_t eve,
                const std::uint8_t channel,
                const std::uint8_t v1,
                const std::uint8_t v2) {
  const PmMessage m = pm::message(eve | channel, v1, v2);
  Pm_WriteShort(pm_stream.get(), when, m);
}

void write_volume(const PmTimestamp when, const std::uint8_t channel, const int volume) {
  const std::uint8_t vol = volume * volume_scale + 0.5f;
  writeevent(when, MIDI_EVENT_CONTROLLER, channel, MIDI_CONTROLLER_MAIN_VOLUME, vol);
  channel_volume[channel] = volume;
}

void update_volume() {
  for (std::uint8_t i = 0; i < channel_volume.size(); i++) {
    write_volume(0, i, channel_volume[i]);
  }
}

void reset_volume() {
  for (std::uint8_t i = 0; i < channel_volume.size(); i++) {
    write_volume(0, i, DEFAULT_VOLUME);
  }
}

void pm_setvolume(const int v) {
  if (pm_volume == v) {
    return;
  }

  pm_volume = v;
  volume_scale = std::sqrt(static_cast<float>(pm_volume) / 15);
  update_volume();
}

void pm_unregistersong([[maybe_unused]] const void* const handle) {
  events.reset();
  midifile.reset();
}

void pm_pause() {
  pm_paused = true;
  pm::write(pm_stream.get(), event_notes_off);
  pm::write(pm_stream.get(), event_sound_off);
}

void pm_resume() {
  pm_paused = false;
  trackstart = Pt_Time();
}

void pm_play([[maybe_unused]] const void* const handle, const int looping) {
  eventpos = 0;
  pm_looping = static_cast<bool>(looping);
  pm_playing = true;
  pm_delta = 0.0;

  if (pm_volume != -1) {  // set pm_volume first, see pm_setvolume()
    reset_volume();
  }

  trackstart = Pt_Time();
}

auto is_sysex_reset(const byte* const msg, const std::size_t len) -> bool {
  if (len < 6) {
    return false;
  }

  switch (msg[1]) {
    case 0x41:  // roland
      switch (msg[3]) {
        case 0x42:  // gs
          switch (msg[4]) {
            case 0x12:                            // dt1
              if (len == 11 && msg[5] == 0x00 &&  // address msb
                  msg[6] == 0x00 &&               // address
                  msg[7] == 0x7F &&               // address lsb
                  ((msg[8] == 0x00 &&             // data     (mode-1)
                    msg[9] == 0x01)
                   ||                  // checksum (mode-1)
                   (msg[8] == 0x01 &&  // data     (mode-2)
                    msg[9] == 0x00)))  // checksum (mode-2)
              {
                // sc-88 system mode set
                // F0 41 <dev> 42 12 00 00 7F 00 01 F7 (mode-1)
                // F0 41 <dev> 42 12 00 00 7F 01 00 F7 (mode-2)
                return true;
              }
              if (len == 11 && msg[5] == 0x40 &&  // address msb
                  msg[6] == 0x00 &&               // address
                  msg[7] == 0x7F &&               // address lsb
                  msg[8] == 0x00 &&               // data (gs reset)
                  msg[9] == 0x41)                 // checksum
              {
                // gs reset
                // F0 41 <dev> 42 12 40 00 7F 00 41 F7
                return true;
              }

              break;
          }
          break;
      }
      break;

    case 0x43:  // yamaha
      switch (msg[3]) {
        case 0x2B:                            // tg300
          if (len == 10 && msg[4] == 0x00 &&  // start address b20 - b14
              msg[5] == 0x00 &&               // start address b13 - b7
              msg[6] == 0x7F &&               // start address b6 - b0
              msg[7] == 0x00 &&               // data
              msg[8] == 0x01)                 // checksum
          {
            // tg300 all parameter reset
            // F0 43 <dev> 2B 00 00 7F 00 01 F7
            return true;
          }
          break;

        case 0x4C:                           // xg
          if (len == 9 && msg[4] == 0x00 &&  // address high
              msg[5] == 0x00 &&              // address mid
              (msg[6] == 0x7E ||             // address low (xg system on)
               msg[6] == 0x7F)
              &&               // address low (xg all parameter reset)
              msg[7] == 0x00)  // data
          {
            // xg system on, xg all parameter reset
            // F0 43 <dev> 4C 00 00 7E 00 F7
            // F0 43 <dev> 4C 00 00 7F 00 F7
            return true;
          }
          break;
      }
      break;

    case 0x7E:  // universal non-real time
      switch (msg[3]) {
        case 0x09:  // general midi
          if (len == 6
              && (msg[4] == 0x01 ||  // gm system on
                  msg[4] == 0x02 ||  // gm system off
                  msg[4] == 0x03))   // gm2 system on
          {
            // gm system on/off, gm2 system on
            // F0 7E <dev> 09 01 F7
            // F0 7E <dev> 09 02 F7
            // F0 7E <dev> 09 03 F7
            return true;
          }
          break;
      }
      break;
  }

  return false;
}

void writesysex(const PmTimestamp when, const int etype, byte* const data, const std::size_t len) {
  // sysex messages in midi files (smf 1.0 pages 6-7):
  // complete:        (F0 ... F7)
  // multi-packet:    (F0 ...) + (F7 ...) + ... + (F7 ... F7)
  // escape sequence: (F7 ...)

  if (len + sysexbufflen > SYSEX_BUFF_SIZE - 1) {
    // ignore messages that are too long
    sysexbufflen = 0;
    return;
  }

  if (etype == MIDI_EVENT_SYSEX_SPLIT && sysexbufflen == 0) {
    // ignore escape sequence
    return;
  }

  if (etype == MIDI_EVENT_SYSEX) {
    // start a new message (discards any previous incomplete message)
    sysexbuff[0] = std::byte{MIDI_EVENT_SYSEX};
    sysexbufflen = 1;
  }

  std::copy_n(reinterpret_cast<const std::byte*>(data), len, sysexbuff.data() + sysexbufflen);
  sysexbufflen += len;

  // process message if it's complete, otherwise do nothing yet
  if (sysexbuff[sysexbufflen - 1] == std::byte{MIDI_EVENT_SYSEX_SPLIT}) {
    Pm_WriteSysEx(pm_stream.get(), when, reinterpret_cast<byte*>(sysexbuff.data()));

    if (is_sysex_reset(reinterpret_cast<byte*>(sysexbuff.data()), sysexbufflen)) {
      reset_volume();
    }

    sysexbufflen = 0;
  }
}

void pm_stop() {
  pm_playing = false;

  // songs can be stopped at any time, so reset everything
  reset_device();

  // abort any partial sysex
  sysexbufflen = 0;
}

void pm_render(void* const vdest, const unsigned bufflen) {
  // wherever you see samples in here, think milliseconds
  PmTimestamp when = trackstart;
  const PmTimestamp newtime = Pt_Time();

  std::fill_n(static_cast<std::byte*>(vdest), bufflen * 4, std::byte{0});

  if (!pm_playing || pm_paused) {
    return;
  }

  while (true) {
    const midi_event_t* const currevent = events[eventpos];

    // how many samples away event is
    double eventdelta = currevent->delta_time * spmc;

    // delay after reset, for real devices only (e.g. roland sc-55)
    if (use_reset_delay) {
      eventdelta += mus_portmidi_reset_delay;
    }

    // how many we will render (rounding down); include delta offset
    const unsigned int samples = eventdelta + pm_delta;

    if (when + samples > newtime) {
      // overshoot; render some samples without processing an event
      pm_delta -= (newtime - when);  // save offset
      trackstart = newtime;
      return;
    }

    use_reset_delay = false;
    pm_delta += eventdelta - samples;
    when += samples;

    switch (currevent->event_type) {
      case MIDI_EVENT_SYSEX:
      case MIDI_EVENT_SYSEX_SPLIT:
        if (mus_portmidi_filter_sysex == 0) {
          writesysex(when, currevent->event_type, currevent->data.sysex.data, currevent->data.sysex.length);
        }
        break;

      case MIDI_EVENT_META:
        switch (currevent->data.meta.type) {
          case MIDI_META_SET_TEMPO:
            spmc = MIDI_spmc(midifile.get(), currevent, 1000);
            break;

          case MIDI_META_END_OF_TRACK:
            if (pm_looping) {
              eventpos = 0;

              // prevent hanging notes (doom2.wad MAP14, MAP22)
              for (std::uint8_t i = 0; i < 16; i++) {
                writeevent(when, 0xB0, i, 0x7B, 0x00);  // all notes off
                writeevent(when, 0xB0, i, 0x79, 0x00);  // reset all controllers
              }

              continue;
            }

            pm_stop();
            return;
        }
        break;  // not interested in most metas

      case MIDI_EVENT_CONTROLLER:
        if (currevent->data.channel.param1 == MIDI_CONTROLLER_MAIN_VOLUME) {
          write_volume(when, currevent->data.channel.channel, currevent->data.channel.param2);
          break;
        }

        if (currevent->data.channel.param1 == 0x79) {
          // ms gs synth resets volume if "reset all controllers" value isn't zero
          writeevent(when, 0xB0, currevent->data.channel.channel, 0x79, 0x00);
          break;
        }

        [[fallthrough]];

      default:
        writeevent(when, currevent->event_type, currevent->data.channel.channel, currevent->data.channel.param1,
                   currevent->data.channel.param2);
        break;
    }

    eventpos++;
  }
}
}  // namespace

const music_player_t pm_player = {pm_name,         pm_init,           pm_shutdown, pm_setvolume, pm_pause, pm_resume,
                                  pm_registersong, pm_unregistersong, pm_play,     pm_stop,      pm_render};

#endif  // HAVE_LIBPORTMIDI
