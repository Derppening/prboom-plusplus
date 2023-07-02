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
//     OPL callback queue.
//
//-----------------------------------------------------------------------------

#ifndef OPL_QUEUE_H
#define OPL_QUEUE_H

#include <cstddef>

#include "opl.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct opl_callback_queue_s opl_callback_queue_t;

opl_callback_queue_t* OPL_Queue_Create(void);
int OPL_Queue_IsEmpty(opl_callback_queue_t* queue);
void OPL_Queue_Clear(opl_callback_queue_t* queue);
void OPL_Queue_Destroy(opl_callback_queue_t* queue);
void OPL_Queue_Push(opl_callback_queue_t* queue, opl_callback_t callback, void* data, unsigned int time);
int OPL_Queue_Pop(opl_callback_queue_t* queue, opl_callback_t* callback, void** data);
unsigned int OPL_Queue_Peek(opl_callback_queue_t* queue);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#ifdef __cplusplus

#include <array>
#include <memory>

inline constexpr std::size_t MAX_OPL_QUEUE = 64;

struct opl_queue_entry_t {
  opl_callback_t callback;
  std::byte* data;
  unsigned int time;
};

struct opl_callback_queue_s {
  std::array<opl_queue_entry_t, MAX_OPL_QUEUE> entries;
  unsigned int num_entries;
};

namespace opl::queue {
auto create() -> std::unique_ptr<opl_callback_queue_t>;
void destroy(std::unique_ptr<opl_callback_queue_t>&& queue);
auto is_empty(const opl_callback_queue_t& queue) -> bool;
void clear(opl_callback_queue_t& queue);
void push(opl_callback_queue_t& queue, opl_callback_t callback, std::byte* data, unsigned int time);
auto pop(opl_callback_queue_t& queue, opl_callback_t& callback, std::byte** data) -> bool;
auto peek(const opl_callback_queue_t& queue) -> unsigned int;
}  // namespace opl::queue

#endif  // __cplusplus

#endif /* #ifndef OPL_QUEUE_H */
