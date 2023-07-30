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
 *  Misc system stuff needed by Doom, implemented for Linux.
 *  Mainly timer handling, and ENDOOM/ENDBOOM.
 *
 *-----------------------------------------------------------------------------
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cassert>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <array>
#include <format>
#include <optional>
#include <vector>

#ifdef _MSC_VER
#define F_OK 0 /* Check for file existence */
#define W_OK 2 /* Check for write permission */
#define R_OK 4 /* Check for read permission */
#include <direct.h>
#include <io.h>
#endif

#include <sys/stat.h>

#include <SDL.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <fcntl.h>
#include <sys/stat.h>

#ifndef PRBOOM_SERVER
#include "m_argv.h"
#endif

#include "doomdef.h"
#include "doomtype.h"
#include "lprintf.h"

#ifndef PRBOOM_SERVER
#include "d_player.h"
#include "e6y.h"
#include "m_fixed.h"
#include "r_fps.h"
#endif

#include "i_system.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "z_zone.h"

#include "m_io.h"

void I_uSleep(unsigned long usecs) {
  SDL_Delay(usecs / 1000);
}

#ifndef PRBOOM_SERVER
namespace {
static bool InDisplay = false;
static int saved_gametic = -1;
}  // namespace

bool realframe = false;

auto I_StartDisplay() -> bool {
  if (InDisplay) {
    return false;
  }

  realframe = (movement_smooth == 0) || (gametic > saved_gametic);

  if (realframe) {
    saved_gametic = gametic;
  }

  InDisplay = true;
  return true;
}

void I_EndDisplay() {
  InDisplay = false;
}

auto I_GetTimeFrac() -> fixed_t {
  fixed_t frac;

  if (movement_smooth == 0) {
    frac = FRACUNIT;
  } else {
    frac = I_TickElapsedTime();
  }

  return frac;
}
#endif

/*
 * I_GetRandomTimeSeed
 *
 * CPhipps - extracted from G_ReloadDefaults because it is O/S based
 */
