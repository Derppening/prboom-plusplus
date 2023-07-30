/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2004 by
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
 *     Dehacked file support
 *     New for the TeamTNT "Boom" engine
 *
 * Author: Ty Halderman, TeamTNT
 *
 *--------------------------------------------------------------------*/

#include <cstdio>
#include <cstring>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>

// killough 5/2/98: fixed headers, removed rendunant external declarations:
#include "d_deh.h"
#include "d_think.h"
#include "doomdef.h"
#include "doomstat.h"
#include "doomtype.h"
#include "e6y.h"  //e6y
#include "g_game.h"
#include "info.h"
#include "m_argv.h"
#include "m_cheat.h"
#include "m_misc.h"
#include "p_enemy.h"
#include "p_inter.h"
#include "sounds.h"
#include "w_wad.h"

// CPhipps - modify to use logical output routine
#include "lprintf.h"

#include "m_io.h"

// e6y: for compatibility with BOOM deh parser
namespace {
auto deh_strcasecmp(const std::string_view str1, const std::string_view str2) -> int {
  if (prboom_comp[PC_BOOM_DEH_PARSER].state && compatibility_level >= boom_compatibility_compatibility
      && compatibility_level <= boom_202_compatibility) {
    return std::strcmp(str1.data(), str2.data());
  }

  return strcasecmp(str1.data(), str2.data());
}
}  // namespace

[[deprecated]] auto deh_strcasecmp(const char* str1, const char* str2) -> int {
  return deh_strcasecmp(std::string_view{str1}, std::string_view{str2});
}

auto deh_getBitsDelims() -> std::string_view {
  if (prboom_comp[PC_BOOM_DEH_PARSER].state && compatibility_level >= boom_compatibility_compatibility
      && compatibility_level <= boom_202_compatibility) {
    return "+";
  }

  return ",+| \t\f\r";
}

// If false, dehacked cheat replacements are ignored.
dboolean deh_apply_cheats = true;

// killough 10/98: new functions, to allow processing DEH files in-memory
// (e.g. from wads)

struct DEHFILE {
  /* cph 2006/08/06 -
   * if lump != NULL, lump is the start of the lump,
   * inp is the current read pos. */
  const byte *inp, *lump;
  long size;
  /* else, !lump, and f is the file being read */
  std::FILE* f;
};

// killough 10/98: emulate IO whether input really comes from a file or not
namespace {
auto dehfgets(char* const buf, std::size_t n, DEHFILE* const fp) -> char* {
  if (fp->lump == nullptr) {             // If this is a real file,
    return (std::fgets)(buf, n, fp->f);  // return regular fgets
  }

  if (n == 0 || *fp->inp == 0 || fp->size <= 0) {  // If no more characters
    return nullptr;
  }

  if (n == 1) {
    fp->size--;
    *buf = *fp->inp++;
  } else {  // copy buffer
    char* p = buf;
    while (n > 1 && *fp->inp != 0 && fp->size && (n--, fp->size--, *p++ = *fp->inp++) != '\n') {
    }
    *p = 0;
  }

  return buf;  // Return buffer pointer
}

auto dehfeof(DEHFILE* const fp) -> int {
  return fp->lump == nullptr ? std::feof(fp->f) : !*fp->inp || fp->size <= 0;
}

auto dehfgetc(DEHFILE* const fp) -> int {
  return fp->lump == nullptr ? std::fgetc(fp->f) : fp->size > 0 ? fp->size--, *fp->inp++ : EOF;
}

auto dehftell(DEHFILE* const fp) -> long {
  return fp->lump == nullptr ? std::ftell(fp->f) : (fp->inp - fp->lump);
}

auto dehfseek(DEHFILE* const fp, long offset) -> int {
  if (fp->lump == nullptr) {
    return std::fseek(fp->f, offset, SEEK_SET);
  }

  const long total = (fp->inp - fp->lump) + fp->size;
  offset = std::clamp(0L, total, offset);
  fp->inp = fp->lump + offset;
  fp->size = total - offset;
  return 0;
}
}  // namespace

// haleyjd 9/22/99
int HelperThing = -1;  // in P_SpawnMapThing to substitute helper thing

// variables used in other routines
bool deh_pars = false;  // in wi_stuff to allow pars in modified games

// #include "d_deh.h" -- we don't do that here but we declare the
// variables.  This externalizes everything that there is a string
// set for in the language files.  See d_deh.h for detailed comments,
// original English values etc.  These are set to the macro values,
// which are set by D_ENGLSH.H or D_FRENCH.H(etc).  BEX files are a
// better way of changing these strings globally by language.

// ====================================================================
// Any of these can be changed using the bex extensions
#include "dstrings.h"  // to get the initial values
/* cph - const's
 *     - removed redundant "can't XXX in a netgame" strings.
 */
const char* s_D_DEVSTR = D_DEVSTR;
const char* s_D_CDROM = D_CDROM;
const char* s_PRESSKEY = PRESSKEY;
const char* s_PRESSYN = PRESSYN;
const char* s_QUITMSG = QUITMSG;
const char* s_QSAVESPOT = QSAVESPOT;        // PRESSKEY;
const char* s_SAVEDEAD = SAVEDEAD;          // PRESSKEY; // remove duplicate y/n
const char* s_QSPROMPT = QSPROMPT;          // PRESSYN;
const char* s_QLPROMPT = QLPROMPT;          // PRESSYN;
const char* s_NEWGAME = NEWGAME;            // PRESSKEY;
const char* s_RESTARTLEVEL = RESTARTLEVEL;  // PRESSYN;
const char* s_NIGHTMARE = NIGHTMARE;        // PRESSYN;
const char* s_SWSTRING = SWSTRING;          // PRESSKEY;
const char* s_MSGOFF = MSGOFF;
const char* s_MSGON = MSGON;
const char* s_NETEND = NETEND;    // PRESSKEY;
const char* s_ENDGAME = ENDGAME;  // PRESSYN; // killough 4/4/98: end
const char* s_DOSY = DOSY;
const char* s_DETAILHI = DETAILHI;
const char* s_DETAILLO = DETAILLO;
const char* s_GAMMALVL0 = GAMMALVL0;
const char* s_GAMMALVL1 = GAMMALVL1;
const char* s_GAMMALVL2 = GAMMALVL2;
const char* s_GAMMALVL3 = GAMMALVL3;
const char* s_GAMMALVL4 = GAMMALVL4;
const char* s_EMPTYSTRING = EMPTYSTRING;
const char* s_GOTARMOR = GOTARMOR;
const char* s_GOTMEGA = GOTMEGA;
const char* s_GOTHTHBONUS = GOTHTHBONUS;
const char* s_GOTARMBONUS = GOTARMBONUS;
const char* s_GOTSTIM = GOTSTIM;
const char* s_GOTMEDINEED = GOTMEDINEED;
const char* s_GOTMEDIKIT = GOTMEDIKIT;
const char* s_GOTSUPER = GOTSUPER;
const char* s_GOTBLUECARD = GOTBLUECARD;
const char* s_GOTYELWCARD = GOTYELWCARD;
const char* s_GOTREDCARD = GOTREDCARD;
const char* s_GOTBLUESKUL = GOTBLUESKUL;
const char* s_GOTYELWSKUL = GOTYELWSKUL;
const char* s_GOTREDSKULL = GOTREDSKULL;
const char* s_GOTINVUL = GOTINVUL;
const char* s_GOTBERSERK = GOTBERSERK;
const char* s_GOTINVIS = GOTINVIS;
const char* s_GOTSUIT = GOTSUIT;
const char* s_GOTMAP = GOTMAP;
const char* s_GOTVISOR = GOTVISOR;
const char* s_GOTMSPHERE = GOTMSPHERE;
const char* s_GOTCLIP = GOTCLIP;
const char* s_GOTCLIPBOX = GOTCLIPBOX;
const char* s_GOTROCKET = GOTROCKET;
const char* s_GOTROCKBOX = GOTROCKBOX;
const char* s_GOTCELL = GOTCELL;
const char* s_GOTCELLBOX = GOTCELLBOX;
const char* s_GOTSHELLS = GOTSHELLS;
const char* s_GOTSHELLBOX = GOTSHELLBOX;
const char* s_GOTBACKPACK = GOTBACKPACK;
const char* s_GOTBFG9000 = GOTBFG9000;
const char* s_GOTCHAINGUN = GOTCHAINGUN;
const char* s_GOTCHAINSAW = GOTCHAINSAW;
const char* s_GOTLAUNCHER = GOTLAUNCHER;
const char* s_GOTPLASMA = GOTPLASMA;
const char* s_GOTSHOTGUN = GOTSHOTGUN;
const char* s_GOTSHOTGUN2 = GOTSHOTGUN2;
const char* s_PD_BLUEO = PD_BLUEO;
const char* s_PD_REDO = PD_REDO;
const char* s_PD_YELLOWO = PD_YELLOWO;
const char* s_PD_BLUEK = PD_BLUEK;
const char* s_PD_REDK = PD_REDK;
const char* s_PD_YELLOWK = PD_YELLOWK;
const char* s_PD_BLUEC = PD_BLUEC;
const char* s_PD_REDC = PD_REDC;
const char* s_PD_YELLOWC = PD_YELLOWC;
const char* s_PD_BLUES = PD_BLUES;
const char* s_PD_REDS = PD_REDS;
const char* s_PD_YELLOWS = PD_YELLOWS;
const char* s_PD_ANY = PD_ANY;
const char* s_PD_ALL3 = PD_ALL3;
const char* s_PD_ALL6 = PD_ALL6;
const char* s_GGSAVED = GGSAVED;
const char* s_HUSTR_MSGU = HUSTR_MSGU;
const char* s_HUSTR_E1M1 = HUSTR_E1M1;
const char* s_HUSTR_E1M2 = HUSTR_E1M2;
const char* s_HUSTR_E1M3 = HUSTR_E1M3;
const char* s_HUSTR_E1M4 = HUSTR_E1M4;
const char* s_HUSTR_E1M5 = HUSTR_E1M5;
const char* s_HUSTR_E1M6 = HUSTR_E1M6;
const char* s_HUSTR_E1M7 = HUSTR_E1M7;
const char* s_HUSTR_E1M8 = HUSTR_E1M8;
const char* s_HUSTR_E1M9 = HUSTR_E1M9;
const char* s_HUSTR_E2M1 = HUSTR_E2M1;
const char* s_HUSTR_E2M2 = HUSTR_E2M2;
const char* s_HUSTR_E2M3 = HUSTR_E2M3;
const char* s_HUSTR_E2M4 = HUSTR_E2M4;
const char* s_HUSTR_E2M5 = HUSTR_E2M5;
const char* s_HUSTR_E2M6 = HUSTR_E2M6;
const char* s_HUSTR_E2M7 = HUSTR_E2M7;
const char* s_HUSTR_E2M8 = HUSTR_E2M8;
const char* s_HUSTR_E2M9 = HUSTR_E2M9;
const char* s_HUSTR_E3M1 = HUSTR_E3M1;
const char* s_HUSTR_E3M2 = HUSTR_E3M2;
const char* s_HUSTR_E3M3 = HUSTR_E3M3;
const char* s_HUSTR_E3M4 = HUSTR_E3M4;
const char* s_HUSTR_E3M5 = HUSTR_E3M5;
const char* s_HUSTR_E3M6 = HUSTR_E3M6;
const char* s_HUSTR_E3M7 = HUSTR_E3M7;
const char* s_HUSTR_E3M8 = HUSTR_E3M8;
const char* s_HUSTR_E3M9 = HUSTR_E3M9;
const char* s_HUSTR_E4M1 = HUSTR_E4M1;
const char* s_HUSTR_E4M2 = HUSTR_E4M2;
const char* s_HUSTR_E4M3 = HUSTR_E4M3;
const char* s_HUSTR_E4M4 = HUSTR_E4M4;
const char* s_HUSTR_E4M5 = HUSTR_E4M5;
const char* s_HUSTR_E4M6 = HUSTR_E4M6;
const char* s_HUSTR_E4M7 = HUSTR_E4M7;
const char* s_HUSTR_E4M8 = HUSTR_E4M8;
const char* s_HUSTR_E4M9 = HUSTR_E4M9;
const char* s_HUSTR_1 = HUSTR_1;
const char* s_HUSTR_2 = HUSTR_2;
const char* s_HUSTR_3 = HUSTR_3;
const char* s_HUSTR_4 = HUSTR_4;
const char* s_HUSTR_5 = HUSTR_5;
const char* s_HUSTR_6 = HUSTR_6;
const char* s_HUSTR_7 = HUSTR_7;
const char* s_HUSTR_8 = HUSTR_8;
const char* s_HUSTR_9 = HUSTR_9;
const char* s_HUSTR_10 = HUSTR_10;
const char* s_HUSTR_11 = HUSTR_11;
const char* s_HUSTR_12 = HUSTR_12;
const char* s_HUSTR_13 = HUSTR_13;
const char* s_HUSTR_14 = HUSTR_14;
const char* s_HUSTR_15 = HUSTR_15;
const char* s_HUSTR_16 = HUSTR_16;
const char* s_HUSTR_17 = HUSTR_17;
const char* s_HUSTR_18 = HUSTR_18;
const char* s_HUSTR_19 = HUSTR_19;
const char* s_HUSTR_20 = HUSTR_20;
const char* s_HUSTR_21 = HUSTR_21;
const char* s_HUSTR_22 = HUSTR_22;
const char* s_HUSTR_23 = HUSTR_23;
const char* s_HUSTR_24 = HUSTR_24;
const char* s_HUSTR_25 = HUSTR_25;
const char* s_HUSTR_26 = HUSTR_26;
const char* s_HUSTR_27 = HUSTR_27;
const char* s_HUSTR_28 = HUSTR_28;
const char* s_HUSTR_29 = HUSTR_29;
const char* s_HUSTR_30 = HUSTR_30;
const char* s_HUSTR_31 = HUSTR_31;
const char* s_HUSTR_32 = HUSTR_32;
const char* s_HUSTR_33 = HUSTR_33;
const char* s_PHUSTR_1 = PHUSTR_1;
const char* s_PHUSTR_2 = PHUSTR_2;
const char* s_PHUSTR_3 = PHUSTR_3;
const char* s_PHUSTR_4 = PHUSTR_4;
const char* s_PHUSTR_5 = PHUSTR_5;
const char* s_PHUSTR_6 = PHUSTR_6;
const char* s_PHUSTR_7 = PHUSTR_7;
const char* s_PHUSTR_8 = PHUSTR_8;
const char* s_PHUSTR_9 = PHUSTR_9;
const char* s_PHUSTR_10 = PHUSTR_10;
const char* s_PHUSTR_11 = PHUSTR_11;
const char* s_PHUSTR_12 = PHUSTR_12;
const char* s_PHUSTR_13 = PHUSTR_13;
const char* s_PHUSTR_14 = PHUSTR_14;
const char* s_PHUSTR_15 = PHUSTR_15;
const char* s_PHUSTR_16 = PHUSTR_16;
const char* s_PHUSTR_17 = PHUSTR_17;
const char* s_PHUSTR_18 = PHUSTR_18;
const char* s_PHUSTR_19 = PHUSTR_19;
const char* s_PHUSTR_20 = PHUSTR_20;
const char* s_PHUSTR_21 = PHUSTR_21;
const char* s_PHUSTR_22 = PHUSTR_22;
const char* s_PHUSTR_23 = PHUSTR_23;
const char* s_PHUSTR_24 = PHUSTR_24;
const char* s_PHUSTR_25 = PHUSTR_25;
const char* s_PHUSTR_26 = PHUSTR_26;
const char* s_PHUSTR_27 = PHUSTR_27;
const char* s_PHUSTR_28 = PHUSTR_28;
const char* s_PHUSTR_29 = PHUSTR_29;
const char* s_PHUSTR_30 = PHUSTR_30;
const char* s_PHUSTR_31 = PHUSTR_31;
const char* s_PHUSTR_32 = PHUSTR_32;
const char* s_THUSTR_1 = THUSTR_1;
const char* s_THUSTR_2 = THUSTR_2;
const char* s_THUSTR_3 = THUSTR_3;
const char* s_THUSTR_4 = THUSTR_4;
const char* s_THUSTR_5 = THUSTR_5;
const char* s_THUSTR_6 = THUSTR_6;
const char* s_THUSTR_7 = THUSTR_7;
const char* s_THUSTR_8 = THUSTR_8;
const char* s_THUSTR_9 = THUSTR_9;
const char* s_THUSTR_10 = THUSTR_10;
const char* s_THUSTR_11 = THUSTR_11;
const char* s_THUSTR_12 = THUSTR_12;
const char* s_THUSTR_13 = THUSTR_13;
const char* s_THUSTR_14 = THUSTR_14;
const char* s_THUSTR_15 = THUSTR_15;
const char* s_THUSTR_16 = THUSTR_16;
const char* s_THUSTR_17 = THUSTR_17;
const char* s_THUSTR_18 = THUSTR_18;
const char* s_THUSTR_19 = THUSTR_19;
const char* s_THUSTR_20 = THUSTR_20;
const char* s_THUSTR_21 = THUSTR_21;
const char* s_THUSTR_22 = THUSTR_22;
const char* s_THUSTR_23 = THUSTR_23;
const char* s_THUSTR_24 = THUSTR_24;
const char* s_THUSTR_25 = THUSTR_25;
const char* s_THUSTR_26 = THUSTR_26;
const char* s_THUSTR_27 = THUSTR_27;
const char* s_THUSTR_28 = THUSTR_28;
const char* s_THUSTR_29 = THUSTR_29;
const char* s_THUSTR_30 = THUSTR_30;
const char* s_THUSTR_31 = THUSTR_31;
const char* s_THUSTR_32 = THUSTR_32;
const char* s_HUSTR_CHATMACRO1 = HUSTR_CHATMACRO1;
const char* s_HUSTR_CHATMACRO2 = HUSTR_CHATMACRO2;
const char* s_HUSTR_CHATMACRO3 = HUSTR_CHATMACRO3;
const char* s_HUSTR_CHATMACRO4 = HUSTR_CHATMACRO4;
const char* s_HUSTR_CHATMACRO5 = HUSTR_CHATMACRO5;
const char* s_HUSTR_CHATMACRO6 = HUSTR_CHATMACRO6;
const char* s_HUSTR_CHATMACRO7 = HUSTR_CHATMACRO7;
const char* s_HUSTR_CHATMACRO8 = HUSTR_CHATMACRO8;
const char* s_HUSTR_CHATMACRO9 = HUSTR_CHATMACRO9;
const char* s_HUSTR_CHATMACRO0 = HUSTR_CHATMACRO0;
const char* s_HUSTR_TALKTOSELF1 = HUSTR_TALKTOSELF1;
const char* s_HUSTR_TALKTOSELF2 = HUSTR_TALKTOSELF2;
const char* s_HUSTR_TALKTOSELF3 = HUSTR_TALKTOSELF3;
const char* s_HUSTR_TALKTOSELF4 = HUSTR_TALKTOSELF4;
const char* s_HUSTR_TALKTOSELF5 = HUSTR_TALKTOSELF5;
const char* s_HUSTR_MESSAGESENT = HUSTR_MESSAGESENT;
const char* s_HUSTR_PLRGREEN = HUSTR_PLRGREEN;
const char* s_HUSTR_PLRINDIGO = HUSTR_PLRINDIGO;
const char* s_HUSTR_PLRBROWN = HUSTR_PLRBROWN;
const char* s_HUSTR_PLRRED = HUSTR_PLRRED;
const char* s_AMSTR_FOLLOWON = AMSTR_FOLLOWON;
const char* s_AMSTR_FOLLOWOFF = AMSTR_FOLLOWOFF;
const char* s_AMSTR_GRIDON = AMSTR_GRIDON;
const char* s_AMSTR_GRIDOFF = AMSTR_GRIDOFF;
const char* s_AMSTR_MARKEDSPOT = AMSTR_MARKEDSPOT;
const char* s_AMSTR_MARKSCLEARED = AMSTR_MARKSCLEARED;
// CPhipps - automap rotate & overlay
const char* s_AMSTR_ROTATEON = AMSTR_ROTATEON;
const char* s_AMSTR_ROTATEOFF = AMSTR_ROTATEOFF;
const char* s_AMSTR_OVERLAYON = AMSTR_OVERLAYON;
const char* s_AMSTR_OVERLAYOFF = AMSTR_OVERLAYOFF;
// e6y: textured automap
const char* s_AMSTR_TEXTUREDON = AMSTR_TEXTUREDON;
const char* s_AMSTR_TEXTUREDOFF = AMSTR_TEXTUREDOFF;

