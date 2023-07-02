/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *
 *  Copyright
 *  (C) 2011 by Nicholai Main
 *  (C) 2021 by Gustavo Rehermann
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

// TODO: some duplicated code with this and the fluidsynth and portmidi
// players should be split off or something

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "alsaplayer.h"

#ifndef HAVE_ALSA
namespace {
auto alsa_name() -> const char* {
  return "alsa midi player (DISABLED)";
}

auto alsa_init([[maybe_unused]] const int samplerate) -> int {
  return 0;
}
}  // namespace

const music_player_t alsa_player = {alsa_name, alsa_init, nullptr, nullptr, nullptr, nullptr,
                                    nullptr,   nullptr,   nullptr, nullptr, nullptr};

#else  // HAVE_ALSA

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <string_view>

#include "i_sound.h"  // for snd_mididev
#include "lprintf.h"
#include "midifile.h"

namespace {
#define CHK_RET(stmt, msg) \
  if ((stmt) < 0) {        \
    return (msg);          \
  }
#define CHK_LPRINT(stmt, ltype, ...) \
  if ((stmt) < 0) {                  \
    lprintf(ltype, __VA_ARGS__);     \
  }
#define CHK_LPRINT_ERR(stmt, ltype, ...)                         \
  {                                                              \
    alsaplayer_err = (stmt);                                     \
    if (alsaplayer_err < 0) {                                    \
      lprintf(ltype, __VA_ARGS__, snd_strerror(alsaplayer_err)); \
    }                                                            \
  }
#define CHK_LPRINT_RET(stmt, ret, ltype, ...) \
  if ((stmt) < 0) {                           \
    lprintf(ltype, __VA_ARGS__);              \
    return (ret);                             \
  }
#define CHK_LPRINT_ERR_RET(stmt, ret, ltype, ...)                \
  {                                                              \
    alsaplayer_err = (stmt);                                     \
    if (alsaplayer_err < 0) {                                    \
      lprintf(ltype, __VA_ARGS__, snd_strerror(alsaplayer_err)); \
      return ret;                                                \
    }                                                            \
  }

std::unique_ptr<midi_event_t*[], decltype([](auto** p) { MIDI_DestroyFlatList(p); })> events;
std::size_t eventpos;
std::unique_ptr<midi_file_t, decltype([](auto* p) { MIDI_FreeFile(p); })> midifile;

bool alsa_playing;
bool alsa_paused;
bool alsa_looping;
int alsa_volume;
bool alsa_open = false;

// if set to 0, can auto-connect to default port
bool alsa_first_connected = false;

double spmc;
double alsa_delta;

unsigned long trackstart;

std::unique_ptr<snd_seq_t, decltype([](auto* p) { snd_seq_close(p); })> seq_handle;
}  // namespace
snd_seq_event_t seq_ev;
namespace {
int out_id;
int out_port;
int out_queue;

constexpr std::size_t SYSEX_BUFF_SIZE = 1024;
std::array<std::byte, SYSEX_BUFF_SIZE> sysexbuff;
std::size_t sysexbufflen;
}  // namespace

int alsaplayer_err;

namespace {
std::unique_ptr<snd_seq_queue_status_t, decltype([](auto* p) { snd_seq_queue_status_free(p); })> queue_status;
}  // namespace

////////////////////

// alsa output list functionality

int alsaplayer_num_outs;
alsaplay_output_t alsaplayer_outputs[64];

void alsaplay_clear_outputs() {
  // clear output list
  alsaplayer_num_outs = 0;
}

namespace {
std::unique_ptr<snd_seq_client_info_t, decltype([](auto* p) { snd_seq_client_info_free(p); })> cinfo;
std::unique_ptr<snd_seq_port_info_t, decltype([](auto* p) { snd_seq_port_info_free(p); })> pinfo;
}  // namespace

