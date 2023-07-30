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
 *    Declarations etc. for logical console output
 *
 *-----------------------------------------------------------------------------*/

#ifndef __LPRINTF__
#define __LPRINTF__

#ifdef __cplusplus
#include <cstdarg>
#include <cstddef>

#include <format>
#include <string_view>
#else
#include <stdarg.h>
#include <stddef.h>
#endif  // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef enum {                 /* Logical output levels */
               LO_INFO = 1,    /* One of these is used in each physical output    */
               LO_CONFIRM = 2, /* call. Which are output, or echoed to console    */
               LO_WARN = 4,    /* if output redirected is determined by the       */
               LO_ERROR = 8,   /* global masks: cons_output_mask,cons_error_mask. */
               LO_FATAL = 16,
               LO_DEBUG = 32,
               LO_ALWAYS = 64,
} OutputLevels;

#ifndef __GNUC__
#define __attribute__(x)
#endif

extern int lprintf(OutputLevels pri, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
extern int cons_output_mask;
extern int cons_error_mask;

/* killough 3/20/98: add const
 * killough 4/25/98: add gcc attributes
 * cphipps 01/11- moved from i_system.h */
void I_Error(const char* error, ...) __attribute__((format(printf, 1, 2)));

int doom_vsnprintf(char* buf, size_t max, const char* fmt, va_list va);
int doom_snprintf(char* buf, size_t max, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#ifdef __cplusplus
auto lvprint(OutputLevels pri, std::string_view fmt, std::format_args args) -> int;

template<typename... Args>
auto lprint(const OutputLevels pri, std::format_string<Args...> fmt, Args&&... args) -> int {
  return lvprint(pri, fmt.get(), std::make_format_args(args...));
}

void I_Error_VFmt(std::string_view fmt, std::format_args args);

template<typename... Args>
void I_Error_Fmt(std::format_string<Args...> fmt, Args&&... args) {
  I_Error_VFmt(fmt.get(), std::make_format_args(args...));
}

// Implementation for C++23 std::vprint_*
auto vprint(std::FILE* stream, std::string_view fmt, std::format_args args) -> int;

inline auto vprint(std::string_view fmt, std::format_args args) -> int {
  return vprint(stdout, fmt, args);
}

// Implementation for C++23 std::print
template<typename... Args>
auto print(std::FILE* stream, std::format_string<Args...> fmt, Args&&... args) -> int {
  return vprint(stream, fmt.get(), std::make_format_args(args...));
}

template<typename... Args>
auto print(std::format_string<Args...> fmt, Args&&... args) -> int {
  return print(stdout, fmt, std::forward<Args>(args)...);
}
#endif  // __cplusplus

#endif
