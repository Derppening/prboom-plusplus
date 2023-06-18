//
// Copyright(C) 2005-2014 Simon Howard
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
//
// Text mode emulation in SDL
//

#include <SDL.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>

#include "doomkeys.h"

#include "txt_main.h"
#include "txt_sdl.h"

#if defined(_MSC_VER) && !defined(__cplusplus)
#define inline __inline
#endif

struct txt_font_t {
  unsigned char* data;
  unsigned int w;
  unsigned int h;
};

// Fonts:

#include "txt_font.h"
#include "txt_largefont.h"
#include "txt_smallfont.h"

// Time between character blinks in ms

#define BLINK_PERIOD 250

namespace {
using unique_SDL_Window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
using unique_SDL_Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;

unique_SDL_Window TXT_SDLWindow{nullptr, SDL_DestroyWindow};
unique_SDL_Surface screenbuffer{nullptr, SDL_FreeSurface};
std::unique_ptr<unsigned char[]> screendata;
int key_mapping = 1;

TxtSDLEventCallbackFunc event_callback;
void* event_callback_data;

int modifier_state[TXT_NUM_MODIFIERS];
}  // namespace

// Font we are using:

static txt_font_t* font;

// #define TANGO

#ifndef TANGO

namespace {
std::array<SDL_Color, 16> ega_colors = {{
    {0x00, 0x00, 0x00, 0xff},  // 0: Black
    {0x00, 0x00, 0xa8, 0xff},  // 1: Blue
    {0x00, 0xa8, 0x00, 0xff},  // 2: Green
    {0x00, 0xa8, 0xa8, 0xff},  // 3: Cyan
    {0xa8, 0x00, 0x00, 0xff},  // 4: Red
    {0xa8, 0x00, 0xa8, 0xff},  // 5: Magenta
    {0xa8, 0x54, 0x00, 0xff},  // 6: Brown
    {0xa8, 0xa8, 0xa8, 0xff},  // 7: Grey
    {0x54, 0x54, 0x54, 0xff},  // 8: Dark grey
    {0x54, 0x54, 0xfe, 0xff},  // 9: Bright blue
    {0x54, 0xfe, 0x54, 0xff},  // 10: Bright green
    {0x54, 0xfe, 0xfe, 0xff},  // 11: Bright cyan
    {0xfe, 0x54, 0x54, 0xff},  // 12: Bright red
    {0xfe, 0x54, 0xfe, 0xff},  // 13: Bright magenta
    {0xfe, 0xfe, 0x54, 0xff},  // 14: Yellow
    {0xfe, 0xfe, 0xfe, 0xff},  // 15: Bright white
}};
}  // namespace

#else

// Colors that fit the Tango desktop guidelines: see
// http://tango.freedesktop.org/ also
// http://uwstopia.nl/blog/2006/07/tango-terminal

namespace {
std::array<SDL_Color, 16> ega_colors = {{
    {0x2e, 0x34, 0x36, 0xff},  // 0: Black
    {0x34, 0x65, 0xa4, 0xff},  // 1: Blue
    {0x4e, 0x9a, 0x06, 0xff},  // 2: Green
    {0x06, 0x98, 0x9a, 0xff},  // 3: Cyan
    {0xcc, 0x00, 0x00, 0xff},  // 4: Red
    {0x75, 0x50, 0x7b, 0xff},  // 5: Magenta
    {0xc4, 0xa0, 0x00, 0xff},  // 6: Brown
    {0xd3, 0xd7, 0xcf, 0xff},  // 7: Grey
    {0x55, 0x57, 0x53, 0xff},  // 8: Dark grey
    {0x72, 0x9f, 0xcf, 0xff},  // 9: Bright blue
    {0x8a, 0xe2, 0x34, 0xff},  // 10: Bright green
    {0x34, 0xe2, 0xe2, 0xff},  // 11: Bright cyan
    {0xef, 0x29, 0x29, 0xff},  // 12: Bright red
    {0x34, 0xe2, 0xe2, 0xff},  // 13: Bright magenta
    {0xfc, 0xe9, 0x4f, 0xff},  // 14: Yellow
    {0xee, 0xee, 0xec, 0xff},  // 15: Bright white
}};
}  // namespace

#endif

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Examine system DPI settings to determine whether to use the large font.

