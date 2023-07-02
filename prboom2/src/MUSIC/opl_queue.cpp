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
//     Queue of waiting callbacks, stored in a binary min heap, so that we
//     can always get the first callback.
//
//-----------------------------------------------------------------------------

#include "opl_queue.h"

#include <cstddef>
#include <cstring>

#include "lprintf.h"

namespace opl::queue {
auto create() -> std::unique_ptr<opl_callback_queue_t> {
  auto queue = std::make_unique<opl_callback_queue_t>();
  queue->num_entries = 0;

  return queue;
}

void destroy([[maybe_unused]] std::unique_ptr<opl_callback_queue_t>&& queue) {}

auto is_empty(const opl_callback_queue_t& queue) -> bool {
  return queue.num_entries == 0;
}

void clear(opl_callback_queue_t& queue) {
  queue.num_entries = 0;
}

void push(opl_callback_queue_t& queue, const opl_callback_t callback, std::byte* const data, const unsigned int time) {
  if (queue.num_entries >= MAX_OPL_QUEUE) {
    lprintf(LO_WARN, "OPL_Queue_Push: Exceeded maximum callbacks\n");
    return;
  }

  // Add to last queue entry.

  unsigned int entry_id = queue.num_entries;
  ++queue.num_entries;

  // Shift existing entries down in the heap.

  while (entry_id > 0) {
    const unsigned int parent_id = (entry_id - 1) / 2;

    // Is the heap condition satisfied?

    if (time >= queue.entries[parent_id].time) {
      break;
    }

    // Move the existing entry down in the heap.

    queue.entries[entry_id] = queue.entries[parent_id];

    // Advance to the parent.

    entry_id = parent_id;
  }

  // Insert new callback data.

  queue.entries[entry_id].callback = callback;
  queue.entries[entry_id].data = data;
  queue.entries[entry_id].time = time;
}

auto pop(opl_callback_queue_t& queue, opl_callback_t& callback, std::byte** const data) -> bool {
  // Empty?

  if (queue.num_entries <= 0) {
    return false;
  }

  // Store the result:

  callback = queue.entries[0].callback;
  *data = queue.entries[0].data;

  // Decrease the heap size, and keep pointer to the last entry in
  // the heap, which must now be percolated down from the top.

  --queue.num_entries;
  opl_queue_entry_t* entry = &queue.entries[queue.num_entries];

  // Percolate down.

  unsigned int i = 0;

  for (;;) {
    const unsigned int child1 = i * 2 + 1;
    const unsigned int child2 = i * 2 + 2;

    unsigned int next_i;
    if (child1 < queue.num_entries && queue.entries[child1].time < entry->time) {
      // Left child is less than entry.
      // Use the minimum of left and right children.

      if (child2 < queue.num_entries && queue.entries[child2].time < queue.entries[child1].time) {
        next_i = child2;
      } else {
        next_i = child1;
      }
    } else if (child2 < queue.num_entries && queue.entries[child2].time < entry->time) {
      // Right child is less than entry.  Go down the right side.

      next_i = child2;
    } else {
      // Finished percolating.
      break;
    }

    // Percolate the next value up and advance.

    queue.entries[i] = queue.entries[next_i];
    i = next_i;
  }

  // Store the old last-entry at its new position.

  queue.entries[i] = *entry;

  return true;
}

auto peek(const opl_callback_queue_t& queue) -> unsigned int {
  if (queue.num_entries > 0) {
    return queue.entries[0].time;
  }
  return 0;
}
}  // namespace opl::queue

auto OPL_Queue_Create() -> opl_callback_queue_t* {
  return opl::queue::create().release();
}

void OPL_Queue_Destroy(opl_callback_queue_t* const queue) {
  opl::queue::destroy(std::unique_ptr<opl_callback_queue_t>{queue});
}

auto OPL_Queue_IsEmpty(opl_callback_queue_t* const queue) -> int {
  return static_cast<int>(opl::queue::is_empty(*queue));
}

void OPL_Queue_Clear(opl_callback_queue_t* const queue) {
  opl::queue::clear(*queue);
}

void OPL_Queue_Push(opl_callback_queue_t* const queue,
                    const opl_callback_t callback,
                    void* const data,
                    const unsigned int time) {
  opl::queue::push(*queue, callback, static_cast<std::byte*>(data), time);
}

auto OPL_Queue_Pop(opl_callback_queue_t* const queue, opl_callback_t* const callback, void** const data) -> int {
  return static_cast<int>(opl::queue::pop(*queue, *callback, reinterpret_cast<std::byte**>(data)));
}

auto OPL_Queue_Peek(opl_callback_queue_t* const queue) -> unsigned int {
  return opl::queue::peek(*queue);
}

#ifdef TEST

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include <format>
#include <iostream>

namespace {
void PrintQueueNode(const opl_callback_queue_t* const queue, const int node, const int depth) {
  if (node >= queue->num_entries) {
    return;
  }

  for (int i = 0; i < depth * 3; ++i) {
    std::puts(" ");
  }

  std::cout << std::format("{}\n", queue->entries[node].time);

  PrintQueueNode(queue, node * 2 + 1, depth + 1);
  PrintQueueNode(queue, node * 2 + 2, depth + 1);
}

void PrintQueue(opl_callback_queue_t* const queue) {
  PrintQueueNode(queue, 0, 0);
}
}  // namespace

int main() {
  opl_callback_queue_t* queue = OPL_Queue_Create();

  for (int iteration = 0; iteration < 5000; ++iteration) {

    for (std::size_t i = 0; i < MAX_OPL_QUEUE; ++i) {
      const unsigned int time = std::rand() % 0x10000;
      OPL_Queue_Push(queue, NULL, NULL, time);
    }

    unsigned int time = 0;

    opl_callback_t callback;
    void* data;

    for (std::size_t i = 0; i < MAX_OPL_QUEUE; ++i) {
      assert(!OPL_Queue_IsEmpty(queue));
      const unsigned int newtime = OPL_Queue_Peek(queue);
      assert(OPL_Queue_Pop(queue, &callback, &data));

      assert(newtime >= time);
      time = newtime;
    }

    assert(OPL_Queue_IsEmpty(queue));
    assert(!OPL_Queue_Pop(queue, &callback, &data));
  }
}

#endif
