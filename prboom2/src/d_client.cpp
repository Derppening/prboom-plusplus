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
 *    Network client. Passes information to/from server, staying
 *    synchronised.
 *    Contains the main wait loop, waiting for network input or
 *    time before doing the next tic.
 *    Rewritten for LxDoom, but based around bits of the old code.
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstddef>

#include <algorithm>
#include <memory>
#include <vector>

#include <sys/types.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#ifdef USE_SDL_NET
#include <SDL.h>
#endif

#include "d_net.h"
#include "doomstat.h"
#include "doomtype.h"
#include "z_zone.h"

#include "d_main.h"
#include "g_game.h"
#include "m_menu.h"
#include "p_checksum.h"

#include "e6y.h"
#include "i_main.h"
#include "i_network.h"
#include "i_system.h"
#include "i_video.h"
#include "lprintf.h"
#include "m_argv.h"
#include "protocol.h"
#include "r_fps.h"

#include "m_io.h"

namespace {
bool server;
int remotetic;   // Tic expected from the remote
int remotesend;  // Tic expected by the remote
}  // namespace

ticcmd_t netcmds[MAXPLAYERS][BACKUPTICS];

namespace {
ticcmd_t* localcmds;
//unsigned numqueuedpackets;
using unique_packet_header_t = std::unique_ptr<packet_header_t, decltype([](auto* p) { Z_Free(p); })>;
std::vector<unique_packet_header_t, z_allocator<unique_packet_header_t>> queuedpacket;
}  // namespace

int maketic;
int ticdup = 1;

namespace {
int xtratics = 0;
}  // namespace

int wanted_player_number;
int solo_net = 0;

extern "C" void D_QuitNetGame();

#ifndef HAVE_NET
doomcom_t* doomcom;
#endif

#ifdef HAVE_NET
void D_InitNetGame() {
  int numplayers = 1;

  int i = M_CheckParm("-net");
  if (i != 0 && i < myargc - 1) {
    i++;
  }

  if (!(netgame = server = (i != 0))) {
    playeringame[consoleplayer = 0] = true;

    // e6y
    // for play, recording or playback using "single-player coop" mode.
    // Equivalent to using prboom_server with -N 1
    solo_net = (M_CheckParm("-solo-net") != 0);
    coop_spawns = (M_CheckParm("-coop_spawns") != 0);
    netgame = solo_net;
  } else {
    // Get game info from server
    const auto packet = unique_packet_header_t{
        static_cast<packet_header_t*>(Z_Malloc(1000, PU_STATIC, nullptr))};
    auto* sinfo = reinterpret_cast<setup_packet_s*>(packet.get() + 1);

    struct {
      packet_header_t head;
      short pn;
    } PACKEDATTR initpacket;

    I_InitNetwork();
    udp_socket = I_Socket(0);
    I_ConnectToServer(myargv[i]);

    do {
      do {
        // Send init packet
        initpacket.pn = doom_htons(wanted_player_number);
        packet_set(&initpacket.head, PKT_INIT, 0);
        I_SendPacket(&initpacket.head, sizeof(initpacket));
        I_WaitForPacket(5000);
      } while (I_GetPacket(packet.get(), 1000) == 0);

      if (packet->type == PKT_DOWN) {
        I_Error("Server aborted the game");
      }
    } while (packet->type != PKT_SETUP);

    // Once we have been accepted by the server, we should tell it when we leave
    I_AtExit(D_QuitNetGame, true);

    // Get info from the setup packet
    consoleplayer = sinfo->yourplayer;
    compatibility_level = sinfo->complevel;
    G_Compatibility();
    startskill = static_cast<skill_t>(sinfo->skill);
    deathmatch = sinfo->deathmatch;
    startmap = sinfo->level;
    startepisode = sinfo->episode;
    ticdup = sinfo->ticdup;
    xtratics = sinfo->extratic;
    G_ReadOptions(sinfo->game_options);

    lprintf(LO_INFO, "\tjoined game as player %d/%d; %d WADs specified\n", consoleplayer + 1,
            numplayers = sinfo->players, sinfo->numwads);
    {
      auto* p = reinterpret_cast<char*>(sinfo->wadnames);
      int i = sinfo->numwads;

      while (i-- != 0) {
        D_AddFile(p, source_net);
        p += std::string_view{p}.length() + 1;
      }
    }
  }
  localcmds = netcmds[displayplayer = consoleplayer];
  for (i = 0; i < numplayers; i++) {
    playeringame[i] = true;
  }
  for (; i < MAXPLAYERS; i++) {
    playeringame[i] = false;
  }
  if (!playeringame[consoleplayer]) {
    I_Error("D_InitNetGame: consoleplayer not in game");
  }
}
#else
void D_InitNetGame() {

  doomcom = static_cast<doomcom_t*>(Z_Malloc(sizeof(*doomcom), PU_STATIC, nullptr));
  doomcom->consoleplayer = 0;
  doomcom->numnodes = 0;
  doomcom->numplayers = 1;
  localcmds = netcmds[consoleplayer];
  solo_net = (M_CheckParm("-solo-net") != 0);
  coop_spawns = (M_CheckParm("-coop_spawns") != 0);
  netgame = solo_net;

  int i;
  for (i = 0; i < doomcom->numplayers; i++) {
    playeringame[i] = true;
  }
  for (; i < MAXPLAYERS; i++) {
    playeringame[i] = false;
  }

  consoleplayer = displayplayer = doomcom->consoleplayer;
}
#endif  // HAVE_NET