const char* s_STSTR_MUS = STSTR_MUS;
const char* s_STSTR_NOMUS = STSTR_NOMUS;
const char* s_STSTR_DQDON = STSTR_DQDON;
const char* s_STSTR_DQDOFF = STSTR_DQDOFF;
const char* s_STSTR_KFAADDED = STSTR_KFAADDED;
const char* s_STSTR_FAADDED = STSTR_FAADDED;
const char* s_STSTR_NCON = STSTR_NCON;
const char* s_STSTR_NCOFF = STSTR_NCOFF;
const char* s_STSTR_BEHOLD = STSTR_BEHOLD;
const char* s_STSTR_BEHOLDX = STSTR_BEHOLDX;
const char* s_STSTR_CHOPPERS = STSTR_CHOPPERS;
const char* s_STSTR_CLEV = STSTR_CLEV;
const char* s_STSTR_COMPON = STSTR_COMPON;
const char* s_STSTR_COMPOFF = STSTR_COMPOFF;
const char* s_E1TEXT = E1TEXT;
const char* s_E2TEXT = E2TEXT;
const char* s_E3TEXT = E3TEXT;
const char* s_E4TEXT = E4TEXT;
const char* s_C1TEXT = C1TEXT;
const char* s_C2TEXT = C2TEXT;
const char* s_C3TEXT = C3TEXT;
const char* s_C4TEXT = C4TEXT;
const char* s_C5TEXT = C5TEXT;
const char* s_C6TEXT = C6TEXT;
const char* s_P1TEXT = P1TEXT;
const char* s_P2TEXT = P2TEXT;
const char* s_P3TEXT = P3TEXT;
const char* s_P4TEXT = P4TEXT;
const char* s_P5TEXT = P5TEXT;
const char* s_P6TEXT = P6TEXT;
const char* s_T1TEXT = T1TEXT;
const char* s_T2TEXT = T2TEXT;
const char* s_T3TEXT = T3TEXT;
const char* s_T4TEXT = T4TEXT;
const char* s_T5TEXT = T5TEXT;
const char* s_T6TEXT = T6TEXT;
const char* s_CC_ZOMBIE = CC_ZOMBIE;
const char* s_CC_SHOTGUN = CC_SHOTGUN;
const char* s_CC_HEAVY = CC_HEAVY;
const char* s_CC_IMP = CC_IMP;
const char* s_CC_DEMON = CC_DEMON;
const char* s_CC_LOST = CC_LOST;
const char* s_CC_CACO = CC_CACO;
const char* s_CC_HELL = CC_HELL;
const char* s_CC_BARON = CC_BARON;
const char* s_CC_ARACH = CC_ARACH;
const char* s_CC_PAIN = CC_PAIN;
const char* s_CC_REVEN = CC_REVEN;
const char* s_CC_MANCU = CC_MANCU;
const char* s_CC_ARCH = CC_ARCH;
const char* s_CC_SPIDER = CC_SPIDER;
const char* s_CC_CYBER = CC_CYBER;
const char* s_CC_HERO = CC_HERO;
// Ty 03/30/98 - new substitutions for background textures
//               during int screens
const char* bgflatE1 = "FLOOR4_8";    // end of DOOM Episode 1
const char* bgflatE2 = "SFLR6_1";     // end of DOOM Episode 2
const char* bgflatE3 = "MFLR8_4";     // end of DOOM Episode 3
const char* bgflatE4 = "MFLR8_3";     // end of DOOM Episode 4
const char* bgflat06 = "SLIME16";     // DOOM2 after MAP06
const char* bgflat11 = "RROCK14";     // DOOM2 after MAP11
const char* bgflat20 = "RROCK07";     // DOOM2 after MAP20
const char* bgflat30 = "RROCK17";     // DOOM2 after MAP30
const char* bgflat15 = "RROCK13";     // DOOM2 going MAP15 to MAP31
const char* bgflat31 = "RROCK19";     // DOOM2 going MAP31 to MAP32
const char* bgcastcall = "BOSSBACK";  // Panel behind cast call

const char* startup1 = "";  // blank lines are default and are not printed
const char* startup2 = "";
const char* startup3 = "";
const char* startup4 = "";
const char* startup5 = "";

/* Ty 05/03/98 - externalized
 * cph - updated for prboom */
const char* savegamename = PACKAGE_TARNAME "-savegame";

// end d_deh.h variable declarations
// ====================================================================

// Do this for a lookup--the pointer (loaded above) is cross-referenced
// to a string key that is the same as the define above.  We will use
// strdups to set these new values that we read from the file, orphaning
// the original value set above.

// CPhipps - make strings pointed to const
struct deh_strs {
  const char** ppstr;       // doubly indirect pointer to string
  std::string_view lookup;  // pointer to lookup string name
  std::optional<std::string_view> orig;
};

