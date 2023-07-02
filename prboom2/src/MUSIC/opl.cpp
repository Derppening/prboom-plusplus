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
//     OPL interface.
//
//-----------------------------------------------------------------------------

#include "config.h"

#include "opl.h"

#include <cstdint>

#include <algorithm>
#include <memory>

#include "dbopl.h"
#include "opl_queue.h"

#include "i_sound.h"  // mus_opl_gain

namespace {
int init_stage_reg_writes = 1;
}  // namespace

unsigned int opl_sample_rate = 22050;

struct opl_timer_t {
  unsigned int rate;         // Number of times the timer is advanced per sec.
  bool enabled;              // Non-zero if timer is enabled.
  unsigned int value;        // Last value that was set.
  unsigned int expire_time;  // Calculated time that timer will expire.
};

namespace {
// Queue of callbacks waiting to be invoked.

std::unique_ptr<opl_callback_queue_t> callback_queue;

// Current time, in number of samples since startup:

unsigned int current_time;

// If non-zero, playback is currently paused.

bool opl_paused;

// Time offset (in samples) due to the fact that callbacks
// were previously paused.

unsigned int pause_offset;

// OPL software emulator structure.

std::unique_ptr<Chip> opl_chip;

// Temporary mixing buffer used by the mixing callback.

std::unique_ptr<int[]> mix_buffer;

// Register number that was written.

int register_num = 0;

// Timers; DBOPL does not do timer stuff itself.

opl_timer_t timer1 = {12500, false, 0, 0};
opl_timer_t timer2 = {3125, false, 0, 0};
}  // namespace

namespace opl {
void init_registers() {
  OPL_InitRegisters();
}

namespace timer {
void calculate_end_time(opl_timer_t& timer) {
  // If the timer is enabled, calculate the time when the timer
  // will expire.

  if (timer.enabled) {
    const int tics = 0x100 - timer.value;
    timer.expire_time = current_time + (tics * opl_sample_rate) / timer.rate;
  }
}
}  // namespace timer
}  // namespace opl

//
// Init/shutdown code.
//

// Initialize the OPL library.  Returns true if initialized
// successfully.

auto OPL_Init(const unsigned int rate) -> int {
  opl_sample_rate = rate;
  opl_paused = false;
  pause_offset = 0;

  // Queue structure of callbacks to invoke.

  callback_queue = opl::queue::create();
  current_time = 0;

  mix_buffer = std::make_unique<int[]>(opl_sample_rate);

  // Create the emulator structure:

  DBOPL_InitTables();
  opl_chip = std::make_unique<Chip>(false);
  opl_chip->Setup(opl_sample_rate);

  opl::init_registers();

  init_stage_reg_writes = 0;

  return 1;
}

// Shut down the OPL library.

void OPL_Shutdown() {
  if (callback_queue) {
    callback_queue.reset();
    mix_buffer.reset();
  }
}

void OPL_SetCallback(const unsigned int ms, const opl_callback_t callback, void* const data) {
  opl::queue::push(*callback_queue, callback, static_cast<std::byte*>(data),
                   current_time - pause_offset + (ms * opl_sample_rate) / 1000);
}

void OPL_ClearCallbacks() {
  opl::queue::clear(*callback_queue);
}

namespace {
void WriteRegister(const unsigned int reg_num, const unsigned int value) {
  switch (reg_num) {
    case OPL_REG_TIMER1:
      timer1.value = value;
      opl::timer::calculate_end_time(timer1);
      break;

    case OPL_REG_TIMER2:
      timer2.value = value;
      opl::timer::calculate_end_time(timer2);
      break;

    case OPL_REG_TIMER_CTRL:
      if ((value & 0x80) != 0) {
        timer1.enabled = false;
        timer2.enabled = false;
      } else {
        if ((value & 0x40) == 0) {
          timer1.enabled = (value & 0x01) != 0;
          opl::timer::calculate_end_time(timer1);
        }

        if ((value & 0x20) == 0) {
          timer1.enabled = (value & 0x02) != 0;
          opl::timer::calculate_end_time(timer2);
        }
      }
      break;

    default:
      opl_chip->WriteReg(reg_num, static_cast<Bit8u>(value));
      break;
  }
}

void OPL_AdvanceTime(const unsigned int nsamples) {
  // Advance time.

  current_time += nsamples;

  if (opl_paused) {
    pause_offset += nsamples;
  }

  // Are there callbacks to invoke now?  Keep invoking them
  // until there are none more left.

  while (!opl::queue::is_empty(*callback_queue) && current_time >= opl::queue::peek(*callback_queue) + pause_offset) {
    // Pop the callback from the queue to invoke it.

    opl_callback_t callback;
    std::byte* callback_data;
    if (!opl::queue::pop(*callback_queue, callback, &callback_data)) {
      break;
    }

    callback(callback_data);
  }
}

void FillBuffer(int16_t* const buffer, const unsigned int nsamples) {
  // FIXME???
  // assert(nsamples < opl_sample_rate);

  opl_chip->GenerateBlock2(nsamples, mix_buffer.get());

  // Mix into the destination buffer, doubling up into stereo.

  for (unsigned int i = 0; i < nsamples; ++i) {
    // clip
    const int sampval = std::clamp(mix_buffer[i] * mus_opl_gain / 50, -32768, 32767);
    buffer[i * 2] = static_cast<std::int16_t>(sampval);
    buffer[i * 2 + 1] = static_cast<std::int16_t>(sampval);
  }
}
}  // namespace

