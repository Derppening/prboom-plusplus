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
 *  System interface for sound.
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <array>
#include <format>
#include <string_view>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_LIBSDL2_MIXER
#define HAVE_MIXER
#endif

#include <SDL.h>
#include <SDL_audio.h>
#include <SDL_endian.h>
#include <SDL_mutex.h>
#include <SDL_thread.h>
#include <SDL_version.h>

#ifdef HAVE_MIXER
#define USE_RWOPS
#include <SDL_mixer.h>
#endif

#include "z_zone.h"

#include "i_sound.h"
#include "lprintf.h"
#include "m_argv.h"
#include "m_misc.h"
#include "m_swap.h"
#include "s_sound.h"
#include "w_wad.h"

#include "doomdef.h"
#include "doomstat.h"
#include "doomtype.h"

#include "d_main.h"
#include "i_system.h"

// e6y
#include "e6y.h"
#include "i_pcsound.h"

int snd_pcspeaker;
int lowpass_filter;

// The number of internal mixing channels,
//  the samples calculated for each mixing step,
//  the size of the 16bit, 2 hardware channel (stereo)
//  mixing buffer, and the samplerate of the raw data.

// Variables used by Boom from Allegro
// created here to avoid changes to core Boom files
int snd_card = 1;
int mus_card = 1;
int detect_voices = 0;  // God knows

namespace {
bool sound_inited = false;
bool first_sound_init = true;
}  // namespace

// MWM 2000-01-08: Sample rate in samples/second
int snd_samplerate = 11025;
int snd_samplecount = 512;

// The actual output device.
int audio_fd;

struct channel_info_t {
  // SFX id of the playing sound effect.
  // Used to catch duplicates (like chainsaw).
  int id;
  // The channel step amount...
  unsigned int step;
  // ... and a 0.16 bit remainder of last step.
  unsigned int stepremainder;
  unsigned int samplerate;
  unsigned int bits;
  float alpha;
  int prevS;
  // The channel data pointers, start and end.
  const unsigned char* data;
  const unsigned char* enddata;
  // Time/gametic that the channel started playing,
  //  used to determine oldest, which automatically
  //  has lowest priority.
  // In case number of active sounds exceeds
  //  available channels.
  int starttime;
  // left and right channel volume (0-127)
  int leftvol;
  int rightvol;
};

channel_info_t channelinfo[MAX_CHANNELS];

// Pitch to stepping lookup, unused.
int steptable[256];

// Volume lookups.
// int   vol_lookup[128 * 256];

// NSM
namespace {
int dumping_sound = 0;
}  // namespace

// lock for updating any params related to sfx
SDL_mutex* sfxmutex;
// lock for updating any params related to music
SDL_mutex* musmutex;

/* cph
 * stopchan
 * Stops a sound, unlocks the data
 */

namespace {
void stopchan(const int i) {
  if (channelinfo[i].data != nullptr) { /* cph - prevent excess unlocks */
    channelinfo[i].data = nullptr;
  }
}

//
// This function adds a sound to the
//  list of currently active sounds,
//  which is maintained as a given number
//  (eight, usually) of internal channels.
// Returns a handle.
//
auto addsfx(const int sfxid, const int channel, const unsigned char* const data, const std::size_t len) -> int {
  channel_info_t* const ci = &channelinfo[channel];

  stopchan(channel);

  if (std::string_view{reinterpret_cast<const char*>(data), 4} == "RIFF" && std::string_view{reinterpret_cast<const char*>(data) + 8, 8} == "WAVEfmt ") {
    // FIXME: can't handle stereo wavs
    // ci->channels = data[22] | (data[23] << 8);
    ci->samplerate = data[24] | (data[25] << 8) | (data[26] << 16) | (data[27] << 24);
    ci->bits = data[34] | (data[35] << 8);
    ci->data = data + 44;
    ci->enddata = data + 44 + (data[40] | (data[41] << 8) | (data[42] << 16) | (data[43] << 24));
    if (ci->enddata > data + len - 2) {
      ci->enddata = data + len - 2;
    }
  } else {
    ci->samplerate = (data[3] << 8) + data[2];
    ci->bits = 8;
    ci->data = data + 8;
    ci->enddata = data + len - 1;
  }

  ci->prevS = 0;

  // Filter from chocolate doom i_sdlsound.c 682-695
  // Low-pass filter for cutoff frequency f:
  //
  // For sampling rate r, dt = 1 / r
  // rc = 1 / 2*pi*f
  // alpha = dt / (rc + dt)

  // Filter to the half sample rate of the original sound effect
  // (maximum frequency, by nyquist)

  if (lowpass_filter != 0) {
    const float dt = 1.0f / snd_samplerate;
    const float rc = 1.0f / (3.14f * ci->samplerate);
    ci->alpha = dt / (rc + dt);
  }

  ci->stepremainder = 0;
  // Should be gametic, I presume.
  ci->starttime = gametic;

  // Preserve sound SFX id,
  //  e.g. for avoiding duplicates of chainsaw.
  ci->id = sfxid;

  return channel;
}

auto getSliceSize() -> int {
  if (snd_samplecount >= 32) {
    return snd_samplecount * snd_samplerate / 11025;
  }

  const int limit = snd_samplerate / TICRATE;

  // Try all powers of two, not exceeding the limit.

  for (int n = 0;; ++n) {
    // 2^n <= limit < 2^n+1 ?

    if ((1 << (n + 1)) > limit) {
      return (1 << n);
    }
  }

  // Should never happen?

  return 1024;
}

void updateSoundParams(const int handle, const int volume, int seperation, const int pitch) {
  const int slot = handle;

#ifdef RANGECHECK
  if ((handle < 0) || (handle >= MAX_CHANNELS)) {
    I_Error("I_UpdateSoundParams: handle out of range");
  }
#endif

  if (snd_pcspeaker != 0) {
    return;
  }

  // Set stepping
  // MWM 2000-12-24: Calculates proportion of channel samplerate
  // to global samplerate for mixing purposes.
  // Patched to shift left *then* divide, to minimize roundoff errors
  // as well as to use SAMPLERATE as defined above, not to assume 11025 Hz
  if (pitched_sounds != 0) {
    channelinfo[slot].step = static_cast<unsigned int>((static_cast<std::uint64_t>(channelinfo[slot].samplerate) * steptable[pitch]) / snd_samplerate);
  } else {
    channelinfo[slot].step = ((channelinfo[slot].samplerate << 16) / snd_samplerate);
  }

  // Separation, that is, orientation/stereo.
  //  range is: 1 - 256
  seperation += 1;

  // Per left/right channel.
  //  x^2 seperation,
  //  adjust volume properly.
  const int leftvol = volume - ((volume * seperation * seperation) >> 16);
  seperation -= 257;
  const int rightvol = volume - ((volume * seperation * seperation) >> 16);

  // Sanity check, clamp volume.
  if (rightvol < 0 || rightvol > 127) {
    I_Error("rightvol out of bounds");
  }

  if (leftvol < 0 || leftvol > 127) {
    I_Error("leftvol out of bounds");
  }

  // Get the proper lookup table piece
  //  for this volume level???
  channelinfo[slot].leftvol = leftvol;
  channelinfo[slot].rightvol = rightvol;
}
}  // namespace

