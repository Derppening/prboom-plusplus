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
 *      Startup and quit functions. Handles signals, inits the
 *      memory management, then calls D_DoomMain. Also contains
 *      I_Init which does other system-related startup stuff.
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef BOOL(WINAPI* SetAffinityFunc)(HANDLE hProcess, DWORD mask);
#else
#include <sched.h>
#endif

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <forward_list>

#include "TEXTSCREEN/txt_main.h"

#include "d_main.h"
#include "doomdef.h"
#include "doomstat.h"
#include "g_game.h"
#include "i_main.h"
#include "i_sound.h"
#include "i_system.h"
#include "i_video.h"
#include "lprintf.h"
#include "m_argv.h"
#include "m_fixed.h"
#include "m_misc.h"
#include "m_random.h"
#include "r_fps.h"
#include "z_zone.h"

#include "e6y.h"

/* Most of the following has been rewritten by Lee Killough
 *
 * I_GetTime
 * killough 4/13/98: Make clock rate adjustable by scale factor
 * cphipps - much made static
 */

namespace {
int basetime = 0;

auto I_GetTime_MS() -> int {
  const int ticks = SDL_GetTicks();

  if (basetime == 0) {
    basetime = ticks;
  }

  return ticks - basetime;
}
}  // namespace

int ms_to_next_tick;

auto I_GetTime_RealTime() -> int {
  const std::int64_t t = I_GetTime_MS();
  const std::int64_t i = t * TICRATE / 1000;

  ms_to_next_tick = (i + 1) * 1000 / TICRATE - t;
  ms_to_next_tick = BETWEEN(0, 1000 / TICRATE, ms_to_next_tick);

  return i;
}

int realtic_clock_rate = 100;

namespace {
auto I_GetTime_Scaled() -> int {
  const std::int64_t t = I_GetTime_MS();
  const std::int64_t i = t * TICRATE * realtic_clock_rate / 100000;

  ms_to_next_tick = (i + 1) * 100000 / realtic_clock_rate / TICRATE - t;
  ms_to_next_tick = BETWEEN(0, 100000 / realtic_clock_rate / TICRATE, ms_to_next_tick);

  return i;
}

auto I_GetTime_FastDemo() -> int {
  static int fasttic;

  ms_to_next_tick = 0;

  return fasttic++;
}

auto I_GetTime_Error() -> int {
  I_Error_Fmt("I_GetTime_Error: GetTime() used before initialization");
  return 0;
}
}  // namespace

int (*I_GetTime)() = I_GetTime_Error;

// During a fast demo, no time elapses in between ticks
namespace {
auto I_TickElapsedTime_FastDemo() -> int {
  return 0;
}

auto I_TickElapsedTime_RealTime() -> int {
  return static_cast<std::int64_t>(I_GetTime_MS()) * TICRATE % 1000 * FRACUNIT / 1000;
}

auto I_TickElapsedTime_Scaled() -> int {
  return static_cast<std::int64_t>(I_GetTime_MS()) * realtic_clock_rate * TICRATE / 100 % 1000 * FRACUNIT / 1000;
}
}  // namespace

int (*I_TickElapsedTime)() = I_TickElapsedTime_RealTime;

void I_Init() {
  /* killough 4/14/98: Adjustable speedup based on realtic_clock_rate */
  if (fastdemo) {
    I_GetTime = I_GetTime_FastDemo;
    I_TickElapsedTime = I_TickElapsedTime_FastDemo;
  } else if (realtic_clock_rate != 100) {
    I_GetTime = I_GetTime_Scaled;
    I_TickElapsedTime = I_TickElapsedTime_Scaled;
  } else {
    I_GetTime = I_GetTime_RealTime;
    I_TickElapsedTime = I_TickElapsedTime_RealTime;
  }

  {
    /* killough 2/21/98: avoid sound initialization if no sound & no music */
    if (!(nomusicparm && nosfxparm)) {
      I_InitSound();
    }
  }

  R_InitInterpolation();
}

