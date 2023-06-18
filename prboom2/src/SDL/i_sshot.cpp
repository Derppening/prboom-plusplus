/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2006 by
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
 *  Screenshot functions, moved out of i_video.c
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <SDL.h>

#ifdef HAVE_LIBSDL2_IMAGE
#include <SDL_image.h>
#endif

#include "doomdef.h"
#include "i_video.h"
#include "v_video.h"

int renderW;
int renderH;

void I_UpdateRenderSize() {
  if (V_GetMode() == VID_MODEGL) {
    renderW = SCREENWIDTH;
    renderH = SCREENHEIGHT;
  } else {
    SDL_GetRendererOutputSize(sdl_renderer, &renderW, &renderH);
  }
}

//
// I_ScreenShot // Modified to work with SDL2 resizeable window and fullscreen desktop - DTIED
//

namespace {
auto I_Screenshot(std::string_view fname) -> int {
  int result = -1;
  auto* pixels = reinterpret_cast<std::byte*>(I_GrabScreen());
  std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> screenshot{nullptr, SDL_FreeSurface};

  if (pixels != nullptr) {
    screenshot.reset(SDL_CreateRGBSurfaceFrom(pixels, renderW, renderH, 24, renderW * 3, 0x000000ff, 0x0000ff00, 0x00ff0000, 0));
  }

  if (screenshot) {
#ifdef HAVE_LIBSDL2_IMAGE
    result = IMG_SavePNG(screenshot.get(), fname.data());
#else
    result = SDL_SaveBMP(screenshot.get(), fname.data());
#endif
  }

  return result;
}
}  // namespace

auto I_ScreenShot(const char* fname) -> int {
  return I_Screenshot(std::string_view{fname});
}

// NSM
// returns current screen contents as RGB24 (raw)
// returned pointer should be freed when done
//
// Modified to work with SDL2 resizeable window and fullscreen desktop - DTIED
//

auto I_GrabScreen() -> unsigned char* {
  static std::vector<std::byte> pixels;

  I_UpdateRenderSize();

#ifdef GL_DOOM
  if (V_GetMode() == VID_MODEGL) {
    return gld_ReadScreen();
  }
#endif

  const auto size = static_cast<std::size_t>(renderW * renderH * 3);
  if (pixels.empty() || size > pixels.size()) {
    pixels.resize(size);
  }

  if (!pixels.empty() && size > 0) {
    const SDL_Rect screen{0, 0, renderW, renderH};
    SDL_RenderReadPixels(sdl_renderer, &screen, SDL_PIXELFORMAT_RGB24, pixels.data(), renderW * 3);
  }

  return reinterpret_cast<unsigned char*>(pixels.data());
}