void I_UpdateSoundParams(const int handle, const int volume, const int seperation, const int pitch) {
  SDL_LockMutex(sfxmutex);
  updateSoundParams(handle, volume, seperation, pitch);
  SDL_UnlockMutex(sfxmutex);
}

//
// SFX API
// Note: this was called by S_Init.
// However, whatever they did in the
// old DPMS based DOS version, this
// were simply dummies in the Linux
// version.
// See soundserver initdata().
//
void I_SetChannels() {
  // Init internal lookups (raw data, mixing buffer, channels).
  // This function sets up internal lookups used during
  //  the mixing process.

  int* const steptablemid = steptable + 128;

  // Okay, reset internal mixing channels to zero.
  for (auto& i : channelinfo) {
    i = {};
  }

  // This table provides step widths for pitch parameters.
  // I fail to see that this is currently used.
  for (int i = -128; i < 128; i++) {
    steptablemid[i] = static_cast<int>(std::pow(1.2, static_cast<double>(i) / 64.0) * 65536.0);
  }

  // Generates volume lookup tables
  //  which also turn the unsigned samples
  //  into signed samples.
  /*
  for (i = 0 ; i < 128 ; i++)
    for (j = 0 ; j < 256 ; j++)
    {
      // proff - made this a little bit softer, because with
      // full volume the sound clipped badly
      vol_lookup[i * 256 + j] = (i * (j - 128) * 256) / 191;
      //vol_lookup[i*256+j] = (i*(j-128)*256)/127;
    }
  */
}

//
// Retrieve the raw data lump index
//  for a given SFX name.
//
auto I_GetSfxLumpNum(sfxinfo_t* sfx) -> int {
  // Different prefix for PC speaker sound effects.
  const char* prefix = snd_pcspeaker != 0 ? "dp" : "ds";

  std::string namebuf = std::format("{}{}", prefix, sfx->name ? sfx->name : "(null)");
  namebuf.resize(std::min<std::size_t>(namebuf.length(), 8));
  return W_CheckNumForName(namebuf.data());  // e6y: make missing sounds non-fatal
}

//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback)
//  is set, but currently not used by mixing.
//
auto I_StartSound(const int id, const int channel, const int vol, const int sep, const int pitch, const int priority) -> int {
//  const unsigned char* data;
//  int lump;
//  size_t len;

  if ((channel < 0) || (channel >= MAX_CHANNELS)) {
#ifdef RANGECHECK
    I_Error("I_StartSound: handle out of range");
#else
    return -1;
#endif
  }

  if (snd_pcspeaker != 0) {
    return I_PCS_StartSound(id, channel, vol, sep, pitch, priority);
  }

  const int lump = S_sfx[id].lumpnum;

  // We will handle the new SFX.
  // Set pointer to raw data.
  std::size_t len = W_LumpLength(lump);

  // e6y: Crash with zero-length sounds.
  // Example wad: dakills (http://www.doomworld.com/idgames/index.php?id=2803)
  // The entries DSBSPWLK, DSBSPACT, DSSWTCHN and DSSWTCHX are all zero-length sounds
  if (len <= 8) {
    return -1;
  }

  /* Find padded length */
  len -= 8;
  // do the lump caching outside the SDL_LockAudio/SDL_UnlockAudio pair
  // use locking which makes sure the sound data is in a malloced area and
  // not in a memory mapped one
  const auto* const data = static_cast<const unsigned char*>(W_LockLumpNum(lump));

  SDL_LockMutex(sfxmutex);

  // Returns a handle (not used).
  addsfx(id, channel, data, len);
  updateSoundParams(channel, vol, sep, pitch);

  SDL_UnlockMutex(sfxmutex);

  return channel;
}

void I_StopSound(const int handle) {
#ifdef RANGECHECK
  if ((handle < 0) || (handle >= MAX_CHANNELS)) {
    I_Error("I_StopSound: handle out of range");
  }
#endif

  if (snd_pcspeaker != 0) {
    I_PCS_StopSound(handle);
    return;
  }

  SDL_LockMutex(sfxmutex);
  stopchan(handle);
  SDL_UnlockMutex(sfxmutex);
}

auto I_SoundIsPlaying(const int handle) -> dboolean {
#ifdef RANGECHECK
  if ((handle < 0) || (handle >= MAX_CHANNELS)) {
    I_Error("I_SoundIsPlaying: handle out of range");
  }
#endif

  if (snd_pcspeaker != 0) {
    return I_PCS_SoundIsPlaying(handle);
  }

  return channelinfo[handle].data != nullptr;
}