auto I_GetRandomTimeSeed() -> unsigned long {
  namespace chrono = std::chrono;

  const auto now = chrono::system_clock::now();
  return static_cast<unsigned long>(chrono::time_point_cast<chrono::seconds>(now).time_since_epoch().count());
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

#ifndef PRBOOM_SERVER
auto I_FileToBuffer(const char* const filename, byte** const data, int* const size) -> bool {
  bool result = false;

  byte* buffer = nullptr;

  std::FILE* hfile = M_fopen(filename, "rb");
  if (hfile != nullptr) {
    fseek(hfile, 0, SEEK_END);
    std::size_t filesize = ftell(hfile);
    fseek(hfile, 0, SEEK_SET);

    buffer = static_cast<byte*>(malloc(filesize));
    if (buffer != nullptr) {
      if (fread(buffer, filesize, 1, hfile) == 1) {
        result = true;

        if (data != nullptr) {
          *data = buffer;
        }

        if (size != nullptr) {
          *size = filesize;
        }
      }
    }

    fclose(hfile);
  }

  if (!result) {
    free(buffer);
    buffer = nullptr;
  }

  return result;
}
#endif  // PRBOOM_SERVER

/*
 * I_Read
 *
 * cph 2001/11/18 - wrapper for read(2) which handles partial reads and aborts
 * on error.
 */
void I_Read(const int fd, void* const vbuf, std::size_t sz) {
  auto* buf = static_cast<unsigned char*>(vbuf);

  while (sz != 0) {
    const int rc = read(fd, buf, sz);
    if (rc <= 0) {
      I_Error_Fmt("I_Read: read failed: {}", rc != 0 ? std::strerror(errno) : "EOF");
    }
    sz -= rc;
    buf += rc;
  }
}

/*
 * I_Filelength
 *
 * Return length of an open file.
 */

auto I_Filelength(int handle) -> int {
  struct stat fileinfo {};
  if (fstat(handle, &fileinfo) == -1) {
    I_Error_Fmt("I_Filelength: {}", strerror(errno));
  }
  return fileinfo.st_size;
}

#ifndef PRBOOM_SERVER

// Return the path where the executable lies -- Lee Killough
// proff_fs 2002-07-04 - moved to i_system
#ifdef _WIN32

void I_SwitchToWindow(HWND hwnd) {
  typedef BOOL(WINAPI * TSwitchToThisWindow)(HWND wnd, BOOL restore);
  static TSwitchToThisWindow SwitchToThisWindow = NULL;

  if (!SwitchToThisWindow)
    SwitchToThisWindow = (TSwitchToThisWindow)GetProcAddress(GetModuleHandle("user32.dll"), "SwitchToThisWindow");

  if (SwitchToThisWindow) {
    HWND hwndLastActive = GetLastActivePopup(hwnd);

    if (IsWindowVisible(hwndLastActive))
      hwnd = hwndLastActive;

    SetForegroundWindow(hwnd);
    Sleep(100);
    SwitchToThisWindow(hwnd, TRUE);
  }
}

auto I_DoomExeDir() -> const char* {
  static const char current_dir_dummy[] = {"."};  // proff - rem extra slash 8/21/03
  static char* base;
  if (!base)  // cache multiple requests
  {
    size_t len = strlen(*myargv);
    char* p = (base = (char*)malloc(len + 1)) + len - 1;
    strcpy(base, *myargv);
    while (p > base && *p != '/' && *p != '\\')
      *p-- = 0;
    if (*p == '/' || *p == '\\')
      *p-- = 0;
    if (strlen(base) < 2 || M_access(base, W_OK) != 0) {
      free(base);
      base = (char*)malloc(1024);
      if (!M_getcwd(base, 1024) || M_access(base, W_OK) != 0)
        strcpy(base, current_dir_dummy);
    }
  }
  return base;
}

auto I_GetTempDir() -> const char* {
  static char tmp_path[PATH_MAX] = {0};

  if (tmp_path[0] == 0) {
    GetTempPath(sizeof(tmp_path), tmp_path);
  }

  return tmp_path;
}

#elif defined(AMIGA)

auto I_DoomExeDir() -> const char* {
  return "PROGDIR:";
}

auto I_GetTempDir() -> const char* {
  return "PROGDIR:";
}

#elif defined(MACOSX)

/* Defined elsewhere */

#else
// cph - V.Aguilar (5/30/99) suggested return ~/.lxdoom/, creating
//  if non-existant
// cph 2006/07/23 - give prboom+ its own dir
namespace {
const std::string_view prboom_dir = "prboom-plus";
}  // namespace

auto I_DoomExeDir() -> const char* {
  static std::string base;

  if (base.empty()) {  // cache multiple requests
    std::string home = M_getenv("HOME");

    // I've had trouble with trailing slashes before...
    if (home.back() == '/') {
      home.resize(home.length() - 1);
    }

    base = std::format("{}/.{}", home, prboom_dir);

    // if ~/.$prboom_dir doesn't exist,
    // create and use directory in XDG_DATA_HOME
    struct stat data_dir {};
    if (M_stat(base.data(), &data_dir) != 0 || !S_ISDIR(data_dir.st_mode)) {
      // SDL creates this directory if it doesn't exist
      const auto prefpath =
          std::unique_ptr<char, decltype(&SDL_free)>{SDL_GetPrefPath("", prboom_dir.data()), SDL_free};

      base = prefpath.get();
      // SDL_GetPrefPath always returns with trailing slash
      if (base.back() == '/') {
        base.resize(base.size() - 1);
      }
    }
    //    mkdir(base, S_IRUSR | S_IWUSR | S_IXUSR);
  }

  return base.data();
}

auto I_GetTempDir() -> const char* {
  return "/tmp";
}

#endif

/*
 * HasTrailingSlash
 *
 * cphipps - simple test for trailing slash on dir names
 */

namespace {
auto HasTrailingSlash(const std::string_view dn) -> bool {
  if (dn.back() == '/') {
    return true;
  }

#if defined(_WIN32)
  if (dn.back() == '\\') {
    return true;
  }
#endif
#if defined(AMIGA)
  if (dn.back() == ':') {
    return true;
  }
#endif

  return false;
}
}  // namespace

auto HasTrailingSlash(const char* dn) -> bool {
  return HasTrailingSlash(std::string_view{dn});
}

/*
 * I_FindFile
 *
 * proff_fs 2002-07-04 - moved to i_system
 *
 * cphipps 19/1999 - writen to unify the logic in FindIWADFile and the WAD
 *      autoloading code.
 * Searches the standard dirs for a named WAD file
 * The dirs are listed at the start of the function
 */

#ifndef MACOSX /* OSX defines its search paths elsewhere. */

#ifdef _WIN32
constexpr char PATH_SEPARATOR = ';';
#else
constexpr char PATH_SEPARATOR = ':';
#endif
namespace {
auto I_FindFileInternal(const std::optional<std::string_view> wfname,
                        const std::optional<std::string_view> ext,
                        bool isStatic) -> char* {
  // lookup table of directories to search
  struct search_s {
    std::optional<std::string> dir;  // directory
    const char* sub;                 // subdirectory
    const char* env;                 // environment variable
    const char* (*func)();           // for I_DoomExeDir
  };

  static const std::array<const search_s, 11> search0 = {{
      {{}, {}, {}, I_DoomExeDir},   // config directory
      {{}, {}, {}, {}},             // current working directory
      {PRBOOMDATADIR, {}, {}, {}},  // supplemental data directory
      {{}, {}, "DOOMWADDIR", {}},   // run-time $DOOMWADDIR
      {DOOMWADDIR, {}, {}, {}},     // build-time configured DOOMWADDIR
      {{}, "doom", "HOME", {}},     // ~/doom
      {{}, {}, "HOME", {}},         // ~
      {"/usr/local/share/games/doom", {}, {}, {}},
      {"/usr/share/games/doom", {}, {}, {}},
      {"/usr/local/share/doom", {}, {}, {}},
      {"/usr/share/doom", {}, {}, {}},
  }};
  static std::vector<search_s> search{};

  static std::string static_p;
  static_p.reserve(PATH_MAX);

  char* p = (isStatic ? static_p.data() : nullptr);

  if (!wfname) {
    return nullptr;
  }

  if (search.empty()) {
    // initialize with the static lookup table
    search.resize(search0.size());
    std::copy(search0.cbegin(), search0.cend(), search.begin());

    // add each directory from the $DOOMWADPATH environment variable
    const char* dwp = M_getenv("DOOMWADPATH");
    if (dwp != nullptr) {
      const std::string_view dup_dwp = dwp;
      std::string_view left = dup_dwp;

      for (;;) {
        const std::size_t idx = left.find_first_of(PATH_SEPARATOR);
        if (idx != std::string_view::npos) {
          search.push_back(search_s{std::string{left.substr(0, idx)}, nullptr, nullptr, nullptr});
          left = std::string_view{left.data() + idx + 1, left.size() - idx - 1};
        } else {
          break;
        }
      }

      search.push_back(search_s{std::string{left}, nullptr, nullptr, nullptr});
    }
  }

  /* Precalculate a length we will need in the loop */
  const std::size_t pl = wfname->length() + (ext ? ext->length() : 0) + 4;

  for (const auto& i : search) {
    std::optional<std::string_view> d;
    /* Each entry in the switch sets d to the directory to look in,
     * and optionally s to a subdirectory of d */
    // switch replaced with lookup table
    if (i.env != nullptr) {
      const auto* env = M_getenv(i.env);
      d = env != nullptr ? std::make_optional(env) : std::nullopt;
      if (!d) {
        continue;
      }
    } else if (i.func != nullptr) {
      const auto* v = i.func();
      d = v != nullptr ? std::make_optional(v) : std::nullopt;
    } else {
      d = i.dir ? std::make_optional(i.dir->data()) : std::nullopt;
    }
    const std::optional<std::string_view> s = i.sub != nullptr ? std::make_optional(i.sub) : std::nullopt;

    const std::size_t expected_len = (d ? d->length() : 0) + (s ? s->length() : 0) + pl;
    if (!isStatic) {
      p = static_cast<char*>(malloc(expected_len));
    } else {
      static_p.resize(std::min<std::size_t>(expected_len, PATH_MAX));
    }

    const auto res =
        std::format_to_n(p, expected_len, "{}{}{}{}{}", d ? *d : "", (d && !HasTrailingSlash(*d)) ? "/" : "",
                         s ? *s : "", (s && !HasTrailingSlash(*s)) ? "/" : "", *wfname);

    if (!isStatic) {
      p[res.size] = '\0';
    } else {
      static_p.resize(std::min<std::size_t>(res.size, PATH_MAX));
    }

    if (ext && M_access(p, F_OK) != 0) {
      if (!isStatic) {
        std::strcat(p, ext->data());
      } else {
        if (static_p.capacity() < static_p.size() + ext->length()) {
          I_Error_Fmt("Assertion Error: Cannot append ext into static_p without reallocation");
        }

        static_p += *ext;
      }
    }

    if (M_access(p, F_OK) == 0) {
      if (!isStatic) {
        lprint(LO_INFO, " found {}\n", p);
      }

      return p;
    }

    if (!isStatic) {
      free(p);
    }
  }

  return nullptr;
}
}  // namespace

auto I_FindFileInternal(const char* wfname, const char* ext, bool isStatic) -> char* {
  return I_FindFileInternal(wfname != nullptr ? std::make_optional(std::string_view{wfname}) : std::nullopt,
                            ext != nullptr ? std::make_optional(std::string_view{ext}) : std::nullopt,
                            isStatic);
}

auto I_FindFile(const char* wfname, const char* ext) -> char* {
  return I_FindFileInternal(wfname, ext, false);
}

auto I_FindFile2(const char* wfname, const char* ext) -> const char* {
  return I_FindFileInternal(wfname, ext, true);
}

#endif

#endif  // PRBOOM_SERVER
