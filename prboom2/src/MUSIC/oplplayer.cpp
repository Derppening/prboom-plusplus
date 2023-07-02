// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005 Simon Howard
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
//  System interface for music.
//
//-----------------------------------------------------------------------------

#include "oplplayer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <array>
#include <list>
#include <string_view>
#include <vector>

#include "doomdef.h"
#include "memio.h"
#include "mus2mid.h"

#include "m_misc.h"
#include "s_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include "midifile.h"
#include "opl.h"

#include "lprintf.h"

// #define OPL_MIDI_DEBUG

constexpr std::size_t MAXMIDLENGTH = (96 * 1024);
constexpr std::size_t GENMIDI_NUM_INSTRS = 128;

constexpr std::string_view GENMIDI_HEADER = "#OPL_II#";
constexpr unsigned short GENMIDI_FLAG_FIXED = 0x0001;  /* fixed pitch */
constexpr unsigned short GENMIDI_FLAG_2VOICE = 0x0004; /* double voice (OPL3) */

struct genmidi_op_t {
  byte tremolo;
  byte attack;
  byte sustain;
  byte waveform;
  byte scale;
  byte level;
};

struct genmidi_voice_t {
  genmidi_op_t modulator;
  byte feedback;
  genmidi_op_t carrier;
  byte unused;
  short base_note_offset;
} PACKEDATTR;

struct genmidi_instr_t {
  unsigned short flags;
  byte fine_tuning;
  byte fixed_note;

  std::array<genmidi_voice_t, 2> voices;
} PACKEDATTR;

// Data associated with a channel of a track that is currently playing.

struct opl_channel_data_t {
  // The instrument currently used for this track.
  const genmidi_instr_t* instrument;

  // Volume level
  int volume;

  // Pitch bend value:
  int bend;
};

// Data associated with a track that is currently playing.

struct opl_track_data_t {
  // Data for each channel.
  std::array<opl_channel_data_t, MIDI_CHANNELS_PER_TRACK> channels;

  // Track iterator used to read new events.
  midi_track_iter_t* iter;

  // Tempo control variables
  unsigned int ticks_per_beat;
  unsigned int ms_per_beat;
};

struct opl_voice_t {
  // Index of this voice:
  int index;

  // The operators used by this voice:
  int op1, op2;

  // Currently-loaded instrument data
  const genmidi_instr_t* current_instr;

  // The voice number in the instrument to use.
  // This is normally set to zero; if this is a double voice
  // instrument, it may be one.
  unsigned int current_instr_voice;

  // The channel currently using this voice.
  opl_channel_data_t* channel;

  // The midi key that this voice is playing.
  unsigned int key;

  // The note being played.  This is normally the same as
  // the key, but if the instrument is a fixed pitch
  // instrument, it is different.
  unsigned int note;

  // The frequency value being used.
  unsigned int freq;

  // The volume of the note being played on this channel.
  unsigned int note_volume;

  // The current volume (register value) that has been set for this channel.
  unsigned int reg_volume;
};