#ifdef HAVE_NET
void D_CheckNetGame() {
  const auto packet = unique_packet_header_t{
      static_cast<packet_header_t*>(Z_Malloc(sizeof(packet_header_t) + 1, PU_STATIC, nullptr))};

  if (server) {
    lprintf(LO_INFO, "D_CheckNetGame: waiting for server to signal game start\n");

    do {
      while (I_GetPacket(packet.get(), sizeof(packet_header_t) + 1) == 0) {
        packet_set(packet.get(), PKT_GO, 0);
        *reinterpret_cast<byte*>(packet.get() + 1) = consoleplayer;
        I_SendPacket(packet.get(), sizeof(packet_header_t) + 1);
        I_uSleep(100000);
      }
    } while (packet->type != PKT_GO);
  }
}

auto D_NetGetWad(const std::string_view name) -> bool {
#if defined(HAVE_SYS_WAIT_H)
  const std::size_t psize = sizeof(packet_header_t) + name.length() + 500;
  unique_packet_header_t packet;
  bool done = false;

  if (!server || name.find_first_of('/') != std::string_view::npos) {
    return false;  // If it contains path info, reject
  }

  do {
    // Send WAD request to remote
    packet = unique_packet_header_t{static_cast<packet_header_t*>(Z_Malloc(psize, PU_STATIC, nullptr))};
    packet_set(packet.get(), PKT_WAD, 0);
    *reinterpret_cast<byte*>(packet.get() + 1) = consoleplayer;
    std::copy(name.cbegin(), name.cend(), reinterpret_cast<char*>(1 + reinterpret_cast<byte*>(packet.get() + 1)));
    I_SendPacket(packet.get(), sizeof(packet_header_t) + name.length() + 2);

    I_uSleep(10000);
  } while (!I_GetPacket(packet.get(), psize) || (packet->type != PKT_WAD));
  packet.reset();

  if (!strcasecmp(reinterpret_cast<char*>(packet.get() + 1), name.data())) {
    byte* p = reinterpret_cast<byte*>(packet.get() + 1) + name.length() + 1;

    /* Automatic wad file retrieval using wget (supports http and ftp, using URLs)
     * Unix systems have all these commands handy, this kind of thing is easy
     * Any windo$e port will have some awkward work replacing these.
     */
    /* cph - caution here. This is data from an untrusted source.
     * Don't pass it via a shell. */
    pid_t pid;
    if ((pid = fork()) == -1) {
      std::perror("fork");
    } else if (pid == 0) {
      /* Child chains to wget, does the download */
      execlp("wget", "wget", p, nullptr);
    }

    /* This is the parent, i.e. main LxDoom process */
    int rv;
    wait(&rv);
    if (!(done = (M_access(name.data(), R_OK) == 0))) {
      if (std::string_view{reinterpret_cast<char*>(p)}.ends_with(".zip")) {
        p += (std::string_view{reinterpret_cast<char*>(p)}.find_last_of('/') + 1);

        if ((pid = fork()) == -1) {
          perror("fork");
        } else if (pid == 0) {
          /* Child executes decompressor */
          execlp("unzip", "unzip", p, name.data(), nullptr);
        }

        /* Parent waits for the file */
        wait(&rv);
        done = M_access(name.data(), R_OK) != 0;
      }

      /* Add more decompression protocols here as desired */
    }

    packet.reset();
  }
  return done;
#else /* HAVE_SYS_WAIT_H */
  return false;
#endif
}

auto D_NetGetWad(const char* const name) -> bool {
  return D_NetGetWad(std::string_view{name});
}