auto I_AnySoundStillPlaying() -> dboolean {
  bool result = false;

  if (snd_pcspeaker != 0) {
    return false;
  }

  for (int i = 0; i < MAX_CHANNELS; i++) {
    result |= channelinfo[i].data != nullptr;
  }

  return result;
}

//
// This function loops all active (internal) sound
//  channels, retrieves a given number of samples
//  from the raw sound data, modifies it according
//  to the current (internal) channel parameters,
//  mixes the per channel samples into the given
//  mixing buffer, and clamping it to the allowed
//  range.
//
// This function currently supports only 16bit.
//

#ifndef HAVE_OWN_MUSIC
namespace {
void Exp_UpdateMusic(void* buff, unsigned nsamp);
}  // namespace
#endif

// from pcsound_sdl.c
extern "C" void PCSound_Mix_Callback(void* udata, Uint8* stream, int len);

namespace {
void I_UpdateSound(void* const unused, Uint8* const stream, const int len) {
  if (snd_midiplayer == nullptr) {  // This is but a temporary fix. Please do remove after a more definitive one!
    std::fill_n(stream, len, 0);
  }

  // NSM: when dumping sound, ignore the callback calls and only
  // service dumping calls
  if (dumping_sound != 0 && unused != reinterpret_cast<const void*>(0xdeadbeef)) {
    return;
  }

#ifndef HAVE_OWN_MUSIC
  // do music update
  if (use_experimental_music != 0) {
    SDL_LockMutex(musmutex);
    Exp_UpdateMusic(stream, len / 4);
    SDL_UnlockMutex(musmutex);
  }
#endif

  if (snd_pcspeaker != 0) {
    PCSound_Mix_Callback(nullptr, stream, len);
    // no sfx mixing
    return;
  }

  SDL_LockMutex(sfxmutex);
  // Left and right channel
  //  are in audio stream, alternating.
  auto* leftout = reinterpret_cast<signed short*>(stream);
  auto* rightout = reinterpret_cast<signed short*>(stream) + 1;

  // Step in stream, left and right, thus two.
  const int step = 2;

  // Determine end, for left channel only
  //  (right channel is implicit).
  const auto* const leftend = reinterpret_cast<signed short*>(leftout + (len / 4) * step);

  // Mix sounds into the mixing buffer.
  // Loop over step*SAMPLECOUNT,
  //  that is 512 values for two channels.
  while (leftout != leftend) {
    // Mix current sound data.
    // Data, from raw sound, for right and left.
    // register unsigned char sample;
    //
    // Reset left/right value.
    // dl = 0;
    // dr = 0;
    int dl = *leftout;
    int dr = *rightout;

    // Love thy L2 chache - made this a loop.
    // Now more channels could be set at compile time
    //  as well. Thus loop those  channels.
    for (int chan = 0; chan < numChannels; chan++) {
      channel_info_t* const ci = channelinfo + chan;

      // Check channel, if active.
      if (ci->data != nullptr) {
        int s;
        // Get the raw data from the channel.
        // no filtering
        // s = ci->data[0] * 0x10000 - 0x800000;

        // linear filtering
        // the old SRC did linear interpolation back into 8 bit, and then expanded to 16 bit.
        // this does interpolation and 8->16 at same time, allowing slightly higher quality
        if (ci->bits == 16) {
          s = static_cast<short>(ci->data[0] | (ci->data[1] << 8)) * (255 - (ci->stepremainder >> 8))
              + static_cast<short>(ci->data[2] | (ci->data[3] << 8)) * (ci->stepremainder >> 8);
        } else {
          s = (ci->data[0] * (0x10000 - ci->stepremainder)) + (ci->data[1] * (ci->stepremainder))
              - 0x800000;  // convert to signed
        }

        // lowpass
        if (lowpass_filter != 0) {
          s = ci->prevS + ci->alpha * (s - ci->prevS);
          ci->prevS = s;
        }

        // Add left and right part
        //  for this channel (sound)
        //  to the current data.
        // Adjust volume accordingly.

        // full loudness (vol=127) is actually 127/191

        dl += ci->leftvol * s / 49152;   // >> 15;
        dr += ci->rightvol * s / 49152;  // >> 15;

        // Increment index ???
        ci->stepremainder += ci->step;

        // MSB is next sample???
        if (ci->bits == 16) {
          ci->data += (ci->stepremainder >> 16) * 2;
        } else {
          ci->data += ci->stepremainder >> 16;
        }

        // Limit to LSB???
        ci->stepremainder &= 0xffffu;

        // Check whether we are done.
        if (ci->data >= ci->enddata) {
          stopchan(chan);
        }
      }
    }

    // Clamp to range. Left hardware channel.
    // Has been char instead of short.
    // if (dl > 127) *leftout = 127;
    // else if (dl < -128) *leftout = -128;
    // else *leftout = dl;

    *leftout = std::clamp<signed short>(static_cast<signed short>(dl), SHRT_MIN, SHRT_MAX);

    // Same for right hardware channel.
    *rightout = std::clamp<signed short>(static_cast<signed short>(dr), SHRT_MIN, SHRT_MAX);

    // Increment current pointers in stream
    leftout += step;
    rightout += step;
  }
  SDL_UnlockMutex(sfxmutex);
}
}  // namespace

void I_ShutdownSound() {
  if (sound_inited) {
    lprintf(LO_INFO, "I_ShutdownSound: ");
#ifdef HAVE_MIXER
    Mix_CloseAudio();
#endif
    SDL_CloseAudio();
    lprintf(LO_INFO, "\n");
    sound_inited = false;

    if (sfxmutex != nullptr) {
      SDL_DestroyMutex(sfxmutex);
      sfxmutex = nullptr;
    }
  }
}

// static SDL_AudioSpec audio;