namespace {
// Operators used by the different voices.
constexpr std::array<std::array<const int, OPL_NUM_VOICES>, 2> voice_operators{
    {{0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12}, {0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15}}};

// Frequency values to use for each note.
constexpr std::array<unsigned short, 668> frequency_curve = {
    0x133,
    0x133,
    0x134,
    0x134,
    0x135,
    0x136,
    0x136,
    0x137,  // -1
    0x137,
    0x138,
    0x138,
    0x139,
    0x139,
    0x13a,
    0x13b,
    0x13b,
    0x13c,
    0x13c,
    0x13d,
    0x13d,
    0x13e,
    0x13f,
    0x13f,
    0x140,
    0x140,
    0x141,
    0x142,
    0x142,
    0x143,
    0x143,
    0x144,
    0x144,

    0x145,
    0x146,
    0x146,
    0x147,
    0x147,
    0x148,
    0x149,
    0x149,  // -2
    0x14a,
    0x14a,
    0x14b,
    0x14c,
    0x14c,
    0x14d,
    0x14d,
    0x14e,
    0x14f,
    0x14f,
    0x150,
    0x150,
    0x151,
    0x152,
    0x152,
    0x153,
    0x153,
    0x154,
    0x155,
    0x155,
    0x156,
    0x157,
    0x157,
    0x158,

    // These are used for the first seven MIDI note values:
    0x158,
    0x159,
    0x15a,
    0x15a,
    0x15b,
    0x15b,
    0x15c,
    0x15d,  // 0
    0x15d,
    0x15e,
    0x15f,
    0x15f,
    0x160,
    0x161,
    0x161,
    0x162,
    0x162,
    0x163,
    0x164,
    0x164,
    0x165,
    0x166,
    0x166,
    0x167,
    0x168,
    0x168,
    0x169,
    0x16a,
    0x16a,
    0x16b,
    0x16c,
    0x16c,

    0x16d,
    0x16e,
    0x16e,
    0x16f,
    0x170,
    0x170,
    0x171,
    0x172,  // 1
    0x172,
    0x173,
    0x174,
    0x174,
    0x175,
    0x176,
    0x176,
    0x177,
    0x178,
    0x178,
    0x179,
    0x17a,
    0x17a,
    0x17b,
    0x17c,
    0x17c,
    0x17d,
    0x17e,
    0x17e,
    0x17f,
    0x180,
    0x181,
    0x181,
    0x182,

    0x183,
    0x183,
    0x184,
    0x185,
    0x185,
    0x186,
    0x187,
    0x188,  // 2
    0x188,
    0x189,
    0x18a,
    0x18a,
    0x18b,
    0x18c,
    0x18d,
    0x18d,
    0x18e,
    0x18f,
    0x18f,
    0x190,
    0x191,
    0x192,
    0x192,
    0x193,
    0x194,
    0x194,
    0x195,
    0x196,
    0x197,
    0x197,
    0x198,
    0x199,

    0x19a,
    0x19a,
    0x19b,
    0x19c,
    0x19d,
    0x19d,
    0x19e,
    0x19f,  // 3
    0x1a0,
    0x1a0,
    0x1a1,
    0x1a2,
    0x1a3,
    0x1a3,
    0x1a4,
    0x1a5,
    0x1a6,
    0x1a6,
    0x1a7,
    0x1a8,
    0x1a9,
    0x1a9,
    0x1aa,
    0x1ab,
    0x1ac,
    0x1ad,
    0x1ad,
    0x1ae,
    0x1af,
    0x1b0,
    0x1b0,
    0x1b1,

    0x1b2,
    0x1b3,
    0x1b4,
    0x1b4,
    0x1b5,
    0x1b6,
    0x1b7,
    0x1b8,  // 4
    0x1b8,
    0x1b9,
    0x1ba,
    0x1bb,
    0x1bc,
    0x1bc,
    0x1bd,
    0x1be,
    0x1bf,
    0x1c0,
    0x1c0,
    0x1c1,
    0x1c2,
    0x1c3,
    0x1c4,
    0x1c4,
    0x1c5,
    0x1c6,
    0x1c7,
    0x1c8,
    0x1c9,
    0x1c9,
    0x1ca,
    0x1cb,

    0x1cc,
    0x1cd,
    0x1ce,
    0x1ce,
    0x1cf,
    0x1d0,
    0x1d1,
    0x1d2,  // 5
    0x1d3,
    0x1d3,
    0x1d4,
    0x1d5,
    0x1d6,
    0x1d7,
    0x1d8,
    0x1d8,
    0x1d9,
    0x1da,
    0x1db,
    0x1dc,
    0x1dd,
    0x1de,
    0x1de,
    0x1df,
    0x1e0,
    0x1e1,
    0x1e2,
    0x1e3,
    0x1e4,
    0x1e5,
    0x1e5,
    0x1e6,

    0x1e7,
    0x1e8,
    0x1e9,
    0x1ea,
    0x1eb,
    0x1ec,
    0x1ed,
    0x1ed,  // 6
    0x1ee,
    0x1ef,
    0x1f0,
    0x1f1,
    0x1f2,
    0x1f3,
    0x1f4,
    0x1f5,
    0x1f6,
    0x1f6,
    0x1f7,
    0x1f8,
    0x1f9,
    0x1fa,
    0x1fb,
    0x1fc,
    0x1fd,
    0x1fe,
    0x1ff,
    0x200,
    0x201,
    0x201,
    0x202,
    0x203,

    // First note of looped range used for all octaves:
    0x204,
    0x205,
    0x206,
    0x207,
    0x208,
    0x209,
    0x20a,
    0x20b,  // 7
    0x20c,
    0x20d,
    0x20e,
    0x20f,
    0x210,
    0x210,
    0x211,
    0x212,
    0x213,
    0x214,
    0x215,
    0x216,
    0x217,
    0x218,
    0x219,
    0x21a,
    0x21b,
    0x21c,
    0x21d,
    0x21e,
    0x21f,
    0x220,
    0x221,
    0x222,

    0x223,
    0x224,
    0x225,
    0x226,
    0x227,
    0x228,
    0x229,
    0x22a,  // 8
    0x22b,
    0x22c,
    0x22d,
    0x22e,
    0x22f,
    0x230,
    0x231,
    0x232,
    0x233,
    0x234,
    0x235,
    0x236,
    0x237,
    0x238,
    0x239,
    0x23a,
    0x23b,
    0x23c,
    0x23d,
    0x23e,
    0x23f,
    0x240,
    0x241,
    0x242,

    0x244,
    0x245,
    0x246,
    0x247,
    0x248,
    0x249,
    0x24a,
    0x24b,  // 9
    0x24c,
    0x24d,
    0x24e,
    0x24f,
    0x250,
    0x251,
    0x252,
    0x253,
    0x254,
    0x256,
    0x257,
    0x258,
    0x259,
    0x25a,
    0x25b,
    0x25c,
    0x25d,
    0x25e,
    0x25f,
    0x260,
    0x262,
    0x263,
    0x264,
    0x265,

    0x266,
    0x267,
    0x268,
    0x269,
    0x26a,
    0x26c,
    0x26d,
    0x26e,  // 10
    0x26f,
    0x270,
    0x271,
    0x272,
    0x273,
    0x275,
    0x276,
    0x277,
    0x278,
    0x279,
    0x27a,
    0x27b,
    0x27d,
    0x27e,
    0x27f,
    0x280,
    0x281,
    0x282,
    0x284,
    0x285,
    0x286,
    0x287,
    0x288,
    0x289,

    0x28b,
    0x28c,
    0x28d,
    0x28e,
    0x28f,
    0x290,
    0x292,
    0x293,  // 11
    0x294,
    0x295,
    0x296,
    0x298,
    0x299,
    0x29a,
    0x29b,
    0x29c,
    0x29e,
    0x29f,
    0x2a0,
    0x2a1,
    0x2a2,
    0x2a4,
    0x2a5,
    0x2a6,
    0x2a7,
    0x2a9,
    0x2aa,
    0x2ab,
    0x2ac,
    0x2ae,
    0x2af,
    0x2b0,

    0x2b1,
    0x2b2,
    0x2b4,
    0x2b5,
    0x2b6,
    0x2b7,
    0x2b9,
    0x2ba,  // 12
    0x2bb,
    0x2bd,
    0x2be,
    0x2bf,
    0x2c0,
    0x2c2,
    0x2c3,
    0x2c4,
    0x2c5,
    0x2c7,
    0x2c8,
    0x2c9,
    0x2cb,
    0x2cc,
    0x2cd,
    0x2ce,
    0x2d0,
    0x2d1,
    0x2d2,
    0x2d4,
    0x2d5,
    0x2d6,
    0x2d8,
    0x2d9,

    0x2da,
    0x2dc,
    0x2dd,
    0x2de,
    0x2e0,
    0x2e1,
    0x2e2,
    0x2e4,  // 13
    0x2e5,
    0x2e6,
    0x2e8,
    0x2e9,
    0x2ea,
    0x2ec,
    0x2ed,
    0x2ee,
    0x2f0,
    0x2f1,
    0x2f2,
    0x2f4,
    0x2f5,
    0x2f6,
    0x2f8,
    0x2f9,
    0x2fb,
    0x2fc,
    0x2fd,
    0x2ff,
    0x300,
    0x302,
    0x303,
    0x304,

    0x306,
    0x307,
    0x309,
    0x30a,
    0x30b,
    0x30d,
    0x30e,
    0x310,  // 14
    0x311,
    0x312,
    0x314,
    0x315,
    0x317,
    0x318,
    0x31a,
    0x31b,
    0x31c,
    0x31e,
    0x31f,
    0x321,
    0x322,
    0x324,
    0x325,
    0x327,
    0x328,
    0x329,
    0x32b,
    0x32c,
    0x32e,
    0x32f,
    0x331,
    0x332,

    0x334,
    0x335,
    0x337,
    0x338,
    0x33a,
    0x33b,
    0x33d,
    0x33e,  // 15
    0x340,
    0x341,
    0x343,
    0x344,
    0x346,
    0x347,
    0x349,
    0x34a,
    0x34c,
    0x34d,
    0x34f,
    0x350,
    0x352,
    0x353,
    0x355,
    0x357,
    0x358,
    0x35a,
    0x35b,
    0x35d,
    0x35e,
    0x360,
    0x361,
    0x363,

    0x365,
    0x366,
    0x368,
    0x369,
    0x36b,
    0x36c,
    0x36e,
    0x370,  // 16
    0x371,
    0x373,
    0x374,
    0x376,
    0x378,
    0x379,
    0x37b,
    0x37c,
    0x37e,
    0x380,
    0x381,
    0x383,
    0x384,
    0x386,
    0x388,
    0x389,
    0x38b,
    0x38d,
    0x38e,
    0x390,
    0x392,
    0x393,
    0x395,
    0x397,

    0x398,
    0x39a,
    0x39c,
    0x39d,
    0x39f,
    0x3a1,
    0x3a2,
    0x3a4,  // 17
    0x3a6,
    0x3a7,
    0x3a9,
    0x3ab,
    0x3ac,
    0x3ae,
    0x3b0,
    0x3b1,
    0x3b3,
    0x3b5,
    0x3b7,
    0x3b8,
    0x3ba,
    0x3bc,
    0x3bd,
    0x3bf,
    0x3c1,
    0x3c3,
    0x3c4,
    0x3c6,
    0x3c8,
    0x3ca,
    0x3cb,
    0x3cd,

    // The last note has an incomplete range, and loops round back to
    // the start.  Note that the last value is actually a buffer overrun
    // and does not fit with the other values.
    0x3cf,
    0x3d1,
    0x3d2,
    0x3d4,
    0x3d6,
    0x3d8,
    0x3da,
    0x3db,  // 18
    0x3dd,
    0x3df,
    0x3e1,
    0x3e3,
    0x3e4,
    0x3e6,
    0x3e8,
    0x3ea,
    0x3ec,
    0x3ed,
    0x3ef,
    0x3f1,
    0x3f3,
    0x3f5,
    0x3f6,
    0x3f8,
    0x3fa,
    0x3fc,
    0x3fe,
    0x36c,
};

// Mapping from MIDI volume level to OPL level value.
constexpr std::array<unsigned int, 128> volume_mapping_table = {
    0,   1,   3,   5,   6,   8,   10,  11,  13,  14,  16,  17,  19,  20,  22,  23,  25,  26,  27,  29,  30,  32,
    33,  34,  36,  37,  39,  41,  43,  45,  47,  49,  50,  52,  54,  55,  57,  59,  60,  61,  63,  64,  66,  67,
    68,  69,  71,  72,  73,  74,  75,  76,  77,  79,  80,  81,  82,  83,  84,  84,  85,  86,  87,  88,  89,  90,
    91,  92,  92,  93,  94,  95,  96,  96,  97,  98,  99,  99,  100, 101, 101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111, 112, 112, 113, 113, 114, 114, 115, 115, 116, 117, 117, 118, 118, 119,
    119, 120, 120, 121, 121, 122, 122, 123, 123, 123, 124, 124, 125, 125, 126, 126, 127, 127};

bool music_initialized = false;

// bool musicpaused = false;
int current_music_volume;

// GENMIDI lump instrument data:
const genmidi_instr_t* main_instrs;
const genmidi_instr_t* percussion_instrs;

// Voices:
std::array<opl_voice_t, OPL_NUM_VOICES> voices;

std::list<opl_voice_t*> voice_free_list;
std::list<opl_voice_t*> voice_alloced_list;

// Track data for playing tracks:
std::vector<opl_track_data_t> tracks;
std::size_t running_tracks = 0;
bool song_looping;
}  // namespace