void NetUpdate() {
  static int lastmadetic;

  if (isExtraDDisplay) {
    return;
  }

  if (server) {  // Receive network packets
    const auto packet = unique_packet_header_t{
        static_cast<packet_header_t*>(Z_Malloc(10000, PU_STATIC, nullptr))};

    std::size_t recvlen;
    while ((recvlen = I_GetPacket(packet.get(), 10000))) {
      switch (packet->type) {
        case PKT_TICS: {
          byte* p = reinterpret_cast<byte*>(packet.get() + 1);
          int tics = *p++;
          unsigned long ptic = doom_ntohl(packet->tic);

          if (ptic > static_cast<unsigned>(remotetic)) {  // Missed some
            packet_set(packet.get(), PKT_RETRANS, remotetic);
            *reinterpret_cast<byte*>(packet.get() + 1) = consoleplayer;
            I_SendPacket(packet.get(), sizeof(*packet.get()) + 1);
          } else {
            if (ptic + tics <= static_cast<unsigned>(remotetic)) {
              break;  // Will not improve things
            }

            remotetic = ptic;

            while (tics--) {
              int players = *p++;

              while (players--) {
                int n = *p++;
                RawToTic(&netcmds[n][remotetic % BACKUPTICS], p);
                p += sizeof(ticcmd_t);
              }

              remotetic++;
            }
          }

          break;
        }

        case PKT_RETRANS:  // Resend request
          remotesend = doom_ntohl(packet->tic);
          break;

        case PKT_DOWN: {  // Server downed
          for (int j = 0; j < MAXPLAYERS; j++) {
            if (j != consoleplayer) {
              playeringame[j] = false;
            }
          }

          server = false;
          doom_printf("Server is down\nAll other players are no longer in the game\n");

          break;
        }

        case PKT_EXTRA:  // Misc stuff
        case PKT_QUIT:   // Player quit
          // Queue packet to be processed when its tic time is reached
          queuedpacket.emplace_back(static_cast<packet_header_t*>(Z_Malloc(recvlen, PU_STATIC, nullptr)));
          std::copy_n(reinterpret_cast<std::byte*>(packet.get()), recvlen, reinterpret_cast<std::byte*>(&queuedpacket.back()));

          break;
        case PKT_BACKOFF:
          /* cph 2003-09-18 -
           * The server sends this when we have got ahead of the other clients. We should
           * stall the input side on this client, to allow other clients to catch up.
           */
          lastmadetic++;
          break;

        default:  // Other packet, unrecognised or redundant
          break;
      }
    }
  }

  {  // Build new ticcmds
    int newtics = I_GetTime() - lastmadetic;
    // e6y    newtics = (newtics > 0 ? newtics : 0);
    lastmadetic += newtics;
    if (ffmap != 0) {
      newtics++;
    }

    while (newtics--) {
      I_StartTic();
      if (maketic - gametic > BACKUPTICS / 2) {
        break;
      }

      // e6y
      // Eliminating the sudden jump of six frames(BACKUPTICS/2)
      // after change of realtic_clock_rate.
      if (maketic - gametic && gametic <= force_singletics_to && realtic_clock_rate < 200) {
        break;
      }

      G_BuildTiccmd(&localcmds[maketic % BACKUPTICS]);
      maketic++;
    }

    if (server && maketic > remotesend) {  // Send the tics to the server
      remotesend -= xtratics;
      if (remotesend < 0) {
        remotesend = 0;
      }

      int sendtics = std::min(maketic - remotesend, 128);  // limit number of sent tics (CVE-2019-20797)

      {
        const std::size_t pkt_size = sizeof(packet_header_t) + 2 + sendtics * sizeof(ticcmd_t);
        const auto packet = unique_packet_header_t{
            static_cast<packet_header_t*>(Z_Malloc(pkt_size, PU_STATIC, nullptr))};

        packet_set(packet.get(), PKT_TICC, maketic - sendtics);
        *reinterpret_cast<byte*>(packet.get() + 1) = sendtics;
        *(reinterpret_cast<byte*>(packet.get() + 1) + 1) = consoleplayer;

        {
          void* tic = reinterpret_cast<byte*>(packet.get() + 1) + 2;
          while (sendtics-- != 0) {
            TicToRaw(tic, &localcmds[remotesend++ % BACKUPTICS]);
            tic = static_cast<byte*>(tic) + sizeof(ticcmd_t);
          }
        }

        I_SendPacket(packet.get(), pkt_size);
      }
    }
  }
}
#else

void D_BuildNewTiccmds() {
  static int lastmadetic;

  int newtics = I_GetTime() - lastmadetic;
  lastmadetic += newtics;

  while (newtics-- > 0) {
    I_StartTic();

    if (maketic - gametic > BACKUPTICS / 2) {
      break;
    }

    G_BuildTiccmd(&localcmds[maketic % BACKUPTICS]);
    maketic++;
  }
}
#endif

