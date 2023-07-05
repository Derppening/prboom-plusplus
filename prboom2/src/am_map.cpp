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
 *   the automap code
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cmath>

#include <array>
#include <format>
#include <limits>
#include <span>
#include <string>

#ifdef GL_DOOM
#include "gl_opengl.h"
#endif

#include "am_map.h"
#include "d_deh.h"  // Ty 03/27/98 - externalizations
#include "doomstat.h"
#include "g_game.h"
#include "lprintf.h"  // jff 08/03/98 - declaration of lprintf
#include "m_bbox.h"
#include "m_misc.h"
#include "p_maputl.h"
#include "p_setup.h"
#include "p_spec.h"
#include "r_demo.h"
#include "r_fps.h"
#include "r_main.h"
#include "st_stuff.h"
#include "v_video.h"
#include "w_wad.h"

extern dboolean gamekeydown[];

// jff 1/7/98 default automap colors added
int mapcolor_back;                          // map background
int mapcolor_grid;                          // grid lines color
int mapcolor_wall;                          // normal 1s wall color
int mapcolor_fchg;                          // line at floor height change color
int mapcolor_cchg;                          // line at ceiling height change color
int mapcolor_clsd;                          // line at sector with floor=ceiling color
int mapcolor_rkey;                          // red key color
int mapcolor_bkey;                          // blue key color
int mapcolor_ykey;                          // yellow key color
int mapcolor_rdor;                          // red door color  (diff from keys to allow option)
int mapcolor_bdor;                          // blue door color (of enabling one but not other )
int mapcolor_ydor;                          // yellow door color
int mapcolor_tele;                          // teleporter line color
int mapcolor_secr;                          // secret sector boundary color
int mapcolor_exit;                          // jff 4/23/98 add exit line color
int mapcolor_unsn;                          // computer map unseen line color
int mapcolor_flat;                          // line with no floor/ceiling changes
int mapcolor_sprt;                          // general sprite color
int mapcolor_item;                          // item sprite color
int mapcolor_frnd;                          // friendly sprite color
int mapcolor_enemy;                         // enemy sprite color
int mapcolor_hair;                          // crosshair color
int mapcolor_sngl;                          // single player arrow color
int mapcolor_plyr[4] = {112, 96, 64, 176};  // colors for player arrows in multiplayer

// jff 3/9/98 add option to not show secret sectors until entered
int map_secret_after;

int map_always_updates;
int map_grid_size;
int map_scroll_speed;
int map_wheel_zoom;
int map_use_multisamling;
int map_textured;
int map_textured_trans;
int map_textured_overlay_trans;
int map_lines_overlay_trans;
int map_overlay_pos_x;
int map_overlay_pos_y;
int map_overlay_pos_width;
int map_overlay_pos_height;

map_things_appearance_t map_things_appearance;
const char* map_things_appearance_list[map_things_appearance_max] = {"classic", "scaled",
#if defined(HAVE_LIBSDL2_IMAGE) && defined(GL_DOOM)
                                                                     "icons"
#endif
};

namespace {
// drawing stuff
constexpr int FB = 0;

// scale on entry
constexpr auto INITSCALEMTOF = static_cast<fixed_t>(.2 * FRACUNIT);
// how much the automap moves window per tic in frame-buffer coordinates
// moves 140 pixels in 1 second
auto F_PANINC() -> int {
  return (gamekeydown[key_speed] ? map_scroll_speed * 2 : map_scroll_speed);
}
// how much zoom-in per tic
// goes to 2x in 1 second
auto M_ZOOMIN() -> int {
  return static_cast<int>(static_cast<float>(FRACUNIT) * (1.00F + F_PANINC() / 200.0F));
}
// how much zoom-out per tic
// pulls out to 0.5x in 1 second
auto M_ZOOMOUT() -> int {
  return static_cast<int>(static_cast<float>(FRACUNIT) / (1.00F + F_PANINC() / 200.0F));
}

constexpr int PLAYERRADIUS = 16 * (1 << MAPBITS);  // e6y
}  // namespace

struct mline_t {
  mpoint_t a, b;
};

//
// The vector graphics for the automap.
//  A line drawing of the player pointing right,
//   starting from the middle.
//
namespace {
constexpr auto player_arrow_R() -> int {
  return (8 * PLAYERRADIUS) / 7;
}
}  // namespace
#define R (player_arrow_R())
std::array<mline_t, 7> player_arrow{{{{-R + R / 8, 0}, {R, 0}},     // -----
                                     {{R, 0}, {R - R / 2, R / 4}},  // ----->
                                     {{R, 0}, {R - R / 2, -R / 4}},
                                     {{-R + R / 8, 0}, {-R - R / 8, R / 4}},  // >---->
                                     {{-R + R / 8, 0}, {-R - R / 8, -R / 4}},
                                     {{-R + 3 * R / 8, 0}, {-R + R / 8, R / 4}},  // >>--->
                                     {{-R + 3 * R / 8, 0}, {-R + R / 8, -R / 4}}}};
#undef R

namespace {
constexpr auto cheat_player_arrow_R() -> int {
  return (8 * PLAYERRADIUS) / 7;
}
}  // namespace
#define R (cheat_player_arrow_R())
std::array<mline_t, 14> cheat_player_arrow{{
    // killough 3/22/98: He's alive, Jim :)
    {{-R + R / 8, 0}, {R, 0}},     // -----
    {{R, 0}, {R - R / 2, R / 4}},  // ----->
    {{R, 0}, {R - R / 2, -R / 4}},
    {{-R + R / 8, 0}, {-R - R / 8, R / 4}},  // >---->
    {{-R + R / 8, 0}, {-R - R / 8, -R / 4}},
    {{-R + 3 * R / 8, 0}, {-R + R / 8, R / 4}},  // >>--->
    {{-R + 3 * R / 8, 0}, {-R + R / 8, -R / 4}},
    {{-R / 10 - R / 6, R / 4}, {-R / 10 - R / 6, -R / 4}},  // J
    {{-R / 10 - R / 6, -R / 4}, {-R / 10 - R / 6 - R / 8, -R / 4}},
    {{-R / 10 - R / 6 - R / 8, -R / 4}, {-R / 10 - R / 6 - R / 8, -R / 8}},
    {{-R / 10, R / 4}, {-R / 10, -R / 4}},  // F
    {{-R / 10, R / 4}, {-R / 10 + R / 8, R / 4}},
    {{-R / 10 + R / 4, R / 4}, {-R / 10 + R / 4, -R / 4}},  // F
    {{-R / 10 + R / 4, R / 4}, {-R / 10 + R / 4 + R / 8, R / 4}},
}};
#undef R

// jff 1/5/98 new symbol for keys on automap
namespace {
constexpr auto cross_mark_R() -> int {
  return FRACUNIT;
}
}  // namespace
#define R (cross_mark_R())
std::array<mline_t, 2> cross_mark{{
    {{-R, 0}, {R, 0}},
    {{0, -R}, {0, R}},
}};
#undef R
// jff 1/5/98 end of new symbol

namespace {
constexpr auto thintriangle_guy_R() -> int {
  return FRACUNIT;
}
}  // namespace
#define R (thintriangle_guy_R())
std::array<mline_t, 3> thintriangle_guy{{{{static_cast<fixed_t>(-.5 * R), static_cast<fixed_t>(-.7 * R)},
                                          {static_cast<fixed_t>(R), static_cast<fixed_t>(0)}},
                                         {{static_cast<fixed_t>(R), static_cast<fixed_t>(0)},
                                          {static_cast<fixed_t>(-.5 * R), static_cast<fixed_t>(.7 * R)}},
                                         {{static_cast<fixed_t>(-.5 * R), static_cast<fixed_t>(.7 * R)},
                                          {static_cast<fixed_t>(-.5 * R), static_cast<fixed_t>(-.7 * R)}}}};
#undef R
#define NUMTHINTRIANGLEGUYLINES (thintriangle_guy.size())

int ddt_cheating = 0;  // killough 2/7/98: make global, rename to ddt_*

namespace {
int leveljuststarted = 1;  // kluge until AM_LevelInit() is called
}  // namespace

enum automapmode_e automapmode;  // Mode that the automap is in

namespace {
// location of window on screen
int f_x;
int f_y;

// size of window on screen
int f_w;
int f_h;

mpoint_t m_paninc;     // how far the window pans each tic (map coords)
fixed_t mtof_zoommul;  // how far the window zooms each tic (map coords)
fixed_t ftom_zoommul;  // how far the window zooms each tic (fb coords)
fixed_t curr_mtof_zoommul;

fixed_t m_x, m_y;    // LL x,y window location on the map (map coords)
fixed_t m_x2, m_y2;  // UR x,y window location on the map (map coords)

fixed_t prev_m_x, prev_m_y;

//
// width/height of window on map (map coords)
//
fixed_t m_w;
fixed_t m_h;

// based on level size
fixed_t min_x;
fixed_t min_y;
fixed_t max_x;
fixed_t max_y;

fixed_t max_w;  // max_x-min_x,
fixed_t max_h;  // max_y-min_y

fixed_t min_scale_mtof;  // used to tell when to stop zooming out
fixed_t max_scale_mtof;  // used to tell when to stop zooming in

// old stuff for recovery later
fixed_t old_m_w, old_m_h;
fixed_t old_m_x, old_m_y;

// used by MTOF to scale from map-to-frame-buffer coords
fixed_t scale_mtof = INITSCALEMTOF;
// used by FTOM to scale from frame-buffer-to-map coords (=1/scale_mtof)
fixed_t scale_ftom;
fixed_t prev_scale_mtof = INITSCALEMTOF;

player_t* plr;  // the player represented by an arrow
}  // namespace

// killough 2/22/98: Remove limit on automap marks,
// and make variables external for use in savegames.

// namespace {
// std::vector<markpoint_t, z_allocator<markpoint_t>> _markpoints;
// }  // namespace