void I_InitSound() {
  // haleyjd: the docs say we should do this
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    lprintf(LO_INFO, "Couldn't initialize SDL audio (%s))\n", SDL_GetError());
    nosfxparm = true;
    nomusicparm = true;
    return;
  }

  if (sound_inited) {
    I_ShutdownSound();
  }

  // Secure and configure sound device first.
  lprintf(LO_INFO, "I_InitSound: ");

  if (use_experimental_music == 0) {
#ifdef HAVE_MIXER

    /* Initialize variables */
    const int audio_rate = snd_samplerate;
    const int audio_channels = 2;
    const int audio_buffers = getSliceSize();

    if (Mix_OpenAudioDevice(audio_rate, MIX_DEFAULT_FORMAT, audio_channels, audio_buffers, nullptr,
                            SDL_AUDIO_ALLOW_FREQUENCY_CHANGE)
        < 0) {
      lprintf(LO_INFO, "couldn't open audio with desired format (%s)\n", SDL_GetError());
      nosfxparm = true;
      nomusicparm = true;
      return;
    }
    // [FG] feed actual sample frequency back into config variable
    Mix_QuerySpec(&snd_samplerate, nullptr, nullptr);
    sound_inited_once = true;  // e6y
    sound_inited = true;
    Mix_SetPostMix(I_UpdateSound, nullptr);
    lprintf(LO_INFO, " configured audio device with %d samples/slice\n", audio_buffers);
  } else
#else   // HAVE_MIXER
  }
#endif  // HAVE_MIXER
  {
    SDL_AudioSpec audio;

    // Open the audio device
    audio.freq = snd_samplerate;
#if (SDL_BYTEORDER == SDL_BIG_ENDIAN)
    audio.format = AUDIO_S16MSB;
#else
    audio.format = AUDIO_S16LSB;
#endif
    audio.channels = 2;
    audio.samples = getSliceSize();
    audio.callback = I_UpdateSound;

    if (SDL_OpenAudio(&audio, nullptr) < 0) {
      lprintf(LO_INFO, "couldn't open audio with desired format (%s))\n", SDL_GetError());
      nosfxparm = true;
      nomusicparm = true;
      return;
    }

    sound_inited_once = true;  // e6y
    sound_inited = true;
    lprintf(LO_INFO, " configured audio device with %d samples/slice\n", audio.samples);
  }
  if (first_sound_init) {
    I_AtExit(I_ShutdownSound, true);
    first_sound_init = false;
  }

  sfxmutex = SDL_CreateMutex();

  // If we are using the PC speaker, we now need to initialise it.
  if (snd_pcspeaker != 0) {
    I_PCS_InitSound();
  }

  if (!nomusicparm) {
    I_InitMusic();
  }

  // Finished initialization.
  lprintf(LO_INFO, "I_InitSound: sound module ready\n");
  SDL_PauseAudio(0);
}

// NSM sound capture routines

// silences sound output, and instead allows sound capture to work
// call this before sound startup
void I_SetSoundCap() {
  dumping_sound = 1;
}

// grabs len samples of audio (16 bit interleaved)
auto I_GrabSound(const int len) -> unsigned char* {
  static unsigned char* buffer = nullptr;
  static size_t buffer_size = 0;

  if (dumping_sound == 0) {
    return nullptr;
  }

  const std::size_t size = len * 4;
  if (buffer == nullptr || size > buffer_size) {
    buffer_size = size * 4;
    buffer = static_cast<unsigned char*>(realloc(buffer, buffer_size));
  }

  if (buffer != nullptr) {
    std::fill_n(buffer, size, 0);
    I_UpdateSound(reinterpret_cast<void*>(0xdeadbeef), buffer, size);
  }
  return buffer;
}

// NSM helper routine for some of the streaming audio
void I_ResampleStream(void* dest,
                      unsigned nsamp,
                      void (*proc)(void* dest, unsigned nsamp),
                      unsigned sratein,
                      unsigned srateout) {  // assumes 16 bit signed interleaved stereo


  auto* sout = static_cast<short*>(dest);

  static short* sin = nullptr;
  static unsigned sinsamp = 0;

  static unsigned remainder = 0;
  const unsigned step = (sratein << 16) / srateout;

  const unsigned nreq = (step * nsamp + remainder) >> 16;

  if (nreq > sinsamp) {
    sin = static_cast<short*>(realloc(sin, (nreq + 1) * 4));
    if (sinsamp == 0) {  // avoid pop when first starting stream
      sin[0] = sin[1] = 0;
    }
    sinsamp = nreq;
  }

  proc(sin + 2, nreq);

  int j = 0;
  for (unsigned i = 0; i < nsamp; i++) {
    *sout++ = (static_cast<unsigned>(sin[j + 0]) * (0x10000 - remainder) + static_cast<unsigned>(sin[j + 2]) * remainder) >> 16;
    *sout++ = (static_cast<unsigned>(sin[j + 1]) * (0x10000 - remainder) + static_cast<unsigned>(sin[j + 3]) * remainder) >> 16;
    remainder += step;
    j += remainder >> 16 << 1;
    remainder &= 0xffff;
  }
  sin[0] = sin[nreq * 2];
  sin[1] = sin[nreq * 2 + 1];
}

#ifndef HAVE_OWN_MUSIC

//
// MUSIC API.
//

int use_experimental_music = -1;

namespace {
void Exp_UpdateMusic(void* buff, unsigned nsamp);
auto Exp_RegisterMusic(const char* filename, musicinfo_t* song) -> int;
auto Exp_RegisterSong(const void* data, size_t len) -> int;
auto Exp_RegisterSongEx(const void* data, size_t len, int try_mus2mid) -> int;
void Exp_SetMusicVolume(int volume);
void Exp_UnRegisterSong(int handle);
void Exp_StopSong(int handle);
void Exp_ResumeSong(int handle);
void Exp_PauseSong(int handle);
void Exp_PlaySong(int handle, int looping);
void Exp_InitMusic();
void Exp_ShutdownMusic();
}  // namespace