#ifdef HAVE_NET
/* cph - data passed to this must be in the Doom (little-) endian */
void D_NetSendMisc(const netmisctype_t type, const std::size_t len, void* const data) {
  if (server) {
    const std::size_t size = sizeof(packet_header_t) + 3 * sizeof(int) + len;
    const auto packet = unique_packet_header_t{
        static_cast<packet_header_t*>(Z_Malloc(size, PU_STATIC, nullptr))};
    int* p = reinterpret_cast<int*>(packet.get() + 1);

    packet_set(packet.get(), PKT_EXTRA, gametic);
    *p++ = LittleLong(type);
    *p++ = LittleLong(consoleplayer);
    *p++ = LittleLong(len);
    memcpy(p, data, len);
    I_SendPacket(packet.get(), size);
  }
}

namespace {
void CheckQueuedPackets() {
  for (int i = 0; static_cast<unsigned>(i) < queuedpacket.size(); i++)
    if (doom_ntohl(queuedpacket[i]->tic) <= gametic)
      switch (queuedpacket[i]->type) {
        case PKT_QUIT: {  // Player quit the game
          const int pn = *reinterpret_cast<byte*>(&queuedpacket[i] + 1);
          playeringame[pn] = false;
          doom_printf("Player %d left the game\n", pn);
          break;
        }

        case PKT_EXTRA: {
          int* const p = reinterpret_cast<int*>(&queuedpacket[i] + 1);
          const std::size_t len = LittleLong(*(p + 2));
          switch (LittleLong(*p)) {
            case nm_plcolour:
              G_ChangedPlayerColour(LittleLong(*(p + 1)), LittleLong(*(p + 3)));
              break;

            case nm_savegamename:
              if (len < SAVEDESCLEN) {
                std::copy_n(reinterpret_cast<std::byte*>(p + 3), len, reinterpret_cast<std::byte*>(savedescription));

                // Force terminating 0 in case
                savedescription[len] = 0;
              }
              break;
          }

          break;
        }

        default:  // Should not be queued
          break;
      }

  {  // Requeue remaining packets
    std::erase_if(queuedpacket, [](const unique_packet_header_t& packet) { return packet->tic <= gametic; });
  }
}
}  // namespace
#endif  // HAVE_NET

void TryRunTics() {
  int runtics;
  const int entertime = I_GetTime();

  // Wait for tics to run
  while (true) {
#ifdef HAVE_NET
    NetUpdate();
#else
    D_BuildNewTiccmds();
#endif
    runtics = (server ? remotetic : maketic) - gametic;
    if (runtics == 0) {
      if (movement_smooth == 0 || window_focused == 0) {
#ifdef HAVE_NET
        if (server) {
          I_WaitForPacket(ms_to_next_tick);
        } else
#endif
          I_uSleep(ms_to_next_tick * 1000);
      }
      if (I_GetTime() - entertime > 10) {
#ifdef HAVE_NET
        if (server) {
          char buf[sizeof(packet_header_t) + 1];
          remotesend--;
          packet_set(reinterpret_cast<packet_header_t*>(buf), PKT_RETRANS, remotetic);
          buf[sizeof(buf) - 1] = consoleplayer;
          I_SendPacket(reinterpret_cast<packet_header_t*>(buf), sizeof(buf));
        }
#endif
        M_Ticker();
        return;
      }

      // if ((displaytime) < (tic_vars.next-SDL_GetTicks()))
      if (gametic > 0) {
        WasRenderedInTryRunTics = true;

        if (movement_smooth != 0 && gamestate == wipegamestate) {
          isExtraDDisplay = true;
          D_Display(I_GetTimeFrac());
          isExtraDDisplay = false;
        }
      }
    } else {
      break;
    }
  }

  while (runtics-- != 0) {
#ifdef HAVE_NET
    if (server) {
      CheckQueuedPackets();
    }
#endif

    if (advancedemo) {
      D_DoAdvanceDemo();
    }

    M_Ticker();
    G_Ticker();
    P_Checksum(gametic);
    gametic++;

#ifdef HAVE_NET
    NetUpdate();  // Keep sending our tics to avoid stalling remote nodes
#endif
  }
}

#ifdef HAVE_NET
void D_QuitNetGame() {
  byte buf[1 + sizeof(packet_header_t)];
  auto* const packet = reinterpret_cast<packet_header_t*>(buf);

  if (!server) {
    return;
  }

  buf[sizeof(packet_header_t)] = consoleplayer;
  packet_set(packet, PKT_QUIT, gametic);

  for (int i = 0; i < 4; i++) {
    I_SendPacket(packet, 1 + sizeof(packet_header_t));
    I_uSleep(10000);
  }
}
#endif