namespace {
/* CPhipps - const
 *         - removed redundant "Can't XXX in a netgame" strings
 */
std::array<deh_strs, 307> deh_strlookup = {
    deh_strs{&s_D_DEVSTR, "D_DEVSTR", {}}, deh_strs{&s_D_CDROM, "D_CDROM", {}}, deh_strs{&s_PRESSKEY, "PRESSKEY", {}},
    deh_strs{&s_PRESSYN, "PRESSYN", {}}, deh_strs{&s_QUITMSG, "QUITMSG", {}}, deh_strs{&s_QSAVESPOT, "QSAVESPOT", {}},
    deh_strs{&s_SAVEDEAD, "SAVEDEAD", {}},
    /* cph - disabled to prevent format string attacks in WAD files
    deh_strs{&s_QSPROMPT, "QSPROMPT", {}},
    deh_strs{&s_QLPROMPT, "QLPROMPT", {}},*/
    deh_strs{&s_NEWGAME, "NEWGAME", {}}, deh_strs{&s_RESTARTLEVEL, "RESTARTLEVEL", {}},
    deh_strs{&s_NIGHTMARE, "NIGHTMARE", {}}, deh_strs{&s_SWSTRING, "SWSTRING", {}}, deh_strs{&s_MSGOFF, "MSGOFF", {}},
    deh_strs{&s_MSGON, "MSGON", {}}, deh_strs{&s_NETEND, "NETEND", {}}, deh_strs{&s_ENDGAME, "ENDGAME", {}},
    deh_strs{&s_DOSY, "DOSY", {}}, deh_strs{&s_DETAILHI, "DETAILHI", {}}, deh_strs{&s_DETAILLO, "DETAILLO", {}},
    deh_strs{&s_GAMMALVL0, "GAMMALVL0", {}}, deh_strs{&s_GAMMALVL1, "GAMMALVL1", {}},
    deh_strs{&s_GAMMALVL2, "GAMMALVL2", {}}, deh_strs{&s_GAMMALVL3, "GAMMALVL3", {}},
    deh_strs{&s_GAMMALVL4, "GAMMALVL4", {}}, deh_strs{&s_EMPTYSTRING, "EMPTYSTRING", {}},
    deh_strs{&s_GOTARMOR, "GOTARMOR", {}}, deh_strs{&s_GOTMEGA, "GOTMEGA", {}},
    deh_strs{&s_GOTHTHBONUS, "GOTHTHBONUS", {}}, deh_strs{&s_GOTARMBONUS, "GOTARMBONUS", {}},
    deh_strs{&s_GOTSTIM, "GOTSTIM", {}}, deh_strs{&s_GOTMEDINEED, "GOTMEDINEED", {}},
    deh_strs{&s_GOTMEDIKIT, "GOTMEDIKIT", {}}, deh_strs{&s_GOTSUPER, "GOTSUPER", {}},
    deh_strs{&s_GOTBLUECARD, "GOTBLUECARD", {}}, deh_strs{&s_GOTYELWCARD, "GOTYELWCARD", {}},
    deh_strs{&s_GOTREDCARD, "GOTREDCARD", {}}, deh_strs{&s_GOTBLUESKUL, "GOTBLUESKUL", {}},
    deh_strs{&s_GOTYELWSKUL, "GOTYELWSKUL", {}}, deh_strs{&s_GOTREDSKULL, "GOTREDSKULL", {}},
    deh_strs{&s_GOTINVUL, "GOTINVUL", {}}, deh_strs{&s_GOTBERSERK, "GOTBERSERK", {}},
    deh_strs{&s_GOTINVIS, "GOTINVIS", {}}, deh_strs{&s_GOTSUIT, "GOTSUIT", {}}, deh_strs{&s_GOTMAP, "GOTMAP", {}},
    deh_strs{&s_GOTVISOR, "GOTVISOR", {}}, deh_strs{&s_GOTMSPHERE, "GOTMSPHERE", {}},
    deh_strs{&s_GOTCLIP, "GOTCLIP", {}}, deh_strs{&s_GOTCLIPBOX, "GOTCLIPBOX", {}},
    deh_strs{&s_GOTROCKET, "GOTROCKET", {}}, deh_strs{&s_GOTROCKBOX, "GOTROCKBOX", {}},
    deh_strs{&s_GOTCELL, "GOTCELL", {}}, deh_strs{&s_GOTCELLBOX, "GOTCELLBOX", {}},
    deh_strs{&s_GOTSHELLS, "GOTSHELLS", {}}, deh_strs{&s_GOTSHELLBOX, "GOTSHELLBOX", {}},
    deh_strs{&s_GOTBACKPACK, "GOTBACKPACK", {}}, deh_strs{&s_GOTBFG9000, "GOTBFG9000", {}},
    deh_strs{&s_GOTCHAINGUN, "GOTCHAINGUN", {}}, deh_strs{&s_GOTCHAINSAW, "GOTCHAINSAW", {}},
    deh_strs{&s_GOTLAUNCHER, "GOTLAUNCHER", {}}, deh_strs{&s_GOTPLASMA, "GOTPLASMA", {}},
    deh_strs{&s_GOTSHOTGUN, "GOTSHOTGUN", {}}, deh_strs{&s_GOTSHOTGUN2, "GOTSHOTGUN2", {}},
    deh_strs{&s_PD_BLUEO, "PD_BLUEO", {}}, deh_strs{&s_PD_REDO, "PD_REDO", {}},
    deh_strs{&s_PD_YELLOWO, "PD_YELLOWO", {}}, deh_strs{&s_PD_BLUEK, "PD_BLUEK", {}},
    deh_strs{&s_PD_REDK, "PD_REDK", {}}, deh_strs{&s_PD_YELLOWK, "PD_YELLOWK", {}},
    deh_strs{&s_PD_BLUEC, "PD_BLUEC", {}}, deh_strs{&s_PD_REDC, "PD_REDC", {}},
    deh_strs{&s_PD_YELLOWC, "PD_YELLOWC", {}}, deh_strs{&s_PD_BLUES, "PD_BLUES", {}},
    deh_strs{&s_PD_REDS, "PD_REDS", {}}, deh_strs{&s_PD_YELLOWS, "PD_YELLOWS", {}}, deh_strs{&s_PD_ANY, "PD_ANY", {}},
    deh_strs{&s_PD_ALL3, "PD_ALL3", {}}, deh_strs{&s_PD_ALL6, "PD_ALL6", {}}, deh_strs{&s_GGSAVED, "GGSAVED", {}},
    deh_strs{&s_HUSTR_MSGU, "HUSTR_MSGU", {}}, deh_strs{&s_HUSTR_E1M1, "HUSTR_E1M1", {}},
    deh_strs{&s_HUSTR_E1M2, "HUSTR_E1M2", {}}, deh_strs{&s_HUSTR_E1M3, "HUSTR_E1M3", {}},
    deh_strs{&s_HUSTR_E1M4, "HUSTR_E1M4", {}}, deh_strs{&s_HUSTR_E1M5, "HUSTR_E1M5", {}},
    deh_strs{&s_HUSTR_E1M6, "HUSTR_E1M6", {}}, deh_strs{&s_HUSTR_E1M7, "HUSTR_E1M7", {}},
    deh_strs{&s_HUSTR_E1M8, "HUSTR_E1M8", {}}, deh_strs{&s_HUSTR_E1M9, "HUSTR_E1M9", {}},
    deh_strs{&s_HUSTR_E2M1, "HUSTR_E2M1", {}}, deh_strs{&s_HUSTR_E2M2, "HUSTR_E2M2", {}},
    deh_strs{&s_HUSTR_E2M3, "HUSTR_E2M3", {}}, deh_strs{&s_HUSTR_E2M4, "HUSTR_E2M4", {}},
    deh_strs{&s_HUSTR_E2M5, "HUSTR_E2M5", {}}, deh_strs{&s_HUSTR_E2M6, "HUSTR_E2M6", {}},
    deh_strs{&s_HUSTR_E2M7, "HUSTR_E2M7", {}}, deh_strs{&s_HUSTR_E2M8, "HUSTR_E2M8", {}},
    deh_strs{&s_HUSTR_E2M9, "HUSTR_E2M9", {}}, deh_strs{&s_HUSTR_E3M1, "HUSTR_E3M1", {}},
    deh_strs{&s_HUSTR_E3M2, "HUSTR_E3M2", {}}, deh_strs{&s_HUSTR_E3M3, "HUSTR_E3M3", {}},
    deh_strs{&s_HUSTR_E3M4, "HUSTR_E3M4", {}}, deh_strs{&s_HUSTR_E3M5, "HUSTR_E3M5", {}},
    deh_strs{&s_HUSTR_E3M6, "HUSTR_E3M6", {}}, deh_strs{&s_HUSTR_E3M7, "HUSTR_E3M7", {}},
    deh_strs{&s_HUSTR_E3M8, "HUSTR_E3M8", {}}, deh_strs{&s_HUSTR_E3M9, "HUSTR_E3M9", {}},
    deh_strs{&s_HUSTR_E4M1, "HUSTR_E4M1", {}}, deh_strs{&s_HUSTR_E4M2, "HUSTR_E4M2", {}},
    deh_strs{&s_HUSTR_E4M3, "HUSTR_E4M3", {}}, deh_strs{&s_HUSTR_E4M4, "HUSTR_E4M4", {}},
    deh_strs{&s_HUSTR_E4M5, "HUSTR_E4M5", {}}, deh_strs{&s_HUSTR_E4M6, "HUSTR_E4M6", {}},
    deh_strs{&s_HUSTR_E4M7, "HUSTR_E4M7", {}}, deh_strs{&s_HUSTR_E4M8, "HUSTR_E4M8", {}},
    deh_strs{&s_HUSTR_E4M9, "HUSTR_E4M9", {}}, deh_strs{&s_HUSTR_1, "HUSTR_1", {}}, deh_strs{&s_HUSTR_2, "HUSTR_2", {}},
    deh_strs{&s_HUSTR_3, "HUSTR_3", {}}, deh_strs{&s_HUSTR_4, "HUSTR_4", {}}, deh_strs{&s_HUSTR_5, "HUSTR_5", {}},
    deh_strs{&s_HUSTR_6, "HUSTR_6", {}}, deh_strs{&s_HUSTR_7, "HUSTR_7", {}}, deh_strs{&s_HUSTR_8, "HUSTR_8", {}},
    deh_strs{&s_HUSTR_9, "HUSTR_9", {}}, deh_strs{&s_HUSTR_10, "HUSTR_10", {}}, deh_strs{&s_HUSTR_11, "HUSTR_11", {}},
    deh_strs{&s_HUSTR_12, "HUSTR_12", {}}, deh_strs{&s_HUSTR_13, "HUSTR_13", {}}, deh_strs{&s_HUSTR_14, "HUSTR_14", {}},
    deh_strs{&s_HUSTR_15, "HUSTR_15", {}}, deh_strs{&s_HUSTR_16, "HUSTR_16", {}}, deh_strs{&s_HUSTR_17, "HUSTR_17", {}},
    deh_strs{&s_HUSTR_18, "HUSTR_18", {}}, deh_strs{&s_HUSTR_19, "HUSTR_19", {}}, deh_strs{&s_HUSTR_20, "HUSTR_20", {}},
    deh_strs{&s_HUSTR_21, "HUSTR_21", {}}, deh_strs{&s_HUSTR_22, "HUSTR_22", {}}, deh_strs{&s_HUSTR_23, "HUSTR_23", {}},
    deh_strs{&s_HUSTR_24, "HUSTR_24", {}}, deh_strs{&s_HUSTR_25, "HUSTR_25", {}}, deh_strs{&s_HUSTR_26, "HUSTR_26", {}},
    deh_strs{&s_HUSTR_27, "HUSTR_27", {}}, deh_strs{&s_HUSTR_28, "HUSTR_28", {}}, deh_strs{&s_HUSTR_29, "HUSTR_29", {}},
    deh_strs{&s_HUSTR_30, "HUSTR_30", {}}, deh_strs{&s_HUSTR_31, "HUSTR_31", {}}, deh_strs{&s_HUSTR_32, "HUSTR_32", {}},
    deh_strs{&s_HUSTR_33, "HUSTR_33", {}}, deh_strs{&s_PHUSTR_1, "PHUSTR_1", {}}, deh_strs{&s_PHUSTR_2, "PHUSTR_2", {}},
    deh_strs{&s_PHUSTR_3, "PHUSTR_3", {}}, deh_strs{&s_PHUSTR_4, "PHUSTR_4", {}}, deh_strs{&s_PHUSTR_5, "PHUSTR_5", {}},
    deh_strs{&s_PHUSTR_6, "PHUSTR_6", {}}, deh_strs{&s_PHUSTR_7, "PHUSTR_7", {}}, deh_strs{&s_PHUSTR_8, "PHUSTR_8", {}},
    deh_strs{&s_PHUSTR_9, "PHUSTR_9", {}}, deh_strs{&s_PHUSTR_10, "PHUSTR_10", {}},
    deh_strs{&s_PHUSTR_11, "PHUSTR_11", {}}, deh_strs{&s_PHUSTR_12, "PHUSTR_12", {}},
    deh_strs{&s_PHUSTR_13, "PHUSTR_13", {}}, deh_strs{&s_PHUSTR_14, "PHUSTR_14", {}},
    deh_strs{&s_PHUSTR_15, "PHUSTR_15", {}}, deh_strs{&s_PHUSTR_16, "PHUSTR_16", {}},
    deh_strs{&s_PHUSTR_17, "PHUSTR_17", {}}, deh_strs{&s_PHUSTR_18, "PHUSTR_18", {}},
    deh_strs{&s_PHUSTR_19, "PHUSTR_19", {}}, deh_strs{&s_PHUSTR_20, "PHUSTR_20", {}},
    deh_strs{&s_PHUSTR_21, "PHUSTR_21", {}}, deh_strs{&s_PHUSTR_22, "PHUSTR_22", {}},
    deh_strs{&s_PHUSTR_23, "PHUSTR_23", {}}, deh_strs{&s_PHUSTR_24, "PHUSTR_24", {}},
    deh_strs{&s_PHUSTR_25, "PHUSTR_25", {}}, deh_strs{&s_PHUSTR_26, "PHUSTR_26", {}},
    deh_strs{&s_PHUSTR_27, "PHUSTR_27", {}}, deh_strs{&s_PHUSTR_28, "PHUSTR_28", {}},
    deh_strs{&s_PHUSTR_29, "PHUSTR_29", {}}, deh_strs{&s_PHUSTR_30, "PHUSTR_30", {}},
    deh_strs{&s_PHUSTR_31, "PHUSTR_31", {}}, deh_strs{&s_PHUSTR_32, "PHUSTR_32", {}},
    deh_strs{&s_THUSTR_1, "THUSTR_1", {}}, deh_strs{&s_THUSTR_2, "THUSTR_2", {}}, deh_strs{&s_THUSTR_3, "THUSTR_3", {}},
    deh_strs{&s_THUSTR_4, "THUSTR_4", {}}, deh_strs{&s_THUSTR_5, "THUSTR_5", {}}, deh_strs{&s_THUSTR_6, "THUSTR_6", {}},
    deh_strs{&s_THUSTR_7, "THUSTR_7", {}}, deh_strs{&s_THUSTR_8, "THUSTR_8", {}}, deh_strs{&s_THUSTR_9, "THUSTR_9", {}},
    deh_strs{&s_THUSTR_10, "THUSTR_10", {}}, deh_strs{&s_THUSTR_11, "THUSTR_11", {}},
    deh_strs{&s_THUSTR_12, "THUSTR_12", {}}, deh_strs{&s_THUSTR_13, "THUSTR_13", {}},
    deh_strs{&s_THUSTR_14, "THUSTR_14", {}}, deh_strs{&s_THUSTR_15, "THUSTR_15", {}},
    deh_strs{&s_THUSTR_16, "THUSTR_16", {}}, deh_strs{&s_THUSTR_17, "THUSTR_17", {}},
    deh_strs{&s_THUSTR_18, "THUSTR_18", {}}, deh_strs{&s_THUSTR_19, "THUSTR_19", {}},
    deh_strs{&s_THUSTR_20, "THUSTR_20", {}}, deh_strs{&s_THUSTR_21, "THUSTR_21", {}},
    deh_strs{&s_THUSTR_22, "THUSTR_22", {}}, deh_strs{&s_THUSTR_23, "THUSTR_23", {}},
    deh_strs{&s_THUSTR_24, "THUSTR_24", {}}, deh_strs{&s_THUSTR_25, "THUSTR_25", {}},
    deh_strs{&s_THUSTR_26, "THUSTR_26", {}}, deh_strs{&s_THUSTR_27, "THUSTR_27", {}},
    deh_strs{&s_THUSTR_28, "THUSTR_28", {}}, deh_strs{&s_THUSTR_29, "THUSTR_29", {}},
    deh_strs{&s_THUSTR_30, "THUSTR_30", {}}, deh_strs{&s_THUSTR_31, "THUSTR_31", {}},
    deh_strs{&s_THUSTR_32, "THUSTR_32", {}}, deh_strs{&s_HUSTR_CHATMACRO1, "HUSTR_CHATMACRO1", {}},
    deh_strs{&s_HUSTR_CHATMACRO2, "HUSTR_CHATMACRO2", {}}, deh_strs{&s_HUSTR_CHATMACRO3, "HUSTR_CHATMACRO3", {}},
    deh_strs{&s_HUSTR_CHATMACRO4, "HUSTR_CHATMACRO4", {}}, deh_strs{&s_HUSTR_CHATMACRO5, "HUSTR_CHATMACRO5", {}},
    deh_strs{&s_HUSTR_CHATMACRO6, "HUSTR_CHATMACRO6", {}}, deh_strs{&s_HUSTR_CHATMACRO7, "HUSTR_CHATMACRO7", {}},
    deh_strs{&s_HUSTR_CHATMACRO8, "HUSTR_CHATMACRO8", {}}, deh_strs{&s_HUSTR_CHATMACRO9, "HUSTR_CHATMACRO9", {}},
    deh_strs{&s_HUSTR_CHATMACRO0, "HUSTR_CHATMACRO0", {}}, deh_strs{&s_HUSTR_TALKTOSELF1, "HUSTR_TALKTOSELF1", {}},
    deh_strs{&s_HUSTR_TALKTOSELF2, "HUSTR_TALKTOSELF2", {}}, deh_strs{&s_HUSTR_TALKTOSELF3, "HUSTR_TALKTOSELF3", {}},
    deh_strs{&s_HUSTR_TALKTOSELF4, "HUSTR_TALKTOSELF4", {}}, deh_strs{&s_HUSTR_TALKTOSELF5, "HUSTR_TALKTOSELF5", {}},
    deh_strs{&s_HUSTR_MESSAGESENT, "HUSTR_MESSAGESENT", {}}, deh_strs{&s_HUSTR_PLRGREEN, "HUSTR_PLRGREEN", {}},
    deh_strs{&s_HUSTR_PLRINDIGO, "HUSTR_PLRINDIGO", {}}, deh_strs{&s_HUSTR_PLRBROWN, "HUSTR_PLRBROWN", {}},
    deh_strs{&s_HUSTR_PLRRED, "HUSTR_PLRRED", {}},
    // deh_strs{c_HUSTR_KEYGREEN, "HUSTR_KEYGREEN", {}},
    // deh_strs{c_HUSTR_KEYINDIGO, "HUSTR_KEYINDIGO", {}},
    // deh_strs{c_HUSTR_KEYBROWN, "HUSTR_KEYBROWN", {}},
    // deh_strs{c_HUSTR_KEYRED, "HUSTR_KEYRED", {}},
    deh_strs{&s_AMSTR_FOLLOWON, "AMSTR_FOLLOWON", {}}, deh_strs{&s_AMSTR_FOLLOWOFF, "AMSTR_FOLLOWOFF", {}},
    deh_strs{&s_AMSTR_GRIDON, "AMSTR_GRIDON", {}}, deh_strs{&s_AMSTR_GRIDOFF, "AMSTR_GRIDOFF", {}},
    deh_strs{&s_AMSTR_MARKEDSPOT, "AMSTR_MARKEDSPOT", {}}, deh_strs{&s_AMSTR_MARKSCLEARED, "AMSTR_MARKSCLEARED", {}},
    deh_strs{&s_STSTR_MUS, "STSTR_MUS", {}}, deh_strs{&s_STSTR_NOMUS, "STSTR_NOMUS", {}},
    deh_strs{&s_STSTR_DQDON, "STSTR_DQDON", {}}, deh_strs{&s_STSTR_DQDOFF, "STSTR_DQDOFF", {}},
    deh_strs{&s_STSTR_KFAADDED, "STSTR_KFAADDED", {}}, deh_strs{&s_STSTR_FAADDED, "STSTR_FAADDED", {}},
    deh_strs{&s_STSTR_NCON, "STSTR_NCON", {}}, deh_strs{&s_STSTR_NCOFF, "STSTR_NCOFF", {}},
    deh_strs{&s_STSTR_BEHOLD, "STSTR_BEHOLD", {}}, deh_strs{&s_STSTR_BEHOLDX, "STSTR_BEHOLDX", {}},
    deh_strs{&s_STSTR_CHOPPERS, "STSTR_CHOPPERS", {}}, deh_strs{&s_STSTR_CLEV, "STSTR_CLEV", {}},
    deh_strs{&s_STSTR_COMPON, "STSTR_COMPON", {}}, deh_strs{&s_STSTR_COMPOFF, "STSTR_COMPOFF", {}},
    deh_strs{&s_E1TEXT, "E1TEXT", {}}, deh_strs{&s_E2TEXT, "E2TEXT", {}}, deh_strs{&s_E3TEXT, "E3TEXT", {}},
    deh_strs{&s_E4TEXT, "E4TEXT", {}}, deh_strs{&s_C1TEXT, "C1TEXT", {}}, deh_strs{&s_C2TEXT, "C2TEXT", {}},
    deh_strs{&s_C3TEXT, "C3TEXT", {}}, deh_strs{&s_C4TEXT, "C4TEXT", {}}, deh_strs{&s_C5TEXT, "C5TEXT", {}},
    deh_strs{&s_C6TEXT, "C6TEXT", {}}, deh_strs{&s_P1TEXT, "P1TEXT", {}}, deh_strs{&s_P2TEXT, "P2TEXT", {}},
    deh_strs{&s_P3TEXT, "P3TEXT", {}}, deh_strs{&s_P4TEXT, "P4TEXT", {}}, deh_strs{&s_P5TEXT, "P5TEXT", {}},
    deh_strs{&s_P6TEXT, "P6TEXT", {}}, deh_strs{&s_T1TEXT, "T1TEXT", {}}, deh_strs{&s_T2TEXT, "T2TEXT", {}},
    deh_strs{&s_T3TEXT, "T3TEXT", {}}, deh_strs{&s_T4TEXT, "T4TEXT", {}}, deh_strs{&s_T5TEXT, "T5TEXT", {}},
    deh_strs{&s_T6TEXT, "T6TEXT", {}}, deh_strs{&s_CC_ZOMBIE, "CC_ZOMBIE", {}},
    deh_strs{&s_CC_SHOTGUN, "CC_SHOTGUN", {}}, deh_strs{&s_CC_HEAVY, "CC_HEAVY", {}}, deh_strs{&s_CC_IMP, "CC_IMP", {}},
    deh_strs{&s_CC_DEMON, "CC_DEMON", {}}, deh_strs{&s_CC_LOST, "CC_LOST", {}}, deh_strs{&s_CC_CACO, "CC_CACO", {}},
    deh_strs{&s_CC_HELL, "CC_HELL", {}}, deh_strs{&s_CC_BARON, "CC_BARON", {}}, deh_strs{&s_CC_ARACH, "CC_ARACH", {}},
    deh_strs{&s_CC_PAIN, "CC_PAIN", {}}, deh_strs{&s_CC_REVEN, "CC_REVEN", {}}, deh_strs{&s_CC_MANCU, "CC_MANCU", {}},
    deh_strs{&s_CC_ARCH, "CC_ARCH", {}}, deh_strs{&s_CC_SPIDER, "CC_SPIDER", {}}, deh_strs{&s_CC_CYBER, "CC_CYBER", {}},
    deh_strs{&s_CC_HERO, "CC_HERO", {}}, deh_strs{&bgflatE1, "BGFLATE1", {}}, deh_strs{&bgflatE2, "BGFLATE2", {}},
    deh_strs{&bgflatE3, "BGFLATE3", {}}, deh_strs{&bgflatE4, "BGFLATE4", {}}, deh_strs{&bgflat06, "BGFLAT06", {}},
    deh_strs{&bgflat11, "BGFLAT11", {}}, deh_strs{&bgflat20, "BGFLAT20", {}}, deh_strs{&bgflat30, "BGFLAT30", {}},
    deh_strs{&bgflat15, "BGFLAT15", {}}, deh_strs{&bgflat31, "BGFLAT31", {}}, deh_strs{&bgcastcall, "BGCASTCALL", {}},
    // Ty 04/08/98 - added 5 general purpose startup announcement
    // strings for hacker use.  See m_menu.c
    deh_strs{&startup1, "STARTUP1", {}}, deh_strs{&startup2, "STARTUP2", {}}, deh_strs{&startup3, "STARTUP3", {}},
    deh_strs{&startup4, "STARTUP4", {}}, deh_strs{&startup5, "STARTUP5", {}},
    deh_strs{&savegamename, "SAVEGAMENAME", {}},  // Ty 05/03/98
};
}  // namespace

const char* deh_newlevel = "NEWLEVEL";  // CPhipps - const

// DOOM shareware/registered/retail (Ultimate) names.
// CPhipps - const**const
extern "C" const char** const mapnames[] = {&s_HUSTR_E1M1, &s_HUSTR_E1M2, &s_HUSTR_E1M3, &s_HUSTR_E1M4, &s_HUSTR_E1M5,
                                            &s_HUSTR_E1M6, &s_HUSTR_E1M7, &s_HUSTR_E1M8, &s_HUSTR_E1M9,

                                            &s_HUSTR_E2M1, &s_HUSTR_E2M2, &s_HUSTR_E2M3, &s_HUSTR_E2M4, &s_HUSTR_E2M5,
                                            &s_HUSTR_E2M6, &s_HUSTR_E2M7, &s_HUSTR_E2M8, &s_HUSTR_E2M9,

                                            &s_HUSTR_E3M1, &s_HUSTR_E3M2, &s_HUSTR_E3M3, &s_HUSTR_E3M4, &s_HUSTR_E3M5,
                                            &s_HUSTR_E3M6, &s_HUSTR_E3M7, &s_HUSTR_E3M8, &s_HUSTR_E3M9,

                                            &s_HUSTR_E4M1, &s_HUSTR_E4M2, &s_HUSTR_E4M3, &s_HUSTR_E4M4, &s_HUSTR_E4M5,
                                            &s_HUSTR_E4M6, &s_HUSTR_E4M7, &s_HUSTR_E4M8, &s_HUSTR_E4M9,

                                            &deh_newlevel,  // spares?  Unused.
                                            &deh_newlevel, &deh_newlevel, &deh_newlevel, &deh_newlevel, &deh_newlevel,
                                            &deh_newlevel, &deh_newlevel, &deh_newlevel};

// CPhipps - const**const
extern "C" const char** const mapnames2[] =  // DOOM 2 map names.
    {
        &s_HUSTR_1,  &s_HUSTR_2,  &s_HUSTR_3,  &s_HUSTR_4,  &s_HUSTR_5,  &s_HUSTR_6,  &s_HUSTR_7,
        &s_HUSTR_8,  &s_HUSTR_9,  &s_HUSTR_10, &s_HUSTR_11,

        &s_HUSTR_12, &s_HUSTR_13, &s_HUSTR_14, &s_HUSTR_15, &s_HUSTR_16, &s_HUSTR_17, &s_HUSTR_18,
        &s_HUSTR_19, &s_HUSTR_20,

        &s_HUSTR_21, &s_HUSTR_22, &s_HUSTR_23, &s_HUSTR_24, &s_HUSTR_25, &s_HUSTR_26, &s_HUSTR_27,
        &s_HUSTR_28, &s_HUSTR_29, &s_HUSTR_30, &s_HUSTR_31, &s_HUSTR_32, &s_HUSTR_33,
};

// CPhipps - const**const
extern "C" const char** const mapnamesp[] =  // Plutonia WAD map names.
    {
        &s_PHUSTR_1,  &s_PHUSTR_2,  &s_PHUSTR_3,  &s_PHUSTR_4,  &s_PHUSTR_5,  &s_PHUSTR_6,
        &s_PHUSTR_7,  &s_PHUSTR_8,  &s_PHUSTR_9,  &s_PHUSTR_10, &s_PHUSTR_11,

        &s_PHUSTR_12, &s_PHUSTR_13, &s_PHUSTR_14, &s_PHUSTR_15, &s_PHUSTR_16, &s_PHUSTR_17,
        &s_PHUSTR_18, &s_PHUSTR_19, &s_PHUSTR_20,

        &s_PHUSTR_21, &s_PHUSTR_22, &s_PHUSTR_23, &s_PHUSTR_24, &s_PHUSTR_25, &s_PHUSTR_26,
        &s_PHUSTR_27, &s_PHUSTR_28, &s_PHUSTR_29, &s_PHUSTR_30, &s_PHUSTR_31, &s_PHUSTR_32,
};

// CPhipps - const**const
extern "C" const char** const mapnamest[] =  // TNT WAD map names.
    {
        &s_THUSTR_1,  &s_THUSTR_2,  &s_THUSTR_3,  &s_THUSTR_4,  &s_THUSTR_5,  &s_THUSTR_6,
        &s_THUSTR_7,  &s_THUSTR_8,  &s_THUSTR_9,  &s_THUSTR_10, &s_THUSTR_11,

        &s_THUSTR_12, &s_THUSTR_13, &s_THUSTR_14, &s_THUSTR_15, &s_THUSTR_16, &s_THUSTR_17,
        &s_THUSTR_18, &s_THUSTR_19, &s_THUSTR_20,

        &s_THUSTR_21, &s_THUSTR_22, &s_THUSTR_23, &s_THUSTR_24, &s_THUSTR_25, &s_THUSTR_26,
        &s_THUSTR_27, &s_THUSTR_28, &s_THUSTR_29, &s_THUSTR_30, &s_THUSTR_31, &s_THUSTR_32,
};

// Function prototypes
[[deprecated]] void lfstrip(char*);              // strip the \r and/or \n off of a line
[[deprecated]] void rstrip(char*);               // strip trailing whitespace
[[deprecated]] auto ptr_lstrip(char*) -> char*;  // point past leading whitespace
[[deprecated]] auto deh_GetData(char*, char*, uint_64_t*, char**, std::FILE*) -> bool;
[[deprecated]] auto deh_procStringSub(char*, char*, char*, std::FILE*) -> bool;
[[deprecated]] auto dehReformatStr(char*) -> char*;

