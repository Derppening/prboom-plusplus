/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2006 by Colin Phipps, Florian Schulze
 *
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
 *  Misc system stuff needed by Doom, implemented for POSIX systems.
 *  Timers and signals.
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstdio>
#include <cstring>

#include <chrono>
#include <format>
#include <thread>

#include "doomdef.h"
#include "i_system.h"

void I_uSleep(const unsigned long usecs) {
  namespace chrono = std::chrono;

  std::this_thread::sleep_for(chrono::microseconds{usecs});
}

/* CPhipps - believe it or not, it is possible with consecutive calls to
 * gettimeofday to receive times out of order, e.g you query the time twice and
 * the second time is earlier than the first. Cheap'n'cheerful fix here.
 * NOTE: only occurs with bad kernel drivers loaded, e.g. pc speaker drv
 */

namespace {
unsigned long lasttimereply;
unsigned long basetime;
}  // namespace

auto I_GetTime_RealTime() -> int {
  namespace chrono = std::chrono;

  const auto now = chrono::system_clock::now();
  auto thistimereply = static_cast<unsigned long>(chrono::time_point_cast<chrono::milliseconds>(now).time_since_epoch().count()) * TICRATE;

  /* Fix for time problem */
  if (basetime != 0) {
    basetime = thistimereply;
    thistimereply = 0;
  } else {
    thistimereply -= basetime;
  }

  if (thistimereply < lasttimereply) {
    thistimereply = lasttimereply;
  }

  return (lasttimereply = thistimereply);
}

/*
 * I_GetRandomTimeSeed
 *
 * CPhipps - extracted from G_ReloadDefaults because it is O/S based
 */
auto I_GetRandomTimeSeed() -> unsigned long {
  /* killough 3/26/98: shuffle random seed, use the clock */
  namespace chrono = std::chrono;

  const auto now = chrono::system_clock::now();
  return static_cast<unsigned long>(chrono::time_point_cast<chrono::milliseconds>(now).time_since_epoch().count());
}

/* cphipps - I_GetVersionString
 * Returns a version string in the given buffer
 */
auto I_GetVersionString(char* const buf, const std::size_t sz) -> const char* {
  const auto result = std::format_to_n(buf, sz, "{} v{} ({})", PACKAGE_NAME, PACKAGE_VERSION, PACKAGE_HOMEPAGE);
  *result.out = '\0';
  return buf;
}

/* cphipps - I_SigString
 * Returns a string describing a signal number
 */
auto I_SigString(char* const buf, const std::size_t sz, int signum) -> const char* {
#ifdef HAVE_STRSIGNAL
  if (strsignal(signum) != nullptr && std::strlen(strsignal(signum)) < sz) {
    std::strcpy(buf, strsignal(signum));
  } else {
    const auto result = std::format_to_n(buf, sz, "signal {}", signum);
    *result.out = '\0';
  }
#else
  const auto result = std::format_to_n(buf, sz, "signal {}", signum);
  *result.out = '\0';
#endif
  return buf;
}
