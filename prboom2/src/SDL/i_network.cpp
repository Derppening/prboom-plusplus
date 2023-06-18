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
 *  Low level UDP network interface. This is shared between the server
 *  and client, with SERVER defined for the former to select some extra
 *  functions. Handles socket creation, and packet send and receive.
 *
 *-----------------------------------------------------------------------------*/

#ifdef HAVE_CONFIG_H

#include "config.h"

#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string_view>

#include <fcntl.h>

#ifdef HAVE_UNISTD_H

#include <unistd.h>

#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_NET

#include <SDL.h>
#include <SDL_net.h>

#include "i_network.h"
#include "lprintf.h"
#include "protocol.h"

#ifndef PRBOOM_SERVER

#include "i_system.h"

#endif
// #include "doomstat.h"

/* cph -
 * Each client will either use the IPv4 socket or the IPv6 socket
 * Each server will use whichever or both that are available
 */
UDP_CHANNEL sentfrom;
IPaddress sentfrom_addr;
UDP_SOCKET udp_socket;

/* Statistics */
std::size_t sentbytes;
std::size_t recvdbytes;

std::unique_ptr<UDP_PACKET, decltype(&SDLNet_FreePacket)> udp_packet{nullptr, SDLNet_FreePacket};

/* I_ShutdownNetwork
 *
 * Shutdown the network code
 */
void I_ShutdownNetwork() {
  udp_packet.reset();
  SDLNet_Quit();
}

/* I_InitNetwork
 *
 * Sets up the network code
 */
void I_InitNetwork() {
  SDLNet_Init();
#ifndef PRBOOM_SERVER
  I_AtExit(I_ShutdownNetwork, true);
#else
  std::atexit(I_ShutdownNetwork);
#endif
  udp_packet.reset(SDLNet_AllocPacket(10000));
}

auto I_AllocPacket(int size) -> std::unique_ptr<UDP_PACKET, decltype(&SDLNet_FreePacket)> {
  return std::unique_ptr<UDP_PACKET, decltype(&SDLNet_FreePacket)>{SDLNet_AllocPacket(size), SDLNet_FreePacket};
}

void I_FreePacket([[maybe_unused]] std::unique_ptr<UDP_PACKET, decltype(&SDLNet_FreePacket)>&& packet) {
}

/* cph - I_WaitForPacket - use select(2) via SDL_net's interface
 * No more I_uSleep loop kludge */

void I_WaitForPacket(const int ms) {
  const SDLNet_SocketSet ss = SDLNet_AllocSocketSet(1);
  SDLNet_UDP_AddSocket(ss, udp_socket);
  SDLNet_CheckSockets(ss, ms);
  SDLNet_FreeSocketSet(ss);
}

/* I_ConnectToServer
 *
 * Connect to a server
 */
IPaddress serverIP;

auto I_ConnectToServer(const char* serv) -> int {
  constexpr std::size_t serv_max_len = 500;

  std::string_view server{serv};

  /* Split serv into address and port */
  if (server.length() > serv_max_len) {
    return 0;
  }

  auto delim_idx = server.find_first_of(':');
  Uint16 port = 5030; /* Default server port */
  if (delim_idx != std::string::npos) {
    port = static_cast<Uint16>(std::atoi(server.substr(delim_idx + 1).data()));
    server = server.substr(0, delim_idx);
  }

  SDLNet_ResolveHost(&serverIP, server.data(), port);
  if (serverIP.host == INADDR_NONE) {
    return -1;
  }

  if (SDLNet_UDP_Bind(udp_socket, 0, &serverIP) == -1) {
    return -1;
  }

  return 0;
}

/* I_Disconnect
 *
 * Disconnect from server
 */
void I_Disconnect() {
  /*  int i;
    UDP_PACKET *packet;
    packet_header_t *pdata = (packet_header_t *)packet->data;
    packet = I_AllocPacket(sizeof(packet_header_t) + 1);

    packet->data[sizeof(packet_header_t)] = consoleplayer;
          pdata->type = PKT_QUIT; pdata->tic = gametic;

    for (i=0; i<4; i++) {
      I_SendPacket(packet);
      I_uSleep(10000);
      }
    I_FreePacket(packet);*/
  SDLNet_UDP_Unbind(udp_socket, 0);
}

