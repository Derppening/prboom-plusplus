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
 *      Zone Memory Allocation, perhaps NeXT ObjectiveC inspired.
 *      Remark: this was the only stuff that, according
 *       to John Carmack, might have been useful for
 *       Quake.
 *
 * Rewritten by Lee Killough, though, since it was not efficient enough.
 *
 *---------------------------------------------------------------------*/

#ifndef __Z_ZONE__
#define __Z_ZONE__

#ifndef __GNUC__
#define __attribute__(x)
#endif

// Include system definitions so that prototypes become
// active before macro replacements below are in effect.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// ZONE MEMORY
// PU - purge tags.

typedef enum {PU_FREE, PU_STATIC, PU_SOUND, PU_MUSIC, PU_LEVEL, PU_LEVSPEC, PU_CACHE,
      /* Must always be last -- killough */ PU_MAX} purge_tag_t;

#define PU_PURGELEVEL PU_CACHE        /* First purgable tag's level */

#ifdef INSTRUMENTED
#define DA(x,y) ,x,y
#define DAC(x,y) x,y
#else
#define DA(x,y) 
#define DAC(x,y) void
#endif

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

void *(Z_Malloc)(size_t size, int tag, void **ptr DA(const char *, int));
void (Z_Free)(void *ptr DA(const char *, int));
void (Z_FreeTags)(int lowtag, int hightag DA(const char *, int));
void (Z_ChangeTag)(void *ptr, int tag DA(const char *, int));
void (Z_Init)(void);
void Z_Close(void);
void *(Z_Calloc)(size_t n, size_t n2, int tag, void **user DA(const char *, int));
void *(Z_Realloc)(void *p, size_t n, int tag, void **user DA(const char *, int));
char *(Z_Strdup)(const char *s, int tag, void **user DA(const char *, int));
void (Z_CheckHeap)(DAC(const char *,int));   // killough 3/22/98: add file/line info
void Z_DumpHistory(char *);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#ifdef INSTRUMENTED
/* cph - save space if not debugging, don't require file 
 * and line to memory calls */
#define Z_Free(a)          (Z_Free)     (a,      __FILE__,__LINE__)
#define Z_FreeTags(a,b)    (Z_FreeTags) (a,b,    __FILE__,__LINE__)
#define Z_ChangeTag(a,b)   (Z_ChangeTag)(a,b,    __FILE__,__LINE__)
#define Z_Malloc(a,b,c)    (Z_Malloc)   (a,b,c,  __FILE__,__LINE__)
#define Z_Strdup(a,b,c)    (Z_Strdup)   (a,b,c,  __FILE__,__LINE__)
#define Z_Calloc(a,b,c,d)  (Z_Calloc)   (a,b,c,d,__FILE__,__LINE__)
#define Z_Realloc(a,b,c,d) (Z_Realloc)  (a,b,c,d,__FILE__,__LINE__)
#define Z_CheckHeap()      (Z_CheckHeap)(__FILE__,__LINE__)
#endif

/* cphipps 2001/11/18 -
 * If we're using memory mapped file access to WADs, we won't need to maintain
 * our own heap. So we *could* let "normal" malloc users use the libc malloc
 * directly, for efficiency. Except we do need a wrapper to handle out of memory
 * errors... damn, ok, we'll leave it for now.
 */
#ifndef HAVE_LIBDMALLOC
// Remove all definitions before including system definitions

#undef malloc
#undef free
#undef realloc
#undef calloc
#undef strdup

#ifdef __cplusplus
#define malloc(n)          Z_Malloc(n,PU_STATIC,nullptr)
#define free(p)            Z_Free(p)
#define realloc(p,n)       Z_Realloc(p,n,PU_STATIC,nullptr)
#define calloc(n1,n2)      Z_Calloc(n1,n2,PU_STATIC,nullptr)
#define strdup(s)          Z_Strdup(s,PU_STATIC,nullptr)
#else
#define malloc(n)          Z_Malloc(n,PU_STATIC,0)
#define free(p)            Z_Free(p)
#define realloc(p,n)       Z_Realloc(p,n,PU_STATIC,0)
#define calloc(n1,n2)      Z_Calloc(n1,n2,PU_STATIC,0)
#define strdup(s)          Z_Strdup(s,PU_STATIC,0)
#endif