// Configuration file variable, containing the port number for the
// adlib chip.

int opl_io_port = 0x388;

namespace {
// Load instrument table from GENMIDI lump:
auto LoadInstrumentTable() -> bool {
  const auto* lump = static_cast<const byte*>(W_CacheLumpName("GENMIDI"));

  // Check header

  //  if (strncmp((const char*)lump, GENMIDI_HEADER.data(), GENMIDI_HEADER.length()) != 0) {
  if (std::string_view{reinterpret_cast<const char*>(lump), GENMIDI_HEADER.length()} == GENMIDI_HEADER.data()) {
    W_UnlockLumpName("GENMIDI");

    return false;
  }

  main_instrs = reinterpret_cast<const genmidi_instr_t*>(lump + GENMIDI_HEADER.length());
  percussion_instrs = main_instrs + GENMIDI_NUM_INSTRS;

  return true;
}

// Get the next available voice from the freelist.
auto GetFreeVoice() -> opl_voice_t* {
  // None available?
  if (voice_free_list.empty()) {
    return nullptr;
  }

  // Add to allocated list
  voice_alloced_list.emplace_front(voice_free_list.front());

  // Remove from free list
  voice_free_list.pop_front();

  return voice_alloced_list.front();
}

// Remove a voice from the allocated voices list.
void RemoveVoiceFromAllocedList(opl_voice_t& voice) {
  // Search the list until we find the voice, then remove it.
  voice_alloced_list.remove(&voice);
}

// Release a voice back to the freelist.
void ReleaseVoice(opl_voice_t& voice) {
  voice.channel = nullptr;
  voice.note = 0;

  // Search to the end of the freelist (This is how Doom behaves!)
  voice_free_list.push_back(&voice);

  // Remove from alloced list.
  RemoveVoiceFromAllocedList(voice);
}

// Load data to the specified Operator
void LoadOperatorData(const int Operator, const genmidi_op_t& data, const bool max_level) {
  // The scale and level fields must be combined for the level register.
  // For the carrier wave we always set the maximum level.

  int level = (data.scale & 0xc0) | (data.level & 0x3f);

  if (max_level) {
    level |= 0x3f;
  }

  OPL_WriteRegister(OPL_REGS_LEVEL + Operator, level);
  OPL_WriteRegister(OPL_REGS_TREMOLO + Operator, data.tremolo);
  OPL_WriteRegister(OPL_REGS_ATTACK + Operator, data.attack);
  OPL_WriteRegister(OPL_REGS_SUSTAIN + Operator, data.sustain);
  OPL_WriteRegister(OPL_REGS_WAVEFORM + Operator, data.waveform);
}

// Set the instrument for a particular voice.
void SetVoiceInstrument(opl_voice_t& voice, const genmidi_instr_t& instr, const unsigned int instr_voice) {
  // Instrument already set for this channel?
  if (voice.current_instr == &instr && voice.current_instr_voice == instr_voice) {
    return;
  }

  voice.current_instr = &instr;
  voice.current_instr_voice = instr_voice;

  const genmidi_voice_t& data = instr.voices[instr_voice];

  // Are we usind modulated feedback mode?
  const bool modulating = (data.feedback & 0x01) == 0;

  // Doom loads the second operator first, then the first.
  // The carrier is set to minimum volume until the voice volume
  // is set in SetVoiceVolume (below).  If we are not using
  // modulating mode, we must set both to minimum volume.
  LoadOperatorData(voice.op2, data.carrier, true);
  LoadOperatorData(voice.op1, data.modulator, !modulating);

  // Set feedback register that control the connection between the
  // two operators.  Turn on bits in the upper nybble; I think this
  // is for OPL3, where it turns on channel A/B.
  OPL_WriteRegister(OPL_REGS_FEEDBACK + voice.index, data.feedback | 0x30);

  // Hack to force a volume update.
  voice.reg_volume = 999;
}

void SetVoiceVolume(opl_voice_t& voice, const unsigned int volume) {
  voice.note_volume = volume;

  const genmidi_voice_t* const opl_voice = &voice.current_instr->voices[voice.current_instr_voice];

  // Multiply note volume and channel volume to get the actual volume.
  const unsigned int full_volume =
      (volume_mapping_table[voice.note_volume] * volume_mapping_table[voice.channel->volume]
       * volume_mapping_table[current_music_volume])
      / (127 * 127);

  // The volume of each instrument can be controlled via GENMIDI:
  const unsigned int op_volume = 0x3f - opl_voice->carrier.level;

  // The volume value to use in the register:
  unsigned int reg_volume = (op_volume * full_volume) / 128;
  reg_volume = (0x3f - reg_volume) | opl_voice->carrier.scale;

  // Update the volume register(s) if necessary.
  if (reg_volume != voice.reg_volume) {
    voice.reg_volume = reg_volume;

    OPL_WriteRegister(OPL_REGS_LEVEL + voice.op2, reg_volume);

    // If we are using non-modulated feedback mode, we must set the
    // volume for both voices.
    // Note that the same register volume value is written for
    // both voices, always calculated from the carrier's level
    // value.

    if ((opl_voice->feedback & 0x01) != 0) {
      OPL_WriteRegister(OPL_REGS_LEVEL + voice.op1, reg_volume);
    }
  }
}

// Initialize the voice table and freelist
void InitVoices() {
  // Start with an empty free list.
  voice_free_list.clear();

  // Initialize each voice.
  for (std::size_t i = 0; i < OPL_NUM_VOICES; ++i) {
    voices[i].index = static_cast<int>(i);
    voices[i].op1 = voice_operators[0][i];
    voices[i].op2 = voice_operators[1][i];
    voices[i].current_instr = nullptr;

    // Add this voice to the freelist.
    ReleaseVoice(voices[i]);
  }
}

// Set music volume (0 - 15)
void I_OPL_SetMusicVolume(const int volume) {
  const int opl_vol = volume * 127 / 15;  // OPL synth internally uses 0-127

  // Internal state variable.
  current_music_volume = opl_vol;

  // Update the volume of all voices.
  for (auto& voice : voices) {
    if (voice.channel != nullptr) {
      SetVoiceVolume(voice, voice.note_volume);
    }
  }
}

void VoiceKeyOff(const opl_voice_t& voice) {
  OPL_WriteRegister(OPL_REGS_FREQ_2 + voice.index, voice.freq >> 8);
}

// Get the frequency that we should be using for a voice.
void KeyOffEvent(opl_track_data_t& track, const midi_event_t& event) {
  /*
  printf("note off: channel %i, %i, %i\n",
         event->data.channel.channel,
         event->data.channel.param1,
         event->data.channel.param2);
  */

  const opl_channel_data_t& channel = track.channels[event.data.channel.channel];
  const unsigned int key = event.data.channel.param1;

  // Turn off voices being used to play this key.
  // If it is a double voice instrument there will be two.

  for (auto& voice : voices) {
    if (voice.channel == &channel && voice.key == key) {
      VoiceKeyOff(voice);

      // Finished with this voice now.
      ReleaseVoice(voice);
    }
  }
}

// Compare the priorities of channels, returning either -1, 0 or 1.

int CompareChannelPriorities(const opl_channel_data_t* const chan1, const opl_channel_data_t* const chan2) {
  // TODO ...

  return 1;
}

// When all voices are in use, we must discard an existing voice to
// play a new note.  Find and free an existing voice.  The channel
// passed to the function is the channel for the new note to be
// played.

auto ReplaceExistingVoice(opl_channel_data_t& channel) -> opl_voice_t& {
  // Check the allocated voices, if we find an instrument that is
  // of a lower priority to the new instrument, discard it.
  // If a voice is being used to play the second voice of an instrument,
  // use that, as second voices are non-essential.
  // Lower numbered MIDI channels implicitly have a higher priority
  // than higher-numbered channels, eg. MIDI channel 1 is never
  // discarded for MIDI channel 2.
  opl_voice_t* result = nullptr;

  {
    const auto it = std::find_if(voice_alloced_list.cbegin(), voice_alloced_list.cend(), [channel](auto* voice) {
      return voice->current_instr_voice != 0
             || (voice->channel > &channel && CompareChannelPriorities(&channel, voice->channel) > 0);
    });
    if (it != voice_alloced_list.cend()) {
      result = *it;
    }
  }

  // If we didn't find a voice, find an existing voice being used to
  // play a note on the same channel, and use that.
  if (result == nullptr) {
    const auto it = std::find_if(voice_alloced_list.cbegin(), voice_alloced_list.cend(),
                                 [channel](auto* voice) { return voice->channel == &channel; });
    if (it != voice_alloced_list.cend()) {
      result = *it;
    }
  }

  // Still nothing found?  Give up and just use the first voice in
  // the list.
  if (result == nullptr) {
    result = voice_alloced_list.front();
  }

  // Stop playing this voice playing and release it back to the free
  // list.
  VoiceKeyOff(*result);
  ReleaseVoice(*result);

  // Re-allocate the voice again and return it.
  return *GetFreeVoice();
}

auto FrequencyForVoice(const opl_voice_t& voice) -> unsigned int {
  unsigned int note = voice.note;

  // Apply note offset.
  // Don't apply offset if the instrument is a fixed note instrument.
  const genmidi_voice_t& gm_voice = voice.current_instr->voices[voice.current_instr_voice];

  if ((voice.current_instr->flags & GENMIDI_FLAG_FIXED) == 0) {
    note += static_cast<signed short>(doom_htows(gm_voice.base_note_offset));
  }

  // Avoid possible overflow due to base note offset:
  if (note > 0x7f) {
    note = voice.note;
  }

  unsigned int freq_index = 64 + 32 * note + voice.channel->bend;

  // If this is the second voice of a double voice instrument, the
  // frequency index can be adjusted by the fine tuning field.
  if (voice.current_instr_voice != 0) {
    freq_index += (voice.current_instr->fine_tuning / 2) - 64;
  }

  // The first 7 notes use the start of the table, while
  // consecutive notes loop around the latter part.
  if (freq_index < 284) {
    return frequency_curve[freq_index];
  }

  const unsigned int sub_index = (freq_index - 284) % (12 * 32);
  unsigned int octave = (freq_index - 284) / (12 * 32);

  // Once the seventh octave is reached, things break down.
  // We can only go up to octave 7 as a maximum anyway (the OPL
  // register only has three bits for octave number), but for the
  // notes in octave 7, the first five bits have octave=7, the
  // following notes have octave=6.  This 7/6 pattern repeats in
  // following octaves (which are technically impossible to
  // represent anyway).
  if (octave >= 7) {
    if (sub_index < 5) {
      octave = 7;
    } else {
      octave = 6;
    }
  }

  // Calculate the resulting register value to use for the frequency.
  return frequency_curve[sub_index + 284] | (octave << 10);
}

// Update the frequency that a voice is programmed to use.

void UpdateVoiceFrequency(opl_voice_t& voice) {
  // Calculate the frequency to use for this voice and update it
  // if neccessary.
  const unsigned int freq = FrequencyForVoice(voice);

  if (voice.freq != freq) {
    OPL_WriteRegister(OPL_REGS_FREQ_1 + voice.index, freq & 0xff);
    OPL_WriteRegister(OPL_REGS_FREQ_2 + voice.index, (freq >> 8) | 0x20);

    voice.freq = freq;
  }
}

// Program a single voice for an instrument.  For a double voice
// instrument (GENMIDI_FLAG_2VOICE), this is called twice for each
// key on event.
void VoiceKeyOn(opl_channel_data_t& channel,
                const genmidi_instr_t& instrument,
                const unsigned int instrument_voice,
                const unsigned int key,
                const unsigned int volume) {

  // Find a voice to use for this new note.
  opl_voice_t* voice = GetFreeVoice();

  // If there are no more voices left, we must decide what to do.
  // If this is the first voice of the instrument, free an existing
  // voice and use that.  Otherwise, if this is the second voice,
  // it isn't as important; just discard it.
  if (voice == nullptr) {
    if (instrument_voice == 0) {
      voice = &ReplaceExistingVoice(channel);
    } else {
      return;
    }
  }

  voice->channel = &channel;
  voice->key = key;

  // Work out the note to use.  This is normally the same as
  // the key, unless it is a fixed pitch instrument.
  if ((instrument.flags & GENMIDI_FLAG_FIXED) != 0) {
    voice->note = instrument.fixed_note;
  } else {
    voice->note = key;
  }

  // Program the voice with the instrument data:
  SetVoiceInstrument(*voice, instrument, instrument_voice);

  // Set the volume level.

  SetVoiceVolume(*voice, volume);

  // Write the frequency value to turn the note on.
  voice->freq = 0;
  UpdateVoiceFrequency(*voice);
}

void KeyOnEvent(opl_track_data_t& track, const midi_event_t& event) {

  /*
  printf("note on: channel %i, %i, %i\n",
         event->data.channel.channel,
         event->data.channel.param1,
         event->data.channel.param2);
  */

  if (event.data.channel.param2 == 0) {  // NSM
    // i have no idea why this is the case, but it is
    // note that you don't see this in any of the base doom/doom2 music
    KeyOffEvent(track, event);
    return;
  }

  // The channel.
  opl_channel_data_t& channel = track.channels[event.data.channel.channel];
  const unsigned int key = event.data.channel.param1;
  const unsigned int volume = event.data.channel.param2;

  // Percussion channel (10) is treated differently.
  const genmidi_instr_t* instrument;
  if (event.data.channel.channel == 9) {
    if (key < 35 || key > 81) {
      return;
    }

    instrument = &percussion_instrs[key - 35];
  } else {
    instrument = channel.instrument;
  }

  // Find and program a voice for this instrument.  If this
  // is a double voice instrument, we must do this twice.

  VoiceKeyOn(channel, *instrument, 0, key, volume);

  if ((instrument->flags & GENMIDI_FLAG_2VOICE) != 0) {
    VoiceKeyOn(channel, *instrument, 1, key, volume);
  }
}

void ProgramChangeEvent(opl_track_data_t& track, const midi_event_t& event) {
  // Set the instrument used on this channel.
  const int channel = event.data.channel.channel;
  const int instrument = event.data.channel.param1;
  track.channels[channel].instrument = &main_instrs[instrument];

  // TODO: Look through existing voices that are turned on on this
  // channel, and change the instrument.
}

void SetChannelVolume(opl_channel_data_t& channel, const unsigned int volume) {
  channel.volume = volume;

  // Update all voices that this channel is using.
  for (auto& voice : voices) {
    if (voice.channel == &channel) {
      SetVoiceVolume(voice, voice.note_volume);
    }
  }
}

void ControllerEvent(opl_track_data_t& track, const midi_event_t& event) {
  /*
  printf("change controller: channel %i, %i, %i\n",
         event->data.channel.channel,
         event->data.channel.param1,
         event->data.channel.param2);
  */

  opl_channel_data_t& channel = track.channels[event.data.channel.channel];
  const unsigned int controller = event.data.channel.param1;
  const unsigned int param = event.data.channel.param2;

  switch (controller) {
    case MIDI_CONTROLLER_MAIN_VOLUME:
      SetChannelVolume(channel, param);
      break;

    default:
#ifdef OPL_MIDI_DEBUG
      lprintf(LO_WARN, "Unknown MIDI controller type: %i\n", controller);
#endif
      break;
  }
}

// Process a pitch bend event.
void PitchBendEvent(opl_track_data_t& track, const midi_event_t& event) {
  // Update the channel bend value.  Only the MSB of the pitch bend
  // value is considered: this is what Doom does.
  opl_channel_data_t& channel = track.channels[event.data.channel.channel];
  channel.bend = event.data.channel.param2 - 64;

  // Update all voices for this channel.
  for (auto& voice : voices) {
    if (voice.channel == &channel) {
      UpdateVoiceFrequency(voice);
    }
  }
}

// Process a meta event.
void MetaEvent([[maybe_unused]] opl_track_data_t& track, const midi_event_t& event) {
  switch (event.data.meta.type) {
      // Things we can just ignore.

    case MIDI_META_SEQUENCE_NUMBER:
    case MIDI_META_TEXT:
    case MIDI_META_COPYRIGHT:
    case MIDI_META_TRACK_NAME:
    case MIDI_META_INSTR_NAME:
    case MIDI_META_LYRICS:
    case MIDI_META_MARKER:
    case MIDI_META_CUE_POINT:
    case MIDI_META_SEQUENCER_SPECIFIC:
      break;

      // End of track - actually handled when we run out of events
      // in the track, see below.

    case MIDI_META_END_OF_TRACK:
      break;

    default:
#ifdef OPL_MIDI_DEBUG
      lprintf(LO_WARN, "Unknown MIDI meta event type: %i\n", event.data.meta.type);
#endif
      break;
  }
}

// Process a MIDI event from a track.

void ProcessEvent(opl_track_data_t& track, const midi_event_t& event) {
  switch (event.event_type) {
    case MIDI_EVENT_NOTE_OFF:
      KeyOffEvent(track, event);
      break;

    case MIDI_EVENT_NOTE_ON:
      KeyOnEvent(track, event);
      break;

    case MIDI_EVENT_CONTROLLER:
      ControllerEvent(track, event);
      break;

    case MIDI_EVENT_PROGRAM_CHANGE:
      ProgramChangeEvent(track, event);
      break;

    case MIDI_EVENT_PITCH_BEND:
      PitchBendEvent(track, event);
      break;

    case MIDI_EVENT_META:
      MetaEvent(track, event);
      break;

      // SysEx events can be ignored.
    case MIDI_EVENT_SYSEX:
    case MIDI_EVENT_SYSEX_SPLIT:
      break;

    default:
#ifdef OPL_MIDI_DEBUG
      lprintf(LO_WARN, "Unknown MIDI event type %i\n", event.event_type);
#endif
      break;
  }
}

void ScheduleTrack(opl_track_data_t& track);

// Restart a song from the beginning.

void RestartSong() {
  running_tracks = tracks.size();

  // fix buggy songs that forget to terminate notes held over loop point
  // sdl_mixer does this as well
  for (auto& voice : voices) {
    if (voice.channel != nullptr && voice.current_instr < percussion_instrs) {
      VoiceKeyOff(voice);
    }
  }

  for (auto& track : tracks) {
    MIDI_RestartIterator(track.iter);
    ScheduleTrack(track);
  }
}

// Callback function invoked when another event needs to be read from
// a track.
void TrackTimerCallback(void* const arg) {
  auto* track = static_cast<opl_track_data_t*>(arg);

  // Get the next event and process it.
  midi_event_t* event = nullptr;
  if (MIDI_GetNextEvent(track->iter, &event) == 0) {
    return;
  }

  ProcessEvent(*track, *event);

  // End of track?
  if (event->event_type == MIDI_EVENT_META && event->data.meta.type == MIDI_META_END_OF_TRACK) {
    --running_tracks;

    // When all tracks have finished, restart the song.
    if (running_tracks <= 0 && song_looping) {
      RestartSong();
    }

    return;
  }

  // Reschedule the callback for the next event in the track.
  ScheduleTrack(*track);
}

void ScheduleTrack(opl_track_data_t& track) {
  static int total = 0;

  // Get the number of milliseconds until the next event.
  const unsigned int nticks = MIDI_GetDeltaTime(track.iter);
  const unsigned int ms = (nticks * track.ms_per_beat) / track.ticks_per_beat;
  total += ms;

  // Set a timer to be invoked when the next event is
  // ready to play.
  OPL_SetCallback(ms, TrackTimerCallback, &track);
}

// Initialize a channel.
void InitChannel([[maybe_unused]] opl_track_data_t& track, opl_channel_data_t& channel) {
  // TODO: Work out sensible defaults?

  channel.instrument = &main_instrs[0];
  channel.volume = 127;
  channel.bend = 0;
}

// Start a MIDI track playing:
void StartTrack(const midi_file_t& file, unsigned int track_num) {
  opl_track_data_t& track = tracks[track_num];
  track.iter = MIDI_IterateTrack(&file, track_num);
  track.ticks_per_beat = MIDI_GetFileTimeDivision(&file);

  // Default is 120 bpm.
  // TODO: this is wrong
  track.ms_per_beat = 500;

  for (std::size_t i = 0; i < MIDI_CHANNELS_PER_TRACK; ++i) {
    InitChannel(track, track.channels[i]);
  }

  // Schedule the first event.
  ScheduleTrack(track);
}

// Start playing a mid
void I_OPL_PlaySong(const void* const handle, const int looping) {
  if (!music_initialized || handle == nullptr) {
    return;
  }

  const auto* file = static_cast<const midi_file_t*>(handle);

  // Allocate track data.
  tracks.resize(MIDI_NumTracks(file));
  song_looping = static_cast<bool>(looping);

  for (std::size_t i = 0; i < tracks.size(); ++i) {
    StartTrack(*file, i);
  }
}

void I_OPL_PauseSong() {
  if (!music_initialized) {
    return;
  }

  // Pause OPL callbacks.
  OPL_SetPaused(1);

  // Turn off all main instrument voices (not percussion).
  // This is what Vanilla does.
  for (auto& voice : voices) {
    if (voice.channel != nullptr && voice.current_instr < percussion_instrs) {
      VoiceKeyOff(voice);
    }
  }
}

void I_OPL_ResumeSong() {
  if (!music_initialized) {
    return;
  }

  OPL_SetPaused(0);
}

void I_OPL_StopSong() {
  if (!music_initialized) {
    return;
  }

  // Stop all playback.
  OPL_ClearCallbacks();

  // Free all voices.
  for (auto& voice : voices) {
    if (voice.channel != nullptr) {
      VoiceKeyOff(voice);
      ReleaseVoice(voice);
    }
  }

  // Free all track data.
  for (auto& track : tracks) {
    MIDI_FreeIterator(track.iter);
  }

  tracks.clear();
}

void I_OPL_UnRegisterSong(const void* const handle) {
  if (!music_initialized) {
    return;
  }

  if (handle != nullptr) {
    MIDI_FreeFile(reinterpret_cast<midi_file_t*>(const_cast<void*>(handle)));
  }
}

// Determine whether memory block is a .mid file
auto IsMid(const byte* const mem, const int len) -> bool {
  return len > 4 && std::string_view{reinterpret_cast<const char*>(mem), 4} == "MThd";
}

// now only takes files in MIDI format
auto I_OPL_RegisterSong(const void* const data, const unsigned len) -> const void* {

  if (!music_initialized) {
    return nullptr;
  }

  midimem_t mf{.data = reinterpret_cast<byte*>(const_cast<void*>(data)), .len = len, .pos = 0};

  // NSM: if a file has a miniscule timecode we have to not load it.
  // if it's 0, we'll hang in scheduling and never finish.  if it's
  // very small but close to 0, there's probably something wrong with it

  // this check of course isn't very accurate, but to actually get the
  // time numbers we have to traverse the tracks and everything
  if (mf.len < 100) {
    lprintf(LO_WARN, "I_OPL_RegisterSong: Very short MIDI (%li bytes)\n", mf.len);
    return nullptr;
  }

  midi_file_t* const result = MIDI_LoadFileSpecial(&mf);

  if (result == nullptr) {
    lprintf(LO_WARN, "I_OPL_RegisterSong: Failed to load MID.\n");
  }

  return result;
}

void I_OPL_ShutdownMusic() {
  if (music_initialized) {
    // Stop currently-playing track, if there is one:
    I_OPL_StopSong();

    OPL_Shutdown();

    // Release GENMIDI lump
    W_UnlockLumpName("GENMIDI");

    music_initialized = false;
  }
}
}  // namespace

// Initialize music subsystem
auto I_OPL_InitMusic(const int samplerate) -> int {
  if (OPL_Init(samplerate) == 0) {
    // printf("Dude.  The Adlib isn't responding.\n");
    return 0;
  }

  // Load instruments from GENMIDI lump:

  if (!LoadInstrumentTable()) {
    OPL_Shutdown();
    return 0;
  }

  InitVoices();

  tracks.clear();
  music_initialized = true;

  return 1;
}

auto I_OPL_SynthName() -> const char* {
  return "opl2 synth player";
}

void I_OPL_RenderSamples(void* const dest, const unsigned nsamp) {
  OPL_Render_Samples(dest, nsamp);
}

const music_player_t opl_synth_player = {I_OPL_SynthName, I_OPL_InitMusic,  I_OPL_ShutdownMusic, I_OPL_SetMusicVolume,
                                         I_OPL_PauseSong, I_OPL_ResumeSong, I_OPL_RegisterSong,  I_OPL_UnRegisterSong,
                                         I_OPL_PlaySong,  I_OPL_StopSong,   I_OPL_RenderSamples};