markpoint_t* markpoints = nullptr;  // where the points are
int markpointnum = 0;               // next point to be assigned (also number of points now)
int markpointnum_max = 0;           // killough 2/22/98

// namespace {
// void _update_markpoints() {
//   markpoints = _markpoints.data();
//   markpointnum = _markpoints.size();
//   markpointnum_max = _markpoints.capacity();
// }
// }  // namespace

namespace {
bool stopped = true;
}

am_frame_t am_frame;

array_t map_lines;

// Implementation of forward-declared macros
namespace {
// translates between frame-buffer and map distances
auto FTOM(const int x) -> fixed_t {
  return FixedMul((x << 16), scale_ftom);
}

// e6y: int64 version to avoid overflows
auto MTOF(const int x) -> fixed_t {
  return ((static_cast<int_64_t>(x) * scale_mtof) >> FRACBITS) >> FRACBITS;
}

// translates between frame-buffer and map coordinates
auto CXMTOF(const int x) -> int {
  return f_x + MTOF(x - m_x);
}
auto CYMTOF(const int y) -> int {
  return f_y + (f_h - MTOF(y - m_y));
}

// precise versions
auto MTOF_F(const int x) -> float {
  return (static_cast<float>(x) * scale_mtof) / static_cast<float>(FRACUNIT) / static_cast<float>(FRACUNIT);
}
auto CXMTOF_F(const int x) -> float {
  return static_cast<float>(f_x) + MTOF_F(x - m_x);
}
auto CYMTOF_F(const int y) -> float {
  return static_cast<float>(f_y) + (f_h - MTOF_F(y - m_y));
}
}  // namespace

namespace {
void AM_rotate(fixed_t& x, fixed_t& y, angle_t a);
void AM_rotate(fixed_t* x, fixed_t* y, angle_t a);

void AM_SetMPointFloatValue(mpoint_t& p) {
  if (am_frame.precise != 0) {
    p.fx = static_cast<float>(p.x);
    p.fy = static_cast<float>(p.y);
  }
}

void AM_SetFPointFloatValue(fpoint_t& p) {
#ifdef GL_DOOM
  p.fx = static_cast<float>(p.x);
  p.fy = static_cast<float>(p.y);
#endif
}

//
// AM_activateNewScale()
//
// Changes the map scale after zooming or translating
//
// Passed nothing, returns nothing
//
void AM_activateNewScale() {
  m_x += m_w / 2;
  m_y += m_h / 2;
  m_w = FTOM(f_w);
  m_h = FTOM(f_h);
  m_x -= m_w / 2;
  m_y -= m_h / 2;
  m_x2 = m_x + m_w;
  m_y2 = m_y + m_h;
}

//
// AM_saveScaleAndLoc()
//
// Saves the current center and zoom
// Affects the variables that remember old scale and loc
//
// Passed nothing, returns nothing
//
void AM_saveScaleAndLoc() {
  old_m_x = m_x;
  old_m_y = m_y;
  old_m_w = m_w;
  old_m_h = m_h;
}

//
// AM_restoreScaleAndLoc()
//
// restores the center and zoom from locally saved values
// Affects global variables for location and scale
//
// Passed nothing, returns nothing
//
void AM_restoreScaleAndLoc() {
  m_w = old_m_w;
  m_h = old_m_h;
  if ((automapmode & am_follow) == 0) {
    m_x = old_m_x;
    m_y = old_m_y;
  } else {
    m_x = (viewx >> FRACTOMAPBITS) - m_w / 2;  // e6y
    m_y = (viewy >> FRACTOMAPBITS) - m_h / 2;  // e6y
  }
  m_x2 = m_x + m_w;
  m_y2 = m_y + m_h;

  // Change the scaling multipliers
  scale_mtof = FixedDiv(f_w << FRACBITS, m_w);
  scale_ftom = FixedDiv(FRACUNIT, scale_mtof);
}
}  // namespace

void AM_setMarkParams(const int num) {
  static std::string namebuf = "AMMNUM0";

  auto& markpoint = markpoints[num];

  markpoint.w = 0;
  markpoint.h = 0;

  const auto result = std::format_to_n(markpoint.label, sizeof(markpoint.label), "{}", num);
  markpoint.label[result.size] = '\0';
  for (int i = 0; i < result.size; i++) {
    namebuf[6] = markpoint.label[i];
    markpoint.widths[i] = V_NamePatchWidth(namebuf.c_str());
    markpoint.w += markpoint.widths[i] + 1;
    markpoint.h = std::max(markpoint.h, V_NamePatchHeight(namebuf.c_str()));
  }
}

namespace {
//
// AM_addMark()
//
// Adds a marker at the current location
// Affects global variables for marked points
//
// Passed nothing, returns nothing
//
void AM_addMark() {
  // killough 2/22/98:
  // remove limit on automap marks

  if (markpointnum >= markpointnum_max) {
    markpointnum_max = markpointnum_max != 0 ? markpointnum_max * 2 : 16;
    markpoints = static_cast<markpoint_t*>(realloc(markpoints, markpointnum_max * sizeof(*markpoints)));
  }

  markpoints[markpointnum] = markpoint_t{.x = m_x + m_w / 2, .y = m_y + m_h / 2};
  AM_setMarkParams(markpointnum);
  markpointnum++;
}

//
// AM_findMinMaxBoundaries()
//
// Determines bounding box of all vertices,
// sets global variables controlling zoom range.
//
// Passed nothing, returns nothing
//
void AM_findMinMaxBoundaries() {
  min_x = min_y = std::numeric_limits<int>::max();
  max_x = max_y = -std::numeric_limits<int>::max();

  for (int i = 0; i < numvertexes; i++) {
    if (vertexes[i].x < min_x) {
      min_x = vertexes[i].x;
    } else if (vertexes[i].x > max_x) {
      max_x = vertexes[i].x;
    }

    if (vertexes[i].y < min_y) {
      min_y = vertexes[i].y;
    } else if (vertexes[i].y > max_y) {
      max_y = vertexes[i].y;
    }
  }

  max_w = (max_x >>= FRACTOMAPBITS) - (min_x >>= FRACTOMAPBITS);  // e6y
  max_h = (max_y >>= FRACTOMAPBITS) - (min_y >>= FRACTOMAPBITS);  // e6y

  const fixed_t a = FixedDiv(f_w << FRACBITS, max_w);
  const fixed_t b = FixedDiv(f_h << FRACBITS, max_h);

  min_scale_mtof = std::min(a, b);
  max_scale_mtof = FixedDiv(f_h << FRACBITS, 2 * PLAYERRADIUS);
}

//
// AM_changeWindowLoc()
//
// Moves the map window by the global variables m_paninc.x, m_paninc.y
//
// Passed nothing, returns nothing
//
void AM_changeWindowLoc() {
  if (m_paninc.x != 0 || m_paninc.y != 0) {
    automapmode = static_cast<automapmode_e>(automapmode & ~am_follow);
  }

  fixed_t incx, incy;
  if (movement_smooth != 0) {
    incx = FixedMul(m_paninc.x, tic_vars.frac);
    incy = FixedMul(m_paninc.y, tic_vars.frac);
  } else {
    incx = m_paninc.x;
    incy = m_paninc.y;
  }

  if ((automapmode & am_rotate) != 0) {
    AM_rotate(&incx, &incy, viewangle - ANG90);
  }

  m_x = prev_m_x + incx;
  m_y = prev_m_y + incy;

  if ((automapmode & am_rotate) == 0) {
    if (m_x + m_w / 2 > max_x) {
      m_x = max_x - m_w / 2;
    } else if (m_x + m_w / 2 < min_x) {
      m_x = min_x - m_w / 2;
    }

    if (m_y + m_h / 2 > max_y) {
      m_y = max_y - m_h / 2;
    } else if (m_y + m_h / 2 < min_y) {
      m_y = min_y - m_h / 2;
    }
  }

  m_x2 = m_x + m_w;
  m_y2 = m_y + m_h;
}
}  // namespace

//
// AM_SetScale
//
void AM_SetScale() {
  AM_findMinMaxBoundaries();
  scale_mtof = FixedDiv(min_scale_mtof, static_cast<int>(0.7 * FRACUNIT));
  if (scale_mtof > max_scale_mtof) {
    scale_mtof = min_scale_mtof;
  }
  scale_ftom = FixedDiv(FRACUNIT, scale_mtof);
}

//
// AM_SetPosition
//
void AM_SetPosition() {
  if ((automapmode & am_overlay) != 0) {
    f_x = map_overlay_pos_x * SCREENWIDTH / 320;
    f_y = map_overlay_pos_y * SCREENHEIGHT / 200;
    f_w = map_overlay_pos_width * SCREENWIDTH / 320;
    f_h = map_overlay_pos_height * SCREENHEIGHT / 200;

    if (f_x + f_w > SCREENWIDTH) {
      f_w = SCREENWIDTH - f_x;
    }
    if (f_y + f_h > SCREENHEIGHT) {
      f_h = SCREENHEIGHT - f_y;
    }

    f_x = viewwindowx + f_x * viewwidth / SCREENWIDTH;
    f_y = viewwindowy + f_y * viewheight / SCREENHEIGHT;
    f_w = f_w * viewwidth / SCREENWIDTH;
    f_h = f_h * viewheight / SCREENHEIGHT;
  } else {
    // default
    f_x = f_y = 0;
    f_w = SCREENWIDTH;                      // killough 2/7/98: get rid of finit_ vars
    f_h = SCREENHEIGHT - ST_SCALED_HEIGHT;  // to allow runtime setting of width/height
  }
}