#ifdef HAVE_MIXER

#include "mus2mid.h"

namespace {
std::array<Mix_Music*, 2> music = {nullptr, nullptr};

// Some tracks are directly streamed from the RWops;
// we need to free them in the end
SDL_RWops* rw_midi = nullptr;

char* music_tmp = nullptr; /* cph - name of music temporary file */

// List of extensions that can be appended to music_tmp. First must be "".
std::array<std::string_view, 3> music_tmp_ext = {"", ".mp3", ".ogg"};
}  // namespace

#endif

void I_ShutdownMusic() {
  if (use_experimental_music != 0) {
    Exp_ShutdownMusic();
    return;
  }

#ifdef HAVE_MIXER
  if (music_tmp != nullptr) {
    S_StopMusic();
    for (const auto& ext : music_tmp_ext) {
      std::string name = std::format("{}{}", music_tmp, ext);
      if (unlink(name.data()) == 0) {
        lprintf(LO_DEBUG, "I_ShutdownMusic: removed %s\n", name.data());
      }
    }
    free(music_tmp);
    music_tmp = nullptr;
  }
#endif
}

void I_InitMusic() {
  if (use_experimental_music != 0) {
    Exp_InitMusic();
    return;
  }

#ifdef HAVE_MIXER
  if (music_tmp == nullptr) {
#ifndef _WIN32
    music_tmp = strdup("/tmp/" PACKAGE_TARNAME "-music-XXXXXX");
    {
      const int fd = mkstemp(music_tmp);
      if (fd < 0) {
        lprintf(LO_ERROR, "I_InitMusic: failed to create music temp file %s", music_tmp);
        free(music_tmp);
        music_tmp = nullptr;
        return;
      } else {
        close(fd);
      }
    }
#else /* !_WIN32 */
    music_tmp = strdup("doom.tmp");
#endif
    I_AtExit(I_ShutdownMusic, true);
  }
  return;
#endif
  lprintf(LO_INFO, "I_InitMusic: Was compiled without SDL_Mixer support.  You should enable experimental music.\n");
}

void I_PlaySong(const int handle, const int looping) {
  if (use_experimental_music != 0) {
    Exp_PlaySong(handle, looping);
    return;
  }

#ifdef HAVE_MIXER
  if (music[handle] != nullptr) {
    // Mix_FadeInMusic(music[handle], looping ? -1 : 0, 500);
    Mix_PlayMusic(music[handle], looping != 0 ? -1 : 0);

    // haleyjd 10/28/05: make sure volume settings remain consistent
    I_SetMusicVolume(snd_MusicVolume);
  }
#endif
}

extern "C" int mus_pause_opt;  // From m_misc.c

void I_PauseSong(const int handle) {
  if (use_experimental_music != 0) {
    Exp_PauseSong(handle);
    return;
  }

#ifdef HAVE_MIXER
  switch (mus_pause_opt) {
    case 0:
      I_StopSong(handle);
      break;
    case 1:
      switch (Mix_GetMusicType(nullptr)) {
        case MUS_NONE:
          break;
        case MUS_MID:
          // SDL_mixer's native MIDI music playing does not pause properly.
          // As a workaround, set the volume to 0 when paused.
          I_SetMusicVolume(0);
          break;
        default:
          Mix_PauseMusic();
          break;
      }
      break;
    default:
      break;
  }
#endif
  // Default - let music continue
}

void I_ResumeSong(const int handle) {
  if (use_experimental_music != 0) {
    Exp_ResumeSong(handle);
    return;
  }

#ifdef HAVE_MIXER
  switch (mus_pause_opt) {
    case 0:
      I_PlaySong(handle, 1);
      break;
    case 1:
      switch (Mix_GetMusicType(NULL)) {
        case MUS_NONE:
          break;
        case MUS_MID:
          I_SetMusicVolume(snd_MusicVolume);
          break;
        default:
          Mix_ResumeMusic();
          break;
      }
      break;
    default:
      break;
  }
#endif
  /* Otherwise, music wasn't stopped */
}

void I_StopSong(const int handle) {
  if (use_experimental_music != 0) {
    Exp_StopSong(handle);
    return;
  }

#ifdef HAVE_MIXER
  // halt music playback
  Mix_HaltMusic();
#endif
}

void I_UnRegisterSong(const int handle) {
  if (use_experimental_music != 0) {
    Exp_UnRegisterSong(handle);
    return;
  }

#ifdef HAVE_MIXER
  if (music[handle] != nullptr) {
    Mix_FreeMusic(music[handle]);
    music[handle] = nullptr;

    // Free RWops
    if (rw_midi != nullptr) {
      // SDL_FreeRW(rw_midi);
      rw_midi = nullptr;
    }
  }
#endif
}