static int Win32_UseLargeFont(void) {
  HDC hdc = GetDC(NULL);
  int dpix;

  if (!hdc) {
    return 0;
  }

  dpix = GetDeviceCaps(hdc, LOGPIXELSX);
  ReleaseDC(NULL, hdc);

  // 144 is the DPI when using "150%" scaling. If the user has this set
  // then consider this an appropriate threshold for using the large font.

  return dpix >= 144;
}

#endif

namespace {
auto FontForName(const std::string_view name) -> txt_font_t* {
  if (name == "small") {
    return &small_font;
  }
  if (name == "normal") {
    return &main_font;
  }
  if (name == "large") {
    return &large_font;
  }
  return nullptr;
}

//
// Select the font to use, based on screen resolution
//
// If the highest screen resolution available is less than
// 640x480, use the small font.
//

void ChooseFont() {
  // Allow normal selection to be overridden from an environment variable:

  const std::string_view env = std::string_view{std::getenv("TEXTSCREEN_FONT")};

  font = FontForName(env);

  if (font != nullptr) {
    return;
  }

  // Get desktop resolution.
  // If in doubt and we can't get a list, always prefer to
  // fall back to the normal font:

  SDL_DisplayMode desktop_info;
  if (SDL_GetCurrentDisplayMode(0, &desktop_info) == 0) {
    font = &main_font;
    return;
  }

  // On tiny low-res screens (eg. palmtops) use the small font.
  // If the screen resolution is at least 1920x1080, this is
  // a modern high-resolution display, and we can use the
  // large font.

  if (desktop_info.w < 640 || desktop_info.h < 480) {
    font = &small_font;
  }
#ifdef _WIN32
  // On Windows we can use the system DPI settings to make a
  // more educated guess about whether to use the large font.

  else if (Win32_UseLargeFont()) {
    font = &large_font;
  }
#endif
  // TODO: Detect high DPI on Linux by inquiring about Gtk+ scale
  // settings. This looks like it should just be a case of shelling
  // out to invoke the 'gsettings' command, eg.
  //   gsettings get org.gnome.desktop.interface text-scaling-factor
  // and using large_font if the result is >= 2.
  else {
    font = &main_font;
  }
}
}  // namespace

//
// Initialize text mode screen
//
// Returns 1 if successful, 0 if an error occurred
//

auto TXT_Init() -> int {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    return 0;
  }

  ChooseFont();

  // Always create the screen at the native screen depth (bpp=0);
  // some systems nowadays don't seem to support true 8-bit palettized
  // screen modes very well and we end up with screwed up colors.
  TXT_SDLWindow = unique_SDL_Window{SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                                     TXT_SCREEN_W * font->w, TXT_SCREEN_H * font->h, 0),
                                    SDL_DestroyWindow};

  if (!TXT_SDLWindow) {
    return 0;
  }

  // Instead, we draw everything into an intermediate 8-bit surface
  // the same dimensions as the screen. SDL then takes care of all the
  // 8->32 bit (or whatever depth) color conversions for us.
  screenbuffer = unique_SDL_Surface{
      SDL_CreateRGBSurface(0, TXT_SCREEN_W * font->w, TXT_SCREEN_H * font->h, 8, 0, 0, 0, 0), SDL_FreeSurface};

  SDL_LockSurface(screenbuffer.get());
  SDL_SetPaletteColors(screenbuffer->format->palette, ega_colors.data(), 0, 16);
  SDL_UnlockSurface(screenbuffer.get());
  // SDL2-TODO SDL_EnableUNICODE(1);

  screendata = std::make_unique<unsigned char[]>(TXT_SCREEN_W * TXT_SCREEN_H * 2);
  std::fill_n(screendata.get(), TXT_SCREEN_W * TXT_SCREEN_H * 2, 0);

  // Ignore all mouse motion events
  //    SDL_EventState(SDL_MOUSEMOTION, SDL_IGNORE);

  // Repeat key presses so we can hold down arrows to scroll down the
  // menu, for example. This is what setup.exe does.

  // SDL2-TODO SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);

  return 1;
}