namespace {
//
// AM_initVariables()
//
// Initialize the variables for the automap
//
// Affects the automap global variables
// Status bar is notified that the automap has been entered
// Passed nothing, returns nothing
//
void AM_initVariables() {
  static auto st_notify = event_t{.type = ev_keyup, .data1 = AM_MSGENTERED, .data2 = 0, .data3 = 0};

  automapmode = static_cast<automapmode_e>(automapmode | am_active);

  m_paninc.x = m_paninc.y = 0;
  ftom_zoommul = FRACUNIT;
  mtof_zoommul = FRACUNIT;

  m_w = FTOM(f_w);
  m_h = FTOM(f_h);

  // find player to center on initially
  int pnum = consoleplayer;
  if (!playeringame[pnum])
    for (pnum = 0; pnum < MAXPLAYERS; pnum++) {
      if (playeringame[pnum]) {
        break;
      }
    }

  plr = &players[pnum];
  m_x = (plr->mo->x >> FRACTOMAPBITS) - m_w / 2;  // e6y
  m_y = (plr->mo->y >> FRACTOMAPBITS) - m_h / 2;  // e6y
  AM_Ticker();
  AM_changeWindowLoc();

  // for saving & restoring
  old_m_x = m_x;
  old_m_y = m_y;
  old_m_w = m_w;
  old_m_h = m_h;

  // inform the status bar of the change
  ST_Responder(&st_notify);
}
}  // namespace

void AM_SetResolution() {
  AM_SetPosition();
  AM_SetScale();
}

namespace {
//
// AM_loadPics()
//
void AM_loadPics() {
  // cph - mark numbers no longer needed cached
}

//
// AM_unloadPics()
//
void AM_unloadPics() {
  // cph - mark numbers no longer needed cached
}
}  // namespace

//
// AM_clearMarks()
//
// Sets the number of marks to 0, thereby clearing them from the display
//
// Affects the global variable markpointnum
// Passed nothing, returns nothing
//
void AM_clearMarks() {
  markpointnum = 0;
}

namespace {
//
// AM_LevelInit()
//
// Initialize the automap at the start of a new level
// should be called at the start of every level
//
// Passed nothing, returns nothing
// Affects automap's global variables
//
// CPhipps - get status bar height from status bar code
void AM_LevelInit() {
  leveljuststarted = 0;

  AM_SetPosition();
  AM_SetScale();
}
}  // namespace

//
// AM_Stop()
//
// Cease automap operations, unload patches, notify status bar
//
// Passed nothing, returns nothing
//
void AM_Stop() {
  static auto st_notify = event_t{.type = evtype_t{}, .data1 = ev_keyup, .data2 = AM_MSGEXITED, .data3 = 0};

  AM_unloadPics();
  automapmode = static_cast<automapmode_e>(automapmode & ~am_active);
  ST_Responder(&st_notify);
  stopped = true;
}

//
// AM_Start()
//
// Start up automap operations,
//  if a new level, or game start, (re)initialize level variables
//  init map variables
//  load mark patches
//
// Passed nothing, returns nothing
//
void AM_Start() {
  static int lastlevel = -1;
  static int lastepisode = -1;

  if (!stopped) {
    AM_Stop();
  }

  stopped = false;

  if (lastlevel != gamemap || lastepisode != gameepisode) {
    AM_LevelInit();
    lastlevel = gamemap;
    lastepisode = gameepisode;
  }

  AM_SetPosition();
  AM_initVariables();
  AM_loadPics();
}

namespace {
//
// AM_minOutWindowScale()
//
// Set the window scale to the maximum size
//
// Passed nothing, returns nothing
//
void AM_minOutWindowScale() {
  scale_mtof = min_scale_mtof;
  scale_ftom = FixedDiv(FRACUNIT, scale_mtof);
  AM_activateNewScale();
}

//
// AM_maxOutWindowScale(void)
//
// Set the window scale to the minimum size
//
// Passed nothing, returns nothing
//
void AM_maxOutWindowScale() {
  scale_mtof = max_scale_mtof;
  scale_ftom = FixedDiv(FRACUNIT, scale_mtof);
  AM_activateNewScale();
}
}  // namespace

//
// AM_Responder()
//
// Handle events (user inputs) in automap mode
//
// Passed an input event, returns true if its handled
//
auto AM_Responder(event_t* const ev) -> dboolean {
  static bool bigstate = false;

  bool rc = false;

  if ((automapmode & am_active) == 0) {
    if (ev->type == ev_keydown && ev->data1 == key_map)  // phares
    {
      AM_Start();
      rc = true;
    }
  } else if (ev->type == ev_keydown) {
    rc = true;
    const int ch = ev->data1;                // phares
    if (ch == key_map_right) {               //    |
      if ((automapmode & am_follow) == 0) {  //    V
        m_paninc.x = FTOM(F_PANINC());
      } else {
        rc = false;
      }
    } else if (ch == key_map_left) {
      if ((automapmode & am_follow) == 0) {
        m_paninc.x = -FTOM(F_PANINC());
      } else {
        rc = false;
      }
    } else if (ch == key_map_up) {
      if ((automapmode & am_follow) == 0) {
        m_paninc.y = FTOM(F_PANINC());
      } else {
        rc = false;
      }
    } else if (ch == key_map_down) {
      if ((automapmode & am_follow) == 0) {
        m_paninc.y = -FTOM(F_PANINC());
      } else {
        rc = false;
      }
    } else if ((ch == key_map_zoomout) || (map_wheel_zoom != 0 && ch == KEYD_MWHEELDOWN)) {
      mtof_zoommul = M_ZOOMOUT();
      ftom_zoommul = M_ZOOMIN();
      curr_mtof_zoommul = mtof_zoommul;
    } else if ((ch == key_map_zoomin) || (map_wheel_zoom != 0 && ch == KEYD_MWHEELUP)) {
      mtof_zoommul = M_ZOOMIN();
      ftom_zoommul = M_ZOOMOUT();
      curr_mtof_zoommul = mtof_zoommul;
    } else if (ch == key_map) {
      bigstate = 0;
      AM_Stop();
    } else if (ch == key_map_gobig) {
      bigstate = !bigstate;
      if (bigstate) {
        AM_saveScaleAndLoc();
        AM_minOutWindowScale();
      } else {
        AM_restoreScaleAndLoc();
      }
    } else if (ch == key_map_follow) {
      automapmode =
          static_cast<automapmode_e>(automapmode ^ am_follow);  // CPhipps - put all automap mode stuff into one enum
      // Ty 03/27/98 - externalized
      plr->message = (automapmode & am_follow) != 0 ? s_AMSTR_FOLLOWON : s_AMSTR_FOLLOWOFF;
    } else if (ch == key_map_grid) {
      automapmode = static_cast<automapmode_e>(automapmode ^ am_grid);  // CPhipps
      // Ty 03/27/98 - *not* externalized
      plr->message = (automapmode & am_grid) != 0 ? s_AMSTR_GRIDON : s_AMSTR_GRIDOFF;
    } else if (ch == key_map_mark) {
      /* Ty 03/27/98 - *not* externalized
       * cph 2001/11/20 - use doom_printf so we don't have our own buffer */
      doom_printf("%s %d", s_AMSTR_MARKEDSPOT, markpointnum);
      AM_addMark();
    } else if (ch == key_map_clear) {
      AM_clearMarks();                      // Ty 03/27/98 - *not* externalized
      plr->message = s_AMSTR_MARKSCLEARED;  //    ^
    }                                       //    |
    else if (ch == key_map_rotate) {
      automapmode = static_cast<automapmode_e>(automapmode ^ am_rotate);
      plr->message = (automapmode & am_rotate) != 0 ? s_AMSTR_ROTATEON : s_AMSTR_ROTATEOFF;
    } else if (ch == key_map_overlay) {
      automapmode = static_cast<automapmode_e>(automapmode ^ am_overlay);
      AM_SetPosition();
      AM_activateNewScale();
      plr->message = (automapmode & am_overlay) != 0 ? s_AMSTR_OVERLAYON : s_AMSTR_OVERLAYOFF;
    }
#ifdef GL_DOOM
    else if (ch == key_map_textured) {
      map_textured = !map_textured;
      M_ChangeMapTextured();
      plr->message = (map_textured != 0 ? s_AMSTR_TEXTUREDON : s_AMSTR_TEXTUREDOFF);
    }
#endif
    else  // phares
    {
      rc = false;
    }
  } else if (ev->type == ev_keyup) {
    rc = false;
    const int ch = ev->data1;
    if (ch == key_map_right) {
      if ((automapmode & am_follow) == 0) {
        m_paninc.x = 0;
      }
    } else if (ch == key_map_left) {
      if ((automapmode & am_follow) == 0) {
        m_paninc.x = 0;
      }
    } else if (ch == key_map_up) {
      if ((automapmode & am_follow) == 0) {
        m_paninc.y = 0;
      }
    } else if (ch == key_map_down) {
      if ((automapmode & am_follow) == 0) {
        m_paninc.y = 0;
      }
    } else if ((ch == key_map_zoomout) || (ch == key_map_zoomin)
               || (map_wheel_zoom && ((ch == KEYD_MWHEELDOWN) || (ch == KEYD_MWHEELUP)))) {
      mtof_zoommul = FRACUNIT;
      ftom_zoommul = FRACUNIT;
    }
  }

  return static_cast<dboolean>(rc);
}

namespace {
//
// AM_rotate()
//
// Rotation in 2D.
// Used to rotate player arrow line character.
//
// Passed the coordinates of a point, and an angle
// Returns the coordinates rotated by the angle
//
// CPhipps - made static & enhanced for automap rotation
void AM_rotate(fixed_t& x, fixed_t& y, angle_t a) {
  const fixed_t tmpx = FixedMul(x, finecosine[a >> ANGLETOFINESHIFT]) - FixedMul(y, finesine[a >> ANGLETOFINESHIFT]);

  y = FixedMul(x, finesine[a >> ANGLETOFINESHIFT]) + FixedMul(y, finecosine[a >> ANGLETOFINESHIFT]);

  x = tmpx;
}

[[deprecated]] void AM_rotate(fixed_t* x, fixed_t* y, angle_t a) {
  AM_rotate(*x, *y, a);
}
}  // namespace