auto I_RegisterSong(const void* const data, const std::size_t len) -> int {
  bool io_errors = false;

  if (use_experimental_music != 0) {
    return Exp_RegisterSong(data, len);
  }

#ifdef HAVE_MIXER
  if (music_tmp == nullptr) {
    return 0;
  }

  // e6y: new logic by me
  // Now you can hear title music in deca.wad
  // http://www.doomworld.com/idgames/index.php?id=8808
  // Ability to use mp3 and ogg as inwad lump

  music[0] = nullptr;

  if (len > 4 && std::memcmp(data, "MUS", 3) != 0) {
    // The header has no MUS signature
    // Let's try to load this song with SDL
    for (const auto& ext : music_tmp_ext) {
      // Current SDL_mixer (up to 1.2.8) cannot load some MP3 and OGG
      // without proper extension
      const std::string name = std::format("{}{}", music_tmp, ext);

      if (ext.empty()) {
        // midi
        rw_midi = SDL_RWFromConstMem(data, len);
        if (rw_midi != nullptr) {
          music[0] = Mix_LoadMUS_RW(rw_midi, SDL_FALSE);
        }
      }

      if (music[0] == nullptr) {
        io_errors = (M_WriteFile(name.data(), data, len) == 0);
        if (!io_errors) {
          music[0] = Mix_LoadMUS(name.data());
        }
      }

      if (music[0] != nullptr) {
        break;  // successfully loaded
      }
    }
  }

  // e6y: from Chocolate-Doom
  // Assume a MUS file and try to convert
  if (len > 4 && music[0] == nullptr) {
    MEMFILE* instream = mem_fopen_read(data, len);
    MEMFILE* const outstream = mem_fopen_write();

    // e6y: from chocolate-doom
    // New mus -> mid conversion code thanks to Ben Ryves <benryves@benryves.com>
    // This plays back a lot of music closer to Vanilla Doom - eg. tnt.wad map02
    int result = mus2mid(instream, outstream);

    if (result != 0) {
      std::size_t muslen = len;
      const auto* musptr = static_cast<const unsigned char*>(data);

      // haleyjd 04/04/10: scan forward for a MUS header. Evidently DMX was
      // capable of doing this, and would skip over any intervening data. That,
      // or DMX doesn't use the MUS header at all somehow.
      while (musptr < static_cast<const unsigned char*>(data) + len - sizeof(musheader)) {
        // if we found a likely header start, reset the mus pointer to that location,
        // otherwise just leave it alone and pray.
        if (std::string_view{reinterpret_cast<const char*>(musptr), 4} == "MUS\x1a") {
          mem_fclose(instream);
          instream = mem_fopen_read(musptr, muslen);
          result = mus2mid(instream, outstream);
          break;
        }

        musptr++;
        muslen--;
      }
    }

    if (result == 0) {
      void* outbuf;
      std::size_t outbuf_len;

      mem_get_buf(outstream, &outbuf, &outbuf_len);

      rw_midi = SDL_RWFromMem(outbuf, outbuf_len);
      if (rw_midi != nullptr) {
        music[0] = Mix_LoadMUS_RW(rw_midi, SDL_FALSE);
      }

      if (music[0] == nullptr) {
        io_errors = M_WriteFile(music_tmp, outbuf, outbuf_len) == 0;

        if (!io_errors) {
          // Load the MUS
          music[0] = Mix_LoadMUS(music_tmp);
        }
      }
    }

    mem_fclose(instream);
    mem_fclose(outstream);
  }

  // Failed to load
  if (music[0] == nullptr) {
    // Conversion failed, free everything
    if (rw_midi != nullptr) {
      // SDL_FreeRW(rw_midi);
      rw_midi = nullptr;
    }

    if (io_errors) {
      lprintf(LO_ERROR, "Error writing song\n");
    } else {
      lprintf(LO_ERROR, "Error loading song: %s\n", Mix_GetError());
    }
  }

#endif

  return (0);
}

// cournia - try to load a music file into SDL_Mixer
//           returns true if could not load the file
auto I_RegisterMusic(const char* filename, musicinfo_t* const song) -> int {
  if (use_experimental_music != 0) {
    return Exp_RegisterMusic(filename, song);
  }

#ifdef HAVE_MIXER
  if (filename == nullptr) {
    return 1;
  }

  if (song == nullptr) {
    return 1;
  }

  music[0] = Mix_LoadMUS(filename);
  if (music[0] == nullptr) {
    lprintf(LO_WARN, "Couldn't load music from %s: %s\nAttempting to load default MIDI music.\n", filename,
            Mix_GetError());
    return 1;
  }

  song->data = nullptr;
  song->handle = 0;
  song->lumpnum = 0;
  return 0;

#else
  return 1;
#endif
}

void I_SetMusicVolume(int volume) {
  if (use_experimental_music != 0) {
    Exp_SetMusicVolume(volume);
    return;
  }

#ifdef HAVE_MIXER
  Mix_VolumeMusic(volume * 8);
#endif
}

/********************************************************

experimental music API

********************************************************/

// note that the "handle" passed around by s_sound is ignored
// however, a handle is maintained for the individual music players

const char* snd_soundfont;  // soundfont name for synths that use it
const char* snd_mididev;    // midi device to use (portmidiplayer)

#include "mus2mid.h"

#include "MUSIC/musicplayer.h"

#include "MUSIC/alsaplayer.h"
#include "MUSIC/dumbplayer.h"
#include "MUSIC/flplayer.h"
#include "MUSIC/madplayer.h"
#include "MUSIC/oplplayer.h"
#include "MUSIC/portmidiplayer.h"
#include "MUSIC/vorbisplayer.h"

namespace {
// list of possible music players
const std::array<const music_player_t*, 7> music_players = {  // until some ui work is done, the order these appear is
                                                              // the autodetect order. of particular importance:  things
                                                              // that play mus have to be last, because mus2midi very
                                                              // often succeeds even on garbage input
    &vorb_player,       // vorbisplayer.h
    &mp_player,         // madplayer.h
    &db_player,         // dumbplayer.h
    &fl_player,         // flplayer.h
    &opl_synth_player,  // oplplayer.h
    &pm_player,         // portmidiplayer.h
    &alsa_player,       // alsaplayer.h
};

std::array<int, music_players.size()> music_player_was_init = {};

constexpr std::string_view PLAYER_VORBIS = "vorbis player";
constexpr std::string_view PLAYER_MAD = "mad mp3 player";
constexpr std::string_view PLAYER_DUMB = "dumb tracker player";
constexpr std::string_view PLAYER_FLUIDSYNTH = "fluidsynth midi player";
constexpr std::string_view PLAYER_OPL2 = "opl2 synth player";
constexpr std::string_view PLAYER_PORTMIDI = "portmidi midi player";
constexpr std::string_view PLAYER_ALSA = "alsa midi player";
}  // namespace