void lfstrip(std::string&);                             // strip the \r and/or \n off of a line
void rstrip(std::string&);                              // strip trailing whitespace
auto ptr_lstrip(std::string_view) -> std::string_view;  // point past leading whitespace
auto deh_GetData(char* s, char* k, uint_64_t& l, char** strval, std::FILE* fpout) -> int;
auto deh_GetData(std::string_view s, std::string& k, uint_64_t& l, std::string* strval, std::FILE* fpout) -> int;
auto deh_procStringSub(std::optional<std::string_view> key,
                       std::optional<std::string_view> lookfor,
                       std::string_view newstring,
                       std::FILE* fpout) -> bool;
auto dehReformatStr(const std::string_view& string) -> std::string;

// Prototypes for block processing functions
// Pointers to these functions are used as the blocks are encountered.

namespace {
void deh_procThing(DEHFILE& fpin, std::FILE* fpout, std::string_view line);
void deh_procFrame(DEHFILE&, std::FILE*, std::string_view);
void deh_procPointer(DEHFILE&, std::FILE*, std::string_view);
void deh_procSounds(DEHFILE&, std::FILE*, std::string_view);
void deh_procAmmo(DEHFILE&, std::FILE*, std::string_view);
void deh_procWeapon(DEHFILE&, std::FILE*, std::string_view);
void deh_procSprite(DEHFILE&, std::FILE*, std::string_view);
void deh_procCheat(DEHFILE&, std::FILE*, std::string_view);
void deh_procMisc(DEHFILE&, std::FILE*, std::string_view);
void deh_procText(DEHFILE&, std::FILE*, std::string_view);
void deh_procPars(DEHFILE&, std::FILE*, std::string_view);
void deh_procStrings(DEHFILE&, std::FILE*, std::string_view);
void deh_procError(DEHFILE&, std::FILE*, std::string_view);
void deh_procBexCodePointers(DEHFILE&, std::FILE*, std::string_view);
void deh_procHelperThing(DEHFILE&, std::FILE*, std::string_view);  // haleyjd 9/22/99
// haleyjd: handlers to fully deprecate the DeHackEd text section
void deh_procBexSounds(DEHFILE&, std::FILE*, std::string_view);
void deh_procBexMusic(DEHFILE&, std::FILE*, std::string_view);
void deh_procBexSprites(DEHFILE&, std::FILE*, std::string_view);
}  // namespace

// Structure deh_block is used to hold the block names that can
// be encountered, and the routines to use to decipher them

struct deh_block {
  const char* key;                                             // a mnemonic block code name // CPhipps - const*
  void (*const fptr)(DEHFILE&, std::FILE*, std::string_view);  // handler
};

constexpr auto DEH_BUFFERMAX = 1024;  // input buffer area size, hardcodedfor now
constexpr auto DEH_MAXKEYLEN = 32;    // as much of any key as we'll look at
constexpr auto DEH_MOBJINFOMAX = 26;  // number of ints in the mobjinfo_t structure (!)

namespace {
// Put all the block header values, and the function to be called when that
// one is encountered, in this array:
constexpr std::array<const deh_block, 18> deh_blocks = {
    // CPhipps - static const
    /* 0 */ deh_block{"Thing", deh_procThing},
    /* 1 */ deh_block{"Frame", deh_procFrame},
    /* 2 */ deh_block{"Pointer", deh_procPointer},
    /* 3 */ deh_block{"Sound", deh_procSounds},  // Ty 03/16/98 corrected from "Sounds"
    /* 4 */ deh_block{"Ammo", deh_procAmmo},
    /* 5 */ deh_block{"Weapon", deh_procWeapon},
    /* 6 */ deh_block{"Sprite", deh_procSprite},
    /* 7 */ deh_block{"Cheat", deh_procCheat},
    /* 8 */ deh_block{"Misc", deh_procMisc},
    /* 9 */ deh_block{"Text", deh_procText},  // --  end of standard "deh" entries,

    //     begin BOOM Extensions (BEX)

    /* 10 */ deh_block{"[STRINGS]", deh_procStrings},          // new string changes
    /* 11 */ deh_block{"[PARS]", deh_procPars},                // alternative block marker
    /* 12 */ deh_block{"[CODEPTR]", deh_procBexCodePointers},  // bex codepointers by mnemonic
    /* 13 */ deh_block{"[HELPER]", deh_procHelperThing},       // helper thing substitution haleyjd 9/22/99
    /* 14 */ deh_block{"[SPRITES]", deh_procBexSprites},       // bex style sprites
    /* 15 */ deh_block{"[SOUNDS]", deh_procBexSounds},         // bex style sounds
    /* 16 */ deh_block{"[MUSIC]", deh_procBexMusic},           // bex style music
    /* 17 */ deh_block{"", deh_procError}                      // dummy to handle anything else
};

// flag to skip included deh-style text, used with INCLUDE NOTEXT directive
bool includenotext = false;

// MOBJINFO - Dehacked block name = "Thing"
// Usage: Thing nn (name)
// These are for mobjinfo_t types.  Each is an integer
// within the structure, so we can use index of the string in this
// array to offset by sizeof(int) into the mobjinfo_t array at [nn]
// * things are base zero but dehacked considers them to start at #1. ***
// CPhipps - static const

constexpr std::array<std::string_view, DEH_MOBJINFOMAX> deh_mobjinfo = {
    "ID #",                // .doomednum
    "Initial frame",       // .spawnstate
    "Hit points",          // .spawnhealth
    "First moving frame",  // .seestate
    "Alert sound",         // .seesound
    "Reaction time",       // .reactiontime
    "Attack sound",        // .attacksound
    "Injury frame",        // .painstate
    "Pain chance",         // .painchance
    "Pain sound",          // .painsound
    "Close attack frame",  // .meleestate
    "Far attack frame",    // .missilestate
    "Death frame",         // .deathstate
    "Exploding frame",     // .xdeathstate
    "Death sound",         // .deathsound
    "Speed",               // .speed
    "Width",               // .radius
    "Height",              // .height
    "Mass",                // .mass
    "Missile damage",      // .damage
    "Action sound",        // .activesound
    "Bits",                // .flags
    "Bits2",               // .flags
    "Respawn frame",       // .raisestate
    "Dropped item",        // .droppeditem
    "Blood color",         // .bloodcolor
};

// Strings that are used to indicate flags ("Bits" in mobjinfo)
// This is an array of bit masks that are related to p_mobj.h
// values, using the smae names without the MF_ in front.
// Ty 08/27/98 new code
//
// killough 10/98:
//
// Convert array to struct to allow multiple values, make array size variable

struct deh_mobjflags_s {
  const char* name;  // CPhipps - const*
  uint_64_t value;
};

// CPhipps - static const
constexpr std::array<const deh_mobjflags_s, 37> deh_mobjflags = {
    deh_mobjflags_s{"SPECIAL", MF_SPECIAL},            // call  P_Specialthing when touched
    deh_mobjflags_s{"SOLID", MF_SOLID},                // block movement
    deh_mobjflags_s{"SHOOTABLE", MF_SHOOTABLE},        // can be hit
    deh_mobjflags_s{"NOSECTOR", MF_NOSECTOR},          // invisible but touchable
    deh_mobjflags_s{"NOBLOCKMAP", MF_NOBLOCKMAP},      // inert but displayable
    deh_mobjflags_s{"AMBUSH", MF_AMBUSH},              // deaf monster
    deh_mobjflags_s{"JUSTHIT", MF_JUSTHIT},            // will try to attack right back
    deh_mobjflags_s{"JUSTATTACKED", MF_JUSTATTACKED},  // take at least 1 step before attacking
    deh_mobjflags_s{"SPAWNCEILING", MF_SPAWNCEILING},  // initially hang from ceiling
    deh_mobjflags_s{"NOGRAVITY", MF_NOGRAVITY},        // don't apply gravity during play
    deh_mobjflags_s{"DROPOFF", MF_DROPOFF},            // can jump from high places
    deh_mobjflags_s{"PICKUP", MF_PICKUP},              // will pick up items
    deh_mobjflags_s{"NOCLIP", MF_NOCLIP},              // goes through walls
    deh_mobjflags_s{"SLIDE", MF_SLIDE},                // keep info about sliding along walls
    deh_mobjflags_s{"FLOAT", MF_FLOAT},                // allow movement to any height
    deh_mobjflags_s{"TELEPORT", MF_TELEPORT},          // don't cross lines or look at heights
    deh_mobjflags_s{"MISSILE", MF_MISSILE},            // don't hit same species, explode on block
    deh_mobjflags_s{"DROPPED", MF_DROPPED},            // dropped, not spawned (like ammo clip)
    deh_mobjflags_s{"SHADOW", MF_SHADOW},              // use fuzzy draw like spectres
    deh_mobjflags_s{"NOBLOOD", MF_NOBLOOD},            // puffs instead of blood when shot
    deh_mobjflags_s{"CORPSE", MF_CORPSE},              // so it will slide down steps when dead
    deh_mobjflags_s{"INFLOAT", MF_INFLOAT},            // float but not to target height
    deh_mobjflags_s{"COUNTKILL", MF_COUNTKILL},        // count toward the kills total
    deh_mobjflags_s{"COUNTITEM", MF_COUNTITEM},        // count toward the items total
    deh_mobjflags_s{"SKULLFLY", MF_SKULLFLY},          // special handling for flying skulls
    deh_mobjflags_s{"NOTDMATCH", MF_NOTDMATCH},        // do not spawn in deathmatch

    // killough 10/98: TRANSLATION consists of 2 bits, not 1:

    deh_mobjflags_s{"TRANSLATION", MF_TRANSLATION1},   // for Boom bug-compatibility
    deh_mobjflags_s{"TRANSLATION1", MF_TRANSLATION1},  // use translation table for color (players)
    deh_mobjflags_s{"TRANSLATION2", MF_TRANSLATION2},  // use translation table for color (players)
    deh_mobjflags_s{"UNUSED1", MF_TRANSLATION2},       // unused bit # 1 -- For Boom bug-compatibility
    deh_mobjflags_s{"UNUSED2", MF_UNUSED2},            // unused bit # 2 -- For Boom compatibility
    deh_mobjflags_s{"UNUSED3", MF_UNUSED3},            // unused bit # 3 -- For Boom compatibility
    deh_mobjflags_s{"UNUSED4", MF_TRANSLUCENT},        // unused bit # 4 -- For Boom compatibility
    deh_mobjflags_s{"TRANSLUCENT", MF_TRANSLUCENT},    // apply translucency to sprite (BOOM)
    deh_mobjflags_s{"TOUCHY", MF_TOUCHY},              // dies on contact with solid objects (MBF)
    deh_mobjflags_s{"BOUNCES", MF_BOUNCES},            // bounces off floors, ceilings and maybe walls (MBF)
    deh_mobjflags_s{"FRIEND", MF_FRIEND},              // a friend of the player(s) (MBF)
};

// STATE - Dehacked block name = "Frame" and "Pointer"
// Usage: Frame nn
// Usage: Pointer nn (Frame nn)
// These are indexed separately, for lookup to the actual
// function pointers.  Here we'll take whatever Dehacked gives
// us and go from there.  The (Frame nn) after the pointer is the
// real place to put this value.  The "Pointer" value is an xref
// that Dehacked uses and is useless to us.
// * states are base zero and have a dummy #0 (TROO)

constexpr std::array<std::string_view, 7> deh_state{{
    // CPhipps - static const*
    "Sprite number",     // .sprite (spritenum_t) // an enum
    "Sprite subnumber",  // .frame (long)
    "Duration",          // .tics (long)
    "Next frame",        // .nextstate (statenum_t)
    // This is set in a separate "Pointer" block from Dehacked
    "Codep Frame",  // pointer to first use of action (actionf_t)
    "Unknown 1",    // .misc1 (long)
    "Unknown 2"     // .misc2 (long)
}};

// SFXINFO_STRUCT - Dehacked block name = "Sounds"
// Sound effects, typically not changed (redirected, and new sfx put
// into the pwad, but not changed here.  Can you tell that Gregdidn't
// know what they were for, mostly?  Can you tell that I don't either?
// Mostly I just put these into the same slots as they are in the struct.
// This may not be supported in our -deh option if it doesn't make sense by then.

// * sounds are base zero but have a dummy #0

constexpr std::array<std::string_view, 9> deh_sfxinfo{{
    // CPhipps - static const*
    "Offset",      // pointer to a name string, changed in text
    "Zero/One",    // .singularity (int, one at a time flag)
    "Value",       // .priority
    "Zero 1",      // .link (sfxinfo_t*) referenced sound if linked
    "Zero 2",      // .pitch
    "Zero 3",      // .volume
    "Zero 4",      // .data (SAMPLE*) sound data
    "Neg. One 1",  // .usefulness
    "Neg. One 2"   // .lumpnum
}};

// MUSICINFO is not supported in Dehacked.  Ignored here.
// * music entries are base zero but have a dummy #0

// SPRITE - Dehacked block name = "Sprite"
// Usage = Sprite nn
// Sprite redirection by offset into the text area - unsupported by BOOM
// * sprites are base zero and dehacked uses it that way.

// static const char *deh_sprite[] = // CPhipps - static const*
// {
//   "Offset"      // supposed to be the offset into the text section
// };

// AMMO - Dehacked block name = "Ammo"
// usage = Ammo n (name)
// Ammo information for the few types of ammo

constexpr std::array<std::string_view, 2> deh_ammo{{
    // CPhipps - static const*
    "Max ammo",  // maxammo[]
    "Per ammo"   // clipammo[]
}};

// WEAPONS - Dehacked block name = "Weapon"
// Usage: Weapon nn (name)
// Basically a list of frames and what kind of ammo (see above)it uses.

constexpr std::array<std::string_view, 6> deh_weapon{{
    // CPhipps - static const*
    "Ammo type",       // .ammo
    "Deselect frame",  // .upstate
    "Select frame",    // .downstate
    "Bobbing frame",   // .readystate
    "Shooting frame",  // .atkstate
    "Firing frame"     // .flashstate
}};

// CHEATS - Dehacked block name = "Cheat"
// Usage: Cheat 0
// Always uses a zero in the dehacked file, for consistency.  No meaning.
// These are just plain funky terms compared with id's
//
// killough 4/18/98: integrated into main cheat table now (see st_stuff.c)

// MISC - Dehacked block name = "Misc"
// Usage: Misc 0
// Always uses a zero in the dehacked file, for consistency.  No meaning.

constexpr std::array<std::string_view, 16> deh_misc{{
    // CPhipps - static const*
    "Initial Health",     // initial_health
    "Initial Bullets",    // initial_bullets
    "Max Health",         // maxhealth
    "Max Armor",          // max_armor
    "Green Armor Class",  // green_armor_class
    "Blue Armor Class",   // blue_armor_class
    "Max Soulsphere",     // max_soul
    "Soulsphere Health",  // soul_health
    "Megasphere Health",  // mega_health
    "God Mode Health",    // god_health
    "IDFA Armor",         // idfa_armor
    "IDFA Armor Class",   // idfa_armor_class
    "IDKFA Armor",        // idkfa_armor
    "IDKFA Armor Class",  // idkfa_armor_class
    "BFG Cells/Shot",     // BFGCELLS
    "Monsters Infight"    // Unknown--not a specific number it seems, but
                          // the logic has to be here somewhere or
                          // it'd happen always
}};

// TEXT - Dehacked block name = "Text"
// Usage: Text fromlen tolen
// Dehacked allows a bit of adjustment to the length (why?)

// BEX extension [CODEPTR]
// Usage: Start block, then each line is:
// FRAME nnn = PointerMnemonic

struct deh_bexptr {
  actionf_t cptr;      // actual pointer to the subroutine
  const char* lookup;  // mnemonic lookup string to be specified in BEX
  // CPhipps - const*
};

const std::array<const deh_bexptr, 88> deh_bexptrs = {
    // CPhipps - static const
    deh_bexptr{reinterpret_cast<actionf_t>(A_Light0), "A_Light0"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_WeaponReady), "A_WeaponReady"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Lower), "A_Lower"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Raise), "A_Raise"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Punch), "A_Punch"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_ReFire), "A_ReFire"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FirePistol), "A_FirePistol"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Light1), "A_Light1"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FireShotgun), "A_FireShotgun"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Light2), "A_Light2"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FireShotgun2), "A_FireShotgun2"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_CheckReload), "A_CheckReload"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_OpenShotgun2), "A_OpenShotgun2"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_LoadShotgun2), "A_LoadShotgun2"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_CloseShotgun2), "A_CloseShotgun2"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FireCGun), "A_FireCGun"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_GunFlash), "A_GunFlash"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FireMissile), "A_FireMissile"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Saw), "A_Saw"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FirePlasma), "A_FirePlasma"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BFGsound), "A_BFGsound"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FireBFG), "A_FireBFG"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BFGSpray), "A_BFGSpray"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Explode), "A_Explode"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Pain), "A_Pain"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_PlayerScream), "A_PlayerScream"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Fall), "A_Fall"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_XScream), "A_XScream"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Look), "A_Look"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Chase), "A_Chase"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FaceTarget), "A_FaceTarget"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_PosAttack), "A_PosAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Scream), "A_Scream"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_SPosAttack), "A_SPosAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_VileChase), "A_VileChase"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_VileStart), "A_VileStart"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_VileTarget), "A_VileTarget"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_VileAttack), "A_VileAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_StartFire), "A_StartFire"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Fire), "A_Fire"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FireCrackle), "A_FireCrackle"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Tracer), "A_Tracer"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_SkelWhoosh), "A_SkelWhoosh"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_SkelFist), "A_SkelFist"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_SkelMissile), "A_SkelMissile"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FatRaise), "A_FatRaise"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FatAttack1), "A_FatAttack1"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FatAttack2), "A_FatAttack2"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_FatAttack3), "A_FatAttack3"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BossDeath), "A_BossDeath"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_CPosAttack), "A_CPosAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_CPosRefire), "A_CPosRefire"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_TroopAttack), "A_TroopAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_SargAttack), "A_SargAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_HeadAttack), "A_HeadAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BruisAttack), "A_BruisAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_SkullAttack), "A_SkullAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Metal), "A_Metal"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_SpidRefire), "A_SpidRefire"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BabyMetal), "A_BabyMetal"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BspiAttack), "A_BspiAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Hoof), "A_Hoof"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_CyberAttack), "A_CyberAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_PainAttack), "A_PainAttack"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_PainDie), "A_PainDie"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_KeenDie), "A_KeenDie"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BrainPain), "A_BrainPain"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BrainScream), "A_BrainScream"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BrainDie), "A_BrainDie"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BrainAwake), "A_BrainAwake"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BrainSpit), "A_BrainSpit"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_SpawnSound), "A_SpawnSound"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_SpawnFly), "A_SpawnFly"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_BrainExplode), "A_BrainExplode"},
    deh_bexptr{reinterpret_cast<actionf_t>(A_Detonate), "A_Detonate"},      // killough 8/9/98
    deh_bexptr{reinterpret_cast<actionf_t>(A_Mushroom), "A_Mushroom"},      // killough 10/98
    deh_bexptr{reinterpret_cast<actionf_t>(A_Die), "A_Die"},                // killough 11/98
    deh_bexptr{reinterpret_cast<actionf_t>(A_Spawn), "A_Spawn"},            // killough 11/98
    deh_bexptr{reinterpret_cast<actionf_t>(A_Turn), "A_Turn"},              // killough 11/98
    deh_bexptr{reinterpret_cast<actionf_t>(A_Face), "A_Face"},              // killough 11/98
    deh_bexptr{reinterpret_cast<actionf_t>(A_Scratch), "A_Scratch"},        // killough 11/98
    deh_bexptr{reinterpret_cast<actionf_t>(A_PlaySound), "A_PlaySound"},    // killough 11/98
    deh_bexptr{reinterpret_cast<actionf_t>(A_RandomJump), "A_RandomJump"},  // killough 11/98
    deh_bexptr{reinterpret_cast<actionf_t>(A_LineEffect), "A_LineEffect"},  // killough 11/98

    deh_bexptr{reinterpret_cast<actionf_t>(A_FireOldBFG),
               "A_FireOldBFG"},  // killough 7/19/98: classic BFG firing function
    {reinterpret_cast<actionf_t>(A_BetaSkullAttack),
     "A_BetaSkullAttack"},  // killough 10/98: beta lost souls attacked different
    deh_bexptr{reinterpret_cast<actionf_t>(A_Stop), "A_Stop"},

    // This NULL entry must be the last in the list
    deh_bexptr{nullptr, "A_NULL"},  // Ty 05/16/98
};

