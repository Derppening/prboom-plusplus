// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 2009 Simon Howard
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
//    Reading of MIDI files.
//
//-----------------------------------------------------------------------------

#include "midifile.h"

#include <cassert>
#include <cstdint>

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <memory>
#include <numeric>
#include <optional>
#include <string_view>
#include <vector>

#ifndef TEST
#include "doomdef.h"
#include "doomtype.h"
#endif

#include "lprintf.h"

#ifdef TEST
#include "m_io.h"
#endif  // TEST

namespace {
constexpr std::string_view HEADER_CHUNK_ID = "MThd";
constexpr std::string_view TRACK_CHUNK_ID = "MTrk";
constexpr std::int32_t MAX_BUFFER_SIZE = 0x10000;
}  // namespace

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

namespace {
#ifndef ntohl
template<std::integral T>
constexpr auto byteswap(T value) noexcept -> T {
  static_assert(std::has_unique_object_representations_v<T>, "T may not have padding bits");

  auto value_representation = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
  std::ranges::reverse(value_representation);
  return std::bit_cast<T>(value_representation);
}

constexpr auto ntohl(const std::uint32_t x) -> std::uint32_t {
  if constexpr (std::endian::native == std::endian::big) {
    return x;
  } else if constexpr (std::endian::native == std::endian::little) {
    return byteswap(x);
  } else {
    static_assert(std::endian::native == std::endian::big || std::endian::native == std::endian::little);
  }
}

constexpr auto ntohs(const std::uint16_t x) -> std::uint16_t {
  if constexpr (std::endian::native == std::endian::big) {
    return x;
  } else if constexpr (std::endian::native == std::endian::little) {
    return byteswap(x);
  } else {
    static_assert(std::endian::native == std::endian::big || std::endian::native == std::endian::little);
  }
}
#endif  // ntohl
}  // namespace

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

struct chunk_header_t {
  std::array<std::byte, 4> chunk_id;
  unsigned int chunk_size;
} PACKEDATTR;

struct midi_header_t {
  chunk_header_t chunk_header;
  unsigned short format_type;
  unsigned short num_tracks;
  unsigned short time_division;
} PACKEDATTR;

#ifdef _MSC_VER
#pragma pack(pop)
#endif

struct midi_track_t {
  // Length in bytes:
  unsigned int data_len;

  // Events in this track:
  std::vector<midi_event_t> events;
};

struct midi_track_iter_s {
  midi_track_t* track;
  unsigned int position;
};

struct midi_file_s {
  midi_header_t header;

  // All tracks in this file:
  std::vector<midi_track_t, z_allocator<midi_track_t>> tracks;

  // Data buffer used to store data read for SysEx or meta events:
  std::vector<byte> buffer;
};