namespace {
void AM_rotatePoint(mpoint_t& p) {
  if (am_frame.precise != 0) {
    p.fx = static_cast<float>(p.x) - am_frame.centerx_f;
    p.fy = static_cast<float>(p.y) - am_frame.centery_f;

    const float f = (p.fx * am_frame.cos_f) - (p.fy * am_frame.sin_f) + am_frame.centerx_f;
    p.fy = (p.fx * am_frame.sin_f) + (p.fy * am_frame.cos_f) + am_frame.centery_f;
    p.fx = f;
  }

  p.x -= am_frame.centerx;
  p.y -= am_frame.centery;

  const fixed_t tmpx = FixedMul(p.x, am_frame.cos) - FixedMul(p.y, am_frame.sin) + am_frame.centerx;
  p.y = FixedMul(p.x, am_frame.sin) + FixedMul(p.y, am_frame.cos) + am_frame.centery;
  p.x = tmpx;
}
}  // namespace

[[deprecated]] void AM_rotatePoint(mpoint_t* const p) {
  AM_rotatePoint(*p);
}

namespace {
//
// AM_changeWindowScale()
//
// Automap zooming
//
// Passed nothing, returns nothing
//
void AM_changeWindowScale() {
  if (movement_smooth != 0) {
    float f_paninc = static_cast<float>(F_PANINC()) / static_cast<float>(FRACUNIT) * static_cast<float>(tic_vars.frac);

    f_paninc = std::max(f_paninc, 0.01F);

    scale_mtof = prev_scale_mtof;
    if (curr_mtof_zoommul == M_ZOOMIN()) {
      mtof_zoommul = static_cast<int>(static_cast<float>(FRACUNIT) * (1.00F + f_paninc / 200.0F));
      ftom_zoommul = static_cast<int>(static_cast<float>(FRACUNIT) / (1.00F + f_paninc / 200.0F));
    }
    if (curr_mtof_zoommul == M_ZOOMOUT()) {
      mtof_zoommul = static_cast<int>(static_cast<float>(FRACUNIT) / (1.00F + f_paninc / 200.0F));
      ftom_zoommul = static_cast<int>(static_cast<float>(FRACUNIT) * (1.00F + f_paninc / 200.0F));
    }
  }

  scale_mtof = FixedMul(scale_mtof, mtof_zoommul);
  scale_ftom = FixedDiv(FRACUNIT, scale_mtof);

  if (scale_mtof < min_scale_mtof) {
    AM_minOutWindowScale();
  } else if (scale_mtof > max_scale_mtof) {
    AM_maxOutWindowScale();
  } else {
    AM_activateNewScale();
  }
}

//
// AM_doFollowPlayer()
//
// Turn on follow mode - the map scrolls opposite to player motion
//
// Passed nothing, returns nothing
//
void AM_doFollowPlayer() {
  m_x = (viewx >> FRACTOMAPBITS) - m_w / 2;
  m_y = (viewy >> FRACTOMAPBITS) - m_h / 2;
  m_x2 = m_x + m_w;
  m_y2 = m_y + m_h;
}
}  // namespace

//
// AM_Ticker()
//
// Updates on gametic - enter follow mode, zoom, or change map location
//
// Passed nothing, returns nothing
//
void AM_Ticker() {
  prev_scale_mtof = scale_mtof;
  prev_m_x = m_x;
  prev_m_y = m_y;
}