// to hold startup code pointers from INFO.C
// CPhipps - static
std::array<actionf_t, NUMSTATES> deh_codeptr;
}  // namespace

// haleyjd: support for BEX SPRITES, SOUNDS, and MUSIC
std::array<std::optional<std::basic_string<char, std::char_traits<char>, z_allocator<char>>>, NUMSPRITES>
    deh_spritenames;
std::array<std::optional<std::basic_string<char, std::char_traits<char>, z_allocator<char>>>, NUMMUSIC> deh_musicnames;
std::array<std::optional<std::basic_string<char, std::char_traits<char>, z_allocator<char>>>, NUMSFX> deh_soundnames;

void D_BuildBEXTables() {
  // moved from ProcessDehFile, then we don't need the static int i
  for (int i = 0; i < EXTRASTATES; i++) {  // remember what they start as for deh xref
    deh_codeptr[i] = states[i].action;
  }

  // initialize extra dehacked states
  for (int i = EXTRASTATES; i < NUMSTATES; i++) {
    states[i].sprite = SPR_TNT1;
    states[i].frame = 0;
    states[i].tics = -1;
    states[i].action = nullptr;
    states[i].nextstate = static_cast<statenum_t>(i);
    states[i].misc1 = 0;
    states[i].misc2 = 0;
    deh_codeptr[i] = states[i].action;
  }

  for (int i = 0; i < NUMSPRITES; i++) {
    deh_spritenames[i] = sprnames[i];
  }

  for (int i = 1; i < NUMMUSIC; i++) {
    deh_musicnames[i] = S_music[i].name;
  }

  deh_musicnames[0] = std::nullopt;

  for (int i = 1; i < NUMSFX; i++) {
    if (S_sfx[i].name != nullptr) {
      deh_soundnames[i] = S_sfx[i].name;
    } else {  // This is possible due to how DEHEXTRA has turned S_sfx into a sparse array
      deh_soundnames[i] = std::nullopt;
    }
  }
  deh_soundnames[0] = std::nullopt;

  // ferk: initialize Thing extra properties (keeping vanilla props in info.c)
  for (int i = 0; i < NUMMOBJTYPES; i++) {
    // mobj id for item dropped on death
    switch (i) {
      case MT_WOLFSS:
      case MT_POSSESSED:
        mobjinfo[i].droppeditem = MT_CLIP;
        break;
      case MT_SHOTGUY:
        mobjinfo[i].droppeditem = MT_SHOTGUN;
        break;
      case MT_CHAINGUY:
        mobjinfo[i].droppeditem = MT_CHAINGUN;
        break;
      default:
        mobjinfo[i].droppeditem = MT_NULL;
    }

    // [FG] colored blood and gibs
    switch (i) {
      case MT_HEAD:
        mobjinfo[i].bloodcolor = 3;  // Blue
        break;
      case MT_BRUISER:
      case MT_KNIGHT:
        mobjinfo[i].bloodcolor = 2;  // Green
        break;
      default:
        mobjinfo[i].bloodcolor = 0;  // Red (normal)
    }
  }
}

int deh_maxhealth;
int deh_max_soul;
int deh_mega_health;

bool IsDehMaxHealth = false;
bool IsDehMaxSoul = false;
bool IsDehMegaHealth = false;
std::array<bool, NUMMOBJTYPES> DEH_mobjinfo_bits;

void deh_changeCompTranslucency() {
  static const std::array<int, 17> predefined_translucency = {
      MT_FIRE,      MT_SMOKE, MT_FATSHOT, MT_BRUISERSHOT, MT_SPAWNFIRE, MT_TROOPSHOT, MT_HEADSHOT, MT_PLASMA, MT_BFG,
      MT_ARACHPLAZ, MT_PUFF,  MT_TFOG,    MT_IFOG,        MT_MISC12,    MT_INV,       MT_INS,      MT_MEGA};

  for (int i : predefined_translucency) {
    if (!DEH_mobjinfo_bits[i]) {
      if (default_comp[comp_translucency] != 0) {
        mobjinfo[i].flags &= ~MF_TRANSLUCENT;
      } else {
        mobjinfo[i].flags |= MF_TRANSLUCENT;
      }
    }
  }
}

void deh_applyCompatibility() {
  const int comp_max = (compatibility_level == doom_12_compatibility ? 199 : 200);

  max_soul = (IsDehMaxSoul ? deh_max_soul : comp_max);
  mega_health = (IsDehMegaHealth ? deh_mega_health : comp_max);

  if (comp[comp_maxhealth] != 0) {
    maxhealth = 100;
    maxhealthbonus = (IsDehMaxHealth ? deh_maxhealth : comp_max);
  } else {
    maxhealth = (IsDehMaxHealth ? deh_maxhealth : 100);
    maxhealthbonus = maxhealth * 2;
  }

  if (!DEH_mobjinfo_bits[MT_SKULL]) {
    if (compatibility_level == doom_12_compatibility) {
      mobjinfo[MT_SKULL].flags |= (MF_COUNTKILL);
    } else {
      mobjinfo[MT_SKULL].flags &= ~(MF_COUNTKILL);
    }
  }

  if (compatibility_level == doom_12_compatibility) {
    // Spiderdemon is not fullbright when attacking in versions before v1.4
    states[S_SPID_ATK1].frame &= ~FF_FULLBRIGHT;
    states[S_SPID_ATK2].frame &= ~FF_FULLBRIGHT;
    states[S_SPID_ATK3].frame &= ~FF_FULLBRIGHT;
    states[S_SPID_ATK4].frame &= ~FF_FULLBRIGHT;

    // Powerups are not fullbright in v1.2
    // Soulsphere fullbright since v1.25s, the rest since v1.4
    states[S_SOUL].frame &= ~FF_FULLBRIGHT;
    states[S_SOUL2].frame &= ~FF_FULLBRIGHT;
    states[S_SOUL3].frame &= ~FF_FULLBRIGHT;
    states[S_SOUL4].frame &= ~FF_FULLBRIGHT;
    states[S_SOUL5].frame &= ~FF_FULLBRIGHT;
    states[S_SOUL6].frame &= ~FF_FULLBRIGHT;
    states[S_PINV].frame &= ~FF_FULLBRIGHT;
    states[S_PINV2].frame &= ~FF_FULLBRIGHT;
    states[S_PINV3].frame &= ~FF_FULLBRIGHT;
    states[S_PINV4].frame &= ~FF_FULLBRIGHT;
    states[S_PSTR].frame &= ~FF_FULLBRIGHT;
    states[S_PINS].frame &= ~FF_FULLBRIGHT;
    states[S_PINS2].frame &= ~FF_FULLBRIGHT;
    states[S_PINS3].frame &= ~FF_FULLBRIGHT;
    states[S_PINS4].frame &= ~FF_FULLBRIGHT;
    states[S_SUIT].frame &= ~FF_FULLBRIGHT;
    states[S_PMAP].frame &= ~FF_FULLBRIGHT;
    states[S_PMAP2].frame &= ~FF_FULLBRIGHT;
    states[S_PMAP3].frame &= ~FF_FULLBRIGHT;
    states[S_PMAP4].frame &= ~FF_FULLBRIGHT;
    states[S_PMAP5].frame &= ~FF_FULLBRIGHT;
    states[S_PMAP6].frame &= ~FF_FULLBRIGHT;
  }

  deh_changeCompTranslucency();
}

// ====================================================================
// ProcessDehFile
// Purpose: Read and process a DEH or BEX file
// Args:    filename    -- name of the DEH/BEX file
//          outfilename -- output file (DEHOUT.TXT), appended to here
// Returns: void
//
// killough 10/98:
// substantially modified to allow input from wad lumps instead of .deh files.
namespace {
void ProcessDehFile(std::optional<std::string_view> filename,
                    const std::optional<std::string_view> outfilename,
                    const int lumpnum) {
  static std::FILE* fileout;  // In case -dehout was used

  // Open output file if we're writing output
  if (outfilename && !outfilename->empty() && fileout == nullptr) {
    static bool firstfile = true;  // to allow append to output log
    if (outfilename == "-") {
      fileout = stdout;
    } else if ((fileout = M_fopen(outfilename->data(), firstfile ? "wt" : "at")) == nullptr) {
      lprint(LO_WARN, "Could not open -dehout file {}\n... using stdout.\n", *outfilename);
      fileout = stdout;
    }

    firstfile = false;
  }

  // killough 10/98: allow DEH files to come from wad lumps
  DEHFILE infile;
  DEHFILE* filein = &infile;
  std::string_view file_or_lump;
  if (filename) {
    if ((infile.f = M_fopen(filename->data(), "rt")) == nullptr) {
      lprint(LO_WARN, "-deh file {} not found\n", *filename);
      return;  // should be checked up front anyway
    }

    infile.lump = nullptr;
    file_or_lump = "file";
  } else {  // DEH file comes from lump indicated by third argument
    infile.size = W_LumpLength(lumpnum);
    infile.inp = infile.lump = static_cast<const byte*>(W_CacheLumpNum(lumpnum));

    // [FG] skip empty DEHACKED lumps
    if (infile.inp == nullptr) {
      lprint(LO_WARN, "skipping empty DEHACKED ({}) lump\n", lumpnum);
      return;
    }

    filename = lumpinfo[lumpnum].wadfile->name;
    file_or_lump = "lump from";
  }

  lprint(LO_INFO, "Loading DEH {} {}\n", file_or_lump, *filename);
  if (fileout != nullptr) {
    print(fileout, "\nLoading DEH {} {}\n\n", file_or_lump, *filename);
  }

  // move deh_codeptr initialisation to D_BuildBEXTables

  // loop until end of file
  static unsigned last_i = deh_blocks.size() - 1;
  static long filepos = 0;

  char inbuffer[DEH_BUFFERMAX];  // Place to put the primary infostring
  while (dehfgets(inbuffer, sizeof(inbuffer), filein) != nullptr) {
    lfstrip(inbuffer);
    if (fileout != nullptr) {
      print(fileout, "Line='{}'\n", inbuffer);
    }

    if (inbuffer[0] == '\0' || inbuffer[0] == '#' || inbuffer[0] == ' ') {
      continue; /* Blank line or comment line */
    }

    // -- If DEH_BLOCKMAX is set right, the processing is independently
    // -- handled based on data in the deh_blocks[] structure array

    // killough 10/98: INCLUDE code rewritten to allow arbitrary nesting,
    // and to greatly simplify code, fix memory leaks, other bugs

    if (!strnicmp(inbuffer, "INCLUDE", 7)) {  // include a file
      // preserve state while including a file
      // killough 10/98: moved to here

      const bool oldnotext = includenotext;  // killough 10/98

      // killough 10/98: exclude if inside wads (only to discourage
      // the practice, since the code could otherwise handle it)

      if (infile.lump != nullptr) {
        if (fileout != nullptr) {
          print(fileout, "No files may be included from wads: {}\n", inbuffer);
        }

        continue;
      }

      // check for no-text directive, used when including a DEH
      // file but using the BEX format to handle strings
      char* nextfile;
      if (!strnicmp(nextfile = ptr_lstrip(inbuffer + 7), "NOTEXT", 6)) {
        includenotext = true, nextfile = ptr_lstrip(nextfile + 6);
      }

      if (fileout != nullptr) {
        print(fileout, "Branching to include file {}...\n", nextfile);
      }

      // killough 10/98:
      // Second argument must be NULL to prevent closing fileout too soon

      ProcessDehFile(nextfile, std::nullopt, 0);  // do the included file

      includenotext = oldnotext;
      if (fileout != nullptr) {
        print(fileout, "...continuing with {}\n", *filename);
      }

      continue;
    }

    bool match;
    unsigned i;
    for (match = false, i = 0; i < deh_blocks.size(); i++)
      if (!strncasecmp(inbuffer, deh_blocks[i].key, std::string_view{deh_blocks[i].key}.length())) {  // matches one
        if (i < deh_blocks.size() - 1) {
          match = true;
        }

        break;  // we got one, that's enough for this block
      }

    if (match) {  // inbuffer matches a valid block code name
      last_i = i;
    } else if (last_i >= 10 && last_i < deh_blocks.size() - 1) {  // restrict to BEX style lumps
      // process that same line again with the last valid block code handler
      i = last_i;
      dehfseek(filein, filepos);
    }

    if (fileout != nullptr) {
      print(fileout, "Processing function [{}] for {}\n", i, deh_blocks[i].key);
    }
    deh_blocks[i].fptr(*filein, fileout, inbuffer);  // call function

    filepos = dehftell(filein);  // back up line start
  }

  if (infile.lump != nullptr) {
    W_UnlockLumpNum(lumpnum);  // Mark purgable
  } else {
    std::fclose(infile.f);  // Close real file
  }

  if (outfilename) {  // killough 10/98: only at top recursion level
    if (fileout != stdout) {
      std::fclose(fileout);
    }

    fileout = nullptr;
  }

  deh_applyCompatibility();
}
}  // namespace

[[deprecated]] void ProcessDehFile(const char* const filename, const char* const outfilename, int lumpnum) {
  ProcessDehFile(filename != nullptr ? std::make_optional(filename) : std::nullopt,
                 outfilename != nullptr ? std::make_optional(outfilename) : std::nullopt, lumpnum);
}