namespace {
// Check the header of a chunk:
auto CheckChunkHeader(const chunk_header_t& chunk, const std::string_view expected_id) -> bool {
  //  const bool result = (memcmp((char*)chunk->chunk_id.data(), expected_id, 4) == 0);
  const bool result = std::string_view{reinterpret_cast<const char*>(chunk.chunk_id.data()), 4} == expected_id;
  if (!result) {
    lprintf(LO_WARN,
            "CheckChunkHeader: Expected '%s' chunk header, "
            "got '%c%c%c%c'\n",
            expected_id.data(), std::to_integer<char>(chunk.chunk_id[0]), std::to_integer<char>(chunk.chunk_id[1]),
            std::to_integer<char>(chunk.chunk_id[2]), std::to_integer<char>(chunk.chunk_id[3]));
  }

  return result;
}

auto CheckChunkHeader(chunk_header_t* const chunk, const char* const expected_id) -> bool {
  return CheckChunkHeader(*chunk, std::string_view{expected_id});
}

// Read a single byte.  Returns nullopt on error.
auto ReadByte(midimem_t& mf) -> std::optional<byte> {
  if (mf.pos >= mf.len) {
    lprintf(LO_WARN, "ReadByte: Unexpected end of file\n");
    return std::nullopt;
  }

  return std::make_optional(mf.data[mf.pos++]);
}

template<typename OutT = void*>
  requires std::is_pointer_v<OutT>
auto ReadMultipleBytes(const OutT dest, const std::size_t len, midimem_t& mf) -> OutT {
  auto* const cdest = reinterpret_cast<std::byte*>(dest);
  for (std::size_t i = 0; i < len; i++) {
    const auto byte = ReadByte(mf);
    if (!byte) {
      lprintf(LO_WARN, "ReadMultipleBytes: Unexpected end of file\n");
      return nullptr;
    }

    cdest[i] = std::byte{*byte};
  }

  return reinterpret_cast<OutT>(cdest + len);
}

// Read a variable-length value.
auto ReadVariableLength(midimem_t& mf) -> std::optional<unsigned int> {
  unsigned int result = 0;

  for (std::size_t i = 0; i < 4; ++i) {
    const auto b = ReadByte(mf);
    if (!b) {
      lprintf(LO_WARN, "ReadVariableLength: Error while reading variable-length value\n");
      return std::nullopt;
    }

    // Insert the bottom seven bits from this byte.
    result <<= 7;
    result |= *b & 0x7f;

    // If the top bit is not set, this is the end.
    if ((*b & 0x80) == 0) {
      return std::make_optional(result);
    }
  }

  lprintf(LO_WARN, "ReadVariableLength: Variable-length value too long: maximum of four bytes\n");
  return std::nullopt;
}

// Read a byte sequence into the data buffer.
template<typename OutT = void*>
  requires std::is_pointer_v<OutT>
auto ReadByteSequence(const std::size_t num_bytes, midimem_t& mf) -> OutT {
  // events can be length 0.  malloc(0) is not portable (can return NULL)
  if (num_bytes == 0) {
    return static_cast<OutT>(malloc(4));
  }

  // Allocate a buffer:
  auto result = std::unique_ptr<byte[], decltype([](auto* p) { free(p); })>(static_cast<byte*>(malloc(num_bytes)));

  if (!result) {
    lprintf(LO_WARN, "ReadByteSequence: Failed to allocate buffer %zu bytes\n", num_bytes);
    return nullptr;
  }

  // Read the data:
  for (std::size_t i = 0; i < num_bytes; ++i) {
    const auto value = ReadByte(mf);
    if (!value) {
      lprintf(LO_WARN, "ReadByteSequence: Error while reading byte %zu\n", i);
      return nullptr;
    }

    result[i] = *value;
  }

  return static_cast<OutT>(result.release());
}

// Read a MIDI channel event.
// two_param indicates that the event type takes two parameters
// (three byte) otherwise it is single parameter (two byte)
auto ReadChannelEvent(midi_event_t event, const byte event_type, const bool two_param, midimem_t& mf)
    -> std::optional<midi_event_t> {
  // Set basics:
  event.event_type = static_cast<midi_event_type_t>(event_type & 0xf0);
  event.data.channel.channel = event_type & 0x0f;

  std::optional<byte> b;

  // Read parameters:
  b = ReadByte(mf);
  if (!b) {
    lprintf(LO_WARN, "ReadChannelEvent: Error while reading channel event parameters\n");
    return std::nullopt;
  }

  event.data.channel.param1 = *b;

  // Second parameter:
  if (two_param) {
    b = ReadByte(mf);
    if (!b) {
      lprintf(LO_WARN, "ReadChannelEvent: Error while reading channel event parameters\n");
      return std::nullopt;
    }

    event.data.channel.param2 = *b;
  }

  return std::make_optional(event);
}

// Read sysex event:
auto ReadSysExEvent(midi_event_t event, const int event_type, midimem_t& mf) -> std::optional<midi_event_t> {
  event.event_type = static_cast<midi_event_type_t>(event_type);

  const auto len = ReadVariableLength(mf);
  if (!len) {
    lprintf(LO_WARN, "ReadSysExEvent: Failed to read length of SysEx block\n");
    return std::nullopt;
  }

  event.data.sysex.length = *len;

  // Read the byte sequence:
  event.data.sysex.data = ReadByteSequence<byte*>(event.data.sysex.length, mf);

  if (event.data.sysex.data == nullptr) {
    lprintf(LO_WARN, "ReadSysExEvent: Failed while reading SysEx event\n");
    return std::nullopt;
  }

  return std::make_optional(event);
}

// Read meta event:
auto ReadMetaEvent(midi_event_t event, midimem_t& mf) -> std::optional<midi_event_t> {
  event.event_type = MIDI_EVENT_META;

  // R.meta event type:
  const auto b = ReadByte(mf);
  if (!b) {
    lprintf(LO_WARN, "ReadMetaEvent: Failed to read meta event type\n");
    return std::nullopt;
  }

  event.data.meta.type = *b;

  // Read length of meta event data:
  const auto len = ReadVariableLength(mf);
  if (!len) {
    lprintf(LO_WARN, "ReadMetaEvent: Failed to read length of MetaEvent block\n");
    return std::nullopt;
  }

  event.data.meta.length = *len;

  // Read the byte sequence:
  event.data.meta.data = ReadByteSequence<byte*>(event.data.meta.length, mf);

  if (event.data.meta.data == nullptr) {
    lprintf(LO_WARN, "ReadMetaEvent: Failed while reading MetaEvent\n");
    return std::nullopt;
  }

  return std::make_optional(event);
}

auto ReadEvent(unsigned int& last_event_type, midimem_t& mf) -> std::optional<midi_event_t> {
  midi_event_t event;

  const auto delta_time = ReadVariableLength(mf);
  if (!delta_time) {
    lprintf(LO_WARN, "ReadEvent: Failed to read event timestamp\n");
    return std::nullopt;
  }

  event.delta_time = *delta_time;

  auto event_type = ReadByte(mf);
  if (!event_type) {
    lprintf(LO_WARN, "ReadEvent: Failed to read event type\n");
    return std::nullopt;
  }

  // All event types have their top bit set.  Therefore, if
  // the top bit is not set, it is because we are using the "same
  // as previous event type" shortcut to save a byte.  Skip back
  // a byte so that we read this byte again.
  if ((*event_type & 0x80) == 0) {
    event_type = std::make_optional(static_cast<byte>(last_event_type));
    mf.pos--;
  } else {
    last_event_type = *event_type;
  }

  // Check event type:
  switch (*event_type & 0xf0) {
    // Two parameter channel events:
    case MIDI_EVENT_NOTE_OFF:
    case MIDI_EVENT_NOTE_ON:
    case MIDI_EVENT_AFTERTOUCH:
    case MIDI_EVENT_CONTROLLER:
    case MIDI_EVENT_PITCH_BEND:
      return ReadChannelEvent(event, *event_type, true, mf);

    // Single parameter channel events:
    case MIDI_EVENT_PROGRAM_CHANGE:
    case MIDI_EVENT_CHAN_AFTERTOUCH:
      return ReadChannelEvent(event, *event_type, false, mf);

    default:
      break;
  }

  // Specific value?
  switch (*event_type) {
    case MIDI_EVENT_SYSEX:
    case MIDI_EVENT_SYSEX_SPLIT:
      return ReadSysExEvent(event, *event_type, mf);

    case MIDI_EVENT_META:
      return ReadMetaEvent(event, mf);

    default:
      break;
  }

  lprintf(LO_WARN, "ReadEvent: Unknown MIDI event type: 0x%x\n", *event_type);
  return std::nullopt;
}

// Free an event:
void FreeEvent(midi_event_t& event) {
  // Some event types have dynamically allocated buffers assigned
  // to them that must be freed.
  switch (event.event_type) {
    case MIDI_EVENT_SYSEX:
    case MIDI_EVENT_SYSEX_SPLIT:
      free(event.data.sysex.data);
      break;

    case MIDI_EVENT_META:
      free(event.data.meta.data);
      break;

    default:
      // Nothing to do.
      break;
  }
}

// Read and check the track chunk header
auto ReadTrackHeader(midi_track_t& track, midimem_t& mf) -> bool {
  chunk_header_t chunk_header;
  const auto* const records_read = ReadMultipleBytes(&chunk_header, sizeof(chunk_header_t), mf);
  if (!records_read) {
    return false;
  }

  if (!CheckChunkHeader(chunk_header, TRACK_CHUNK_ID)) {
    return false;
  }

  track.data_len = ntohl(chunk_header.chunk_size);

  return true;
}

auto ReadTrack(midimem_t& mf) -> std::optional<midi_track_t> {
  //  midi_track_t track{.data_len = {}, .events = nullptr, .num_events = 0, .num_event_mem = 0 /* NSM */};
  midi_track_t track{.data_len = {}, .events = {}};

  // Read the header:
  if (!ReadTrackHeader(track, mf)) {
    return std::nullopt;
  }

  // Then the events:
  unsigned int last_event_type = 0;

  while (true) {
    // Resize the track slightly larger to hold another event:
    /*
    new_events = realloc(track->events,
                         sizeof(midi_event_t) * (track->num_events + 1));
    */
    /*
    if (track.events.size() == track.events.capacity()) {  // depending on the state of the heap and the malloc
    implementation, realloc()
      // one more event at a time can be VERY slow.  10sec+ in MSVC
      track.events.reserve(track.events.size() + 100);
    }
    */
    // We use std::vector's internal reallocation - Runs in amortized O(n) instead of O(n^2)

    // Read the next event:
    const auto event = ReadEvent(last_event_type, mf);
    if (!event) {
      return std::nullopt;
    }

    track.events.emplace_back(std::move(*event));

    // End of track?
    if (event->event_type == MIDI_EVENT_META && event->data.meta.type == MIDI_META_END_OF_TRACK) {
      break;
    }
  }

  return std::make_optional(track);
}

// Free a track:
void FreeTrack(midi_track_t& track) {
  for (auto& event : track.events) {
    FreeEvent(event);
  }

  track.events.clear();
}

auto ReadAllTracks(midi_file_t& file, midimem_t& mf) -> bool {
  // Read each track:
  for (std::size_t i = 0; i < file.tracks.capacity(); ++i) {
    auto track = ReadTrack(mf);
    if (!track) {
      return false;
    }

    file.tracks.emplace_back(std::move(*track));
  }

  return true;
}

// Read and check the header chunk.
auto ReadFileHeader(midi_file_t& file, midimem_t& mf) -> bool {
  const auto* records_read = ReadMultipleBytes(&file.header, sizeof(midi_header_t), mf);

  if (records_read == nullptr) {
    return false;
  }

  if (!CheckChunkHeader(&file.header.chunk_header, HEADER_CHUNK_ID.data())
      || ntohl(file.header.chunk_header.chunk_size) != 6) {
    lprintf(LO_WARN, "ReadFileHeader: Invalid MIDI chunk header! chunk_size=%ld\n",
            ntohl(file.header.chunk_header.chunk_size));
    return false;
  }

  const unsigned int format_type = ntohs(file.header.format_type);
  file.tracks.reserve(ntohs(file.header.num_tracks));

  if ((format_type != 0 && format_type != 1) || file.tracks.capacity() < 1) {
    lprintf(LO_WARN, "ReadFileHeader: Only type 0/1 "
                     "MIDI files supported!\n");
    return false;
  }
  // NSM
  file.header.time_division = ntohs(file.header.time_division);

  return true;
}
}  // namespace