void alsaplay_refresh_outputs() {
  // port type and capabilities required from valid MIDI output
  constexpr int OUT_CAPS_DESIRED = (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);

  {
    decltype(cinfo)::pointer cinfo_v{};
    snd_seq_client_info_malloc(&cinfo_v);
    cinfo.reset(cinfo_v);
  }
  {
    decltype(pinfo)::pointer pinfo_v{};
    snd_seq_port_info_malloc(&pinfo_v);
    pinfo.reset(pinfo_v);
  }

  if (!seq_handle) {
    lprintf(LO_WARN, "alsaplay_refresh_outputs: Can't list ALSA output ports: seq_handle is not initialized\n");
    return;
  }

  alsaplay_clear_outputs();

  // clear client info
  snd_seq_client_info_set_client(cinfo.get(), -1);

  while (snd_seq_query_next_client(seq_handle.get(), cinfo.get()) == 0) {
    // list ports of each client

    const int client_num = snd_seq_client_info_get_client(cinfo.get());

    if (client_num == out_id) {
      // skip self
      continue;
    }

    if (snd_seq_client_info_get_num_ports(cinfo.get()) == 0) {
      // skip clients without ports
      continue;
    }

    // clear port info
    snd_seq_port_info_set_client(pinfo.get(), client_num);
    snd_seq_port_info_set_port(pinfo.get(), -1);

    while (snd_seq_query_next_port(seq_handle.get(), pinfo.get()) == 0) {
      const int port_num = snd_seq_port_info_get_port(pinfo.get());

      // check if port is valid midi output

      if ((snd_seq_port_info_get_type(pinfo.get()) & SND_SEQ_PORT_TYPE_MIDI_GENERIC) == 0) {
        continue;
      }

      if ((snd_seq_port_info_get_capability(pinfo.get()) & OUT_CAPS_DESIRED) != OUT_CAPS_DESIRED) {
        continue;
      }

      // add to outputs list

      const int out_ind = alsaplayer_num_outs++;

      alsaplayer_outputs[out_ind].client = client_num;
      alsaplayer_outputs[out_ind].port = port_num;

      const std::string_view client_name = snd_seq_client_info_get_name(cinfo.get());

      lprintf(LO_INFO, "alsaplay_refresh_outputs: output #%d: (%d:%d) %s\n", out_ind, client_num, port_num,
              client_name.data());

      // client name only up to 100 chars, so it always fits within a 120 byte buffer
      std::format_to(alsaplayer_outputs[out_ind].name, "{:.{}s} ({}:{})", client_name, 100, client_num, port_num);
    }
  }

  cinfo.reset();
  pinfo.reset();
}

int alsaplay_connect_output(const int which) {
  if (which >= alsaplayer_num_outs) {
    lprintf(LO_WARN, "alsaplay_connect_output: tried to connect to output listing at index out of bounds: %d\n", which);
    return -1;
  }

  return alsa_midi_set_dest(alsaplayer_outputs[which].client, alsaplayer_outputs[which].port);
}

const char* alsaplay_get_output_name(int which) {
  if (!seq_handle) {
    return nullptr;
  }

  if (which >= alsaplayer_num_outs) {
    return nullptr;
  }

  return alsaplayer_outputs[which].name;
}

////////////////////

// alsa utility functions