namespace {
// ====================================================================
// deh_procBexCodePointers
// Purpose: Handle [CODEPTR] block, BOOM Extension
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procBexCodePointers(DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {
  char inbuffer[DEH_BUFFERMAX];

  // Ty 05/16/98 - initialize it to something, dummy!
  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  // for this one, we just read 'em until we hit a blank line
  while ((dehfeof(&fpin) == 0) && (inbuffer[0] != '\0' && inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98: really exit on blank line
    }

    // killough 8/98: allow hex numbers in input:
    char key[DEH_MAXKEYLEN];
    int indexnum;
    char mnemonic[DEH_MAXKEYLEN];  // to hold the codepointer mnemonic
    if ((3 != std::sscanf(inbuffer, "%s %i = %s", key, &indexnum, mnemonic))
        || (stricmp(key, "FRAME"))) {  // NOTE: different format from normal
      if (fpout != nullptr) {
        print(fpout, "Invalid BEX codepointer line - must start with 'FRAME': '{}'\n", inbuffer);
      }

      return;  // early return
    }

    if (fpout != nullptr) {
      print(fpout, "Processing pointer at index {}: {}\n", indexnum, mnemonic);
    }

    if (indexnum < 0 || indexnum >= NUMSTATES) {
      if (fpout != nullptr) {
        print(fpout, "Bad pointer number {} of {}\n", indexnum,
              static_cast<std::underlying_type_t<statenum_t>>(NUMSTATES));
      }

      return;  // killough 10/98: fix SegViol
    }
    std::strcpy(key, "A_");  // reusing the key area to prefix the mnemonic
    std::strcat(key, ptr_lstrip(mnemonic));

    bool found = false;  // know if we found this one during lookup or not
    int i = -1;          // looper - incremented to start at zero at the top of the loop
    do {                 // Ty 05/16/98 - fix loop logic to look for null ending entry
      ++i;
      if (!stricmp(key, deh_bexptrs[i].lookup)) {       // Ty 06/01/98  - add  to states[].action for new djgcc version
        states[indexnum].action = deh_bexptrs[i].cptr;  // assign
        if (fpout != nullptr) {
          print(fpout, " - applied {} from codeptr[{}] to states[{}]\n", deh_bexptrs[i].lookup, i, indexnum);
        }

        found = true;
      }
    } while (!found && (deh_bexptrs[i].cptr != nullptr));

    if (!found) {
      if (fpout != nullptr) {
        print(fpout, "Invalid frame pointer mnemonic '{}' at {}\n", mnemonic, indexnum);
      }
    }
  }
}

//---------------------------------------------------------------------------
// To be on the safe, compatible side, we manually convert DEH bitflags
// to prboom types - POPE
//---------------------------------------------------------------------------
auto getConvertedDEHBits(const uint_64_t bits) -> uint_64_t {
  static constexpr std::array<const uint_64_t, 32> bitMap = {
      /* cf linuxdoom-1.10 p_mobj.h */
      MF_SPECIAL,       // 0 Can be picked up - When touched the thing can be picked up.
      MF_SOLID,         // 1 Obstacle - The thing is solid and will not let you (or others) pass through it
      MF_SHOOTABLE,     // 2 Shootable - Can be shot.
      MF_NOSECTOR,      // 3 Total Invisibility - Invisible, but can be touched
      MF_NOBLOCKMAP,    // 4 Don't use the blocklinks (inert but displayable)
      MF_AMBUSH,        // 5 Semi deaf - The thing is a deaf monster
      MF_JUSTHIT,       // 6 In pain - Will try to attack right back after being hit
      MF_JUSTATTACKED,  // 7 Steps before attack - Will take at least one step before attacking
      MF_SPAWNCEILING,  // 8 Hangs from ceiling - When the level starts, this thing will be at ceiling height.
      MF_NOGRAVITY,     // 9 No gravity - Gravity does not affect this thing
      MF_DROPOFF,  // 10 Travels over cliffs - Monsters normally do not walk off ledges/steps they could not walk up.
                   // With this set they can walk off any height of cliff. Usually only used for flying monsters.
      MF_PICKUP,   // 11 Pick up items - The thing can pick up gettable items.
      MF_NOCLIP,   // 12 No clipping - Thing can walk through walls.
      MF_SLIDE,  // 13 Slides along walls - Keep info about sliding along walls (don't really know much about this one).
      MF_FLOAT,  // 14 Floating - Thing can move to any height
      MF_TELEPORT,   // 15 Semi no clipping - Don't cross lines or look at teleport heights. (don't really know much
                     // about this one either).
      MF_MISSILE,    // 16 Projectiles - Behaves like a projectile, explodes when hitting something that blocks movement
      MF_DROPPED,    // 17 Disappearing weapon - Dropped, not spawned (like an ammo clip) I have not had much success in
                     // using this one.
      MF_SHADOW,     // 18 Partial invisibility - Drawn like a spectre.
      MF_NOBLOOD,    // 19 Puffs (vs. bleeds) - If hit will spawn bullet puffs instead of blood splats.
      MF_CORPSE,     // 20 Sliding helpless - Will slide down steps when dead.
      MF_INFLOAT,    // 21 No auto levelling - float but not to target height (?)
      MF_COUNTKILL,  // 22 Affects kill % - counted as a killable enemy and affects percentage kills on level summary.
      MF_COUNTITEM,  // 23 Affects item % - affects percentage items gathered on level summary.
      MF_SKULLFLY,   // 24 Running - special handling for flying skulls.
      MF_NOTDMATCH,  // 25 Not in deathmatch - do not spawn in deathmatch (like keys)
      MF_TRANSLATION1,  // 26 Color 1 (grey / red)
      MF_TRANSLATION2,  // 27 Color 2 (brown / red)
      // Convert bit 28 to MF_TOUCHY, not (MF_TRANSLATION1|MF_TRANSLATION2)
      // fixes bug #1576151 (part 1)
      MF_TOUCHY,      // 28 - explodes on contact (MBF)
      MF_BOUNCES,     // 29 - bounces off walls and floors (MBF)
      MF_FRIEND,      // 30 - friendly monster helps players (MBF)
      MF_TRANSLUCENT  // e6y: Translucency via dehacked/bex doesn't work without it
  };

  uint_64_t shiftBits = bits;
  uint_64_t convertedBits = 0;
  for (auto bit : bitMap) {
    if ((shiftBits & 0x1) != 0) {
      convertedBits |= bit;
    }

    shiftBits >>= 1;
  }

  return convertedBits;
}

//---------------------------------------------------------------------------
// See usage below for an explanation of this function's existence - POPE
//---------------------------------------------------------------------------
void setMobjInfoValue(const int mobjInfoIndex, const int keyIndex, const uint_64_t value) {
  if (mobjInfoIndex >= NUMMOBJTYPES || mobjInfoIndex < 0) {
    return;
  }

  mobjinfo_t* const mi = &mobjinfo[mobjInfoIndex];
  switch (keyIndex) {
    case 0:
      mi->doomednum = static_cast<int>(value);
      return;
    case 1:
      mi->spawnstate = static_cast<int>(value);
      return;
    case 2:
      mi->spawnhealth = static_cast<int>(value);
      return;
    case 3:
      mi->seestate = static_cast<int>(value);
      return;
    case 4:
      mi->seesound = static_cast<int>(value);
      return;
    case 5:
      mi->reactiontime = static_cast<int>(value);
      return;
    case 6:
      mi->attacksound = static_cast<int>(value);
      return;
    case 7:
      mi->painstate = static_cast<int>(value);
      return;
    case 8:
      mi->painchance = static_cast<int>(value);
      return;
    case 9:
      mi->painsound = static_cast<int>(value);
      return;
    case 10:
      mi->meleestate = static_cast<int>(value);
      return;
    case 11:
      mi->missilestate = static_cast<int>(value);
      return;
    case 12:
      mi->deathstate = static_cast<int>(value);
      return;
    case 13:
      mi->xdeathstate = static_cast<int>(value);
      return;
    case 14:
      mi->deathsound = static_cast<int>(value);
      return;
    case 15:
      mi->speed = static_cast<int>(value);
      return;
    case 16:
      mi->radius = static_cast<int>(value);
      return;
    case 17:
      mi->height = static_cast<int>(value);
      return;
    case 18:
      mi->mass = static_cast<int>(value);
      return;
    case 19:
      mi->damage = static_cast<int>(value);
      return;
    case 20:
      mi->activesound = static_cast<int>(value);
      return;
    case 21:
      mi->flags = value;
      return;
    // e6y
    // Correction of wrong processing of "Respawn frame" entry.
    // There is no more synch on http://www.doomworld.com/sda/dwdemo/w303-115.zip
    // (with correction in PIT_CheckThing)
    case 22:
      if (prboom_comp[PC_FORCE_INCORRECT_PROCESSING_OF_RESPAWN_FRAME_ENTRY].state) {
        mi->raisestate = static_cast<int>(value);
        return;
      }
      break;
    case 23:
      if (!prboom_comp[PC_FORCE_INCORRECT_PROCESSING_OF_RESPAWN_FRAME_ENTRY].state) {
        mi->raisestate = static_cast<int>(value);
        return;
      }
      break;
    case 24:
      mi->droppeditem = static_cast<mobjtype_t>(value - 1);
      return;  // make it base zero (deh is 1-based)
    case 25:
      mi->bloodcolor = static_cast<int>(value);
      return;
    default:
      return;
  }
}

// ====================================================================
// deh_procThing
// Purpose: Handle DEH Thing block
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
// Ty 8/27/98 - revised to also allow mnemonics for
// bit masks for monster attributes
//
void deh_procThing(DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {
  char inbuffer[DEH_BUFFERMAX];

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);
  if (fpout != nullptr) {
    print(fpout, "Thing line: '{}'\n", inbuffer);
  }

  // killough 8/98: allow hex numbers in input:
  char key[DEH_MAXKEYLEN];
  int indexnum;
  int ix = std::sscanf(inbuffer, "%s %i", key, &indexnum);
  if (fpout != nullptr) {
    print(fpout, "count={}, Thing {}\n", ix, indexnum);
  }

  // Note that the mobjinfo[] array is base zero, but object numbers
  // in the dehacked file start with one.  Grumble.
  --indexnum;

  // now process the stuff
  // Note that for Things we can look up the key and use its offset
  // in the array of key strings as an int offset in the structure

  // get a line until a blank or end of file--it's not
  // blank now because it has our incoming key in it
  while ((dehfeof(&fpin) == 0) && inbuffer[0] && (inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    lfstrip(inbuffer);  // toss the end of line

    // killough 11/98: really bail out on blank lines (break != continue)
    if (inbuffer[0] == '\0') {
      break;  // bail out with blank line between sections
    }

    // e6y: Correction of wrong processing of Bits parameter if its value is equal to zero
    // No more desync on HACX demos.
    uint_64_t value;  // All deh values are ints or longs
    char* strval;
    const int bGetData = deh_GetData(inbuffer, key, &value, &strval, fpout);

    if (bGetData == 0) {
      // Old code: if (!deh_GetData(inbuffer,key,&value,&strval,fpout)) // returns TRUE if ok
      if (fpout != nullptr) {
        print(fpout, "Bad data pair in '{}'\n", inbuffer);
      }

      continue;
    }
    for (ix = 0; ix < DEH_MOBJINFOMAX; ix++) {
      if (deh_strcasecmp(key, deh_mobjinfo[ix]) != 0) {
        continue;
      }

      if (deh_strcasecmp(key, "Bits") != 0) {
        // standard value set

        // The old code here was the cause of a DEH-related bug in prboom.
        // When the mobjinfo_t.flags member was graduated to an int64, this
        // code was caught unawares and was indexing each property of the
        // mobjinfo as if it were still an int32. This caused sets of the
        // "raisestate" member to partially overwrite the "flags" member,
        // thus screwing everything up and making most DEH patches result in
        // unshootable enemy types. Moved to a separate function above
        // and stripped of all hairy struct address indexing. - POPE
        setMobjInfoValue(indexnum, ix, value);
      } else {
        // bit set
        // e6y: Correction of wrong processing of Bits parameter if its value is equal to zero
        // No more desync on HACX demos.
        if (bGetData == 1) {  // proff
          value = getConvertedDEHBits(value);
          mobjinfo[indexnum].flags = value;
          DEH_mobjinfo_bits[indexnum] = true;  // e6y: changed by DEH
        } else {
          // figure out what the bits are
          value = 0;

          // killough 10/98: replace '+' kludge with strtok() loop
          // Fix error-handling case ('found' var wasn't being reset)
          //
          // Use OR logic instead of addition, to allow repetition
          for (; (strval = std::strtok(strval, deh_getBitsDelims().data())) != nullptr; strval = nullptr) {
            std::size_t iy;
            for (iy = 0; iy < deh_mobjflags.size(); iy++) {
              if (deh_strcasecmp(strval, deh_mobjflags[iy].name)) {
                continue;
              }

              if (fpout != nullptr) {
                print(fpout, "ORed value {:#018x} {}\n", deh_mobjflags[iy].value, strval);
              }

              value |= deh_mobjflags[iy].value;
              break;
            }

            if (iy >= deh_mobjflags.size() && fpout != nullptr) {
              print(fpout, "Could not find bit mnemonic {}\n", strval);
            }
          }

          // Don't worry about conversion -- simply print values
          if (fpout != nullptr) {
            print(fpout, "Bits = {:#018x}\n", value);
          }

          mobjinfo[indexnum].flags = value;    // e6y
          DEH_mobjinfo_bits[indexnum] = true;  // e6y: changed by DEH
        }
      }

      if (fpout != nullptr) {
        print(fpout, "Assigned {:#018x} to {}({}) at index {}\n", value, key, indexnum, ix);
      }
    }
  }
}

// ====================================================================
// deh_procFrame
// Purpose: Handle DEH Frame block
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procFrame(DEHFILE& fpin, std::FILE* fpout, const std::string_view line) {
  char inbuffer[DEH_BUFFERMAX];

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  // killough 8/98: allow hex numbers in input:
  char key[DEH_MAXKEYLEN];
  int indexnum;
  std::sscanf(inbuffer, "%s %i", key, &indexnum);

  if (fpout != nullptr) {
    print(fpout, "Processing Frame at index {}: {}\n", indexnum, key);
  }

  if (indexnum < 0 || indexnum >= NUMSTATES) {
    if (fpout != nullptr) {
      print(fpout, "Bad frame number {} of {}\n", indexnum, static_cast<std::underlying_type_t<statenum_t>>(NUMSTATES));
    }
  }

  while ((dehfeof(&fpin) == 0) && (inbuffer[0] != '\0') && (inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) != nullptr) {

      lfstrip(inbuffer);
      if (inbuffer[0] == '\0') {
        break;  // killough 11/9
      }

      uint_64_t value;                                            // All deh values are ints or longs
      if (!deh_GetData(inbuffer, key, &value, nullptr, fpout)) {  // returns TRUE if ok
        if (fpout != nullptr) {
          print(fpout, "Bad data pair in '{}'\n", inbuffer);
        }

        continue;
      }
      if (deh_strcasecmp(key, deh_state[0]) == 0) {  // Sprite number
        if (fpout != nullptr) {
          print(fpout, " - sprite = {}\n", static_cast<long>(value));
        }

        states[indexnum].sprite = static_cast<spritenum_t>(value);
      } else if (deh_strcasecmp(key, deh_state[1]) == 0) {  // Sprite subnumber
        if (fpout != nullptr) {
          print(fpout, " - frame = {}\n", static_cast<long>(value));
        }

        states[indexnum].frame = static_cast<long>(value);  // long
      } else if (deh_strcasecmp(key, deh_state[2]) == 0) {  // Duration
        if (fpout != nullptr) {
          print(fpout, " - tics = {}\n", static_cast<long>(value));
        }

        states[indexnum].tics = static_cast<long>(value);   // long
      } else if (deh_strcasecmp(key, deh_state[3]) == 0) {  // Next frame
        if (fpout != nullptr) {
          print(fpout, " - nextstate = {}\n", static_cast<long>(value));
        }

        states[indexnum].nextstate = static_cast<statenum_t>(value);
      } else if (deh_strcasecmp(key, deh_state[4]) == 0) {  // Codep frame (not set in Frame deh block)
        if (fpout != nullptr) {
          print(fpout, " - codep, should not be set in Frame section!\n");
        }

        /* nop */
      } else if (deh_strcasecmp(key, deh_state[5]) == 0) {  // Unknown 1
        if (fpout != nullptr) {
          print(fpout, " - misc1 = {}\n", static_cast<long>(value));
        }

        states[indexnum].misc1 = static_cast<long>(value);  // long
      } else if (deh_strcasecmp(key, deh_state[6]) == 0) {  // Unknown 2
        if (fpout != nullptr) {
          print(fpout, " - misc2 = {}\n", static_cast<long>(value));
        }

        states[indexnum].misc2 = static_cast<long>(value);  // long
      } else if (fpout != nullptr) {
        print(fpout, "Invalid frame string index for '{}'\n", key);
      }
    } else {
      break;
    }
  }
}

// ====================================================================
// deh_procPointer
// Purpose: Handle DEH Code pointer block, can use BEX [CODEPTR] instead
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procPointer(DEHFILE& fpin, std::FILE* fpout, const std::string_view line) {  // done
  char inbuffer[DEH_BUFFERMAX];

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);
  // NOTE: different format from normal

  // killough 8/98: allow hex numbers in input, fix error case:
  char key[DEH_MAXKEYLEN];
  int indexnum;
  if (std::sscanf(inbuffer, "%*s %*i (%s %i)", key, &indexnum) != 2) {
    if (fpout != nullptr) {
      print(fpout, "Bad data pair in '{}'\n", inbuffer);
    }

    return;
  }

  if (fpout != nullptr) {
    print(fpout, "Processing Pointer at index {}: {}\n", indexnum, key);
  }

  if (indexnum < 0 || indexnum >= NUMSTATES) {
    if (fpout != nullptr) {
      print(fpout, "Bad pointer number {} of {}\n", indexnum,
            static_cast<std::underlying_type_t<statenum_t>>(NUMSTATES));
    }

    return;
  }

  while (dehfeof(&fpin) == 0 && inbuffer[0] != '\0' && inbuffer[0] != ' ') {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98
    }

    uint_64_t value;                                            // All deh values are ints or longs
    if (!deh_GetData(inbuffer, key, &value, nullptr, fpout)) {  // returns TRUE if ok
      if (fpout != nullptr) {
        print(fpout, "Bad data pair in '{}'\n", inbuffer);
      }

      continue;
    }

    if (value >= NUMSTATES) {
      if (fpout != nullptr) {
        print(fpout, "Bad pointer number {} of {}\n", static_cast<long>(value),
              static_cast<std::underlying_type_t<statenum_t>>(NUMSTATES));
      }

      return;
    }

    if (deh_strcasecmp(key, deh_state[4]) == 0) {  // Codep frame (not set in Frame deh block)
      states[indexnum].action = deh_codeptr[value];

      if (fpout != nullptr) {
        print(fpout, " - applied from codeptr[{}] to states[{}]\n", static_cast<long>(value), indexnum);
      }

      // Write BEX-oriented line to match:
      // for (i=0;i<NUMSTATES;i++) could go past the end of the array
      for (auto deh_bexptr : deh_bexptrs) {
        if (!std::memcmp(&deh_bexptr.cptr, &deh_codeptr[value], sizeof(actionf_t))) {
          if (fpout != nullptr) {
            print(fpout, "BEX [CODEPTR] -> FRAME {} = {}\n", indexnum, &deh_bexptr.lookup[2]);
          }

          break;
        }

        if (deh_bexptr.cptr == nullptr) {  // stop at null entry
          break;
        }
      }
    } else if (fpout != nullptr) {
      print(fpout, "Invalid frame pointer index for '{}' at {}\n", key, static_cast<long>(value));
    }
  }
}

// ====================================================================
// deh_procSounds
// Purpose: Handle DEH Sounds block
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procSounds(DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {
  char inbuffer[DEH_BUFFERMAX];

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  // killough 8/98: allow hex numbers in input:
  char key[DEH_MAXKEYLEN];
  int indexnum;
  std::sscanf(inbuffer, "%s %i", key, &indexnum);

  if (fpout != nullptr) {
    print(fpout, "Processing Sounds at index {}: {}\n", indexnum, key);
  }

  if (indexnum < 0 || indexnum >= NUMSFX) {
    if (fpout != nullptr) {
      print(fpout, "Bad sound number {} of {}\n", indexnum, static_cast<std::underlying_type_t<sfxenum_t>>(NUMSFX));
    }
  }

  while ((dehfeof(&fpin) == 0) && inbuffer[0] != '\0' && (inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98
    }

    uint_64_t value;                                            // All deh values are ints or longs
    if (!deh_GetData(inbuffer, key, &value, nullptr, fpout)) {  // returns TRUE if ok
      if (fpout != nullptr) {
        print(fpout, "Bad data pair in '{}'\n", inbuffer);
      }

      continue;
    }

    if (deh_strcasecmp(key, deh_sfxinfo[0]) == 0) {  // Offset
      // we don't know what this is, I don't think
    } else if (deh_strcasecmp(key, deh_sfxinfo[1]) == 0) {  // Zero/One
      S_sfx[indexnum].singularity = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_sfxinfo[2]) == 0) {  // Value
      S_sfx[indexnum].priority = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_sfxinfo[3]) == 0) {  // Zero 1
      // S_sfx[indexnum].link = (sfxinfo_t *)value;
      // .link - don't set pointers from DeHackEd
    } else if (deh_strcasecmp(key, deh_sfxinfo[4]) == 0) {  // Zero 2
      S_sfx[indexnum].pitch = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_sfxinfo[5]) == 0) {  // Zero 3
      S_sfx[indexnum].volume = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_sfxinfo[6]) == 0) {  // Zero 4
      // S_sfx[indexnum].data = (void *) value; // killough 5/3/98: changed cast
      // .data - don't set pointers from DeHackEd
    } else if (deh_strcasecmp(key, deh_sfxinfo[7]) == 0) {  // Neg. One 1
      S_sfx[indexnum].usefulness = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_sfxinfo[8]) == 0) {  // Neg. One 2
      S_sfx[indexnum].lumpnum = static_cast<int>(value);
    } else if (fpout != nullptr) {
      print(fpout, "Invalid sound string index for '{}'\n", key);
    }
  }
}