void MIDI_FreeFile(midi_file_t* const file) {
  file->tracks.clear();

  file->~midi_file_t();
  free(file);
}

auto MIDI_LoadFile(midimem_t* const mf) -> midi_file_t* {
  std::unique_ptr<midi_file_t, decltype([](auto* p) {
                    MIDI_FreeFile(p);
                    free(p);
                  })>
      file{static_cast<midi_file_t*>(malloc(sizeof(midi_file_t)))};

  if (!file) {
    return nullptr;
  }

  new (file.get()) midi_file_t{};

  file->tracks.clear();
  file->buffer.clear();

  // Read MIDI file header
  if (!ReadFileHeader(*file, *mf)) {
    file.reset();
    return nullptr;
  }

  // Read all tracks:
  if (!ReadAllTracks(*file, *mf)) {
    file.reset();
    return nullptr;
  }

  return file.release();
}

// Get the number of tracks in a MIDI file.

unsigned int MIDI_NumTracks(const midi_file_t* file) {
  return file->tracks.size();
}

// Start iterating over the events in a track.

auto MIDI_IterateTrack(const midi_file_t* const file, const unsigned int track) -> midi_track_iter_t* {
  assert(track < file->tracks.size());

  midi_track_iter_t* const iter = static_cast<midi_track_iter_t*>(malloc(sizeof(*iter)));
  iter->track = const_cast<midi_track_t*>(&file->tracks[track]);
  iter->position = 0;

  return iter;
}