#else

#ifdef HAVE_LIBDMALLOC
#include <dmalloc.h>
#endif

#endif

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

void Z_ZoneHistory(char *);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#ifdef __cplusplus

#include <limits>
#include <memory_resource>
#include <new>

template<typename T>
class z_allocator_base {
 protected:
  using value_type = T;

  [[nodiscard]] auto allocate(const std::size_t n, const purge_tag_t pu = PU_STATIC, void** const data = nullptr)
      -> T* {
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_array_new_length{};
    }

    if (const auto p = static_cast<T*>(Z_Malloc(n * sizeof(T), pu, data))) {
      return p;
    }

    throw std::bad_alloc{};
  }

  void deallocate(T* const p, [[maybe_unused]] const std::size_t n) noexcept { Z_Free(p); }
};

template<typename T, purge_tag_t PU = PU_STATIC, typename UserT = void>
class z_allocator : protected z_allocator_base<T> {
 public:
  using typename z_allocator_base<T>::value_type;

  template<typename U>
  struct rebind {
    using other = z_allocator<U, PU, UserT>;
  };

  z_allocator() noexcept = default;

  explicit z_allocator(UserT** data) : _data{data} {}

  template<typename U, purge_tag_t PU2, typename UserT2 = void>
  constexpr z_allocator([[maybe_unused]] const z_allocator<U, PU2, UserT2>& other) noexcept {}

  [[nodiscard]] auto allocate(const std::size_t n) -> T* { return z_allocator_base<T>::allocate(n, PU, _data); }

  using z_allocator_base<T>::deallocate;

 private:
  UserT** _data{nullptr};
};

template<typename T, purge_tag_t PU>
  requires(PU < PU_PURGELEVEL)
class z_allocator<T, PU, void> : protected z_allocator_base<T> {
 public:
  using typename z_allocator_base<T>::value_type;

  template<typename U>
  struct rebind {
    using other = z_allocator<U, PU, void>;
  };

  z_allocator() noexcept = default;

  template<typename U, purge_tag_t PU2, typename UserT2 = void>
  constexpr z_allocator([[maybe_unused]] const z_allocator<U, PU2, UserT2>& other) noexcept {}

  [[nodiscard]] auto allocate(const std::size_t n) -> T* { return z_allocator_base<T>::allocate(n, PU); }

  using z_allocator_base<T>::deallocate;
};

template<typename T>
class z_allocator<T, PU_STATIC, void> : protected z_allocator_base<T> {
 public:
  using typename z_allocator_base<T>::value_type;

  template<typename U>
  struct rebind {
    using other = z_allocator<U, PU_STATIC, void>;
  };

  z_allocator() noexcept = default;

  template<typename U, purge_tag_t PU2, typename UserT2 = void>
  constexpr z_allocator([[maybe_unused]] const z_allocator<U, PU2, UserT2>& other) noexcept {}

  [[nodiscard]] auto allocate(const std::size_t n) -> T* { return z_allocator_base<T>::allocate(n); }

  using z_allocator_base<T>::deallocate;
};

template<typename T1, typename T2>
constexpr bool operator==(const z_allocator_base<T1>&, const z_allocator_base<T2>&) noexcept {
  return true;
}

template<typename T1, typename T2>
constexpr bool operator!=(const z_allocator_base<T1>&, const z_allocator_base<T2>&) noexcept {
  return false;
}

class z_memory_resource : std::pmr::memory_resource {
 public:
  z_memory_resource(const purge_tag_t pu = PU_STATIC, void** const user = nullptr);

  auto do_allocate(const std::size_t bytes, [[maybe_unused]] const std::size_t alignment) noexcept -> void* override;

  void do_deallocate(void* const p,
                     [[maybe_unused]] const std::size_t bytes,
                     [[maybe_unused]] const std::size_t alignment) noexcept override;

  [[nodiscard]] auto do_is_equal(const std::pmr::memory_resource& other) const noexcept -> bool override;

 private:
  purge_tag_t _pu_;
  void** _user_;
};

#endif  // __cplusplus

#endif