// ====================================================================
// deh_procAmmo
// Purpose: Handle DEH Ammo block
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procAmmo(DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {
  char inbuffer[DEH_BUFFERMAX];

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  // killough 8/98: allow hex numbers in input:
  char key[DEH_MAXKEYLEN];
  int indexnum;
  std::sscanf(inbuffer, "%s %i", key, &indexnum);

  if (fpout != nullptr) {
    print(fpout, "Processing Ammo at index {}: {}\n", indexnum, key);
  }

  if (indexnum < 0 || indexnum >= NUMAMMO) {
    if (fpout != nullptr) {
      print(fpout, "Bad ammo number {} of {}\n", indexnum, static_cast<std::underlying_type_t<ammotype_t>>(NUMAMMO));
    }
  }

  while ((dehfeof(&fpin) == 0) && inbuffer[0] != '\0' && inbuffer[0] != ' ') {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98
    }

    uint_64_t value;                                            // All deh values are ints or longs
    if (!deh_GetData(inbuffer, key, &value, nullptr, fpout)) {  // returns TRUE if ok
      if (fpout != nullptr) {
        print(fpout, "Bad data pair in '{}'\n", inbuffer);
      }

      continue;
    }

    if (deh_strcasecmp(key, deh_ammo[0]) == 0) {  // Max ammo
      maxammo[indexnum] = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_ammo[1]) == 0) {  // Per ammo
      clipammo[indexnum] = static_cast<int>(value);
    } else if (fpout != nullptr) {
      print(fpout, "Invalid ammo string index for '{}'\n", key);
    }
  }
}

// ====================================================================
// deh_procWeapon
// Purpose: Handle DEH Weapon block
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procWeapon(DEHFILE& fpin, std::FILE* fpout, const std::string_view line) {
  char inbuffer[DEH_BUFFERMAX];

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  // killough 8/98: allow hex numbers in input:
  char key[DEH_MAXKEYLEN];
  int indexnum;
  std::sscanf(inbuffer, "%s %i", key, &indexnum);

  if (fpout != nullptr) {
    print(fpout, "Processing Weapon at index {}: {}\n", indexnum, key);
  }

  if (indexnum < 0 || indexnum >= NUMWEAPONS) {
    if (fpout != nullptr) {
      print(fpout, "Bad weapon number {} of {}\n", indexnum, static_cast<std::underlying_type_t<ammotype_t>>(NUMAMMO));
    }
  }

  while ((dehfeof(&fpin) == 0) && inbuffer[0] != '\0' && (inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98
    }

    uint_64_t value;                                            // All deh values are ints or longs
    if (!deh_GetData(inbuffer, key, &value, nullptr, fpout)) {  // returns TRUE if ok
      if (fpout != nullptr) {
        print(fpout, "Bad data pair in '{}'\n", inbuffer);
      }

      continue;
    }

    if (deh_strcasecmp(key, deh_weapon[0]) == 0) {  // Ammo type
      weaponinfo[indexnum].ammo = static_cast<ammotype_t>(value);
    } else if (deh_strcasecmp(key, deh_weapon[1]) == 0) {  // Deselect frame
      weaponinfo[indexnum].upstate = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_weapon[2]) == 0) {  // Select frame
      weaponinfo[indexnum].downstate = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_weapon[3]) == 0) {  // Bobbing frame
      weaponinfo[indexnum].readystate = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_weapon[4]) == 0) {  // Shooting frame
      weaponinfo[indexnum].atkstate = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_weapon[5]) == 0) {  // Firing frame
      weaponinfo[indexnum].flashstate = static_cast<int>(value);
    } else if (fpout != nullptr) {
      print(fpout, "Invalid weapon string index for '{}'\n", key);
    }
  }
}

// ====================================================================
// deh_procSprite
// Purpose: Dummy - we do not support the DEH Sprite block
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procSprite(DEHFILE& fpin, std::FILE* fpout, const std::string_view line) {  // Not supported
  char inbuffer[DEH_BUFFERMAX];

  // Too little is known about what this is supposed to do, and
  // there are better ways of handling sprite renaming.  Not supported.
  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  // killough 8/98: allow hex numbers in input:
  char key[DEH_MAXKEYLEN];
  int indexnum;
  std::sscanf(inbuffer, "%s %i", key, &indexnum);

  if (fpout != nullptr) {
    print(fpout, "Ignoring Sprite offset change at index {}: {}\n", indexnum, key);
  }

  while ((dehfeof(&fpin) == 0) && inbuffer[0] != '\0' && (inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98
    }

    // ignore line
    if (fpout != nullptr) {
      print(fpout, "- {}\n", inbuffer);
    }
  }
}

// ====================================================================
// deh_procPars
// Purpose: Handle BEX extension for PAR times
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procPars(DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {  // extension
  char inbuffer[DEH_BUFFERMAX];

  // new item, par times
  // usage: After [PARS] Par 0 section identifier, use one or more of these
  // lines:
  //  par 3 5 120
  //  par 14 230
  // The first would make the par for E3M5 be 120 seconds, and the
  // second one makes the par for MAP14 be 230 seconds.  The number
  // of parameters on the line determines which group of par values
  // is being changed.  Error checking is done based on current fixed
  // array sizes of[4][10] and [32]

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  // killough 8/98: allow hex numbers in input:
  char key[DEH_MAXKEYLEN];
  int indexnum;
  std::sscanf(inbuffer, "%s %i", key, &indexnum);

  if (fpout != nullptr) {
    print(fpout, "Processing Par value at index {}: {}\n", indexnum, key);
  }

  // indexnum is a dummy entry
  while ((dehfeof(&fpin) == 0) && (inbuffer[0] != '\0') && (inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    lfstrip(M_Strlwr(inbuffer));  // lowercase it
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98
    }

    int episode, level, partime, oldpar;
    if (3 != std::sscanf(inbuffer, "par %i %i %i", &episode, &level, &partime)) {  // not 3
      if (2 != std::sscanf(inbuffer, "par %i %i", &level, &partime)) {             // not 2
        if (fpout != nullptr) {
          print(fpout, "Invalid par time setting string: {}\n", inbuffer);
        }
      } else {  // is 2
        // Ty 07/11/98 - wrong range check, not zero-based
        if (level < 1 || level > 32) {  // base 0 array (but 1-based parm)
          if (fpout != nullptr) {
            print(fpout, "Invalid MAPnn value MAP{}\n", level);
          }
        } else {
          oldpar = cpars[level - 1];

          if (fpout != nullptr) {
            print(fpout, "Changed par time for MAP{:02d} from {} to {}\n", level, oldpar, partime);
          }

          cpars[level - 1] = partime;
          deh_pars = true;
        }
      }
    } else {  // is 3
      // note that though it's a [4][10] array, the "left" and "top" aren't used,
      // effectively making it a base 1 array.
      // Ty 07/11/98 - level was being checked against max 3 - dumb error
      // Note that episode 4 does not have par times per original design
      // in Ultimate DOOM so that is not supported here.
      if (episode < 1 || episode > 3 || level < 1 || level > 9) {
        if (fpout != nullptr) {
          print(fpout, "Invalid ExMx values E{}M{}\n", episode, level);
        }
      } else {
        oldpar = pars[episode][level];
        pars[episode][level] = partime;

        if (fpout != nullptr) {
          print(fpout, "Changed par time for E{}M{} from {} to {}\n", episode, level, oldpar, partime);
        }

        deh_pars = true;
      }
    }
  }
}

// ====================================================================
// deh_procCheat
// Purpose: Handle DEH Cheat block
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procCheat(DEHFILE& fpin, std::FILE* fpout, const std::string_view line) {  // done

  if (fpout != nullptr) {
    print(fpout, "Processing Cheat: {}\n", line);
  }

  char inbuffer[DEH_BUFFERMAX];
  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);
  while ((dehfeof(&fpin) == 0) && inbuffer[0] != '\0' && (inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98
    }

    char key[DEH_MAXKEYLEN];
    uint_64_t value;                                            // All deh values are ints or longs
    char ch = 0;                                                // CPhipps - `writable' null string to initialise...
    char* strval = &ch;                                         // pointer to the value area
    if (!deh_GetData(inbuffer, key, &value, &strval, fpout)) {  // returns TRUE if ok
      if (fpout != nullptr) {
        print(fpout, "Bad data pair in '{}'\n", inbuffer);
      }

      continue;
    }

    // Otherwise we got a (perhaps valid) cheat name,
    // so look up the key in the array

    // killough 4/18/98: use main cheat code table in st_stuff.c now
    for (int ix = 0; cheat[ix].cheat; ix++) {
      if (cheat[ix].deh_cheat) {                   // killough 4/18/98: skip non-deh
        if (!stricmp(key, cheat[ix].deh_cheat)) {  // found the cheat, ignored case
          // replace it but don't overflow it.  Use current length as limit.
          // Ty 03/13/98 - add 0xff code
          // Deal with the fact that the cheats in deh files are extended
          // with character 0xFF to the original cheat length, which we don't do.
          int iy;
          for (iy = 0; strval[iy]; iy++) {
            strval[iy] = (strval[iy] == static_cast<char>(0xff)) ? '\0' : strval[iy];
          }

          iy = ix;  // killough 4/18/98

          // Ty 03/14/98 - skip leading spaces
          char* p = strval;  // utility pointer
          while (p[0] == ' ') {
            ++p;
          }

          // Ty 03/16/98 - change to use a strdup and orphan the original
          // Also has the advantage of allowing length changes.
          // std::strncpy(cheat[iy].cheat,p,strlen(cheat[iy].cheat));
#if 0
          {  // killough 9/12/98: disable cheats which are prefixes of this one
            int i;
            for (i = 0; cheat[i].cheat; i++)
              if (cheat[i].when & not_deh && !strncasecmp(cheat[i].cheat, cheat[iy].cheat, strlen(cheat[i].cheat))
                  && i != iy)
                cheat[i].deh_modified = true;
          }
#endif

          // e6y: ability to ignore cheats in dehacked files.
          if (deh_apply_cheats && M_CheckParm("-nocheats") == 0) {
            cheat[iy].cheat = strdup(p);
            if (fpout != nullptr) {
              print(fpout, "Assigned new cheat '{}' to cheat '{}' at index {}\n", p, cheat[ix].deh_cheat,
                    iy);  // killough 4/18/98
            }
          }
        }
      }
    }

    if (fpout != nullptr) {
      print(fpout, "- {}\n", inbuffer);
    }
  }
}

// ====================================================================
// deh_procMisc
// Purpose: Handle DEH Misc block
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procMisc(DEHFILE& fpin, std::FILE* fpout, const std::string_view line) {  // done
  char inbuffer[DEH_BUFFERMAX];

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  while ((dehfeof(&fpin) == 0) && inbuffer[0] != '\0' && (inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98
    }

    char key[DEH_MAXKEYLEN];
    uint_64_t value;                                            // All deh values are ints or longs
    if (!deh_GetData(inbuffer, key, &value, nullptr, fpout)) {  // returns TRUE if ok
      if (fpout != nullptr) {
        print(fpout, "Bad data pair in '{}'\n", inbuffer);
      }

      continue;
    }
    // Otherwise it's ok
    if (fpout != nullptr) {
      print(fpout, "Processing Misc item '{}'\n", key);
    }

    if (deh_strcasecmp(key, deh_misc[0]) == 0) {  // Initial Health
      initial_health = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[1]) == 0) {  // Initial Bullets
      initial_bullets = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[2]) == 0) {                // Max Health
      IsDehMaxHealth = true, deh_maxhealth = static_cast<int>(value);  // e6y
    } else if (deh_strcasecmp(key, deh_misc[3]) == 0) {                // Max Armor
      max_armor = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[4]) == 0) {  // Green Armor Class
      green_armor_class = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[5]) == 0) {  // Blue Armor Class
      blue_armor_class = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[6]) == 0) {             // Max Soulsphere
      IsDehMaxSoul = true, deh_max_soul = static_cast<int>(value);  // e6y
    } else if (deh_strcasecmp(key, deh_misc[7]) == 0) {             // Soulsphere Health
      soul_health = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[8]) == 0) {                   // Megasphere Health
      IsDehMegaHealth = true, deh_mega_health = static_cast<int>(value);  // e6y
    } else if (deh_strcasecmp(key, deh_misc[9]) == 0) {                   // God Mode Health
      god_health = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[10]) == 0) {  // IDFA Armor
      idfa_armor = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[11]) == 0) {  // IDFA Armor Class
      idfa_armor_class = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[12]) == 0) {  // IDKFA Armor
      idkfa_armor = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[13]) == 0) {  // IDKFA Armor Class
      idkfa_armor_class = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[14]) == 0) {  // BFG Cells/Shot
      bfgcells = static_cast<int>(value);
    } else if (deh_strcasecmp(key, deh_misc[15]) == 0) {  // Monsters Infight
      // e6y: Dehacked support - monsters infight
      if (value == 202) {
        monsters_infight = 0;
      } else if (value == 221) {
        monsters_infight = 1;
      } else if (fpout != nullptr) {
        print(fpout, "Invalid value for 'Monsters Infight': {}", static_cast<int>(value));
      }

      /* No such switch in DOOM - nop */  // e6y ;
    } else if (fpout != nullptr) {
      print(fpout, "Invalid misc item string index for '{}'\n", key);
    }
  }
}