void MIDI_FreeIterator(midi_track_iter_t* iter) {
  free(iter);
}

// Get the time until the next MIDI event in a track.

auto MIDI_GetDeltaTime(midi_track_iter_t* const iter) -> unsigned int {
  if (iter->position < iter->track->events.size()) {
    const midi_event_t* const next_event = &iter->track->events[iter->position];

    return next_event->delta_time;
  }

  return 0;
}

// Get a pointer to the next MIDI event.

auto MIDI_GetNextEvent(midi_track_iter_t* const iter, midi_event_t** const event) -> int {
  if (iter->position < iter->track->events.size()) {
    *event = &iter->track->events[iter->position];
    ++iter->position;

    return 1;
  }

  return 0;
}

unsigned int MIDI_GetFileTimeDivision(const midi_file_t* const file) {
  return file->header.time_division;
}

void MIDI_RestartIterator(midi_track_iter_t* const iter) {
  iter->position = 0;
}

namespace {
void MIDI_PrintFlatListDBG(const midi_event_t** evs) {
  while (true) {
    const midi_event_t* event = *evs++;

    if (event->delta_time > 0) {
      std::printf("Delay: %i ticks\n", event->delta_time);
    }

    switch (event->event_type) {
      case MIDI_EVENT_NOTE_OFF:
        std::printf("MIDI_EVENT_NOTE_OFF\n");
        break;
      case MIDI_EVENT_NOTE_ON:
        std::printf("MIDI_EVENT_NOTE_ON\n");
        break;
      case MIDI_EVENT_AFTERTOUCH:
        std::printf("MIDI_EVENT_AFTERTOUCH\n");
        break;
      case MIDI_EVENT_CONTROLLER:
        std::printf("MIDI_EVENT_CONTROLLER\n");
        break;
      case MIDI_EVENT_PROGRAM_CHANGE:
        std::printf("MIDI_EVENT_PROGRAM_CHANGE\n");
        break;
      case MIDI_EVENT_CHAN_AFTERTOUCH:
        std::printf("MIDI_EVENT_CHAN_AFTERTOUCH\n");
        break;
      case MIDI_EVENT_PITCH_BEND:
        std::printf("MIDI_EVENT_PITCH_BEND\n");
        break;
      case MIDI_EVENT_SYSEX:
        std::printf("MIDI_EVENT_SYSEX\n");
        break;
      case MIDI_EVENT_SYSEX_SPLIT:
        std::printf("MIDI_EVENT_SYSEX_SPLIT\n");
        break;
      case MIDI_EVENT_META:
        std::printf("MIDI_EVENT_META\n");
        break;

      default:
        std::printf("(unknown)\n");
        break;
    }
    switch (event->event_type) {
      case MIDI_EVENT_NOTE_OFF:
      case MIDI_EVENT_NOTE_ON:
      case MIDI_EVENT_AFTERTOUCH:
      case MIDI_EVENT_CONTROLLER:
      case MIDI_EVENT_PROGRAM_CHANGE:
      case MIDI_EVENT_CHAN_AFTERTOUCH:
      case MIDI_EVENT_PITCH_BEND:
        std::printf("\tChannel: %i\n", event->data.channel.channel);
        std::printf("\tParameter 1: %i\n", event->data.channel.param1);
        std::printf("\tParameter 2: %i\n", event->data.channel.param2);
        break;

      case MIDI_EVENT_SYSEX:
      case MIDI_EVENT_SYSEX_SPLIT:
        std::printf("\tLength: %i\n", event->data.sysex.length);
        break;

      case MIDI_EVENT_META:
        std::printf("\tMeta type: %i\n", event->data.meta.type);
        std::printf("\tLength: %i\n", event->data.meta.length);
        break;
    }
    if (event->event_type == MIDI_EVENT_META && event->data.meta.type == MIDI_META_END_OF_TRACK) {
      std::printf("gotta go!\n");
      return;
    }
  }
}
}  // namespace