// order in which players are to be tried
const char* music_player_order[] = {
    PLAYER_VORBIS.data(),
    PLAYER_MAD.data(),
    PLAYER_DUMB.data(),
    PLAYER_FLUIDSYNTH.data(),
    PLAYER_OPL2.data(),
    PLAYER_PORTMIDI.data(),
    PLAYER_ALSA.data(),
};

// prefered MIDI device
const char* snd_midiplayer;

const char* midiplayers[midi_player_last + 1] = {"sdl", "fluidsynth", "opl2", "portmidi", "alsa", nullptr};

namespace {
int current_player = -1;
const void* music_handle = nullptr;

// songs played directly from wad (no mus->mid conversion)
// won't have this
void* song_data = nullptr;
}  // namespace

int mus_fluidsynth_chorus;
int mus_fluidsynth_reverb;
int mus_fluidsynth_gain;              // NSM  fine tune fluidsynth output level
int mus_opl_gain;                     // NSM  fine tune OPL output level
const char* mus_portmidi_reset_type;  // portmidi reset type
int mus_portmidi_reset_delay;         // portmidi delay after reset
int mus_portmidi_filter_sysex;        // portmidi block sysex from midi files
int mus_portmidi_reverb_level;        // portmidi reverb send level
int mus_portmidi_chorus_level;        // portmidi chorus send level

namespace {
void Exp_ShutdownMusic() {
  S_StopMusic();

  for (std::size_t i = 0; i < music_players.size(); i++) {
    if (music_player_was_init[i]) {
      music_players[i]->shutdown();
    }
  }

  if (musmutex != nullptr) {
    SDL_DestroyMutex(musmutex);
    musmutex = nullptr;
  }
}

void Exp_InitMusic() {
  musmutex = SDL_CreateMutex();

  // todo not so greedy
  for (std::size_t i = 0; i < music_players.size(); i++) {
    music_player_was_init[i] = music_players[i]->init(snd_samplerate);
  }
  I_AtExit(Exp_ShutdownMusic, true);
}

void Exp_PlaySong(const int handle, const int looping) {
  if (music_handle != nullptr) {
    SDL_LockMutex(musmutex);
    music_players[current_player]->play(music_handle, looping);
    music_players[current_player]->setvolume(snd_MusicVolume);
    SDL_UnlockMutex(musmutex);
  }
}

extern "C" int mus_pause_opt;  // From m_misc.c

void Exp_PauseSong(const int handle) {
  if (music_handle == nullptr) {
    return;
  }

  SDL_LockMutex(musmutex);
  switch (mus_pause_opt) {
    case 0:
      music_players[current_player]->stop();
      break;
    case 1:
      music_players[current_player]->pause();
      break;
    default:  // Default - let music continue
      break;
  }
  SDL_UnlockMutex(musmutex);
}

void Exp_ResumeSong(const int handle) {
  if (music_handle == nullptr) {
    return;
  }

  SDL_LockMutex(musmutex);
  switch (mus_pause_opt) {
    case 0:  // i'm not sure why we can guarantee looping=true here,
             // but that's what the old code did
      music_players[current_player]->play(music_handle, 1);
      break;
    case 1:
      music_players[current_player]->resume();
      break;
    default:  // Default - music was never stopped
      break;
  }
  SDL_UnlockMutex(musmutex);
}

void Exp_StopSong(const int handle) {
  if (music_handle != nullptr) {
    SDL_LockMutex(musmutex);
    music_players[current_player]->stop();
    SDL_UnlockMutex(musmutex);
  }
}

void Exp_UnRegisterSong(const int handle) {
  if (music_handle != nullptr) {
    SDL_LockMutex(musmutex);
    music_players[current_player]->unregistersong(music_handle);
    music_handle = nullptr;
    if (song_data != nullptr) {
      free(song_data);
      song_data = nullptr;
    }
    SDL_UnlockMutex(musmutex);
  }
}

void Exp_SetMusicVolume(const int volume) {
  if (music_handle != nullptr) {
    SDL_LockMutex(musmutex);
    music_players[current_player]->setvolume(volume);
    SDL_UnlockMutex(musmutex);
  }
}

// returns 1 on success, 0 on failure
auto Exp_RegisterSongEx(const void* data, size_t len, int try_mus2mid) -> int {
//  int i, j;
//  bool io_errors = false;
//
//  MEMFILE* instream;
//  MEMFILE* outstream;
//  void* outbuf;
//  size_t outbuf_len;
//  int result;

  // try_mus2mid = 0; // debug: supress mus2mid conversion completely

  if (music_handle != nullptr) {
    Exp_UnRegisterSong(0);
  }

  // e6y: new logic by me
  // Now you can hear title music in deca.wad
  // http://www.doomworld.com/idgames/index.php?id=8808
  // Ability to use mp3 and ogg as inwad lump

  if (len > 4 && std::memcmp(data, "MUS", 3) != 0) {
    // The header has no MUS signature
    // Let's try to load this song directly

    // go through music players in order
    bool found = false;

    for (std::size_t j = 0; j < music_players.size(); j++) {
      found = false;
      for (std::size_t i = 0; i < music_players.size(); i++) {
        if (std::string_view{music_players[i]->name()} == std::string_view{music_player_order[j]}) {
          found = true;

          if (music_player_was_init[i]) {
            const void* temp_handle = music_players[i]->registersong(data, len);
            if (temp_handle != nullptr) {
              SDL_LockMutex(musmutex);
              current_player = i;
              music_handle = temp_handle;
              SDL_UnlockMutex(musmutex);
              lprintf(LO_INFO, "Exp_RegisterSongEx: Using player %s\n", music_players[i]->name());
              return 1;
            }
          } else {
            lprintf(LO_INFO, "Exp_RegisterSongEx: Music player %s on preferred list but it failed to init\n",
                    music_players[i]->name());
          }
        }
      }

      if (!found) {
        lprintf(LO_INFO,
                "Exp_RegisterSongEx: Couldn't find preferred music player %s in list\n  (typo or support not included "
                "at compile time)\n",
                music_player_order[j]);
      }
    }
    // load failed
  }

  // load failed? try mus2mid
  if (len > 4 && try_mus2mid != 0) {

    MEMFILE* instream = mem_fopen_read(data, len);
    MEMFILE* outstream = mem_fopen_write();

    // e6y: from chocolate-doom
    // New mus -> mid conversion code thanks to Ben Ryves <benryves@benryves.com>
    // This plays back a lot of music closer to Vanilla Doom - eg. tnt.wad map02
    int result = mus2mid(instream, outstream);
    if (result != 0) {
      std::size_t muslen = len;
      const auto* musptr = static_cast<const unsigned char*>(data);

      // haleyjd 04/04/10: scan forward for a MUS header. Evidently DMX was
      // capable of doing this, and would skip over any intervening data. That,
      // or DMX doesn't use the MUS header at all somehow.
      while (musptr < static_cast<const unsigned char*>(data) + len - sizeof(musheader)) {
        // if we found a likely header start, reset the mus pointer to that location,
        // otherwise just leave it alone and pray.
        if (std::string_view{reinterpret_cast<const char*>(musptr), 4} == "MUS\x1a") {
          mem_fclose(instream);
          instream = mem_fopen_read(musptr, muslen);
          result = mus2mid(instream, outstream);
          break;
        }

        musptr++;
        muslen--;
      }
    }
    if (result == 0) {
      void* outbuf{};
      std::size_t outbuf_len{};

      mem_get_buf(outstream, &outbuf, &outbuf_len);

      // recopy so we can free the MEMFILE
      song_data = malloc(outbuf_len);
      if (song_data != nullptr) {
        memcpy(song_data, outbuf, outbuf_len);
      }

      mem_fclose(instream);
      mem_fclose(outstream);

      if (song_data != nullptr) {
        return Exp_RegisterSongEx(song_data, outbuf_len, 0);
      }
    }
  }

  lprintf(LO_ERROR, "Exp_RegisterSongEx: Failed\n");
  return 0;
}

auto Exp_RegisterSong(const void* const data, const std::size_t len) -> int {
  Exp_RegisterSongEx(data, len, 1);
  return 0;
}

// try register external music file (not in WAD)

auto Exp_RegisterMusic(const char* const filename, musicinfo_t* const song) -> int {
  int len = M_ReadFile(filename, reinterpret_cast<byte**>(&song_data));

  if (len == -1) {
    lprintf(LO_WARN, "Couldn't read %s\nAttempting to load default MIDI music.\n", filename);
    return 1;
  }

  if (Exp_RegisterSongEx(song_data, len, 1) == 0) {
    free(song_data);
    song_data = nullptr;
    lprintf(LO_WARN, "Couldn't load music from %s\nAttempting to load default MIDI music.\n", filename);
    return 1;  // failure
  }

  song->data = nullptr;
  song->handle = 0;
  song->lumpnum = 0;
  return 0;
}

void Exp_UpdateMusic(void* const buff, const unsigned nsamp) {
  if (music_handle == nullptr) {
    std::fill_n(static_cast<std::byte*>(buff), nsamp * 4, std::byte{0});
    return;
  }

  music_players[current_player]->render(buff, nsamp);
}
}  // namespace