namespace {
//
// AM_clipMline()
//
// Automap clipping of lines.
//
// Based on Cohen-Sutherland clipping algorithm but with a slightly
// faster reject and precalculated slopes. If the speed is needed,
// use a hash algorithm to handle the common cases.
//
// Passed the line's coordinates on map and in the frame buffer performs
// clipping on them in the lines frame coordinates.
// Returns true if any part of line was not clipped
//
auto AM_clipMline(mline_t& ml, fline_t& fl) -> bool {
  enum { LEFT = 1, RIGHT = 2, BOTTOM = 4, TOP = 8 };

  int outcode1 = 0;
  int outcode2 = 0;

  constexpr auto DOOUTCODE = [](int& oc, int mx, int my) {
    oc = 0;
    if (my < f_y) {
      oc |= TOP;
    } else if (my >= f_y + f_h) {
      oc |= BOTTOM;
    }

    if (mx < f_x) {
      oc |= LEFT;
    } else if (mx >= f_x + f_w) {
      oc |= RIGHT;
    }
  };

  // do trivial rejects and outcodes
  if (ml.a.y > m_y2) {
    outcode1 = TOP;
  } else if (ml.a.y < m_y) {
    outcode1 = BOTTOM;
  }

  if (ml.b.y > m_y2) {
    outcode2 = TOP;
  } else if (ml.b.y < m_y) {
    outcode2 = BOTTOM;
  }

  if ((outcode1 & outcode2) != 0) {
    return false;  // trivially outside
  }

  if (ml.a.x < m_x) {
    outcode1 |= LEFT;
  } else if (ml.a.x > m_x2) {
    outcode1 |= RIGHT;
  }

  if (ml.b.x < m_x) {
    outcode2 |= LEFT;
  } else if (ml.b.x > m_x2) {
    outcode2 |= RIGHT;
  }

  if ((outcode1 & outcode2) != 0) {
    return false;  // trivially outside
  }

  // transform to frame-buffer coordinates.
  fl.a.x = CXMTOF(ml.a.x);
  fl.a.y = CYMTOF(ml.a.y);
  fl.b.x = CXMTOF(ml.b.x);
  fl.b.y = CYMTOF(ml.b.y);

  DOOUTCODE(outcode1, fl.a.x, fl.a.y);
  DOOUTCODE(outcode2, fl.b.x, fl.b.y);

  if ((outcode1 & outcode2) != 0) {
    return false;
  }

  if (am_frame.precise != 0) {
    fl.a.fx = CXMTOF_F(ml.a.fx);
    fl.a.fy = CYMTOF_F(ml.a.fy);
    fl.b.fx = CXMTOF_F(ml.b.fx);
    fl.b.fy = CYMTOF_F(ml.b.fy);
  }

  while ((outcode1 | outcode2) != 0) {
    // may be partially inside box
    // find an outside point
    int outside;
    if (outcode1 != 0) {
      outside = outcode1;
    } else {
      outside = outcode2;
    }

    // clip to each side
    fpoint_t tmp;
    if ((outside & TOP) != 0) {
      const int dy = fl.a.y - fl.b.y;
      const int dx = fl.b.x - fl.a.x;
      // 'int64' math to avoid overflows on long lines
      tmp.x = fl.a.x + static_cast<fixed_t>((static_cast<int_64_t>(dx) * (fl.a.y - f_y)) / dy);
      tmp.y = f_y;

      if (am_frame.precise != 0) {
        const float dy_f = fl.a.fy - fl.b.fy;
        const float dx_f = fl.b.fx - fl.a.fx;
        tmp.fx = fl.a.fx + (dx_f * (fl.a.fy - f_y)) / dy_f;
        tmp.fy = static_cast<float>(f_y);
      }
    } else if ((outside & BOTTOM) != 0) {
      const int dy = fl.a.y - fl.b.y;
      const int dx = fl.b.x - fl.a.x;
      tmp.x = fl.a.x + static_cast<fixed_t>((static_cast<int_64_t>(dx) * (fl.a.y - (f_y + f_h))) / dy);
      tmp.y = f_y + f_h - 1;

      if (am_frame.precise != 0) {
        const float dy_f = fl.a.fy - fl.b.fy;
        const float dx_f = fl.b.fx - fl.a.fx;
        tmp.fx = fl.a.fx + (dx_f * (fl.a.fy - (f_y + f_h))) / dy_f;
        tmp.fy = static_cast<float>(f_y + f_h - 1);
      }
    } else if ((outside & RIGHT) != 0) {
      const int dy = fl.b.y - fl.a.y;
      const int dx = fl.b.x - fl.a.x;
      tmp.y = fl.a.y + static_cast<fixed_t>((static_cast<int_64_t>(dy) * (f_x + f_w - 1 - fl.a.x)) / dx);
      tmp.x = f_x + f_w - 1;

      if (am_frame.precise != 0) {
        const float dy_f = fl.b.fy - fl.a.fy;
        const float dx_f = fl.b.fx - fl.a.fx;
        tmp.fy = fl.a.fy + (dy_f * (f_x + f_w - 1 - fl.a.fx)) / dx_f;
        tmp.fx = static_cast<float>(f_x + f_w - 1);
      }
    } else if ((outside & LEFT) != 0) {
      const int dy = fl.b.y - fl.a.y;
      const int dx = fl.b.x - fl.a.x;
      tmp.y = fl.a.y + static_cast<fixed_t>((static_cast<int_64_t>(dy) * (f_x - fl.a.x)) / dx);
      tmp.x = f_x;

      if (am_frame.precise != 0) {
        const float dy_f = fl.b.fy - fl.a.fy;
        const float dx_f = fl.b.fx - fl.a.fx;
        tmp.fy = fl.a.fy + (dy_f * (f_x - fl.a.fx)) / dx_f;
        tmp.fx = static_cast<float>(f_x);
      }
    }

    if (outside == outcode1) {
      fl.a = tmp;
      DOOUTCODE(outcode1, fl.a.x, fl.a.y);
    } else {
      fl.b = tmp;
      DOOUTCODE(outcode2, fl.b.x, fl.b.y);
    }

    if ((outcode1 & outcode2) != 0) {
      return false;  // trivially outside
    }
  }

  return true;
}

//
// AM_drawMline()
//
// Clip lines, draw visible parts of lines.
//
// Passed the map coordinates of the line, and the color to draw it
// Color -1 is special and prevents drawing. Color 247 is special and
// is translated to black, allowing Color 0 to represent feature disable
// in the defaults file.
// Returns nothing.
//
void AM_drawMline(mline_t& ml, int color) {
  static fline_t fl;

  if (color == -1) {  // jff 4/3/98 allow not drawing any sort of line by setting its color to -1
    return;
  }

  if (color == 247) {  // jff 4/3/98 if color is 247 (xparent), use black
    color = 0;
  }

  if (AM_clipMline(ml, fl)) {
    // draws it on frame buffer using fb coords
    if (map_use_multisamling != 0) {
      V_DrawLineWu(&fl, color);
    } else {
      V_DrawLine(&fl, color);
    }
  }
}

//
// AM_drawGrid()
//
// Draws blockmap aligned grid lines.
//
// Passed the color to draw the grid lines
// Returns nothing
//
void AM_drawGrid(const int color) {
  //  fixed_t x, y;
  //  fixed_t start, end;
  //  mline_t ml;
  //  fixed_t minlen, extx, exty;
  //  fixed_t minx, miny;
  fixed_t gridsize = map_grid_size << MAPBITS;

  if (map_grid_size == -1) {
    const fixed_t oprtimal_gridsize = m_h / 16;

    gridsize = 8;
    while (gridsize < oprtimal_gridsize) {
      gridsize <<= 1;
    }

    if (gridsize - oprtimal_gridsize > oprtimal_gridsize - (gridsize >> 1)) {
      gridsize >>= 1;
    }
  }

  // [RH] Calculate a minimum for how long the grid lines should be so that
  // they cover the screen at any rotation.
  const fixed_t minlen =
      M_DoubleToInt(std::sqrt(std::pow(static_cast<float>(m_w), 2.0F) + std::pow(static_cast<float>(m_h), 2.0F)));
  const fixed_t extx = (minlen - m_w) / 2;
  const fixed_t exty = (minlen - m_h) / 2;

  const fixed_t minx = m_x;
  const fixed_t miny = m_y;

  // Fix vanilla automap grid bug: losing grid lines near the map boundary
  // due to unnecessary addition of MAPBLOCKUNITS to start
  // Proper math is to just subtract the remainder; AM_drawMLine will take care
  // of clipping if an extra line is offscreen.

  // Figure out start of vertical gridlines
  fixed_t start = minx - extx;
  if ((start - (bmaporgx >> FRACTOMAPBITS)) % gridsize) {
    start -= ((start - (bmaporgx >> FRACTOMAPBITS)) % gridsize);
  }
  fixed_t end = minx + minlen - extx;

  // draw vertical gridlines
  for (fixed_t x = start; x < end; x += gridsize) {
    mline_t ml{.a = {.x = x, .y = miny - exty}, .b = {.x = x, .y = ml.a.y + minlen}};

    if ((automapmode & am_rotate) != 0) {
      AM_rotatePoint(ml.a);
      AM_rotatePoint(ml.b);
    } else {
      AM_SetMPointFloatValue(ml.a);
      AM_SetMPointFloatValue(ml.b);
    }
    AM_drawMline(ml, color);
  }

  // Figure out start of horizontal gridlines
  start = miny - exty;
  if ((start - (bmaporgy >> FRACTOMAPBITS)) % gridsize) {
    start -= ((start - (bmaporgy >> FRACTOMAPBITS)) % gridsize);
  }
  end = miny + minlen - exty;

  // draw horizontal gridlines
  for (fixed_t y = start; y < end; y += gridsize) {
    mline_t ml{.a = {.x = minx - extx, .y = y}, .b = {.x = ml.a.x + minlen, .y = y}};

    if ((automapmode & am_rotate) != 0) {
      AM_rotatePoint(ml.a);
      AM_rotatePoint(ml.b);
    } else {
      AM_SetMPointFloatValue(ml.a);
      AM_SetMPointFloatValue(ml.b);
    }
    AM_drawMline(ml, color);
  }
}

//
// AM_DoorColor()
//
// Returns the 'color' or key needed for a door linedef type
//
// Passed the type of linedef, returns:
//   -1 if not a keyed door
//    0 if a red key required
//    1 if a blue key required
//    2 if a yellow key required
//    3 if a multiple keys required
//
// jff 4/3/98 add routine to get color of generalized keyed door
//
auto AM_DoorColor(int type) -> int {
  if (GenLockedBase <= type && type < GenDoorBase) {
    type -= GenLockedBase;
    type = (type & LockedKey) >> LockedKeyShift;
    if (!type || type == 7) {
      return 3;  // any or all keys
    }

    return (type - 1) % 3;
  }

  switch (type) {  // closed keyed door
    case 26:
    case 32:
    case 99:
    case 133:
      /*bluekey*/
      return 1;
    case 27:
    case 34:
    case 136:
    case 137:
      /*yellowkey*/
      return 2;
    case 28:
    case 33:
    case 134:
    case 135:
      /*redkey*/
      return 0;
    default:
      return -1;  // not a keyed door
  }
}

//
// Determines visible lines, draws them.
// This is LineDef based, not LineSeg based.
//
// jff 1/5/98 many changes in this routine
// backward compatibility not needed, so just changes, no ifs
// addition of clauses for:
//    doors opening, keyed door id, secret sectors,
//    teleports, exit lines, key things
// ability to suppress any of added features or lines with no height changes
//
// support for gamma correction in automap abandoned
//
// jff 4/3/98 changed mapcolor_xxxx=0 as control to disable feature
// jff 4/3/98 changed mapcolor_xxxx=-1 to disable drawing line completely
//
void AM_drawWalls() {
  static mline_t l;

  // draw the unclipped visible portions of all lines
  for (int i = 0; i < numlines; i++) {
    const line_t& line = lines[i];

    if (line.bbox[BOXLEFT] >> FRACTOMAPBITS > am_frame.bbox[BOXRIGHT]
        || line.bbox[BOXRIGHT] >> FRACTOMAPBITS < am_frame.bbox[BOXLEFT]
        || line.bbox[BOXBOTTOM] >> FRACTOMAPBITS > am_frame.bbox[BOXTOP]
        || line.bbox[BOXTOP] >> FRACTOMAPBITS < am_frame.bbox[BOXBOTTOM]) {
      continue;
    }

    l.a.x = line.v1->x >> FRACTOMAPBITS;
    l.a.y = line.v1->y >> FRACTOMAPBITS;
    l.b.x = line.v2->x >> FRACTOMAPBITS;
    l.b.y = line.v2->y >> FRACTOMAPBITS;

    if ((automapmode & am_rotate) != 0) {
      AM_rotatePoint(l.a);
      AM_rotatePoint(l.b);
    } else {
      AM_SetMPointFloatValue(l.a);
      AM_SetMPointFloatValue(l.b);
    }

    // if line has been seen or IDDT has been used
    if (ddt_cheating != 0 || (line.flags & ML_MAPPED) != 0) {
      if ((line.flags & ML_DONTDRAW) != 0 && ddt_cheating == 0) {
        continue;
      }

      {
        /* cph - show keyed doors and lines */
        int amd;
        if ((mapcolor_bdor != 0 || mapcolor_ydor != 0 || mapcolor_rdor != 0)
            && (line.flags & ML_SECRET) == 0 /* non-secret */
            && (amd = AM_DoorColor(lines[i].special)) != -1) {
          {
            switch (amd) /* closed keyed door */
            {
              case 1:
                /*bluekey*/
                AM_drawMline(l, mapcolor_bdor != 0 ? mapcolor_bdor : mapcolor_cchg);
                continue;
              case 2:
                /*yellowkey*/
                AM_drawMline(l, mapcolor_ydor != 0 ? mapcolor_ydor : mapcolor_cchg);
                continue;
              case 0:
                /*redkey*/
                AM_drawMline(l, mapcolor_rdor != 0 ? mapcolor_rdor : mapcolor_cchg);
                continue;
              case 3:
                /*any or all*/
                AM_drawMline(l, mapcolor_clsd != 0 ? mapcolor_clsd : mapcolor_cchg);
                continue;
            }
          }
        }
      }

      if /* jff 4/23/98 add exit lines to automap */
          (mapcolor_exit != 0
           && (line.special == 11 || line.special == 52 || line.special == 197 || line.special == 51
               || line.special == 124 || line.special == 198)) {
        AM_drawMline(l, mapcolor_exit); /* exit line */
        continue;
      }

      if (line.backsector == nullptr) {
        // jff 1/10/98 add new color for 1S secret sector boundary
        if (mapcolor_secr != 0 &&  // jff 4/3/98 0 is disable
            ((map_secret_after != 0 && P_WasSecret(line.frontsector) && !P_IsSecret(line.frontsector))
             || (map_secret_after == 0 && P_WasSecret(line.frontsector)))) {
          AM_drawMline(l, mapcolor_secr);  // line bounding secret sector
        } else {                           // jff 2/16/98 fixed bug
          AM_drawMline(l, mapcolor_wall);  // special was cleared
        }
      } else { /* now for 2S lines */
        // jff 1/10/98 add color change for all teleporter types
        if (mapcolor_tele != 0 && (line.flags & ML_SECRET) == 0
            && (line.special == 39 || line.special == 97 || line.special == 125
                || line.special == 126)) {  // teleporters
          AM_drawMline(l, mapcolor_tele);
        } else if (line.flags & ML_SECRET) {                               // secret door
          AM_drawMline(l, mapcolor_wall);                                  // wall color
        } else if (mapcolor_clsd != 0 && (line.flags & ML_SECRET) == 0 &&  // non-secret closed door
                   ((line.backsector->floorheight == line.backsector->ceilingheight)
                    || (line.frontsector->floorheight == line.frontsector->ceilingheight))) {
          AM_drawMline(l, mapcolor_clsd);  // non-secret closed door
        }                                  // jff 1/6/98 show secret sector 2S lines
        else if (mapcolor_secr != 0 &&     // jff 2/16/98 fixed bug
                 (                         // special was cleared after getting it
                     (map_secret_after != 0
                      && ((P_WasSecret(line.frontsector) && !P_IsSecret(line.frontsector))
                          || (P_WasSecret(line.backsector) && !P_IsSecret(line.backsector))))
                     ||  // jff 3/9/98 add logic to not show secret til after entered
                     (   // if map_secret_after is true
                         map_secret_after == 0 && (P_WasSecret(line.frontsector) || P_WasSecret(line.backsector))))) {
          AM_drawMline(l, mapcolor_secr);  // line bounding secret sector
        }                                  // jff 1/6/98 end secret sector line change
        else if (line.backsector->floorheight != line.frontsector->floorheight) {
          AM_drawMline(l, mapcolor_fchg);  // floor level change
        } else if (line.backsector->ceilingheight != line.frontsector->ceilingheight) {
          AM_drawMline(l, mapcolor_cchg);  // ceiling level change
        } else if (mapcolor_flat != 0 && ddt_cheating != 0) {
          AM_drawMline(l, mapcolor_flat);  // 2S lines that appear only in IDDT
        }
      }
    } else if (plr->powers[pw_allmap] != 0) {  // now draw the lines only visible because the player has computermap
                                               // computermap visible lines
      if ((line.flags & ML_DONTDRAW) == 0) {   // invisible flag lines do not show
        if (mapcolor_flat != 0 || lines[i].backsector == nullptr
            || line.backsector->floorheight != line.frontsector->floorheight
            || line.backsector->ceilingheight != line.frontsector->ceilingheight) {
          AM_drawMline(l, mapcolor_unsn);
        }
      }
    }
  }
}

//
// AM_drawLineCharacter()
//
// Draws a vector graphic according to numerous parameters
//
// Passed the structure defining the vector graphic shape, the number
// of vectors in it, the scale to draw it at, the angle to draw it at,
// the color to draw it with, and the map coordinates to draw it at.
// Returns nothing
//
void AM_drawLineCharacter(std::span<mline_t> lineguys,
                          const fixed_t scale,
                          angle_t angle,
                          const int color,
                          const fixed_t x,
                          const fixed_t y) {
  if ((automapmode & am_rotate) != 0) {
    angle -= viewangle - ANG90;  // cph
  }

  for (const auto& lineguy : lineguys) {
    mline_t l{};
    l.a.x = lineguy.a.x;
    l.a.y = lineguy.a.y;

    if (scale != 0) {
      l.a.x = FixedMul(scale, l.a.x);
      l.a.y = FixedMul(scale, l.a.y);
    }

    if (angle != 0) {
      AM_rotate(l.a.x, l.a.y, angle);
    }

    l.a.x += x;
    l.a.y += y;

    l.b.x = lineguy.b.x;
    l.b.y = lineguy.b.y;

    if (scale != 0) {
      l.b.x = FixedMul(scale, l.b.x);
      l.b.y = FixedMul(scale, l.b.y);
    }

    if (angle != 0) {
      AM_rotate(l.b.x, l.b.y, angle);
    }

    l.b.x += x;
    l.b.y += y;

    l.a.fx = static_cast<float>(l.a.x);
    l.a.fy = static_cast<float>(l.a.y);
    l.b.fx = static_cast<float>(l.b.x);
    l.b.fy = static_cast<float>(l.b.y);

    AM_drawMline(l, color);
  }
}

INLINE void AM_GetMobjPosition(const mobj_t& mo, mpoint_t& p, angle_t& angle) {
  if (!paused && movement_smooth != 0) {
    p.x = mo.PrevX + FixedMul(tic_vars.frac, mo.x - mo.PrevX);
    p.y = mo.PrevY + FixedMul(tic_vars.frac, mo.y - mo.PrevY);
    if (mo.player != nullptr) {
      angle = mo.player->prev_viewangle
              + FixedMul(tic_vars.frac, R_SmoothPlaying_Get(mo.player) - mo.player->prev_viewangle);
    } else {
      angle = mo.angle;
    }
  } else {
    p.x = mo.x;
    p.y = mo.y;
    angle = mo.angle;
  }

  p.x = p.x >> FRACTOMAPBITS;
  p.y = p.y >> FRACTOMAPBITS;
}

//
// AM_drawPlayers()
//
// Draws the player arrow in single player,
// or all the player arrows in a netgame.
//
// Passed nothing, returns nothing
//
void AM_drawPlayers() {
  //  int i;
  //  angle_t angle;
  //  mpoint_t pt;
  //  fixed_t scale;

#if defined(HAVE_LIBSDL2_IMAGE) && defined(GL_DOOM)
  if (V_GetMode() == VID_MODEGL) {
    if (map_things_appearance == map_things_appearance_icon) {
      return;
    }
  }
#endif

  fixed_t scale;
  if (map_things_appearance == map_things_appearance_scaled) {
    scale = std::clamp(4 << FRACBITS, 256 << FRACBITS, plr->mo->radius) >> FRACTOMAPBITS;
  } else {
    scale = 16 << MAPBITS;
  }

  mpoint_t pt;
  if (!netgame) {
    pt.x = viewx >> FRACTOMAPBITS;
    pt.y = viewy >> FRACTOMAPBITS;
    if ((automapmode & am_rotate) != 0) {
      AM_rotatePoint(pt);
    } else {
      AM_SetMPointFloatValue(pt);
    }

    if (ddt_cheating != 0) {
      AM_drawLineCharacter(cheat_player_arrow, scale, viewangle, mapcolor_sngl, pt.x, pt.y);
    } else {
      AM_drawLineCharacter(player_arrow, scale, viewangle, mapcolor_sngl, pt.x, pt.y);
    }

    return;
  }

  for (int i = 0; i < MAXPLAYERS; i++) {
    const player_t& p = players[i];

    if ((deathmatch && !demoplayback) && &p != plr) {
      continue;
    }

    if (playeringame[i]) {
      angle_t angle;
      AM_GetMobjPosition(*p.mo, pt, angle);

      if ((automapmode & am_rotate) != 0) {
        AM_rotatePoint(pt);
      } else {
        AM_SetMPointFloatValue(pt);
      }

      AM_drawLineCharacter(player_arrow, scale, angle,
                           p.powers[pw_invisibility] ? 246                /* *close* to black */
                                                     : mapcolor_plyr[i],  // jff 1/6/98 use default color
                           pt.x, pt.y);
    }
  }
}

void AM_ProcessNiceThing(mobj_t& mobj, const angle_t angle, const fixed_t x, const fixed_t y) {
#ifdef GL_DOOM
  constexpr float shadow_scale_factor = 1.3F;

  struct map_nice_icon_param_t {
    spritenum_t sprite;
    int icon;
    int radius;
    int rotate;
    unsigned char r, g, b;
  };

  static constexpr std::array<map_nice_icon_param_t, 44> icons{{{SPR_STIM, am_icon_health, 12, 0, 100, 100, 200},
                                                                {SPR_MEDI, am_icon_health, 16, 0, 100, 100, 200},
                                                                {SPR_BON1, am_icon_health, 10, 0, 0, 0, 200},

                                                                {SPR_BON2, am_icon_armor, 10, 0, 0, 200, 0},
                                                                {SPR_ARM1, am_icon_armor, 16, 0, 100, 200, 100},
                                                                {SPR_ARM2, am_icon_armor, 16, 0, 100, 100, 200},

                                                                {SPR_CLIP, am_icon_ammo, 10, 0, 180, 150, 50},
                                                                {SPR_AMMO, am_icon_ammo, 16, 0, 180, 150, 50},
                                                                {SPR_ROCK, am_icon_ammo, 10, 0, 180, 150, 50},
                                                                {SPR_BROK, am_icon_ammo, 16, 0, 180, 150, 50},

                                                                {SPR_CELL, am_icon_ammo, 10, 0, 180, 150, 50},
                                                                {SPR_CELP, am_icon_ammo, 16, 0, 180, 150, 50},
                                                                {SPR_SHEL, am_icon_ammo, 10, 0, 180, 150, 50},
                                                                {SPR_SBOX, am_icon_ammo, 16, 0, 180, 150, 50},
                                                                {SPR_BPAK, am_icon_ammo, 16, 0, 180, 150, 50},

                                                                {SPR_BKEY, am_icon_key, 10, 0, 0, 0, 255},
                                                                {SPR_BSKU, am_icon_key, 10, 0, 0, 0, 255},
                                                                {SPR_YKEY, am_icon_key, 10, 0, 255, 255, 0},
                                                                {SPR_YSKU, am_icon_key, 10, 0, 255, 255, 0},
                                                                {SPR_RKEY, am_icon_key, 10, 0, 255, 0, 0},
                                                                {SPR_RSKU, am_icon_key, 10, 0, 255, 0, 0},

                                                                {SPR_PINV, am_icon_power, 16, 0, 220, 100, 220},
                                                                {SPR_PSTR, am_icon_power, 16, 0, 220, 100, 220},
                                                                {SPR_PINS, am_icon_power, 16, 0, 220, 100, 220},
                                                                {SPR_SUIT, am_icon_power, 16, 0, 220, 100, 220},
                                                                {SPR_PMAP, am_icon_power, 16, 0, 220, 100, 220},
                                                                {SPR_PVIS, am_icon_power, 16, 0, 220, 100, 220},
                                                                {SPR_SOUL, am_icon_power, 16, 0, 220, 100, 220},
                                                                {SPR_MEGA, am_icon_power, 16, 0, 220, 100, 220},

                                                                {SPR_BFUG, am_icon_weap, 20, 0, 220, 180, 100},
                                                                {SPR_MGUN, am_icon_weap, 20, 0, 220, 180, 100},
                                                                {SPR_CSAW, am_icon_weap, 20, 0, 220, 180, 100},
                                                                {SPR_LAUN, am_icon_weap, 20, 0, 220, 180, 100},
                                                                {SPR_PLAS, am_icon_weap, 20, 0, 220, 180, 100},
                                                                {SPR_SHOT, am_icon_weap, 20, 0, 220, 180, 100},
                                                                {SPR_SGN2, am_icon_weap, 20, 0, 220, 180, 100},

                                                                {SPR_BLUD, am_icon_bullet, 8, 0, 255, 0, 0},
                                                                {SPR_PUFF, am_icon_bullet, 8, 0, 255, 255, 115},
                                                                {SPR_MISL, am_icon_bullet, 8, 0, 91, 71, 43},
                                                                {SPR_PLSS, am_icon_bullet, 8, 0, 115, 115, 255},
                                                                {SPR_PLSE, am_icon_bullet, 8, 0, 115, 115, 255},
                                                                {SPR_BFS1, am_icon_bullet, 12, 0, 119, 255, 111},
                                                                {SPR_BFE1, am_icon_bullet, 12, 0, 119, 255, 111},

                                                                {NUMSPRITES, {}, {}, {}, {}, {}, {}}}};

  bool need_shadow = true;

  int type = am_icon_normal;
  unsigned char r = 220;
  unsigned char g = 180;
  unsigned char b = 100;
  unsigned char a = 255;
  int radius = mobj.radius;
  bool rotate = true;

  if (mobj.player != nullptr) {
    const player_t& p = *mobj.player;
    const int color = mapcolor_plyr[&p - players];
    const unsigned char* playpal = V_GetPlaypal();

    if ((deathmatch && !demoplayback) && &p != plr)
      return;

    type = am_icon_player;

    r = playpal[3 * color + 0];
    g = playpal[3 * color + 1];
    b = playpal[3 * color + 2];
    a = p.powers[pw_invisibility] ? 128 : 255;

    radius = mobj.radius;
    rotate = true;
  } else if ((mobj.flags & MF_COUNTKILL) != 0) {
    if ((mobj.flags & MF_CORPSE) != 0) {
      need_shadow = false;
      type = am_icon_corpse;
      r = 120;
      a = 128;
    } else {
      type = am_icon_monster;
      r = 200;
    }
    g = 0;
    b = 0;
    radius = std::clamp(4 << FRACBITS, 256 << FRACBITS, mobj.radius);
    rotate = true;
  } else {
    for (std::size_t i = 0; icons[i].sprite < NUMSPRITES; i++) {
      if (mobj.sprite == icons[i].sprite) {
        type = icons[i].icon;
        r = icons[i].r;
        g = icons[i].g;
        b = icons[i].b;
        radius = icons[i].radius << 16;
        rotate = icons[i].rotate;

        break;
      }
    }
  }

  const float fradius = MTOF_F(radius >> FRACTOMAPBITS);
  if (fradius < 1.0F) {
    return;
  }

  if (fradius < 4.0F) {
    need_shadow = false;
  }

  const float fx = CXMTOF_F(x);
  const float fy = CYMTOF_F(y);

  const float shadow_radius = fradius * shadow_scale_factor;
  if (fx + shadow_radius < 0 || fx - shadow_radius > static_cast<float>(SCREENWIDTH) || fy + shadow_radius < 0
      || fy - shadow_radius > static_cast<float>(SCREENHEIGHT)) {
    return;
  }

  const angle_t ang = (rotate ? angle : 0) + ((automapmode & am_rotate) != 0 ? ANG90 - viewangle : 0);
  const float rot = -static_cast<float>(ang) / static_cast<float>(1u << 31) * static_cast<float>(M_PI);

  gld_AddNiceThing(type, fx, fy, fradius, rot, r, g, b, a);
  if (need_shadow) {
    gld_AddNiceThing(am_icon_shadow, fx, fy, shadow_radius, rot, 0, 0, 0, 128);
  }
#endif
}

void AM_DrawNiceThings() {
#ifdef GL_DOOM
  gld_ClearNiceThings();

  // draw players
  for (int i = 0; i < MAXPLAYERS; i++) {
    const player_t& player = players[i];

    if ((deathmatch && !demoplayback) && &player != plr) {
      continue;
    }

    if (playeringame[i]) {
      mobj_t& t = *players[i].mo;
      mpoint_t p;
      angle_t angle;

      AM_GetMobjPosition(t, p, angle);

      if ((automapmode & am_rotate) != 0) {
        AM_rotatePoint(p);
      } else {
        AM_SetMPointFloatValue(p);
      }

      AM_ProcessNiceThing(t, angle, p.x, p.y);
    }
  }

  // walls
  if (ddt_cheating == 2) {
    // for all sectors
    for (int i = 0; i < numsectors; i++) {
      if (!(players[displayplayer].cheats & CF_NOCLIP)
          && (sectors[i].bbox[BOXLEFT] > am_frame.bbox[BOXRIGHT] || sectors[i].bbox[BOXRIGHT] < am_frame.bbox[BOXLEFT]
              || sectors[i].bbox[BOXBOTTOM] > am_frame.bbox[BOXTOP]
              || sectors[i].bbox[BOXTOP] < am_frame.bbox[BOXBOTTOM])) {
        continue;
      }

      mobj_t* t = sectors[i].thinglist;
      while (t != nullptr) {  // for all things in that sector
        if (!t->player) {
          mpoint_t p;
          angle_t angle;

          AM_GetMobjPosition(*t, p, angle);

          if ((automapmode & am_rotate) != 0) {
            AM_rotatePoint(p);
          }

          AM_ProcessNiceThing(*t, angle, p.x, p.y);
        }

        t = t->snext;
      }
    }
  }

  // marked locations on the automap
  {
    int anim_flash_level = (gametic % 32);

    // Flashing animation for hilight
    // Pulsates between 0.5-1.0F (multiplied with hilight alpha)
    if (anim_flash_level >= 16) {
      anim_flash_level = 32 - anim_flash_level;
    }
    anim_flash_level = 127 + anim_flash_level * 8;

    // do not want to have too small marks
    float radius = MTOF_F(16 << MAPBITS);
    radius = std::clamp(8.0F, 128.0F, radius);

    for (const auto& markpoint : std::span{
             markpoints, static_cast<std::size_t>(markpointnum)}) {  // killough 2/22/98: remove automap mark limit
      if (markpoint.x != -1) {
        mpoint_t p{.x = markpoint.x, .y = markpoint.y};

        if ((automapmode & am_rotate) != 0) {
          AM_rotatePoint(p);
        } else {
          AM_SetMPointFloatValue(p);
        }

        p.fx = CXMTOF_F(p.fx);
        p.fy = CYMTOF_F(p.fy);

        gld_AddNiceThing(am_icon_mark, p.fx, p.fy, radius, 0, 255, 255, 0,
                         static_cast<unsigned char>(anim_flash_level));
      }
    }
  }

#endif
}

//
// AM_drawThings()
//
// Draws the things on the automap in double IDDT cheat mode
//
// Passed colors and colorrange, no longer used
// Returns nothing
//
void AM_drawThings() {
#if defined(HAVE_LIBSDL2_IMAGE) && defined(GL_DOOM)
  if (V_GetMode() == VID_MODEGL) {
    if (map_things_appearance == map_things_appearance_icon) {
      AM_DrawNiceThings();
      return;
    }
  }
#endif

  if (ddt_cheating != 2) {
    return;
  }

  // for all sectors
  for (int i = 0; i < numsectors; i++) {
    const auto& sector = sectors[i];

    // e6y
    // Two-pass method for better usability of automap:
    // The first one will draw all things except enemies
    // The second one is for enemies only
    // Stop after first pass if the current sector has no enemies
    int enemies = 0;

    if ((players[displayplayer].cheats & CF_NOCLIP) == 0
        && (sector.bbox[BOXLEFT] > am_frame.bbox[BOXRIGHT] || sector.bbox[BOXRIGHT] < am_frame.bbox[BOXLEFT]
            || sector.bbox[BOXBOTTOM] > am_frame.bbox[BOXTOP] || sector.bbox[BOXTOP] < am_frame.bbox[BOXBOTTOM])) {
      continue;
    }

    for (int pass = 0; pass < 2; pass += (enemies != 0 ? 1 : 2)) {
      const mobj_t* t = sector.thinglist;
      while (t != nullptr) {  // for all things in that sector
        // e6y: stop if all enemies from current sector already has been drawn
        if (pass == 1 && enemies == 0) {
          break;
        }

        const bool count_kill_not_corpse = (t->flags & (MF_COUNTKILL | MF_CORPSE)) == MF_COUNTKILL;
        if (count_kill_not_corpse) {
          if (pass == 0) {
            enemies++;
          } else {
            enemies--;
          }
        }

        if (pass == (count_kill_not_corpse ? 0 : 1)) {
          t = t->snext;
          continue;
        }

        fixed_t scale;
        if (map_things_appearance == map_things_appearance_scaled) {
          scale = std::clamp(4 << FRACBITS, 256 << FRACBITS, t->radius) >> FRACTOMAPBITS;  // * 16 / 20;
        } else {
          scale = 16 << MAPBITS;
        }

        mpoint_t p;
        angle_t angle;
        AM_GetMobjPosition(*t, p, angle);

        if ((automapmode & am_rotate) != 0) {
          AM_rotatePoint(p);
        } else {
          AM_SetMPointFloatValue(p);
        }

        // jff 1/5/98 case over doomednum of thing being drawn
        if (mapcolor_rkey != 0 || mapcolor_ykey != 0 || mapcolor_bkey != 0) {
          int color = -1;
          switch (t->info->doomednum) {
            // jff 1/5/98 treat keys special
            case 38:
            case 13:  // jff  red key
              color = mapcolor_rkey != -1 ? mapcolor_rkey : mapcolor_sprt;
              break;
            case 39:
            case 6:  // jff yellow key
              color = mapcolor_ykey != -1 ? mapcolor_ykey : mapcolor_sprt;
              break;
            case 40:
            case 5:  // jff blue key
              color = mapcolor_bkey != -1 ? mapcolor_bkey : mapcolor_sprt;
              break;
          }

          if (color != -1) {
            AM_drawLineCharacter(cross_mark, scale, t->angle, color, p.x, p.y);
            t = t->snext;
            continue;
          }
        }

        // jff 1/5/98 end added code for keys
        // jff previously entire code
        const int color = [t] {
          if ((t->flags & MF_FRIEND) != 0 && t->player == nullptr) {
            return mapcolor_frnd;
          }

          /* cph 2006/07/30 - Show count-as-kills in red. */
          if ((t->flags & (MF_COUNTKILL | MF_CORPSE)) == MF_COUNTKILL) {
            return mapcolor_enemy;
          }

          /* bbm 2/28/03 Show countable items in yellow. */
          if ((t->flags & MF_COUNTITEM) != 0) {
            return mapcolor_item;
          }

          return mapcolor_sprt;
        }();
        AM_drawLineCharacter(thintriangle_guy, scale, angle, color, p.x, p.y);
        t = t->snext;
      }
    }
  }
}

//
// AM_drawMarks()
//
// Draw the marked locations on the automap
//
// Passed nothing, returns nothing
//
// killough 2/22/98:
// Rewrote AM_drawMarks(). Removed limit on marks.
//
void AM_drawMarks() {
  std::string namebuf = "AMMNUM0";

#if defined(HAVE_LIBSDL2_IMAGE) && defined(GL_DOOM)
  if (V_GetMode() == VID_MODEGL) {
    if (map_things_appearance == map_things_appearance_icon) {
      return;
    }
  }
#endif

  for (const auto& markpoint :
       std::span{markpoints, static_cast<std::size_t>(markpointnum)}) {  // killough 2/22/98: remove automap mark limit
    if (markpoint.x != -1) {
      mpoint_t p{
          .x = p.x = markpoint.x,  // - m_x + prev_m_x;
          .y = p.y = markpoint.y   // - m_y + prev_m_y;
      };

      if ((automapmode & am_rotate) != 0) {
        AM_rotatePoint(p);
      } else {
        AM_SetMPointFloatValue(p);
      }

      p.x = CXMTOF(p.x) - markpoint.w * SCREENWIDTH / 320 / 2;
      p.y = CYMTOF(p.y) - markpoint.h * SCREENHEIGHT / 200 / 2;
      if (am_frame.precise != 0) {
        p.fx = CXMTOF_F(p.fx) - static_cast<float>(markpoint.w) * SCREENWIDTH / 320.0F / 2.0F;
        p.fy = CYMTOF_F(p.fy) - static_cast<float>(markpoint.h) * SCREENHEIGHT / 200.0F / 2.0F;
      }

      if (V_GetMode() == VID_MODEGL ? p.y < f_y + f_h && p.y + markpoint.h * SCREENHEIGHT / 200 >= f_y
                                    : p.y < f_y + f_h && p.y >= f_y) {
        int w = 0;
        for (std::size_t k = 0; k < std::string_view{markpoint.label}.length(); k++) {
          namebuf[6] = markpoint.label[k];

          if (p.x < f_x + f_w && p.x + markpoint.widths[k] * SCREENWIDTH / 320 >= f_x) {
            float fx, fy;
            int x, y, flags;

            switch (render_stretch_hud) {
              default:
              case patch_stretch_16x10:
                fx = p.fx / patches_scalex;
                fy = p.fy * 200.0F / SCREENHEIGHT;

                x = p.x / patches_scalex;
                y = p.y * 200 / SCREENHEIGHT;

                flags = VPT_ALIGN_LEFT | VPT_STRETCH;
                break;
              case patch_stretch_4x3:
                fx = p.fx * 320.0F / WIDE_SCREENWIDTH;
                fy = p.fy * 200.0F / WIDE_SCREENHEIGHT;

                x = p.x * 320 / WIDE_SCREENWIDTH;
                y = p.y * 200 / WIDE_SCREENHEIGHT;

                flags = VPT_ALIGN_LEFT | VPT_STRETCH;
                break;
              case patch_stretch_full:
                fx = p.fx * 320.0F / SCREENWIDTH;
                fy = p.fy * 200.0F / SCREENHEIGHT;

                x = p.x * 320 / SCREENWIDTH;
                y = p.y * 200 / SCREENHEIGHT;

                flags = VPT_ALIGN_WIDE | VPT_STRETCH;
                break;
            }

            if (am_frame.precise != 0) {
              V_DrawNamePatchPrecise(fx, fy, FB, namebuf.c_str(), CR_DEFAULT, static_cast<patch_translation_e>(flags));
            } else {
              V_DrawNamePatch(x, y, FB, namebuf.c_str(), CR_DEFAULT, static_cast<patch_translation_e>(flags));
            }
          }

          w += markpoint.widths[k] + 1;
          p.x += w * SCREENWIDTH / 320;
          if (am_frame.precise != 0) {
            p.fx += static_cast<float>(w) * SCREENWIDTH / 320.0F;
          }
        }
      }
    }
  }
}

//
// AM_drawCrosshair()
//
// Draw the single point crosshair representing map center
//
// Passed the color to draw the pixel with
// Returns nothing
//
// CPhipps - made static inline, and use the general pixel plotter function

void AM_drawCrosshair(const int color) {
  fline_t line;

  line.a.x = f_x + (f_w / 2) - 1;
  line.a.y = f_y + (f_h / 2);
  line.b.x = f_x + (f_w / 2) + 1;
  line.b.y = f_y + (f_h / 2);
  AM_SetFPointFloatValue(line.a);
  AM_SetFPointFloatValue(line.b);
  V_DrawLine(&line, color);

  line.a.x = f_x + (f_w / 2);
  line.a.y = f_y + (f_h / 2) - 1;
  line.b.x = f_x + (f_w / 2);
  line.b.y = f_y + (f_h / 2) + 1;
  AM_SetFPointFloatValue(line.a);
  AM_SetFPointFloatValue(line.b);

  V_DrawLine(&line, color);
}
}  // namespace