// NSM: an alternate iterator tool.

auto MIDI_GenerateFlatList(midi_file_t* const file) -> midi_event_t** {
  //  unsigned i;

  int totaldelta = 0;

  std::vector<int> trackpos(file->tracks.size());
  std::vector<int> tracktime(file->tracks.size());
  std::size_t trackactive = file->tracks.size();

  const int totalevents = std::accumulate(file->tracks.cbegin(), file->tracks.cend(), 0,
                                          [](const auto acc, const auto& v) { return acc + v.events.size(); });

  auto ret = std::unique_ptr<midi_event_t* [], decltype([](auto* p) { free(p); })> {
    static_cast<midi_event_t**>(malloc(totalevents * sizeof(midi_event_t**)))
  };
  midi_event_t** epos = ret.get();

  while (trackactive != 0) {
    unsigned delta = 0x10000000;
    int nextrk = -1;

    for (std::size_t i = 0; i < file->tracks.size(); i++) {
      if (trackpos[i] != -1 && file->tracks[i].events[trackpos[i]].delta_time - tracktime[i] < delta) {
        delta = file->tracks[i].events[trackpos[i]].delta_time - tracktime[i];
        nextrk = i;
      }
    }

    if (nextrk == -1) {  // unexpected EOF (not every track got end track)
      break;
    }

    *epos = file->tracks[nextrk].events.data() + trackpos[nextrk];

    for (unsigned i = 0; i < file->tracks.size(); i++) {
      if (i == static_cast<unsigned>(nextrk)) {
        tracktime[i] = 0;
        trackpos[i]++;
      } else
        tracktime[i] += delta;
    }

    // yes, this clobbers the original timecodes
    epos[0]->delta_time = delta;
    totaldelta += delta;

    if (epos[0]->event_type == MIDI_EVENT_META
        && epos[0]->data.meta.type == MIDI_META_END_OF_TRACK) {  // change end of track into no op
      trackactive--;
      trackpos[nextrk] = -1;
      epos[0]->data.meta.type = MIDI_META_TEXT;
    } else if (static_cast<unsigned>(trackpos[nextrk]) == file->tracks[nextrk].events.size()) {
      lprintf(LO_WARN, "MIDI_GenerateFlatList: Unexpected end of track\n");
      return nullptr;
    }

    epos++;
  }

  if (trackactive != 0) {  // unexpected EOF
    lprintf(LO_WARN, "MIDI_GenerateFlatList: Unexpected end of midi file\n");
    return nullptr;
  }

  // last end of track event is preserved though
  epos[-1]->data.meta.type = MIDI_META_END_OF_TRACK;

  if (totaldelta < 100) {
    lprintf(LO_WARN, "MIDI_GeneratFlatList: very short file %i\n", totaldelta);
    return nullptr;
  }

  // MIDI_PrintFlatListDBG (ret);

  return ret.release();
}

