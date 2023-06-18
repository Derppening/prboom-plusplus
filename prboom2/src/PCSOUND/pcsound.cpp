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
//    PC speaker interface.
//
//-----------------------------------------------------------------------------

#include <cstdlib>
#include <cstring>

#include <string_view>

#include "pcsound.h"

// e6y
#include "lprintf.h"

#ifdef USE_WIN32_PCSOUND_DRIVER
extern "C" pcsound_driver_t pcsound_win32_driver;
#endif

#ifdef HAVE_LINUX_KD_H
extern "C" pcsound_driver_t pcsound_linux_driver;
#endif

extern "C" pcsound_driver_t pcsound_sdl_driver;

namespace {
pcsound_driver_t* const drivers[] = {
#ifdef HAVE_LINUX_KD_H
    &pcsound_linux_driver,
#endif
#ifdef USE_WIN32_PCSOUND_DRIVER
    &pcsound_win32_driver,
#endif
    &pcsound_sdl_driver,
};

pcsound_driver_t* pcsound_driver = nullptr;
}  // namespace

auto PCSound_Init(pcsound_callback_func callback_func) -> int {
  if (pcsound_driver != nullptr) {
    return 1;
  }

  // Check if the environment variable is set

  const std::string_view driver_name = std::string_view{std::getenv("PCSOUND_DRIVER")};

  if (!driver_name.empty()) {
    for (auto* const driver : drivers) {
      if (strcasecmp(driver->name, driver_name.data()) == 0) {
        // Found the driver!

        if (driver->init_func(callback_func) != 0) {
          pcsound_driver = driver;
        } else {
          lprintf(LO_WARN, "Failed to initialise PC sound driver: %s\n", driver->name);
          break;
        }
      }
    }
  } else {
    // Try all drivers until we find a working one

    for (auto* const driver : drivers) {
      if (driver->init_func(callback_func) != 0) {
        pcsound_driver = driver;
        break;
      }
    }
  }

  if (pcsound_driver != nullptr) {
    lprintf(LO_INFO, "Using PC sound driver: %s\n", pcsound_driver->name);
    return 1;
  } else {
    lprintf(LO_WARN, "Failed to find a working PC sound driver.\n");
    return 0;
  }
}

void PCSound_Shutdown() {
  pcsound_driver->shutdown_func();
  pcsound_driver = nullptr;
}