void TXT_Shutdown() {
  screendata.reset();
  screenbuffer.reset();
  TXT_SDLWindow.reset();
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

auto TXT_GetScreenData() -> unsigned char* {
  return screendata.get();
}

namespace {
inline void UpdateCharacter(const int x, const int y) {
  unsigned char* p = &screendata[(y * TXT_SCREEN_W + x) * 2];
  unsigned char character = p[0];

  int fg = p[1] & 0xf;
  int bg = (p[1] >> 4) & 0xf;

  if (bg & 0x8) {
    // blinking

    bg &= ~0x8;

    if (((SDL_GetTicks() / BLINK_PERIOD) % 2) == 0) {
      fg = bg;
    }
  }

  // How many bytes per line?
  const unsigned int bytes = (font->w + 7) / 8;
  p = &font->data[character * font->h * bytes];

  auto* s = static_cast<unsigned char*>(screenbuffer->pixels) + (y * font->h * screenbuffer->pitch) + (x * font->w);

  for (unsigned int y1 = 0; y1 < font->h; ++y1) {
    unsigned char* s1 = s;
    unsigned int bit = 0;

    for (unsigned int x1 = 0; x1 < font->w; ++x1) {
      if (*p & (1 << (7 - bit))) {
        *s1++ = fg;
      } else {
        *s1++ = bg;
      }

      ++bit;
      if (bit == 8) {
        ++p;
        bit = 0;
      }
    }

    if (bit != 0) {
      ++p;
    }

    s += screenbuffer->pitch;
  }
}
}  // namespace

void TXT_UpdateScreenArea(int x, int y, const int w, const int h) {
  SDL_LockSurface(screenbuffer.get());

  int x_end = std::clamp(x + w, 0, TXT_SCREEN_W);
  int y_end = std::clamp(y + h, 0, TXT_SCREEN_H);
  x = std::clamp(x, 0, TXT_SCREEN_W);
  y = std::clamp(y, 0, TXT_SCREEN_H);

  for (int y1 = y; y1 < y_end; ++y1) {
    for (int x1 = x; x1 < x_end; ++x1) {
      UpdateCharacter(x1, y1);
    }
  }

  SDL_Rect rect;
  rect.x = x * font->w;
  rect.y = y * font->h;
  rect.w = (x_end - x) * font->w;
  rect.h = (y_end - y) * font->h;

  SDL_UnlockSurface(screenbuffer.get());

  SDL_BlitSurface(screenbuffer.get(), &rect, SDL_GetWindowSurface(TXT_SDLWindow.get()), &rect);
  SDL_UpdateWindowSurfaceRects(TXT_SDLWindow.get(), &rect, 1);
}

void TXT_UpdateScreen() {
  TXT_UpdateScreenArea(0, 0, TXT_SCREEN_W, TXT_SCREEN_H);
}

void TXT_GetMousePosition(int* const x, int* const y) {
  SDL_GetMouseState(x, y);

  *x /= font->w;
  *y /= font->h;
}

//
// Translates the SDL key
//

namespace {
auto TranslateKey(const SDL_Keysym* const sym) -> int {
  switch (sym->sym) {
    case SDLK_LEFT:
      return KEY_LEFTARROW;
    case SDLK_RIGHT:
      return KEY_RIGHTARROW;
    case SDLK_DOWN:
      return KEY_DOWNARROW;
    case SDLK_UP:
      return KEY_UPARROW;
    case SDLK_ESCAPE:
      return KEY_ESCAPE;
    case SDLK_RETURN:
      return KEY_ENTER;
    case SDLK_TAB:
      return KEY_TAB;
    case SDLK_F1:
      return KEY_F1;
    case SDLK_F2:
      return KEY_F2;
    case SDLK_F3:
      return KEY_F3;
    case SDLK_F4:
      return KEY_F4;
    case SDLK_F5:
      return KEY_F5;
    case SDLK_F6:
      return KEY_F6;
    case SDLK_F7:
      return KEY_F7;
    case SDLK_F8:
      return KEY_F8;
    case SDLK_F9:
      return KEY_F9;
    case SDLK_F10:
      return KEY_F10;
    case SDLK_F11:
      return KEY_F11;
    case SDLK_F12:
      return KEY_F12;
    case SDLK_PRINTSCREEN:
      return KEY_PRTSCR;

    case SDLK_BACKSPACE:
      return KEY_BACKSPACE;
    case SDLK_DELETE:
      return KEY_DEL;

    case SDLK_PAUSE:
      return KEY_PAUSE;

    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
      return KEY_RSHIFT;

    case SDLK_LCTRL:
    case SDLK_RCTRL:
      return KEY_RCTRL;

    case SDLK_LALT:
    case SDLK_RALT:
      return KEY_RALT;

    case SDLK_CAPSLOCK:
      return KEY_CAPSLOCK;
    case SDLK_SCROLLLOCK:
      return KEY_SCRLCK;

    case SDLK_HOME:
      return KEY_HOME;
    case SDLK_INSERT:
      return KEY_INS;
    case SDLK_END:
      return KEY_END;
    case SDLK_PAGEUP:
      return KEY_PGUP;
    case SDLK_PAGEDOWN:
      return KEY_PGDN;

#ifdef SDL_HAVE_APP_KEYS
    case SDLK_APP1:
      return KEY_F1;
    case SDLK_APP2:
      return KEY_F2;
    case SDLK_APP3:
      return KEY_F3;
    case SDLK_APP4:
      return KEY_F4;
    case SDLK_APP5:
      return KEY_F5;
    case SDLK_APP6:
      return KEY_F6;
#endif

    default:
      break;
  }

  // Returned value is different, depending on whether key mapping is
  // enabled.  Key mapping is preferable most of the time, for typing
  // in text, etc.  However, when we want to read raw keyboard codes
  // for the setup keyboard configuration dialog, we want the raw
  // key code.

  if (key_mapping) {
    // Unicode characters beyond the ASCII range need to be
    // mapped up into textscreen's Unicode range.

#if 0
    // SDL2-TODO
        if (sym->unicode < 128)
        {
            return sym->unicode;
        }
        else
        {
            return sym->unicode - 128 + TXT_UNICODE_BASE;
        }
#endif
    return 0;
  }

  // Keypad mapping is only done when we want a raw value:
  // most of the time, the keypad should behave as it normally
  // does.

  switch (sym->sym) {
    case SDLK_KP_0:
      return KEYP_0;
    case SDLK_KP_1:
      return KEYP_1;
    case SDLK_KP_2:
      return KEYP_2;
    case SDLK_KP_3:
      return KEYP_3;
    case SDLK_KP_4:
      return KEYP_4;
    case SDLK_KP_5:
      return KEYP_5;
    case SDLK_KP_6:
      return KEYP_6;
    case SDLK_KP_7:
      return KEYP_7;
    case SDLK_KP_8:
      return KEYP_8;
    case SDLK_KP_9:
      return KEYP_9;

    case SDLK_KP_PERIOD:
      return KEYP_PERIOD;
    case SDLK_KP_MULTIPLY:
      return KEYP_MULTIPLY;
    case SDLK_KP_PLUS:
      return KEYP_PLUS;
    case SDLK_KP_MINUS:
      return KEYP_MINUS;
    case SDLK_KP_DIVIDE:
      return KEYP_DIVIDE;
    case SDLK_KP_EQUALS:
      return KEYP_EQUALS;
    case SDLK_KP_ENTER:
      return KEYP_ENTER;

    default:
      return tolower(sym->sym);
  }
}

// Convert an SDL button index to textscreen button index.
//
// Note special cases because 2 == mid in SDL, 3 == mid in textscreen/setup

auto SDLButtonToTXTButton(const int button) -> int {
  switch (button) {
    case SDL_BUTTON_LEFT:
      return TXT_MOUSE_LEFT;
    case SDL_BUTTON_RIGHT:
      return TXT_MOUSE_RIGHT;
    case SDL_BUTTON_MIDDLE:
      return TXT_MOUSE_MIDDLE;
    default:
      return TXT_MOUSE_BASE + button - 1;
  }
}

auto MouseHasMoved() -> bool {
  static int last_x = 0, last_y = 0;

  int x, y;
  TXT_GetMousePosition(&x, &y);

  if (x != last_x || y != last_y) {
    last_x = x;
    last_y = y;
    return true;
  }

  return false;
}

// Examine a key press/release and update the modifier key state
// if necessary.

void UpdateModifierState(const SDL_Keysym* const sym, const bool pressed) {
  txt_modifier_t mod;

  switch (sym->sym) {
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
      mod = TXT_MOD_SHIFT;
      break;

    case SDLK_LCTRL:
    case SDLK_RCTRL:
      mod = TXT_MOD_CTRL;
      break;

    case SDLK_LALT:
    case SDLK_RALT:
      mod = TXT_MOD_ALT;
      break;

    default:
      return;
  }

  if (pressed) {
    ++modifier_state[mod];
  } else {
    --modifier_state[mod];
  }
}
}  // namespace

auto TXT_GetChar() -> signed int {
  SDL_Event ev;

  while (SDL_PollEvent(&ev) != 0) {
    // If there is an event callback, allow it to intercept this
    // event.

    if (event_callback != nullptr) {
      if (event_callback(&ev, event_callback_data) != 0) {
        continue;
      }
    }

    // Process the event.

    switch (ev.type) {
      case SDL_MOUSEBUTTONDOWN:
        if (ev.button.button < TXT_MAX_MOUSE_BUTTONS) {
          return SDLButtonToTXTButton(ev.button.button);
        }
        break;

      case SDL_KEYDOWN:
        UpdateModifierState(&ev.key.keysym, 1);

        return TranslateKey(&ev.key.keysym);

      case SDL_KEYUP:
        UpdateModifierState(&ev.key.keysym, 0);
        break;

      case SDL_QUIT:
        // Quit = escape
        return 27;

      case SDL_MOUSEMOTION:
        if (MouseHasMoved()) {
          return 0;
        }
        break;

      default:
        break;
    }
  }

  return -1;
}

auto TXT_GetModifierState(const txt_modifier_t mod) -> int {
  if (mod < TXT_NUM_MODIFIERS) {
    return modifier_state[mod] > 0;
  }

  return 0;
}

namespace {
auto SpecialKeyName(const int key) -> std::string_view {
  switch (key) {
    case ' ':
      return "SPACE";
    case KEY_RIGHTARROW:
      return "RIGHT";
    case KEY_LEFTARROW:
      return "LEFT";
    case KEY_UPARROW:
      return "UP";
    case KEY_DOWNARROW:
      return "DOWN";
    case KEY_ESCAPE:
      return "ESC";
    case KEY_ENTER:
      return "ENTER";
    case KEY_TAB:
      return "TAB";
    case KEY_F1:
      return "F1";
    case KEY_F2:
      return "F2";
    case KEY_F3:
      return "F3";
    case KEY_F4:
      return "F4";
    case KEY_F5:
      return "F5";
    case KEY_F6:
      return "F6";
    case KEY_F7:
      return "F7";
    case KEY_F8:
      return "F8";
    case KEY_F9:
      return "F9";
    case KEY_F10:
      return "F10";
    case KEY_F11:
      return "F11";
    case KEY_F12:
      return "F12";
    case KEY_BACKSPACE:
      return "BKSP";
    case KEY_PAUSE:
      return "PAUSE";
    case KEY_EQUALS:
      return "EQUALS";
    case KEY_MINUS:
      return "MINUS";
    case KEY_RSHIFT:
      return "SHIFT";
    case KEY_RCTRL:
      return "CTRL";
    case KEY_RALT:
      return "ALT";
    case KEY_CAPSLOCK:
      return "CAPS";
    case KEY_SCRLCK:
      return "SCRLCK";
    case KEY_HOME:
      return "HOME";
    case KEY_END:
      return "END";
    case KEY_PGUP:
      return "PGUP";
    case KEY_PGDN:
      return "PGDN";
    case KEY_INS:
      return "INS";
    case KEY_DEL:
      return "DEL";
    case KEY_PRTSCR:
      return "PRTSC";
      /*
case KEYP_0:          return "PAD0";
case KEYP_1:          return "PAD1";
case KEYP_2:          return "PAD2";
case KEYP_3:          return "PAD3";
case KEYP_4:          return "PAD4";
case KEYP_5:          return "PAD5";
case KEYP_6:          return "PAD6";
case KEYP_7:          return "PAD7";
case KEYP_8:          return "PAD8";
case KEYP_9:          return "PAD9";
case KEYP_UPARROW:    return "PAD_U";
case KEYP_DOWNARROW:  return "PAD_D";
case KEYP_LEFTARROW:  return "PAD_L";
case KEYP_RIGHTARROW: return "PAD_R";
case KEYP_MULTIPLY:   return "PAD*";
case KEYP_PLUS:       return "PAD+";
case KEYP_MINUS:      return "PAD-";
case KEYP_DIVIDE:     return "PAD/";
        */

    default:
      return {};
  }
}
}  // namespace

void TXT_GetKeyDescription(int key, char* buf, size_t buf_len) {
  const std::string_view keyname = SpecialKeyName(key);

  if (!keyname.empty()) {
    TXT_StringCopy(buf, keyname.data(), buf_len);
  } else if (std::isprint(key) != 0) {
    TXT_snprintf(buf, buf_len, "%c", std::toupper(key));
  } else {
    TXT_snprintf(buf, buf_len, "??%i", key);
  }
}

// Searches the desktop screen buffer to determine whether there are any
// blinking characters.

auto TXT_ScreenHasBlinkingChars() -> int {
  // Check all characters in screen buffer

  for (int y = 0; y < TXT_SCREEN_H; ++y) {
    for (int x = 0; x < TXT_SCREEN_W; ++x) {
      unsigned char* p = &screendata[(y * TXT_SCREEN_W + x) * 2];

      if (p[1] & 0x80) {
        // This character is blinking

        return 1;
      }
    }
  }

  // None found

  return 0;
}

// Sleeps until an event is received, the screen needs to be redrawn,
// or until timeout expires (if timeout != 0)

void TXT_Sleep(int timeout) {
  if (TXT_ScreenHasBlinkingChars() != 0) {
    const int time_to_next_blink = BLINK_PERIOD - (SDL_GetTicks() % BLINK_PERIOD);

    // There are blinking characters on the screen, so we
    // must time out after a while

    if (timeout == 0 || timeout > time_to_next_blink) {
      // Add one so it is always positive

      timeout = time_to_next_blink + 1;
    }
  }

  if (timeout == 0) {
    // We can just wait forever until an event occurs

    SDL_WaitEvent(nullptr);
  } else {
    // Sit in a busy loop until the timeout expires or we have to
    // redraw the blinking screen

    const unsigned int start_time = SDL_GetTicks();

    while (SDL_GetTicks() < start_time + timeout) {
      if (SDL_PollEvent(nullptr) != 0) {
        // Received an event, so stop waiting

        break;
      }

      // Don't hog the CPU

      SDL_Delay(1);
    }
  }
}

void TXT_EnableKeyMapping(int enable) {
  key_mapping = enable;
}

void TXT_SetWindowTitle(char* title) {
  SDL_SetWindowTitle(TXT_SDLWindow.get(), title);
}

void TXT_SDL_SetEventCallback(TxtSDLEventCallbackFunc callback, void* user_data) {
  event_callback = callback;
  event_callback_data = user_data;
}

// Safe string functions.
void TXT_StringCopy(char* dest, const char* src, size_t dest_len) {
  if (dest_len < 1) {
    return;
  }

  dest[dest_len - 1] = '\0';
  strncpy(dest, src, dest_len - 1);
}

void TXT_StringConcat(char* dest, const char* src, size_t dest_len) {
  std::size_t offset = strlen(dest);
  if (offset > dest_len) {
    offset = dest_len;
  }

  TXT_StringCopy(dest + offset, src, dest_len - offset);
}

// On Windows, vsnprintf() is _vsnprintf().
#ifdef _WIN32
#if _MSC_VER < 1400 /* not needed for Visual Studio 2008 */
#define vsnprintf _vsnprintf
#endif
#endif

// Safe, portable vsnprintf().
auto TXT_vsnprintf(char* buf, size_t buf_len, const char* s, va_list args) -> int {
  if (buf_len < 1) {
    return 0;
  }

  // Windows (and other OSes?) has a vsnprintf() that doesn't always
  // append a trailing \0. So we must do it, and write into a buffer
  // that is one byte shorter; otherwise this function is unsafe.
  int result = vsnprintf(buf, buf_len, s, args);

  // If truncated, change the final char in the buffer to a \0.
  // A negative result indicates a truncated buffer on Windows.
  if (result < 0 || static_cast<std::size_t>(result) >= buf_len) {
    buf[buf_len - 1] = '\0';
    result = buf_len - 1;
  }

  return result;
}

// Safe, portable snprintf().
auto TXT_snprintf(char* buf, size_t buf_len, const char* s, ...) -> int {
  va_list args;
  va_start(args, s);
  int result = TXT_vsnprintf(buf, buf_len, s, args);
  va_end(args);
  return result;
}