void M_ChangeMapGridSize() {
  if (map_grid_size > 0) {
    map_grid_size = std::max(map_grid_size, 8);
  }
}

void M_ChangeMapTextured() {
#ifdef GL_DOOM
  if (V_GetMode() == VID_MODEGL) {
    gld_ProcessTexturedMap();
  }
#endif
}

void M_ChangeMapMultisamling() {
  if (map_use_multisamling != 0 && V_GetMode() != VID_MODEGL) {
    V_InitFlexTranTable();
  }
}

//=============================================================================
//
// AM_drawSubsectors
//
//=============================================================================

void AM_drawSubsectors() {
#ifdef GL_DOOM
  if (V_GetMode() == VID_MODEGL) {
    gld_MapDrawSubsectors(plr, f_x, f_y, m_x, m_y, f_w, f_h, scale_mtof);
  }
#endif
}

namespace {
void AM_setFrameVariables() {
  const auto angle = static_cast<float>(ANG90 - viewangle) / static_cast<float>(1u << 31) * static_cast<float>(M_PI);
  am_frame.sin_f = std::sin(angle);
  am_frame.cos_f = std::cos(angle);
  am_frame.sin = finesine[(ANG90 - viewangle) >> ANGLETOFINESHIFT];
  am_frame.cos = finecosine[(ANG90 - viewangle) >> ANGLETOFINESHIFT];

  am_frame.centerx = m_x + m_w / 2;
  am_frame.centery = m_y + m_h / 2;
  am_frame.centerx_f = static_cast<float>(m_x) + static_cast<float>(m_w) / 2.0F;
  am_frame.centery_f = static_cast<float>(m_y) + static_cast<float>(m_h) / 2.0F;

  if ((automapmode & am_rotate) != 0) {
    const auto dx = static_cast<float>(m_x2 - am_frame.centerx);
    const auto dy = static_cast<float>(m_y2 - am_frame.centery);
    const fixed_t r = M_DoubleToInt(std::sqrt(dx * dx + dy * dy));

    am_frame.bbox[BOXLEFT] = am_frame.centerx - r;
    am_frame.bbox[BOXRIGHT] = am_frame.centerx + r;
    am_frame.bbox[BOXBOTTOM] = am_frame.centery - r;
    am_frame.bbox[BOXTOP] = am_frame.centery + r;
  } else {
    am_frame.bbox[BOXLEFT] = m_x;
    am_frame.bbox[BOXRIGHT] = m_x2;
    am_frame.bbox[BOXBOTTOM] = m_y;
    am_frame.bbox[BOXTOP] = m_y2;
  }

  am_frame.precise = static_cast<int>(V_GetMode() == VID_MODEGL);
}
}  // namespace