void OPL_Render_Samples(void* const dest, const unsigned buffer_len) {
  unsigned int filled = 0;

  auto* const buffer = static_cast<short*>(dest);

  // Repeatedly call the OPL emulator update function until the buffer is
  // full.

  while (filled < buffer_len) {
    unsigned int nsamples;

    // Work out the time until the next callback waiting in
    // the callback queue must be invoked.  We can then fill the
    // buffer with this many samples.

    if (opl_paused || !opl::queue::is_empty(*callback_queue)) {
      nsamples = buffer_len - filled;
    } else {
      const unsigned int next_callback_time = opl::queue::peek(*callback_queue) + pause_offset;

      nsamples = next_callback_time - current_time;

      if (nsamples > buffer_len - filled) {
        nsamples = buffer_len - filled;
      }
    }

    // Add emulator output to buffer.

    FillBuffer(buffer + filled * 2, nsamples);
    filled += nsamples;

    // Invoke callbacks for this point in time.

    OPL_AdvanceTime(nsamples);
  }
}

void OPL_WritePort(const opl_port_t port, const unsigned int value) {
  if (port == OPL_REGISTER_PORT) {
    register_num = value;
  } else if (port == OPL_DATA_PORT) {
    WriteRegister(register_num, value);
  }
}

auto OPL_ReadPort([[maybe_unused]] const opl_port_t port) -> unsigned int {
  unsigned int result = 0;

  if (timer1.enabled && current_time > timer1.expire_time) {
    result |= 0x80;  // Either have expired
    result |= 0x40;  // Timer 1 has expired
  }

  if (timer2.enabled && current_time > timer2.expire_time) {
    result |= 0x80;  // Either have expired
    result |= 0x20;  // Timer 2 has expired
  }

  return result;
}

//
// Higher-level functions, based on the lower-level functions above
// (register write, etc).
//

auto OPL_ReadStatus() -> unsigned int {
  return OPL_ReadPort(OPL_REGISTER_PORT);
}

// Write an OPL register value

void OPL_WriteRegister(const int reg, const int value) {
  OPL_WritePort(OPL_REGISTER_PORT, reg);

  // For timing, read the register port six times after writing the
  // register number to cause the appropriate delay

  for (int i = 0; i < 6; ++i) {
    // An oddity of the Doom OPL code: at startup initialization,
    // the spacing here is performed by reading from the register
    // port; after initialization, the data port is read, instead.

    if (init_stage_reg_writes != 0) {
      OPL_ReadPort(OPL_REGISTER_PORT);
    } else {
      OPL_ReadPort(OPL_DATA_PORT);
    }
  }

  OPL_WritePort(OPL_DATA_PORT, value);

  // Read the register port 24 times after writing the value to
  // cause the appropriate delay

  for (int i = 0; i < 24; ++i) {
    OPL_ReadStatus();
  }
}

// Initialize registers on startup

void OPL_InitRegisters() {
  // Initialize level registers

  for (int r = OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r) {
    OPL_WriteRegister(r, 0x3f);
  }

  // Initialize other registers
  // These two loops write to registers that actually don't exist,
  // but this is what Doom does ...
  // Similarly, the <= is also intenational.

  for (int r = OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r) {
    OPL_WriteRegister(r, 0x00);
  }

  // More registers ...

  for (int r = 1; r < OPL_REGS_LEVEL; ++r) {
    OPL_WriteRegister(r, 0x00);
  }

  // Re-initialize the low registers:

  // Reset both timers and enable interrupts:
  OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x60);
  OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x80);

  // "Allow FM chips to control the waveform of each operator":
  OPL_WriteRegister(OPL_REG_WAVEFORM_ENABLE, 0x20);

  // Keyboard split point on (?)
  OPL_WriteRegister(OPL_REG_FM_MODE, 0x40);
}

void OPL_SetPaused(const int paused) {
  opl_paused = static_cast<bool>(paused);
}