void MIDI_DestroyFlatList(midi_event_t** const evs) {
  free(evs);
}

namespace {
// NSM: timing assist functions
// they compute samples per midi clock, where midi clock is the units
// that the time deltas are in, and samples is an arbitrary unit of which
// "sndrate" of them occur per second

constexpr auto compute_spmc_normal(const unsigned mpq, const unsigned tempo, const unsigned sndrate)
    -> double {  // returns samples per midi clock

  // inputs: mpq (midi clocks per quarternote, from header)
  // tempo (from tempo event, in microseconds per quarternote)
  // sndrate (sound sample rate in hz)

  // samples   quarternote     microsec    samples    second
  // ------- = ----------- * ----------- * ------- * --------
  // midiclk     midiclk     quarternote   second    microsec

  // return  =  (1 / mpq)  *    tempo    * sndrate * (1 / 1000000)

  return static_cast<double>(tempo) / 1000000 * sndrate / mpq;
}

constexpr double compute_spmc_smpte(const unsigned smpte_fps,
                                    const unsigned mpf,
                                    const unsigned sndrate) {  // returns samples per midi clock

  // inputs: smpte_fps (24, 25, 29, 30)
  // mpf (midi clocks per frame, 0-255)
  // sndrate (sound sample rate in hz)

  // tempo is ignored here

  // samples     frame      seconds    samples
  // ------- = --------- * --------- * -------
  // midiclk    midiclk      frame     second

  // return  = (1 / mpf) * (1 / fps) * sndrate

  double fps;  // actual frames per second
  switch (smpte_fps) {
    case 24:
    case 25:
    case 30:
      fps = smpte_fps;
      break;

    case 29:
      // i hate NTSC, i really do
      fps = smpte_fps * 1000.0 / 1001.0;
      break;

    default:
      lprintf(LO_WARN, "MIDI_spmc: Unexpected SMPTE timestamp %i\n", smpte_fps);
      // assume
      fps = 30.0;
      break;
  }

  return static_cast<double>(sndrate) / fps / mpf;
}
}  // namespace