auto alsa_midi_default_dest() -> int {
  static int status;  // alsa error code
  static int code;    // *our* error code, for control flow

  static constexpr std::string_view loopback_check_name = "MIDI THROUGH";  // uppercase for comparison
  static constexpr signed char upper_diff = 'A' - 'a';
  constexpr int loopback_check_len = loopback_check_name.length();

  // port type and capabilities required from valid MIDI output
  constexpr int OUT_CAPS_DESIRED = (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);

  int loopback_cl = -1;
  int loopback_prt = 0;
  std::string_view loopback_name;

  {
    decltype(cinfo)::pointer cinfo_v{};
    snd_seq_client_info_malloc(&cinfo_v);
    cinfo.reset(cinfo_v);
  }
  {
    decltype(pinfo)::pointer pinfo_v{};
    snd_seq_port_info_malloc(&pinfo_v);
    pinfo.reset(pinfo_v);
  }

  if (!seq_handle) {
    lprintf(LO_WARN, "alsa_midi_default_dest: Can't list ALSA output ports: seq_handle is not initialized\n");
    return 0;
  }

  alsaplay_clear_outputs();

  // clear client info
  snd_seq_client_info_set_client(cinfo.get(), -1);

  while (snd_seq_query_next_client(seq_handle.get(), cinfo.get()) == 0) {
    // list ports of each client

    const int client_num = snd_seq_client_info_get_client(cinfo.get());

    if (client_num == out_id) {
      // skip self
      continue;
    }

    if (snd_seq_client_info_get_num_ports(cinfo.get()) == 0) {
      // skip clients without ports
      continue;
    }

    const std::string_view client_name = snd_seq_client_info_get_name(cinfo.get());

    if (client_name.length() >= loopback_check_len) {
      // check for and skip loopback (eg MIDI Through)

      int i{0};

      for (; i < loopback_check_len; i++) {
        char a = client_name[i];
        a = (a < 'a') || (a > 'z') ? a : a + upper_diff;

        if (a != loopback_check_name[i]) {
          break;
        }
      }

      if (i == loopback_check_len) {
        loopback_cl = client_num;
        loopback_name = client_name;

        continue;
      }
    }

    // clear port info
    snd_seq_port_info_set_client(pinfo.get(), client_num);
    snd_seq_port_info_set_port(pinfo.get(), -1);

    while (snd_seq_query_next_port(seq_handle.get(), pinfo.get()) == 0) {
      const int port_num = snd_seq_port_info_get_port(pinfo.get());

      // check if port is valid midi output

      if ((snd_seq_port_info_get_type(pinfo.get()) & SND_SEQ_PORT_TYPE_MIDI_GENERIC) == 0) {
        continue;
      }

      if ((snd_seq_port_info_get_capability(pinfo.get()) & OUT_CAPS_DESIRED) != OUT_CAPS_DESIRED) {
        continue;
      }

      if (loopback_cl == client_num) {
        // save as midi through port

        loopback_prt = port_num;
        break;
      }

      // connect to this port

      if (alsa_midi_set_dest(client_num, port_num) != 0) {
        lprintf(LO_WARN, "alsa_midi_default_dest: error connecting to default port %i:%i (%s): %s\n", client_num,
                port_num, client_name.data(), snd_strerror(alsaplayer_err));

        return 0;
      }

      lprintf(LO_INFO, "alsa_midi_default_dest: connected to default port %i:%i (%s)\n", client_num, port_num,
              client_name.data());

      return 1;
    }
  }

  // try midi through as last resort fallback

  if (loopback_cl != -1) {
    if ((status = alsa_midi_set_dest(loopback_cl, loopback_prt)) != 0) {
      lprintf(LO_WARN, "alsa_midi_default_dest: (fallback) error connecting to default port %i:%i (%s): %s\n",
              loopback_cl, loopback_prt, loopback_name.data(), snd_strerror(status));

      return 0;
    }

    lprintf(LO_INFO, "alsa_midi_default_dest: (fallback) connected to default port %i:%i (%s)\n", loopback_cl,
            loopback_prt, loopback_name.data());

    return 1;
  }

  // no default port
  lprintf(LO_WARN, "alsa_midi_default_dest: no default port found\n");

  return 0;
}