// e6y
void I_Init2() {
  if (fastdemo) {
    I_GetTime = I_GetTime_FastDemo;
    I_TickElapsedTime = I_TickElapsedTime_FastDemo;
  } else {
    if (realtic_clock_rate != 100) {
      I_GetTime = I_GetTime_Scaled;
      I_TickElapsedTime = I_TickElapsedTime_Scaled;
    } else {
      I_GetTime = I_GetTime_RealTime;
      I_TickElapsedTime = I_TickElapsedTime_RealTime;
    }
  }
  R_InitInterpolation();
  force_singletics_to = gametic + BACKUPTICS;
}

/* cleanup handling -- killough:
 */
namespace {
void I_SignalHandler(const int s) {
  char buf[2048];

  signal(s, SIG_IGN); /* Ignore future instances of this signal.*/

  I_ExeptionProcess();  // e6y

  std::strcpy(buf, "Exiting on signal: ");
  I_SigString(buf + std::strlen(buf), 2000 - std::strlen(buf), s);

  /* If corrupted memory could cause crash, dump memory
   * allocation history, which points out probable causes
   */
  if (s == SIGSEGV || s == SIGILL || s == SIGFPE) {
    Z_DumpHistory(buf);
  }

  I_Error_Fmt("I_SignalHandler: {}", buf);
}

//
// e6y: exeptions handling
//

ExeptionsList_t current_exception_index;
}  // namespace

ExeptionParam_t ExeptionsParams[EXEPTION_MAX + 1] = {
    {nullptr},
    {"gld_CreateScreenSizeFBO: Access violation in glFramebufferTexture2DEXT.\n\n"
     "Are you using ATI graphics? Try to update your drivers "
     "or change gl_compatibility variable in cfg to 1.\n"},
    {nullptr}};

void I_ExeptionBegin(ExeptionsList_t exception_index) {
  if (current_exception_index == EXEPTION_NONE) {
    current_exception_index = exception_index;
  } else {
    I_Error_Fmt("I_SignalStateSet: signal_state set!");
  }
}

void I_ExeptionEnd() {
  current_exception_index = EXEPTION_NONE;
}

void I_ExeptionProcess() {
  if (current_exception_index > EXEPTION_NONE && current_exception_index < EXEPTION_MAX) {
    I_Error_Fmt("{}", ExeptionsParams[current_exception_index].error_message);
  }
}

/* killough 2/22/98: Add support for ENDBOOM, which is PC-specific
 *
 * this converts BIOS color codes to ANSI codes.
 * Its not pretty, but it does the job - rain
 * CPhipps - made static
 */

namespace {
inline auto convert(int color, int* const bold) -> int {
  if (color > 7) {
    color -= 8;
    *bold = 1;
  }

  switch (color) {
    case 0:
      return 0;
    case 1:
      return 4;
    case 2:
      return 2;
    case 3:
      return 6;
    case 4:
      return 1;
    case 5:
      return 5;
    case 6:
      return 3;
    case 7:
      return 7;
    default:
      return 0;
  }
}
}  // namespace

/* CPhipps - flags controlling ENDOOM behaviour */
enum { endoom_colours = 1, endoom_nonasciichars = 2, endoom_droplastline = 4 };

int endoom_mode;