// if event is NULL, compute with default starting tempo (120BPM)
auto MIDI_spmc(const midi_file_t* const file, const midi_event_t* const ev, const unsigned sndrate) -> double {
  const unsigned headerval = MIDI_GetFileTimeDivision(file);

  if (headerval & 0x8000) {  // SMPTE
                             // i don't really have any files to test this on...
    const int smpte_fps = -static_cast<short>(headerval) >> 8;
    return compute_spmc_smpte(smpte_fps, headerval & 0xff, sndrate);
  }

  // normal timing
  unsigned tempo = 500000;  // default 120BPM
  if (ev != nullptr) {
    if (ev->event_type == MIDI_EVENT_META) {
      if (ev->data.meta.length == 3) {
        tempo = (unsigned)ev->data.meta.data[0] << 16 | (unsigned)ev->data.meta.data[1] << 8
                | (unsigned)ev->data.meta.data[2];
      } else {
        lprintf(LO_WARN, "MIDI_spmc: wrong length tempo meta message in midi file\n");
      }
    } else {
      lprintf(LO_WARN, "MIDI_spmc: passed non-meta event\n");
    }
  }

  return compute_spmc_normal(headerval, tempo, sndrate);
}

/*
The timing system used by the OPL driver is very interesting. But there are too many edge cases
in multitrack (type 1) midi tempo changes that it simply can't handle without a major rework.
The alternative is that we recook the file into a single track file with no tempo changes at
load time.
*/

auto MIDI_LoadFileSpecial(midimem_t* const mf) -> midi_file_t* {
  auto base = std::unique_ptr<midi_file_t, decltype([](auto* p) { MIDI_FreeFile(p); })>{MIDI_LoadFile(mf)};

  if (!base) {
    return nullptr;
  }

  auto flatlist = std::unique_ptr<midi_event_t* [], decltype([](auto* p) { MIDI_DestroyFlatList(p); })> {
    MIDI_GenerateFlatList(base.get())
  };
  if (!flatlist) {
    return nullptr;
  }

  auto ret =
      std::unique_ptr<midi_file_t, decltype([](auto* p) { free(p); })>{(midi_file_t*)malloc(sizeof(midi_file_t))};

  ret->header.format_type = 0;
  ret->header.num_tracks = 1;
  ret->header.time_division = 10000;

  ret->tracks.resize(1);
  ret->buffer.clear();

  ret->tracks.front().events.clear();

  double opi = MIDI_spmc(base.get(), nullptr, 20000);

  int epos = 0;
  while (true) {
    midi_event_t* const oldev = flatlist[epos];
    ret->tracks.front().events.push_back({});
    auto& nextev = ret->tracks.front().events.back();

    // figure delta time
    nextev.delta_time = static_cast<unsigned int>(opi * oldev->delta_time);

    if (oldev->event_type == MIDI_EVENT_SYSEX || oldev->event_type == MIDI_EVENT_SYSEX_SPLIT) {
      // opl player can't process any sysex...
      epos++;
      continue;
    }

    if (oldev->event_type == MIDI_EVENT_META) {
      if (oldev->data.meta.type == MIDI_META_SET_TEMPO) {  // adjust future tempo scaling
        opi = MIDI_spmc(base.get(), oldev, 20000);

        // insert event as dummy
        nextev.event_type = MIDI_EVENT_META;
        nextev.data.meta.type = MIDI_META_TEXT;
        nextev.data.meta.length = 0;
        nextev.data.meta.data = static_cast<byte*>(malloc(4));
        epos++;

        continue;
      }

      if (oldev->data.meta.type == MIDI_META_END_OF_TRACK) {  // reproduce event and break
        nextev.event_type = MIDI_EVENT_META;
        nextev.data.meta.type = MIDI_META_END_OF_TRACK;
        nextev.data.meta.length = 0;
        nextev.data.meta.data = static_cast<byte*>(malloc(4));
        epos++;

        break;
      }

      // other meta events not needed
      epos++;
      continue;
    }
    // non meta events can simply be copied (excluding delta time)
    nextev.event_type = oldev->event_type;
    epos++;
  }

  return ret.release();
}