void M_ChangeMIDIPlayer() {
#ifdef HAVE_OWN_MUSIC
  // do not bother about small memory leak
  snd_midiplayer = strdup(midiplayers[midi_player_sdl]);
  use_experimental_music = 0;
  return;
#endif

  bool experimental_music;
  if (strcasecmp(snd_midiplayer, midiplayers[midi_player_sdl]) == 0) {
    experimental_music = false;
  } else {
    experimental_music = true;

    if (strcasecmp(snd_midiplayer, midiplayers[midi_player_fluidsynth]) == 0) {
      music_player_order[3] = PLAYER_FLUIDSYNTH.data();
      music_player_order[4] = PLAYER_OPL2.data();
      music_player_order[6] = PLAYER_PORTMIDI.data();
      music_player_order[5] = PLAYER_ALSA.data();
    } else if (strcasecmp(snd_midiplayer, midiplayers[midi_player_opl2]) == 0) {
      music_player_order[3] = PLAYER_OPL2.data();
      music_player_order[6] = PLAYER_PORTMIDI.data();
      music_player_order[4] = PLAYER_ALSA.data();
      music_player_order[5] = PLAYER_FLUIDSYNTH.data();
    } else if (strcasecmp(snd_midiplayer, midiplayers[midi_player_alsa]) == 0) {
      music_player_order[3] = PLAYER_ALSA.data();
      music_player_order[5] = PLAYER_FLUIDSYNTH.data();
      music_player_order[6] = PLAYER_OPL2.data();
      music_player_order[4] = PLAYER_PORTMIDI.data();
    } else if (strcasecmp(snd_midiplayer, midiplayers[midi_player_portmidi]) == 0) {
      music_player_order[3] = PLAYER_PORTMIDI.data();
      music_player_order[6] = PLAYER_ALSA.data();
      music_player_order[4] = PLAYER_FLUIDSYNTH.data();
      music_player_order[5] = PLAYER_OPL2.data();
    }
  }

#if 1
  if (use_experimental_music == -1) {
    use_experimental_music = static_cast<int>(experimental_music);
  } else {
    if (experimental_music && use_experimental_music != 0) {
      S_StopMusic();
      S_RestartMusic();
    }
  }
#else
  S_StopMusic();

  if (use_experimental_music != static_cast<int>(experimental_music)) {
    I_ShutdownMusic();

    S_Stop();
    I_ShutdownSound();

    use_experimental_music = static_cast<int>(experimental_music);

    I_InitSound();
  }

  S_RestartMusic();
#endif
}

#endif