namespace {
void PrintVer() {
  char vbuf[200];
  lprint(LO_INFO, "{}\n", I_GetVersionString(vbuf, 200));
}

//
// ENDOOM support using text mode emulation
//
void I_EndDoom() {
//  const unsigned char* endoom_data;
//  unsigned char* screendata;

#ifndef _WIN32
  PrintVer();
#endif

  if (!showendoom || demorecording) {
    return;
  }

  /* CPhipps - ENDOOM/ENDBOOM selection */
  const int lump_eb = W_CheckNumForName("ENDBOOM"); /* jff 4/1/98 sign our work    */
  const int lump_ed = W_CheckNumForName("ENDOOM");  /* CPhipps - also maybe ENDOOM */

  int lump;
  if (lump_eb == -1) {
    lump = lump_ed;
  } else if (lump_ed == -1) {
    lump = lump_eb;
  } else { /* Both ENDOOM and ENDBOOM are present */
    constexpr auto LUMP_IS_NEW = [](const int num) {
      return lumpinfo[num].source != source_iwad && lumpinfo[num].source != source_auto_load;
    };

    switch ((LUMP_IS_NEW(lump_ed) ? 1 : 0) | (LUMP_IS_NEW(lump_eb) ? 2 : 0)) {
      case 1:
        lump = lump_ed;
        break;
      case 2:
        lump = lump_eb;
        break;
      default:
        /* Both lumps have equal priority, both present */
        lump = (P_Random(pr_misc) & 1) != 0 ? lump_ed : lump_eb;
        break;
    }
  }

  if (lump != -1) {
    const auto* endoom_data = static_cast<const unsigned char*>(W_CacheLumpNum(lump));

    // Set up text mode screen
    TXT_Init();

    // Make sure the new window has the right title and icon
    I_SetWindowCaption();
    I_SetWindowIcon();

    // Write the data to the screen memory
    unsigned char* screendata = TXT_GetScreenData();
    std::copy_n(reinterpret_cast<const std::byte*>(endoom_data), 4000, reinterpret_cast<std::byte*>(screendata));

    // Wait for a keypress
    while (true) {
      TXT_UpdateScreen();

      if (TXT_GetChar() > 0) {
        break;
      }

      TXT_Sleep(0);
    }

    // Shut down text mode screen
    TXT_Shutdown();
  }
}
}  // namespace

// Schedule a function to be called when the program exits.
// If run_if_error is true, the function is called if the exit
// is due to an error (I_Error)
// Copyright(C) 2005-2014 Simon Howard

struct atexit_listentry_t {
  atexit_func_t func;
  bool run_on_error;
};

namespace {
//atexit_listentry_t* exit_funcs = nullptr;
std::forward_list<atexit_listentry_t> exit_funcs;
}  // namespace

void I_AtExit(const atexit_func_t func, const bool run_on_error) {
  auto entry = atexit_listentry_t{
      .func = func,
      .run_on_error = run_on_error
  };

  exit_funcs.emplace_front(std::move(entry));
}

/* I_SafeExit
 * This function is called instead of exit() by functions that might be called
 * during the exit process (i.e. after exit() has already been called)
 * Prevent infinitely recursive exits -- killough
 */

void I_SafeExit(const int rc) {
  // Run through all exit functions
  for (const auto& entry : exit_funcs) {
    if (rc == 0 || entry.run_on_error) {
      entry.func();
    }
  }

  std::exit(rc);
}

namespace {
void I_Quit() {
  if (demorecording) {
    G_CheckDemoStatus();
  } else {
    I_EndDoom();
  }

  M_SaveDefaults();
  I_DemoExShutdown();
}
}  // namespace

#ifdef SECURE_UID
uid_t stored_euid = -1;
#endif

//
// Ability to use only the allowed CPUs
//

namespace {
void I_SetAffinityMask() {
  // Forcing single core only for "SDL MIDI Player"
  process_affinity_mask = 0;
  if (!strcasecmp(snd_midiplayer, midiplayers[midi_player_sdl])) {
    process_affinity_mask = 1;
  }

  // Set the process affinity mask so that all threads
  // run on the same processor.  This is a workaround for a bug in
  // SDL_mixer that causes occasional crashes.
  if (process_affinity_mask != 0) {
    const char* errbuf = nullptr;
#ifdef _WIN32
    HMODULE kernel32_dll;
    SetAffinityFunc SetAffinity = nullptr;
    int ok = false;

    // Find the kernel interface DLL.
    kernel32_dll = LoadLibrary("kernel32.dll");

    if (kernel32_dll) {
      // Find the SetProcessAffinityMask function.
      SetAffinity = (SetAffinityFunc)GetProcAddress(kernel32_dll, "SetProcessAffinityMask");

      // If the function was not found, we are on an old (Win9x) system
      // that doesn't have this function.  That's no problem, because
      // those systems don't support SMP anyway.

      if (SetAffinity) {
        ok = SetAffinity(GetCurrentProcess(), process_affinity_mask);
      }
    }

    if (!ok) {
      errbuf = WINError();
    }
#elif defined(HAVE_SCHED_SETAFFINITY)
    // POSIX version:
    {
      cpu_set_t set;

      CPU_ZERO(&set);

      for (int i = 0; i < 16; i++) {
        CPU_SET((process_affinity_mask >> i) & 1, &set);
      }

      if (sched_setaffinity(getpid(), sizeof(set), &set) == -1) {
        errbuf = std::strerror(errno);
      }
    }
#else
    return;
#endif

    if (errbuf == nullptr) {
      lprint(LO_INFO, "I_SetAffinityMask: manual affinity mask is {}\n", process_affinity_mask);
    } else {
      lprint(LO_ERROR, "I_SetAffinityMask: failed to set process affinity mask ({})\n", errbuf);
    }
  }
}
}  // namespace