#ifdef TEST

namespace {
auto MIDI_EventTypeToString(const midi_event_type_t event_type) -> std::string_view {
  switch (event_type) {
    case MIDI_EVENT_NOTE_OFF:
      return "MIDI_EVENT_NOTE_OFF";
    case MIDI_EVENT_NOTE_ON:
      return "MIDI_EVENT_NOTE_ON";
    case MIDI_EVENT_AFTERTOUCH:
      return "MIDI_EVENT_AFTERTOUCH";
    case MIDI_EVENT_CONTROLLER:
      return "MIDI_EVENT_CONTROLLER";
    case MIDI_EVENT_PROGRAM_CHANGE:
      return "MIDI_EVENT_PROGRAM_CHANGE";
    case MIDI_EVENT_CHAN_AFTERTOUCH:
      return "MIDI_EVENT_CHAN_AFTERTOUCH";
    case MIDI_EVENT_PITCH_BEND:
      return "MIDI_EVENT_PITCH_BEND";
    case MIDI_EVENT_SYSEX:
      return "MIDI_EVENT_SYSEX";
    case MIDI_EVENT_SYSEX_SPLIT:
      return "MIDI_EVENT_SYSEX_SPLIT";
    case MIDI_EVENT_META:
      return "MIDI_EVENT_META";

    default:
      return "(unknown)";
  }
}
}  // namespace

void PrintTrack(midi_track_t* const track) {
  for (const auto& event : track->events) {
    if (event.delta_time > 0) {
      std::printf("Delay: %i ticks\n", event.delta_time);
    }

    std::printf("Event type: %s (%i)\n", MIDI_EventTypeToString(event.event_type).data(), event.event_type);

    switch (event.event_type) {
      case MIDI_EVENT_NOTE_OFF:
      case MIDI_EVENT_NOTE_ON:
      case MIDI_EVENT_AFTERTOUCH:
      case MIDI_EVENT_CONTROLLER:
      case MIDI_EVENT_PROGRAM_CHANGE:
      case MIDI_EVENT_CHAN_AFTERTOUCH:
      case MIDI_EVENT_PITCH_BEND:
        std::printf("\tChannel: %i\n", event.data.channel.channel);
        std::printf("\tParameter 1: %i\n", event.data.channel.param1);
        std::printf("\tParameter 2: %i\n", event.data.channel.param2);
        break;

      case MIDI_EVENT_SYSEX:
      case MIDI_EVENT_SYSEX_SPLIT:
        std::printf("\tLength: %i\n", event.data.sysex.length);
        break;

      case MIDI_EVENT_META:
        std::printf("\tMeta type: %i\n", event.data.meta.type);
        std::printf("\tLength: %i\n", event.data.meta.length);
        break;
    }
  }
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::printf("Usage: %s <filename>\n", argv[0]);
    std::exit(1);
  }

  FILE* const f = M_fopen(argv[1], "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "Failed to open %s\n", argv[1]);
    std::exit(1);
  }

  midimem_t mf;
  std::fseek(f, 0, SEEK_END);
  mf.len = std::ftell(f);
  mf.pos = 0;

  rewind(f);
  mf.data = static_cast<byte*>(std::malloc(mf.len));
  std::fread(const_cast<byte*>(mf.data), 1, mf.len, f);
  std::fclose(f);

  midi_file_t* const file = MIDI_LoadFile(&mf);

  if (file == nullptr) {
    std::fprintf(stderr, "Failed to open %s\n", argv[1]);
    std::exit(1);
  }

  for (std::size_t i = 0; i < file->tracks.size(); ++i) {
    std::printf("\n== Track %zu ==\n\n", i);

    PrintTrack(&file->tracks[i]);
  }

  return 0;
}

#endif
