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
//    PC speaker driver for Linux.
//
//-----------------------------------------------------------------------------

#include "config.h"

#ifdef HAVE_LINUX_KD_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>

#include <memory>
#include <string_view>

#include <SDL.h>
#include <SDL_thread.h>

#include "pcsound.h"

//e6y
#include "lprintf.h"

constexpr const std::string_view CONSOLE_DEVICE = "/dev/console";

namespace {
int console_handle;
pcsound_callback_func callback;
bool sound_thread_running = false;
SDL_Thread* sound_thread_handle;

auto SoundThread([[maybe_unused]] void* data) -> int {
  int frequency;
  int duration;

  while (sound_thread_running) {
    callback(&duration, &frequency);

    int cycles;
    if (frequency != 0) {
      cycles = PCSOUND_8253_FREQUENCY / frequency;
    } else {
      cycles = 0;
    }

    ioctl(console_handle, KIOCSOUND, cycles);

    usleep(duration * 1000);
  }

  return 0;
}

int PCSound_Linux_Init(pcsound_callback_func callback_func) {
  // Try to open the console

  console_handle = open(CONSOLE_DEVICE.data(), O_WRONLY);

  if (console_handle == -1) {
    // Don't have permissions for the console device?

    lprint(LO_WARN, "PCSound_Linux_Init: Failed to open '{}': {}\n", CONSOLE_DEVICE.data(), std::strerror(errno));
    return 0;
  }

  if (ioctl(console_handle, KIOCSOUND, 0) < 0) {
    // KIOCSOUND not supported: non-PC linux?

    close(console_handle);
    return 0;
  }

  // Start a thread up to generate PC speaker output

  callback = callback_func;
  sound_thread_running = true;

  sound_thread_handle = SDL_CreateThread(SoundThread, "sound_thread_handle", NULL);

  return 1;
}

void PCSound_Linux_Shutdown() {
  sound_thread_running = false;
  SDL_WaitThread(sound_thread_handle, nullptr);
  close(console_handle);
}
}  // namespace

extern "C" pcsound_driver_t pcsound_linux_driver = {
    "Linux",
    PCSound_Linux_Init,
    PCSound_Linux_Shutdown,
};

#endif /* #ifdef HAVE_LINUX_KD_H */