//
// Sets the priority class for the prboom-plus process
//

void I_SetProcessPriority() {
  if (process_priority != 0) {
    const char* errbuf = nullptr;

#ifdef _WIN32
    {
      DWORD dwPriorityClass = NORMAL_PRIORITY_CLASS;

      if (process_priority == 1)
        dwPriorityClass = HIGH_PRIORITY_CLASS;
      else if (process_priority == 2)
        dwPriorityClass = REALTIME_PRIORITY_CLASS;

      if (SetPriorityClass(GetCurrentProcess(), dwPriorityClass) == 0) {
        errbuf = WINError();
      }
    }
#else
    return;
#endif

    if (errbuf == NULL) {
      lprint(LO_INFO, "I_SetProcessPriority: priority for the process is {}\n", process_priority);
    } else {
      lprint(LO_ERROR, "I_SetProcessPriority: failed to set priority for the process ({})\n", errbuf);
    }
  }
}

// int main(int argc, const char * const * argv)
auto main(int argc, char** argv) -> int {
#ifdef SECURE_UID
  /* First thing, revoke setuid status (if any) */
  stored_euid = geteuid();
  if (getuid() != stored_euid)
    if (seteuid(getuid()) < 0)
      fprintf(stderr, "Failed to revoke setuid\n");
    else
      fprintf(stderr, "Revoked uid %d\n", stored_euid);
#endif

  myargc = argc;
  myargv = static_cast<char**>(malloc(sizeof(myargv[0]) * myargc));
  std::copy_n(argv, myargc, myargv);

  // e6y: Check for conflicts.
  // Conflicting command-line parameters could cause the engine to be confused
  // in some cases. Added checks to prevent this.
  // Example: glboom.exe -record mydemo -playdemo demoname
  ParamsMatchingCheck();

  // e6y: was moved from D_DoomMainSetup
  // init subsystems
  // jff 9/3/98 use logical output routine
  lprint(LO_INFO, "M_LoadDefaults: Load system defaults.\n");
  M_LoadDefaults();  // load before initing other systems

  /* Version info */
  lprintf(LO_INFO, "\n");
  PrintVer();

  /* cph - Z_Close must be done after I_Quit, so we register it first. */
  I_AtExit(Z_Close, true);
  /*
     killough 1/98:

     This fixes some problems with exit handling
     during abnormal situations.

     The old code called I_Quit() to end program,
     while now I_Quit() is installed as an exit
     handler and exit() is called to exit, either
     normally or abnormally. Seg faults are caught
     and the error handler is used, to prevent
     being left in graphics mode or having very
     loud SFX noise because the sound card is
     left in an unstable state.
  */

  Z_Init(); /* 1/18/98 killough: start up memory stuff first */

  I_AtExit(I_Quit, false);
#ifndef PRBOOM_DEBUG
  if (M_CheckParm("-devparm") == 0) {
    signal(SIGSEGV, I_SignalHandler);
  }
  signal(SIGTERM, I_SignalHandler);
  signal(SIGFPE, I_SignalHandler);
  signal(SIGILL, I_SignalHandler);
  signal(SIGINT, I_SignalHandler); /* killough 3/6/98: allow CTRL-BRK during init */
  signal(SIGABRT, I_SignalHandler);
#endif

  // Ability to use only the allowed CPUs
  I_SetAffinityMask();

  // Priority class for the prboom-plus process
  I_SetProcessPriority();

  /* cphipps - call to video specific startup code */
  I_PreInitGraphics();

  D_DoomMain();
  return 0;
}