// ====================================================================
// deh_procText
// Purpose: Handle DEH Text block
// Notes:   We look things up in the current information and if found
//          we replace it.  At the same time we write the new and
//          improved BEX syntax to the log file for future use.
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procText(DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {
  char inbuffer[DEH_BUFFERMAX * 2];  // can't use line -- double size buffer too.

  // e6y
  // Correction for DEHs which swap the values of two strings. For example:
  // Text 4 4  Text 4 4;   Text 6 6      Text 6 6
  // BOSSBOS2  BOS2BOSS;   RUNNINSTALKS  STALKSRUNNIN
  // It corrects buggy behaviour on "All Hell is Breaking Loose" TC
  // http://www.doomworld.com/idgames/index.php?id=6480
  static std::array<bool, NUMSPRITES + 1> sprnames_state;
  static std::array<bool, NUMSFX> S_sfx_state;
  static std::array<bool, NUMMUSIC> S_music_state;

  // Ty 04/11/98 - Included file may have NOTEXT skip flag set
  if (includenotext) {  // flag to skip included deh-style text
    if (fpout != nullptr) {
      print(fpout, "Skipped text block because of notext directive\n");
    }

    std::strcpy(inbuffer, line.data());

    while ((dehfeof(&fpin) == 0) && inbuffer[0] != '\0' && (inbuffer[0] != ' ')) {
      dehfgets(inbuffer, sizeof(inbuffer), &fpin);  // skip block
    }

    // Ty 05/17/98 - don't care if this fails
    return;  // ************** Early return
  }

  // killough 8/98: allow hex numbers in input:
  char key[DEH_MAXKEYLEN];
  int fromlen, tolen;  // as specified on the text block line
  std::sscanf(line.data(), "%s %i %i", key, &fromlen, &tolen);

  if (fpout != nullptr) {
    print(fpout, "Processing Text (key={}, from={}, to={})\n", key, fromlen, tolen);
  }

  // killough 10/98: fix incorrect usage of feof
  {
    int c, totlen = 0;
    while (totlen < fromlen + tolen && (c = dehfgetc(&fpin)) != EOF) {
      if (c != '\r') {
        inbuffer[totlen++] = c;
      }
    }

    inbuffer[totlen] = '\0';
  }

  bool found = false;  // to allow early exit once found

  // if the from and to are 4, this may be a sprite rename.  Check it
  // against the array and process it as such if it matches.  Remember
  // that the original names are (and should remain) uppercase.
  // Future: this will be from a separate [SPRITES] block.
  if (fromlen == 4 && tolen == 4) {
    int i = 0;
    while (sprnames[i] != nullptr) {  // null terminated list in info.c //jff 3/19/98 check pointer
      if (!strnicmp(sprnames[i], inbuffer, fromlen) && !sprnames_state[i]) {  // not first char
        if (fpout != nullptr) {
          print(fpout, "Changing name of sprite at index {} from {} to {:.{}s}\n", i, sprnames[i], &inbuffer[fromlen],
                tolen);
        }
        // Ty 03/18/98 - not using strdup because length is fixed

        // killough 10/98: but it's an array of pointers, so we must
        // use strdup unless we redeclare sprnames and change all else
        {
          // CPhipps - fix constness problem
          char* s;
          sprnames[i] = s = strdup(sprnames[i]);

          // e6y: flag the sprite as changed
          sprnames_state[i] = true;

          std::strncpy(s, &inbuffer[fromlen], tolen);
        }

        found = true;
        break;  // only one will match--quit early
      }

      ++i;  // next array element
    }
  }

  if (!found && fromlen < 7 && tolen < 7) {        // lengths of music and sfx are 6 or shorter
    const int usedlen = std::min(fromlen, tolen);  // shorter of fromlen and tolen if not matched
    if (fromlen != tolen) {
      if (fpout != nullptr) {
        print(fpout, "Warning: Mismatched lengths from={}, to={}, used {}\n", fromlen, tolen, usedlen);
      }
    }

    // Try sound effects entries - see sounds.c
    for (int i = 1; i < NUMSFX; i++) {
      // skip empty dummy entries in S_sfx[]
      if (S_sfx[i].name == nullptr) {
        continue;
      }

      // avoid short prefix erroneous match
      if (std::string_view{S_sfx[i].name}.length() != static_cast<std::size_t>(fromlen)) {
        continue;
      }

      if (!strnicmp(S_sfx[i].name, inbuffer, fromlen) && !S_sfx_state[i]) {
        if (fpout != nullptr) {
          print(fpout, "Changing name of sfx from {} to {:.{}s}\n", S_sfx[i].name, &inbuffer[fromlen], usedlen);
        }

        S_sfx[i].name = strdup(&inbuffer[fromlen]);

        // e6y: flag the SFX as changed
        S_sfx_state[i] = true;

        found = true;
        break;  // only one matches, quit early
      }
    }
    if (!found) {  // not yet
      // Try music name entries - see sounds.c
      for (int i = 1; i < NUMMUSIC; i++) {
        // avoid short prefix erroneous match
        if (std::string_view{S_music[i].name}.length() != static_cast<std::size_t>(fromlen)) {
          continue;
        }

        if (!strnicmp(S_music[i].name, inbuffer, fromlen) && !S_music_state[i]) {
          if (fpout != nullptr) {
            print(fpout, "Changing name of music from {} to {:.{}s}\n", S_music[i].name, &inbuffer[fromlen], usedlen);
          }

          S_music[i].name = strdup(&inbuffer[fromlen]);

          // e6y: flag the music as changed
          S_music_state[i] = true;

          found = true;
          break;  // only one matches, quit early
        }
      }
    }  // end !found test
  }

  std::string line2;  // duplicate line for rerouting
  if (!found) {       // Nothing we want to handle here--see if strings can deal with it.
    if (fpout != nullptr) {
      print(fpout, "Checking text area through strings for '{:.12s}{}' from={} to={}\n", inbuffer,
            (std::string_view{inbuffer}.length() > 12) ? "..." : "", fromlen, tolen);
    }

    if (static_cast<size_t>(fromlen) <= strlen(inbuffer)) {
      line2 = &inbuffer[fromlen];
      inbuffer[fromlen] = '\0';
    }

    deh_procStringSub(nullptr, inbuffer, line2.data(), fpout);
  }
}

void deh_procError([[maybe_unused]] DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {
  char inbuffer[DEH_BUFFERMAX];

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  if (fpout != nullptr) {
    print(fpout, "Unmatched Block: '{}'\n", inbuffer);
  }
}

// ====================================================================
// deh_procStrings
// Purpose: Handle BEX [STRINGS] extension
// Args:    fpin  -- input file stream
//          fpout -- output file stream (DEHOUT.TXT)
//          line  -- current line in file to process
// Returns: void
//
void deh_procStrings(DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {

  if (fpout != nullptr) {
    print(fpout, "Processing extended string substitution\n");
  }

  // holds the final result of the string after concatenation
  //  static char* holdstring = nullptr;
  static std::string holdstring;
  holdstring.reserve(128);

  char inbuffer[DEH_BUFFERMAX];
  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  // Ty 04/24/98 - have to allow inbuffer to start with a blank for
  // the continuations of C1TEXT etc.
  while ((dehfeof(&fpin) == 0) && inbuffer[0]) /* && (inbuffer[0] != ' ') */ {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    if (inbuffer[0] == '#') {
      continue;  // skip comment lines
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0' && holdstring.empty()) {
      break;  // killough 11/98
    }

    char key[DEH_MAXKEYLEN];
    char* strval;  // holds the string value of the line

    if (holdstring[0] == '\0') {                                  // first one--get the key
      uint_64_t value;                                            // All deh values are ints or longs
      if (!deh_GetData(inbuffer, key, &value, &strval, fpout)) {  // returns TRUE if ok
        if (fpout != nullptr) {
          print(fpout, "Bad data pair in '{}'\n", inbuffer);
        }

        continue;
      }
    }

    static std::size_t maxstrlen = 128;  // maximum string length, bumped 128 at a time as needed
    while (holdstring.length() + std::string_view{inbuffer}.length() > maxstrlen) {  // Ty03/29/98 - fix stupid error
      // killough 11/98: allocate enough the first time
      maxstrlen = holdstring.length() + std::string_view{inbuffer}.length();

      if (fpout != nullptr) {
        print(fpout, "* increased buffer from to {} for buffer size {}\n", static_cast<long>(maxstrlen),
              static_cast<int>(std::strlen(inbuffer)));
      }

      holdstring.reserve(maxstrlen);
    }

    // concatenate the whole buffer if continuation or the value iffirst
    holdstring += ptr_lstrip(((holdstring[0] != '\0') ? inbuffer : strval));
    rstrip(holdstring);

    // delete any trailing blanks past the backslash
    // note that blanks before the backslash will be concatenated
    // but ones at the beginning of the next line will not, allowing
    // indentation in the file to read well without affecting the
    // string itself.
    if (holdstring.back() == '\\') {
      holdstring.pop_back();
      continue;  // ready to concatenate
    }

    if (holdstring[0] != '\0') {  // didn't have a backslash, trap above would catch that
      // go process the current string
      const bool found = deh_procStringSub(key, nullptr, holdstring.data(),
                                           fpout);  // looking for string continuation - supply keyand not search string

      if (!found) {
        if (fpout != nullptr) {
          print(fpout, "Invalid string key '{}', substitution skipped.\n", key);
        }
      }

      holdstring.clear();  // empty string for the next one
    }
  }
}
}  // namespace

// ====================================================================
// deh_procStringSub
// Purpose: Common string parsing and handling routine for DEH and BEX
// Args:    key       -- place to put the mnemonic for the string if found
//          lookfor   -- original value string to look for
//          newstring -- string to put in its place if found
//          fpout     -- file stream pointer for log file (DEHOUT.TXT)
// Returns: bool: True if string found, false if not
//
auto deh_procStringSub(const std::optional<std::string_view> key,
                       const std::optional<std::string_view> lookfor,
                       const std::string_view newstring,
                       std::FILE* const fpout) -> bool {
  bool found = false;  // loop exit flag

  for (int i = 0; i < deh_strlookup.size(); i++) {
    if (!deh_strlookup[i].orig) {
      deh_strlookup[i].orig = *deh_strlookup[i].ppstr;
    }

    found = lookfor != nullptr ? !stricmp(deh_strlookup[i].orig->data(), lookfor->data())
                               : !stricmp(deh_strlookup[i].lookup.data(), key->data());

    if (found) {
      char* t;
      *deh_strlookup[i].ppstr = t = strdup(newstring.data());  // orphan originalstring
      found = true;

      // Handle embedded \n's in the incoming string, convert to 0x0a's
      {
        for (const char* s = *deh_strlookup[i].ppstr; s[0] != '\0'; ++s, ++t) {
          if (*s == '\\' && (s[1] == 'n' || s[1] == 'N')) {  // found one
            ++s, *t = '\n';                                  // skip one extra for second character
          } else {
            *t = *s;
          }
        }

        *t = '\0';  // cap off the target string
      }

      if (key) {
        if (fpout != nullptr) {
          print(fpout, "Assigned key {} => '{}'\n", *key, newstring);
        }
      }

      if (!key) {
        if (fpout != nullptr) {
          print(fpout, "Assigned '{:.12s}{}' to'{:.12s}{}' at key {}\n", *lookfor,
                (lookfor->length() > 12) ? "..." : "", newstring.data(), (newstring.length() > 12) ? "..." : "",
                deh_strlookup[i].lookup.data());
        }
      }

      if (!key) {  // must have passed an old style string so showBEX
        if (fpout != nullptr) {
          print(fpout, "*BEX FORMAT:\n{} = {}\n*END BEX\n", deh_strlookup[i].lookup, dehReformatStr(newstring.data()));
        }
      }

      break;
    }
  }

  if (!found) {
    if (fpout != nullptr) {
      print(fpout, "Could not find '{:.12s}'\n", key ? *key : *lookfor);
    }
  }

  return found;
}

auto deh_procStringSub(char* const key, char* const lookfor, char* const newstring, std::FILE* const fpout)
    -> bool {
  return deh_procStringSub(key != nullptr ? std::make_optional(key) : std::nullopt,
                           lookfor != nullptr ? std::make_optional(lookfor) : std::nullopt, newstring, fpout);
}

namespace {
//========================================================================
// haleyjd 9/22/99
//
// deh_procHelperThing
//
// Allows handy substitution of any thing for helper dogs.  DEH patches
// are being made frequently for this purpose and it requires a complete
// rewiring of the DOG thing.  I feel this is a waste of effort, and so
// have added this new [HELPER] BEX block

void deh_procHelperThing(DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {
  char inbuffer[DEH_BUFFERMAX];

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  while ((dehfeof(&fpin) == 0) && inbuffer[0] != '\0' && (inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;
    }

    char key[DEH_MAXKEYLEN];
    uint_64_t value;                                            // All deh values are ints or longs
    if (!deh_GetData(inbuffer, key, &value, nullptr, fpout)) {  // returns TRUE if ok
      if (fpout != nullptr) {
        print(fpout, "Bad data pair in '{}'\n", inbuffer);
      }

      continue;
    }

    // Otherwise it's ok
    if (fpout != nullptr) {
      print(fpout, "Processing Helper Thing item '{}'\n", key);
      print(fpout, "value is {}", static_cast<int>(value));
    }

    if (strncasecmp(key, "type", 4) == 0) {
      HelperThing = static_cast<int>(value);
    }
  }
}

//
// deh_procBexSprites
//
// Supports sprite name substitutions without requiring use
// of the DeHackEd Text block
//
void deh_procBexSprites(DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {
  char inbuffer[DEH_BUFFERMAX];

  if (fpout != nullptr) {
    print(fpout, "Processing sprite name substitution\n");
  }

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  while ((dehfeof(&fpin) == 0) && inbuffer[0] != '\0' && (inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    if (inbuffer[0] == '#') {
      continue;  // skip comment lines
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98
    }

    char key[DEH_MAXKEYLEN];
    uint_64_t value;                                            // All deh values are ints or longs
    char* strval;                                               // holds the string value of the line
    if (!deh_GetData(inbuffer, key, &value, &strval, fpout)) {  // returns TRUE if ok
      if (fpout != nullptr) {
        print(fpout, "Bad data pair in '{}'\n", inbuffer);
      }

      continue;
    }

    // do it
    const std::string_view candidate = ptr_lstrip(strval);
    if (candidate.length() != 4) {
      if (fpout != nullptr) {
        print(fpout, "Bad length for sprite name '{}'\n", candidate);
      }

      continue;
    }

    int rover = 0;
    while (deh_spritenames[rover]) {
      if (strncasecmp(deh_spritenames[rover]->c_str(), key, 4) == 0) {
        if (fpout != nullptr) {
          print(fpout, "Substituting '{}' for sprite '{}'\n", candidate, *deh_spritenames[rover]);
        }

        sprnames[rover] = strdup(candidate.data());
        break;
      }

      rover++;
    }
  }
}

// ditto for sound names
void deh_procBexSounds(DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {
  char inbuffer[DEH_BUFFERMAX];

  if (fpout != nullptr) {
    print(fpout, "Processing sound name substitution\n");
  }

  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  while (dehfeof(&fpin) == 0 && inbuffer[0] != '\0' && inbuffer[0] != ' ') {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    if (inbuffer[0] == '#') {
      continue;  // skip comment lines
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98
    }

    char key[DEH_MAXKEYLEN];
    uint_64_t value;                                            // All deh values are ints or longs
    char* strval;                                               // holds the string value of the line
    if (!deh_GetData(inbuffer, key, &value, &strval, fpout)) {  // returns TRUE if ok
      if (fpout) {
        print(fpout, "Bad data pair in '{}'\n", inbuffer);
      }

      continue;
    }

    // do it
    const std::string_view candidate = ptr_lstrip(strval);
    const std::size_t len = candidate.length();
    if (len < 1 || len > 6) {
      if (fpout != nullptr) {
        print(fpout, "Bad length for sound name '{}'\n", candidate.data());
      }

      continue;
    }

    int rover = 1;
    while (deh_soundnames[rover]) {
      if (strncasecmp(deh_soundnames[rover]->c_str(), key, 6) == 0) {
        if (fpout != nullptr) {
          print(fpout, "Substituting '{}' for sound '{}'\n", candidate, *deh_soundnames[rover]);
        }

        S_sfx[rover].name = strdup(candidate.data());
        break;
      }

      rover++;
    }
  }
}

// ditto for music names
void deh_procBexMusic(DEHFILE& fpin, std::FILE* const fpout, const std::string_view line) {
  if (fpout != nullptr) {
    print(fpout, "Processing music name substitution\n");
  }

  char inbuffer[DEH_BUFFERMAX];
  std::strncpy(inbuffer, line.data(), DEH_BUFFERMAX - 1);

  while (dehfeof(&fpin) == 0 && inbuffer[0] != '\0' && (inbuffer[0] != ' ')) {
    if (dehfgets(inbuffer, sizeof(inbuffer), &fpin) == nullptr) {
      break;
    }

    if (inbuffer[0] == '#') {
      continue;  // skip comment lines
    }

    lfstrip(inbuffer);
    if (inbuffer[0] == '\0') {
      break;  // killough 11/98
    }

    char key[DEH_MAXKEYLEN];
    uint_64_t value;                                            // All deh values are ints or longs
    char* strval;                                               // holds the string value of the line
    if (!deh_GetData(inbuffer, key, &value, &strval, fpout)) {  // returns TRUE if ok
      if (fpout != nullptr) {
        print(fpout, "Bad data pair in '{}'\n", inbuffer);
      }

      continue;
    }

    // do it
    const std::string_view candidate = ptr_lstrip(strval);
    const std::size_t len = candidate.length();
    if (len < 1 || len > 6) {
      if (fpout != nullptr) {
        print(fpout, "Bad length for music name '{}'\n", candidate);
      }

      continue;
    }

    int rover = 1;
    while (deh_musicnames[rover]) {
      if (strncasecmp(deh_musicnames[rover]->c_str(), key, 6) == 0) {
        if (fpout != nullptr) {
          print(fpout, "Substituting '{}' for music '{}'\n", candidate, *deh_musicnames[rover]);
        }

        S_music[rover].name = strdup(candidate.data());
        break;
      }

      rover++;
    }
  }
}
}  // namespace

// ====================================================================
// General utility function(s)
// ====================================================================

// ====================================================================
// dehReformatStr
// Purpose: Convert a string into a continuous string with embedded
//          linefeeds for "\n" sequences in the source string
// Args:    string -- the string to convert
// Returns: the converted string (converted in a static buffer)
//
auto dehReformatStr(char* string) -> char* {
  static char buff[DEH_BUFFERMAX];  // only processing the changed string,
  //  don't need double buffer

  const char* s = string;  // source
  char* t = buff;          // target
  // let's play...

  while (s[0] != '\0') {
    if (*s == '\n') {
      ++s;
      *t++ = '\\';
      *t++ = 'n';
      *t++ = '\\';
      *t++ = '\n';
    } else {
      *t++ = *s++;
    }
  }

  *t = '\0';
  return buff;
}

auto dehReformatStr(const std::string_view& string) -> std::string {
  std::string buff;
  buff.reserve(DEH_BUFFERMAX);

  for (auto c : string) {
    if (c == '\n') {
      buff += "\\n\\\n";
    } else {
      buff.push_back(c);
    }
  }

  return buff;
}

// ====================================================================
// lfstrip
// Purpose: Strips CR/LF off the end of a string
// Args:    s -- the string to work on
// Returns: void -- the string is modified in place
//
// killough 10/98: only strip at end of line, not entire string

void lfstrip(char* s) {  // strip the \r and/or \n off of a line
  char* p = s + std::strlen(s);
  while (p > s && (*--p == '\r' || *p == '\n')) {
    *p = 0;
  }
}

void lfstrip(std::string& s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
    s.pop_back();
  }
}

// ====================================================================
// rstrip
// Purpose: Strips trailing blanks off a string
// Args:    s -- the string to work on
// Returns: void -- the string is modified in place
//
void rstrip(char* s) {              // strip trailing whitespace
  char* p = s + strlen(s);          // killough 4/4/98: same here
  while (p > s && isspace(*--p)) {  // break on first non-whitespace
    *p = '\0';
  }
}

void rstrip(std::string& s) {
  while (!s.empty() && std::isspace(s.back())) {
    s.pop_back();
  }
}

// ====================================================================
// ptr_lstrip
// Purpose: Points past leading whitespace in a string
// Args:    s -- the string to work on
// Returns: char * pointing to the first nonblank character in the
//          string.  The original string is not changed.
//
char* ptr_lstrip(char* p) {  // point past leading whitespace
  while (std::isspace(*p)) {
    p++;
  }

  return p;
}

auto ptr_lstrip(const std::string_view p) -> std::string_view {
  const auto new_begin = std::find_if_not(p.cbegin(), p.cend(), [](char c) { return std::isspace(c); });
  return new_begin != p.cend() ? std::string_view{new_begin, p.cend()} : p;
}

// ====================================================================
// deh_GetData
// Purpose: Get a key and data pair from a passed string
// Args:    s -- the string to be examined
//          k -- a place to put the key
//          l -- pointer to a long integer to store the number
//          strval -- a pointer to the place in s where the number
//                    value comes from.  Pass NULL to not use this.
//          fpout  -- stream pointer to output log (DEHOUT.TXT)
// Notes:   Expects a key phrase, optional space, equal sign,
//          optional space and a value, mostly an int but treated
//          as a long just in case.  The passed pointer to hold
//          the key must be DEH_MAXKEYLEN in size.

auto deh_GetData(char* const s, char* const k, uint_64_t& l, char** const strval, std::FILE* const fpout) -> int {
  std::string buffer;  // to hold key in progress
  buffer.reserve(DEH_MAXKEYLEN);

  // e6y: Correction of wrong processing of Bits parameter if its value is equal to zero
  // No more desync on HACX demos.
  int okrc = 1;  // assume good unless we have problems

  int val = 0;  // to hold value of pair - defaults in case not otherwise set

  char* t;  // current char
  int i;    // iterator
  for (i = 0, t = s; *t != '\0' && i < DEH_MAXKEYLEN; t++, i++) {
    if (*t == '=') {
      break;
    }

    buffer.push_back(*t);  // copy it
  }

  buffer.pop_back();  // terminate the key before the '='
  if (*t == '\0') {   // end of string with no equal sign
    okrc = 0;
  } else {
    if (*++t == '\0') {
      val = 0;  // in case "thiskey =" with no value
      okrc = 0;
    }

    // we've incremented t
    // e6y: Correction of wrong processing of Bits parameter if its value is equal to zero
    // No more desync on HACX demos.
    // Old code: e6y val = strtol(t,NULL,0);  // killough 8/9/98: allow hex or octal input
    if (M_StrToInt(t, &val) == 0) {
      val = 0;
      okrc = 2;
    }
  }

  // go put the results in the passed pointers
  l = val;  // may be a faked zero

  // if spaces between key and equal sign, strip them
  std::strcpy(k, ptr_lstrip(buffer.c_str()).data());  // could be a zero-length string

  if (strval != nullptr) {  // pass NULL if you don't want this back
    *strval = t;            // pointer, has to be somewhere in s,
                            // even if pointing at the zero byte.
  }

  return okrc;
}

auto deh_GetData(const std::string_view s,
                 std::string& k,
                 uint_64_t& l,
                 std::string* const strval,
                 std::FILE* const fpout) -> int {
  std::string buffer;  // to hold key in progress
  buffer.reserve(DEH_MAXKEYLEN);

  // e6y: Correction of wrong processing of Bits parameter if its value is equal to zero
  // No more desync on HACX demos.
  int okrc = 1;  // assume good unless we have problems

  int val = 0;  // to hold value of pair - defaults in case not otherwise set

  const char* t;  // current char
  int i;          // iterator
  for (i = 0, t = s.data(); *t != '\0' && i < DEH_MAXKEYLEN; t++, i++) {
    if (*t == '=') {
      break;
    }

    buffer.push_back(*t);  // copy it
  }

  buffer.pop_back();  // terminate the key before the '='
  if (*t == '\0') {   // end of string with no equal sign
    okrc = 0;
  } else {
    if (*++t == '\0') {
      val = 0;  // in case "thiskey =" with no value
      okrc = 0;
    }

    // we've incremented t
    // e6y: Correction of wrong processing of Bits parameter if its value is equal to zero
    // No more desync on HACX demos.
    // Old code: e6y val = strtol(t,NULL,0);  // killough 8/9/98: allow hex or octal input
    if (M_StrToInt(t, &val) == 0) {
      val = 0;
      okrc = 2;
    }
  }

  // go put the results in the passed pointers
  l = val;  // may be a faked zero

  // if spaces between key and equal sign, strip them
  k = ptr_lstrip(buffer.c_str());  // could be a zero-length string

  if (strval != nullptr) {     // pass NULL if you don't want this back
    *strval = std::string{t};  // pointer, has to be somewhere in s,
    // even if pointing at the zero byte.
  }

  return okrc;
}

auto deh_GetData(char* const s, char* const k, uint_64_t* const l, char** const strval, std::FILE* const fpout)
    -> bool {
  return deh_GetData(s, k, *l, strval, fpout);
}