//
// AM_Drawer()
//
// Draws the entire automap
//
// Passed nothing, returns nothing
//

void AM_Drawer() {
  // CPhipps - all automap modes put into one enum
  if (!(automapmode & am_active))
    return;

  if (automapmode & am_follow)
    AM_doFollowPlayer();

  // Change the zoom if necessary
  if (ftom_zoommul != FRACUNIT)
    AM_changeWindowScale();

  // Change x,y location
  if (m_paninc.x || m_paninc.y)
    AM_changeWindowLoc();

  AM_setFrameVariables();

#ifdef GL_DOOM
  if (V_GetMode() == VID_MODEGL) {
    // do not use multisampling in automap mode if map_use_multisamling 0
    gld_MultisamplingSet();
  }
#endif

  if ((automapmode & am_overlay) == 0) {  // cph - If not overlay mode, clear background for the automap
    V_FillRect(FB, f_x, f_y, f_w, f_h, static_cast<byte>(mapcolor_back));  // jff 1/5/98 background default color
  }

  if (map_textured != 0) {
    AM_drawSubsectors();
  }

  if ((automapmode & am_grid) != 0) {
    AM_drawGrid(mapcolor_grid);  // jff 1/7/98 grid default color
  }
  AM_drawWalls();
  AM_drawPlayers();
  AM_drawThings();                  // jff 1/5/98 default double IDDT sprite
  AM_drawCrosshair(mapcolor_hair);  // jff 1/7/98 default crosshair color

#if defined(GL_DOOM)
  if (V_GetMode() == VID_MODEGL) {
    gld_DrawMapLines();
    M_ArrayClear(&map_lines);

#if defined(HAVE_LIBSDL2_IMAGE)
    if (map_things_appearance == map_things_appearance_icon) {
      gld_DrawNiceThings(f_x, f_y, f_w, f_h);
    }
#endif
  }
#endif

  AM_drawMarks();
}