/*
 * I_Socket
 *
 * Sets the given socket non-blocking, binds to the given port, or first
 * available if none is given
 */
auto I_Socket(Uint16 port) -> UDP_SOCKET {
  if (port != 0) {
    return SDLNet_UDP_Open(port);
  }

  UDP_SOCKET sock;
  port = IPPORT_RESERVED;
  while ((sock = SDLNet_UDP_Open(port)) == nullptr) {
    port++;
  }
  return sock;
}

void I_CloseSocket(UDP_SOCKET sock) {
  SDLNet_UDP_Close(sock);
}

auto I_RegisterPlayer(IPaddress* ipaddr) -> UDP_CHANNEL {
  static int freechannel;
  return SDLNet_UDP_Bind(udp_socket, freechannel++, ipaddr);
}

void I_UnRegisterPlayer(UDP_CHANNEL channel) {
  SDLNet_UDP_Unbind(udp_socket, channel);
}

namespace {
/*
 * ChecksumPacket
 *
 * Returns the checksum of a given network packet
 */
auto ChecksumPacket(const packet_header_t* buffer, std::size_t len) -> byte {
  const auto* p = reinterpret_cast<const byte*>(buffer);
  byte sum = 0;

  if (len == 0) {
    return 0;
  }

  while (--len > 0) {
    sum += *(++p);
  }

  return sum;
}
}  // namespace

auto I_GetPacket(packet_header_t* buffer, const std::size_t buflen) -> std::size_t {
  const int status = SDLNet_UDP_Recv(udp_socket, udp_packet.get());
  auto len = static_cast<std::size_t>(udp_packet->len);
  if (buflen < len) {
    len = buflen;
  }
  if ((status != 0) && (len > 0)) {
    std::copy_n(reinterpret_cast<const std::byte*>(udp_packet->data), len, reinterpret_cast<std::byte*>(buffer));
  }
  sentfrom = udp_packet->channel;
  sentfrom_addr = udp_packet->address;
  const int checksum = buffer->checksum;
  buffer->checksum = 0;
  if ((status != 0) && (len > 0)) {
    const byte psum = ChecksumPacket(buffer, len);  // https://logicaltrust.net/blog/2019/10/prboom1.html
                                              /*    fprintf(stderr, "recvlen = %u, stolen = %u, csum = %u, psum = %u\n",
                                                udp_packet->len, len, checksum, psum); */
    if (psum == checksum) {
      return len;
    }
  }

  return 0;
}

void I_SendPacket(packet_header_t* const packet, const std::size_t len) {
  packet->checksum = ChecksumPacket(packet, len);
  std::copy_n(reinterpret_cast<const std::byte*>(packet), (udp_packet->len = len), reinterpret_cast<std::byte*>(udp_packet->data));
  SDLNet_UDP_Send(udp_socket, 0, udp_packet.get());
}

void I_SendPacketTo(packet_header_t* packet, const std::size_t len, UDP_CHANNEL* const to) {
  packet->checksum = ChecksumPacket(packet, len);
  std::copy_n(reinterpret_cast<const std::byte*>(packet), (udp_packet->len = len), reinterpret_cast<std::byte*>(udp_packet->data));
  SDLNet_UDP_Send(udp_socket, *to, udp_packet.get());
}

void I_PrintAddress([[maybe_unused]] FILE* fp, [[maybe_unused]] UDP_CHANNEL* addr) {
  /*
    char *addy;
    Uint16 port;
    IPaddress *address;

    address = SDLNet_UDP_GetPeerAddress(udp_socket, player);

  //FIXME: if it cant resolv it may freeze up
    addy = SDLNet_ResolveIP(address);
    port = address->port;

    if(addy != NULL)
        fprintf(fp, "%s:%d", addy, port);
    else
      fprintf(fp, "Error");
  */
}

#endif /* HAVE_NET */