namespace {
auto alsa_midi_open() -> const char* {
  {
    decltype(seq_handle)::pointer seq_handle_v{};
    if (snd_seq_open(&seq_handle_v, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
      return "could not open sequencer";
    }
    seq_handle.reset(seq_handle_v);
  }

  CHK_RET(snd_seq_set_client_name(seq_handle.get(), "PrBoom+ MIDI"), "could not set client name")

  out_id = snd_seq_client_id(seq_handle.get());

  CHK_RET(out_port = snd_seq_create_simple_port(
              seq_handle.get(), "Music", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_READ,
              SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_SOFTWARE),
          "could not open alsa port")

  out_queue = snd_seq_alloc_named_queue(seq_handle.get(), "prboom music queue");

  {
    decltype(queue_status)::pointer queue_status_v{};
    snd_seq_queue_status_malloc(&queue_status_v);
    queue_status.reset(queue_status_v);
  }

  alsa_open = true;
  return nullptr;
}
}  // namespace

auto alsa_midi_set_dest(const int client, const int port) -> int {
  static int last_client = -1;
  static int last_port = 0;

  if (!seq_handle) {
    return -2;
  }

  // disconnect if previously connected

  if (last_client != 0) {
    snd_seq_disconnect_to(seq_handle.get(), out_port, last_client, last_port);

    last_client = client;
    last_port = port;
  }

  // connects to a destination alsa-midi client and port

  CHK_LPRINT_ERR_RET(snd_seq_connect_to(seq_handle.get(), out_port, client, port), -3, LO_WARN,
                     "alsa_midi_set_dest: error connecting to (%d:%d): %s", last_client, last_port);

  alsa_first_connected = true;
  return 0;
}

auto alsa_now() -> unsigned long {
  // get current position in millisecs

  // update queue status
  CHK_LPRINT_ERR_RET(snd_seq_get_queue_status(seq_handle.get(), out_queue, queue_status.get()), 0, LO_WARN,
                     "alsaplayer: alsa_now(): error getting queue status: %s\n");

  const snd_seq_real_time_t* const time = snd_seq_queue_status_get_real_time(queue_status.get());

  if (time == nullptr) {
    lprintf(LO_WARN, "alsaplayer: alsa_now(): error getting realtime position from queue status\n");
    return 0;
  }

  return time->tv_sec * 1000 + (time->tv_nsec / 1000000);  // (s,ns) to ms
}

////////////////////

// alsa player callbacks

namespace {
auto alsa_now_realtime() -> const snd_seq_real_time_t* {
  // get current position in millisecs

  // update queue status
  CHK_LPRINT_ERR_RET(snd_seq_get_queue_status(seq_handle.get(), out_queue, queue_status.get()), nullptr, LO_WARN,
                     "alsaplayer: alsa_now(): error getting queue status: %s\n");

  const snd_seq_real_time_t* const time = snd_seq_queue_status_get_real_time(queue_status.get());

  if (time == nullptr) {
    lprintf(LO_WARN, "alsaplayer: alsa_now(): error getting realtime position from queue status\n");
  }

  return time;
}

void alsa_midi_evt_start(const unsigned long when) {
  snd_seq_ev_clear(&seq_ev);

  // source
  snd_seq_ev_set_source(&seq_ev, out_port);

  // schedule
  if (when != 0) {
    // ms into (s,ns)
    const snd_seq_real_time_t rtime{.tv_sec = static_cast<unsigned int>(when / 1000),
                                    .tv_nsec = static_cast<unsigned int>((when % 1000) * 1000000)};

    snd_seq_ev_schedule_real(&seq_ev, out_queue, 0, &rtime);
  }

  else {
    snd_seq_ev_schedule_real(&seq_ev, out_queue, 0, alsa_now_realtime());
  }

  // priority
  snd_seq_ev_set_priority(&seq_ev, 0);

  // destination
  snd_seq_ev_set_subs(&seq_ev);
}

void alsa_midi_evt_finish() {
  CHK_LPRINT_ERR(snd_seq_event_output(seq_handle.get(), &seq_ev), LO_WARN,
                 "alsa_midi_evt_finish: could not output alsa midi event: %s\n");
}

void alsa_midi_evt_flush() {
  CHK_LPRINT_ERR(snd_seq_drain_output(seq_handle.get()), LO_WARN,
                 "alsa_midi_evt_finish: could not drain alsa sequencer output: %s\n");
}

void alsa_midi_write_event(const unsigned long when,
                           const midi_event_type_t type,
                           const int channel,
                           const int v1,
                           const int v2) {
  // ported from portmidiplayer.c (no pun intended!)
  alsa_midi_evt_start(when);

  // set event value fields
  switch (type) {
    case MIDI_EVENT_NOTE_OFF:
      snd_seq_ev_set_noteoff(&seq_ev, channel, v1, v2);
      break;

    case MIDI_EVENT_NOTE_ON:
      snd_seq_ev_set_noteon(&seq_ev, channel, v1, v2);
      break;

    case MIDI_EVENT_AFTERTOUCH:
      snd_seq_ev_set_keypress(&seq_ev, channel, v1, v2);
      break;

    case MIDI_EVENT_PROGRAM_CHANGE:
      snd_seq_ev_set_pgmchange(&seq_ev, channel, v1);
      break;

    case MIDI_EVENT_CHAN_AFTERTOUCH:
      snd_seq_ev_set_chanpress(&seq_ev, channel, v1);
      break;

    case MIDI_EVENT_PITCH_BEND:
      snd_seq_ev_set_pitchbend(&seq_ev, channel, v1 << 8 | v2);
      break;

    case MIDI_EVENT_CONTROLLER:
      snd_seq_ev_set_controller(&seq_ev, channel, v1, v2);
      break;

    default:
      // unknown type
      lprintf(LO_WARN, "alsa_midi_write_event: unknown midi event type: %d\n", type);
      return;
  }

  alsa_midi_evt_finish();
}

void alsa_midi_write_control(const unsigned long when, const int channel, const int v1, const int v2) {
  alsa_midi_write_event(when, MIDI_EVENT_CONTROLLER, channel, v1, v2);
}

void alsa_midi_write_control_now(const int channel, const int v1, const int v2) {
  // send event now, disregarding 'when'
  alsa_midi_write_control(0, channel, v1, v2);
}

void alsa_midi_all_notes_off_chan(const int channel) {
  alsa_midi_write_control_now(channel, 123, 0);
  alsa_midi_evt_flush();
}

void alsa_midi_all_notes_off() {
  // sends All Notes Off event in all channels
  for (int i = 0; i < 16; i++) {
    alsa_midi_all_notes_off_chan(i);
  }
}

auto alsa_midi_init_connect_default_port() -> int {
  // load MIDI device specified in config

  if (snd_mididev != nullptr && !std::string_view{snd_mididev}.empty()) {
    snd_seq_addr_t seqaddr;

    CHK_LPRINT_ERR(snd_seq_parse_address(seq_handle.get(), &seqaddr, snd_mididev), LO_WARN,
                   "alsa_init: Error connecting to configured MIDI output port \"%s\": %s", snd_mididev)

    return alsa_midi_set_dest(seqaddr.client, seqaddr.port) == 0;
  }

  // connect to default
  return alsa_midi_default_dest();
}

////////////////////

// alsa player callbacks

auto alsa_name() -> const char* {
  return "alsa midi player";
}

int alsa_init([[maybe_unused]] const int samplerate) {
  const char* msg = alsa_midi_open();

  lprintf(LO_INFO, "alsaplayer: Trying to open ALSA output port\n");

  if (msg != nullptr) {
    lprintf(LO_WARN, "alsa_init: alsa_midi_open() failed: %s\n", msg);
    return 0;
  }

  lprintf(LO_INFO, "alsaplayer: Successfully opened port: %d\n", out_port);

  alsaplay_refresh_outputs();  // make output list and print it out

  return 1;
}

void alsa_shutdown() {
  if (seq_handle) {
    alsa_midi_all_notes_off();
    alsa_midi_evt_flush();

    snd_seq_free_queue(seq_handle.get(), out_queue);
    queue_status.reset();

    snd_seq_delete_simple_port(seq_handle.get(), out_port);
    seq_handle.reset();
  }

  alsa_open = false;
}

auto alsa_registersong(const void* const data, const unsigned len) -> const void* {
  midimem_t mf{.data = static_cast<const byte*>(data), .len = len, .pos = 0};

  midifile.reset(MIDI_LoadFile(&mf));

  if (!midifile) {
    lprintf(LO_WARN, "alsa_registersong: Failed to load MIDI.\n");
    return nullptr;
  }

  events.reset(MIDI_GenerateFlatList(midifile.get()));
  if (!events) {
    midifile.reset();
    return nullptr;
  }

  eventpos = 0;

  // implicit 120BPM (this is correct to spec)
  // spmc = compute_spmc (MIDI_GetFileTimeDivision (midifile), 500000, 1000);
  spmc = MIDI_spmc(midifile.get(), nullptr, 1000);

  // handle not used
  return data;
}

std::array<int, 16> channelvol;

void alsa_setchvolume(const int ch, const int v, const unsigned long when) {
  channelvol[ch] = v;
  alsa_midi_write_control(when, ch, 7, channelvol[ch] * alsa_volume / 15);
  alsa_midi_evt_flush();
}

void alsa_refreshvolume() {
  for (int i = 0; i < 16; i++) {
    alsa_midi_write_control_now(i, 7, channelvol[i] * alsa_volume / 15);
  }

  alsa_midi_evt_flush();
}

void alsa_clearchvolume() {
  for (int i = 0; i < 16; i++) {
    channelvol[i] = 127;  // default: max
  }
}

void alsa_setvolume(const int v) {
  static bool firsttime = true;

  if (alsa_volume == v && !firsttime) {
    return;
  }

  firsttime = false;
  alsa_volume = v;

  alsa_refreshvolume();
}

void alsa_unregistersong([[maybe_unused]] const void* const handle) {
  events.reset();
  midifile.reset();
}

void alsa_pause() {
  alsa_paused = true;
  alsa_midi_all_notes_off();

  snd_seq_stop_queue(seq_handle.get(), out_queue, nullptr);
}

void alsa_resume() {
  alsa_paused = false;
  trackstart = alsa_now();

  snd_seq_continue_queue(seq_handle.get(), out_queue, nullptr);
}

static void alsa_play([[maybe_unused]] const void* const handle, const int looping) {
  std::unique_ptr<snd_seq_queue_timer_t, decltype([](auto* p) { snd_seq_queue_timer_free(p); })> timer;

  // reinit queue

  if (!alsa_first_connected) {
    // connect to default port if haven't connected at least once yet
    alsa_midi_init_connect_default_port();
  }

  if (out_queue != 0) {
    snd_seq_free_queue(seq_handle.get(), out_queue);
    queue_status.reset();
  }

  // make queue
  out_queue = snd_seq_alloc_named_queue(seq_handle.get(), "prboom music queue");

  {
    decltype(queue_status)::pointer queue_status_v{};
    snd_seq_queue_status_malloc(&queue_status_v);
    queue_status.reset(queue_status_v);
  }

  // set queue resolution

  {
    decltype(timer)::pointer timer_v{};
    snd_seq_queue_timer_malloc(&timer_v);
    timer.reset(timer_v);
  }

  int status = snd_seq_get_queue_timer(seq_handle.get(), out_queue, timer.get());

  if (status < 0) {
    lprintf(LO_WARN, "alsa_play: error getting sched queue timer: %s\n", snd_strerror(status));

    goto finish;
  }

  snd_seq_queue_timer_set_resolution(timer.get(), 1000000 / 32);  // 1000000 ns = 1 ms, so this is 1/32 ms

  status = snd_seq_set_queue_timer(seq_handle.get(), out_queue, timer.get());

  if (status < 0) {
    lprintf(LO_WARN, "alsa_play: error setting sched queue timer with new resolution: %s\n", snd_strerror(status));

    goto finish;
  }

  lprintf(LO_INFO, "alsa_play: success\n");

finish:
  timer.reset();

  // initialize state stuff
  eventpos = 0;
  alsa_looping = static_cast<bool>(looping);
  alsa_playing = true;
  // alsa_paused = 0;
  alsa_delta = 0.0;
  alsa_clearchvolume();
  alsa_refreshvolume();
  trackstart = alsa_now();

  // start scheduling queue
  snd_seq_start_queue(seq_handle.get(), out_queue, nullptr);
}

void alsa_midi_writesysex(const unsigned long when,
                          [[maybe_unused]] const int etype,
                          unsigned char* const data,
                          const int len) {
  // sysex code is untested
  // it's possible to use an auto-resizing buffer here, but a malformed
  // midi file could make it grow arbitrarily large (since it must grow
  // until it hits an 0xf7 terminator)
  if (len + sysexbufflen > SYSEX_BUFF_SIZE) {
    lprintf(LO_WARN, "alsaplayer: ignoring large or malformed sysex message\n");
    sysexbufflen = 0;
    return;
  }

  std::copy_n(reinterpret_cast<const std::byte*>(data), len, sysexbuff.data() + sysexbufflen);
  sysexbufflen += len;

  if (sysexbuff[sysexbufflen - 1] == std::byte{0xf7}) {  // terminator
    alsa_midi_evt_start(when);
    snd_seq_ev_set_sysex(&seq_ev, sysexbufflen, sysexbuff.data());
    alsa_midi_evt_finish();

    sysexbufflen = 0;
  }
}

void alsa_stop() {
  alsa_playing = false;

  // songs can be stopped at any time, so reset everything
  for (int i = 0; i < 16; i++) {
    alsa_midi_write_control_now(i, 123, 0);  // all notes off
    alsa_midi_write_control_now(i, 121, 0);  // reset all parameters

    // RPN sequence to adjust pitch bend range (RPN value 0x0000)
    alsa_midi_write_control_now(i, 0x65, 0x00);
    alsa_midi_write_control_now(i, 0x64, 0x00);
    // reset pitch bend range to central tuning +/- 2 semitones and 0 cents
    alsa_midi_write_control_now(i, 0x06, 0x02);
    alsa_midi_write_control_now(i, 0x26, 0x00);
    // end of RPN sequence
    alsa_midi_write_control_now(i, 0x64, 0x7f);
    alsa_midi_write_control_now(i, 0x65, 0x7f);
  }
  alsa_midi_evt_flush();
  // abort any partial sysex
  sysexbufflen = 0;

  snd_seq_stop_queue(seq_handle.get(), out_queue, nullptr);
}

void alsa_render(void* const vdest, const unsigned bufflen) {
  // wherever you see samples in here, think milliseconds

  const unsigned long newtime = alsa_now();
  const unsigned long length = newtime - trackstart;

  // timerpos = newtime;

  unsigned sampleswritten = 0;
  unsigned samples;

  std::fill_n(static_cast<std::byte*>(vdest), bufflen * 4, std::byte{0});

  if (!alsa_playing || alsa_paused) {
    return;
  }

  while (true) {
    const midi_event_t* const currevent = events[eventpos];

    // how many samples away event is
    const double eventdelta = currevent->delta_time * spmc;

    // how many we will render (rounding down); include delta offset
    samples = static_cast<unsigned>(eventdelta + alsa_delta);

    if (samples + sampleswritten > length) {  // overshoot; render some samples without processing an event
      break;
    }

    sampleswritten += samples;
    alsa_delta -= samples;

    // process event
    const unsigned long when = trackstart + sampleswritten;
    switch (currevent->event_type) {
      case MIDI_EVENT_SYSEX:
      case MIDI_EVENT_SYSEX_SPLIT:
        alsa_midi_writesysex(when, currevent->event_type, currevent->data.sysex.data, currevent->data.sysex.length);
        break;

      case MIDI_EVENT_META:  // tempo is the only meta message we're interested in
        if (currevent->data.meta.type == MIDI_META_SET_TEMPO) {
          spmc = MIDI_spmc(midifile.get(), currevent, 1000);
        } else if (currevent->data.meta.type == MIDI_META_END_OF_TRACK) {
          if (alsa_looping) {
            eventpos = 0;
            alsa_delta += eventdelta;
            // fix buggy songs that forget to terminate notes held over loop point
            // sdl_mixer does this as well
            for (int i = 0; i < 16; i++) {
              alsa_midi_write_control(when, i, 123, 0);  // all notes off
            }
            continue;
          }
          // stop
          alsa_stop();
          return;
        }
        break;  // not interested in most metas

      case MIDI_EVENT_CONTROLLER:
        if (currevent->data.channel.param1 == 7) {  // volume event
          alsa_setchvolume(currevent->data.channel.channel, currevent->data.channel.param2, when);
          break;
        }
        [[fallthrough]];

      default:
        alsa_midi_write_event(when, currevent->event_type, currevent->data.channel.channel,
                              currevent->data.channel.param1, currevent->data.channel.param2);
        break;
    }

    // if the event was a "reset all controllers", we need to additionally re-fix the volume (which itself was reset)
    if (currevent->event_type == MIDI_EVENT_CONTROLLER && currevent->data.channel.param1 == 121) {
      alsa_setchvolume(currevent->data.channel.channel, 127, when);
    }

    // event processed so advance midiclock
    alsa_delta += eventdelta;
    eventpos++;
  }

  if (samples + sampleswritten > length) {  // broke due to next event being past the end of current render buffer
    // finish buffer, return
    samples = length - sampleswritten;
    alsa_delta -= samples;  // save offset
  }

  trackstart = newtime;

  alsa_midi_evt_flush();
}
}  // namespace

const music_player_t alsa_player = {alsa_name,  alsa_init,   alsa_shutdown,     alsa_setvolume,
                                    alsa_pause, alsa_resume, alsa_registersong, alsa_unregistersong,
                                    alsa_play,  alsa_stop,   alsa_render};

#endif  // HAVE_ALSA
