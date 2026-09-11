/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <string.h>
#include <cstddef>
#include <algorithm>

#ifdef __WIN32__
    #include <winsock2.h>
    #include <ws2tcpip.h>

    #define socket_t    SOCKET
    #define sockaddr_t  SOCKADDR
    #define sockaddr_in_t  SOCKADDR_IN
#else
    #include <unistd.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>

    #define socket_t    int
    #define sockaddr_t  struct sockaddr
    #define sockaddr_in_t  struct sockaddr_in
    #define closesocket close
#endif

#ifndef INVALID_SOCKET
    #define INVALID_SOCKET  (socket_t)-1
#endif

#include "LAN.h"


namespace melonDS
{

const u32 kDiscoveryMagic = 0x444E414C; // LAND
const u32 kLANMagic = 0x504E414C; // LANP
const u32 kPacketMagic = 0x4946494E; // NIFI

// Keep the v1 handshake, player layout and discovery usable by older builds.
// New clients advertise this optional extension in ENet's connect user data;
// old hosts ignore it. Extension packets are sent only to opted-in peers.
const u32 kProtocolVersion = 1;
const u32 kPortCapability = 0x32504E4C; // LNP2

const u32 kLocalhost = 0x0100007F;

enum
{
    Chan_Cmd = 0,           // channel 0 -- control commands
    Chan_MP,                // channel 1 -- MP data exchange
};

enum
{
    Cmd_ClientInit = 1,     // 01 -- host->client -- init new client and assign ID
    Cmd_PlayerInfo,         // 02 -- client->host -- send client player info to host
    Cmd_PlayerList,         // 03 -- host->client -- broadcast updated player list
    Cmd_PlayerConnect,      // 04 -- both -- signal connected state (ready to receive MP frames)
    Cmd_PlayerDisconnect,   // 05 -- both -- signal disconnected state (not receiving MP frames)
    Cmd_PortSupport,        // 06 -- host->client -- optional endpoint extension accepted
    Cmd_PeerPorts,          // 07 -- host->client -- capable peers and observed UDP ports
};

const int kDiscoveryPort = 7063;
const int kLANPort = 7064;


LAN::LAN() noexcept : Inited(false)
{

    DiscoverySocket = INVALID_SOCKET;
    DiscoveryLastTick = 0;

    Active = false;
    IsHost = false;
    Host = nullptr;
    //Lag = false;

    memset(RemotePeers, 0, sizeof(RemotePeers));
    memset(Players, 0, sizeof(Players));
    MyPlayer = {};
    HostAddress = 0;
    NumPlayers = 0;
    MaxPlayers = 0;

    ConnectedBitmask = 0;

    LastHostID = -1;
    LastHostPeer = nullptr;

    FrameCount = 0;

    // TODO make this somewhat nicer
    if (enet_initialize() != 0)
    {
        Platform::Log(Platform::LogLevel::Error, "LAN: failed to initialize enet\n");
        return;
    }

    Platform::Log(Platform::LogLevel::Info, "LAN: enet initialized\n");
    Inited = true;
}

LAN::~LAN() noexcept
{
    EndSession();

    Inited = false;
    enet_deinitialize();


    Platform::Log(Platform::LogLevel::Info, "LAN: enet deinitialized\n");
}


std::map<u32, LAN::DiscoveryData> LAN::GetDiscoveryList()
{
    std::lock_guard lock(SessionMutex);
    auto ret = DiscoveryList;
    return ret;
}

std::vector<LAN::Player> LAN::GetPlayerList()
{
    std::lock_guard lock(SessionMutex);

    std::vector<Player> ret;
    for (int i = 0; i < 16; i++)
    {
        if (Players[i].Status == Player_None) continue;

        // make a copy of the player entry, fix up the address field
        Player newp = Players[i];
        if (newp.ID == MyPlayer.ID)
        {
            newp.IsLocalPlayer = true;
            newp.Address = kLocalhost;
        }
        else
        {
            newp.IsLocalPlayer = false;
            if (newp.Status == Player_Host)
                newp.Address = HostAddress;
        }

        ret.push_back(newp);
    }

    return ret;
}


bool LAN::StartDiscovery()
{
    std::lock_guard lock(SessionMutex);
    if (!Inited) return false;
    EndDiscovery();

    int res;

    DiscoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (DiscoverySocket == INVALID_SOCKET)
    {
        DiscoverySocket = INVALID_SOCKET;
        return false;
    }

    sockaddr_in_t saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port = htons(kDiscoveryPort);
    res = bind(DiscoverySocket, (const sockaddr_t*)&saddr, sizeof(saddr));
    if (res < 0)
    {
        closesocket(DiscoverySocket);
        DiscoverySocket = INVALID_SOCKET;
        return false;
    }

    int opt_true = 1;
    res = setsockopt(DiscoverySocket, SOL_SOCKET, SO_BROADCAST, (const char*)&opt_true, sizeof(int));
    if (res < 0)
    {
        closesocket(DiscoverySocket);
        DiscoverySocket = INVALID_SOCKET;
        return false;
    }

    DiscoveryLastTick = (u32)Platform::GetMSCount();
    DiscoveryList.clear();

    Active = true;
    return true;
}

void LAN::EndDiscovery()
{
    std::lock_guard lock(SessionMutex);
    if (!Inited) return;

    if (DiscoverySocket != INVALID_SOCKET)
    {
        closesocket(DiscoverySocket);
        DiscoverySocket = INVALID_SOCKET;
    }

    if (!Host)
        Active = false;
}

bool LAN::StartHost(const char* playername, int numplayers)
{
    std::lock_guard lock(SessionMutex);
    if (!Inited) return false;
    if (numplayers < 2 || numplayers > 16) return false;
    EndSession();
    Stats = {};

    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = kLANPort;

    Host = enet_host_create(&addr, 16, 2, 0, 0);
    if (!Host)
    {
        return false;
    }


    Player* player = &Players[0];
    memset(player, 0, sizeof(Player));
    player->ID = 0;
    strncpy(player->Name, playername, 31);
    player->Status = Player_Host;
    player->Address = kLocalhost;
    PeerPorts[0] = kLANPort;
    PortPeers = 1;
    PortProtocol = EndpointsReceived = true;
    NumPlayers = 1;
    MaxPlayers = numplayers;
    memcpy(&MyPlayer, player, sizeof(Player));


    HostAddress = kLocalhost;
    LastHostID = -1;
    LastHostPeer = nullptr;

    Active = true;
    IsHost = true;
    Connection = ClientState::Connected;

    StartDiscovery();
    return true;
}

bool LAN::StartClient(const char* playername, const char* host)
{
    std::lock_guard lock(SessionMutex);
    EndSession();
    Stats = {};
    if (!Inited) return false;

    ENetAddress addr{};
    // No DNS or five-second ENet wait on the UI thread.
    if (enet_address_set_host_ip(&addr, host) != 0) return false;
    addr.port = kLANPort;
    Host = enet_host_create(nullptr, 16, 2, 0, 0);
    if (!Host) return false;
    ENetPeer* peer = enet_host_connect(Host, &addr, 2, kPortCapability);
    if (!peer)
    {
        EndSession();
        return false;
    }

    MyPlayer.ID = -1;
    strncpy(MyPlayer.Name, playername, sizeof(MyPlayer.Name) - 1);
    MyPlayer.Status = Player_Connecting;
    HostAddress = addr.host;
    RemotePeers[0] = peer;
    peer->data = &Players[0];
    ConnectionStartTick = static_cast<u32>(Platform::GetMSCount());
    Connection = ClientState::Connecting;
    Active = true;
    return true;
}

void LAN::EndSession()
{
    ++PendingStops;
    std::lock_guard lock(SessionMutex);
    EndDiscovery();
    Active = false;
    while (!RXQueue.empty())
    {
        enet_packet_destroy(RXQueue.front());
        RXQueue.pop();
    }
    if (Host)
    {
        for (auto* peer : RemotePeers)
            if (peer) enet_peer_disconnect_now(peer, 0);
        enet_host_destroy(Host);
        Host = nullptr;
    }
    memset(RemotePeers, 0, sizeof(RemotePeers));
    memset(PeerConnectTicks, 0, sizeof(PeerConnectTicks));
    memset(PeerPorts, 0, sizeof(PeerPorts));
    PortPeers = 0;
    PortProtocol = EndpointsReceived = false;
    memset(Players, 0, sizeof(Players));
    MyPlayer = {};
    HostAddress = 0;
    NumPlayers = MaxPlayers = 0;
    ConnectedBitmask = 0;
    LastHostID = -1;
    LastHostPeer = nullptr;
    FrameCount = 0;
    IsHost = false;
    ClientInitReceived = false;
    Connection = ClientState::Idle;
    DiscoveryList.clear();
    --PendingStops;
}

LAN::ClientState LAN::GetClientState()
{
    std::lock_guard lock(SessionMutex);
    return Connection;
}

LAN::ReceiveStats LAN::GetReceiveStats()
{
    std::lock_guard lock(SessionMutex);
    auto result = Stats;
    result.QueuedPackets = RXQueue.size();
    if (!RXQueue.empty())
    {
        const auto* header = reinterpret_cast<const MPPacketHeader*>(RXQueue.front()->data);
        result.OldestQueuedAgeMS = static_cast<u32>(Platform::GetMSCount() - header->Magic);
    }
    return result;
}

int LAN::GetNumPlayers()
{
    std::lock_guard lock(SessionMutex);
    return NumPlayers;
}

int LAN::GetMaxPlayers()
{
    std::lock_guard lock(SessionMutex);
    return MaxPlayers;
}


void LAN::ProcessDiscovery()
{
    if (DiscoverySocket == INVALID_SOCKET)
        return;

    u32 tick = (u32)Platform::GetMSCount();
    if ((tick - DiscoveryLastTick) < 1000)
        return;

    DiscoveryLastTick = tick;

    if (IsHost)
    {
        // advertise this LAN session over the network

        DiscoveryData beacon;
        memset(&beacon, 0, sizeof(beacon));
        beacon.Magic = kDiscoveryMagic;
        beacon.Version = kProtocolVersion;
        beacon.Tick = tick;
        snprintf(beacon.SessionName, 64, "%s's game", MyPlayer.Name);
        beacon.NumPlayers = NumPlayers;
        beacon.MaxPlayers = MaxPlayers;
        beacon.Status = 0; // TODO

        sockaddr_in_t saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        saddr.sin_port = htons(kDiscoveryPort);

        sendto(DiscoverySocket, (const char*)&beacon, sizeof(beacon), 0, (const sockaddr_t*)&saddr, sizeof(saddr));
    }
    else
    {

        // listen for LAN sessions

        fd_set fd;
        struct timeval tv;
        for (unsigned received = 0; received < 64; ++received)
        {
            FD_ZERO(&fd); FD_SET(DiscoverySocket, &fd);
            tv.tv_sec = 0; tv.tv_usec = 0;
            if (select(DiscoverySocket+1, &fd, nullptr, nullptr, &tv) <= 0)
                break;

            DiscoveryData beacon;
            sockaddr_in_t raddr;
            socklen_t ralen = sizeof(raddr);

            int rlen = recvfrom(DiscoverySocket, (char*)&beacon, sizeof(beacon), 0, (sockaddr_t*)&raddr, &ralen);
            if (rlen < sizeof(beacon)) continue;
            if (beacon.Magic != kDiscoveryMagic) continue;
            if (beacon.Version != kProtocolVersion) continue;
            if (beacon.MaxPlayers < 2 || beacon.MaxPlayers > 16) continue;
            if (beacon.NumPlayers > beacon.MaxPlayers) continue;

            u32 key = ntohl(raddr.sin_addr.s_addr);

            beacon.Magic = tick;
            beacon.SessionName[63] = '\0';
            DiscoveryList[key] = beacon;
        }

        // cleanup: remove hosts that haven't given a sign of life in the last 5 seconds

        std::vector<u32> deletelist;

        for (const auto& [key, data] : DiscoveryList)
        {
            u32 age = tick - data.Magic;
            if (age < 5000) continue;

            deletelist.push_back(key);
        }

        for (const auto& key : deletelist)
        {
            DiscoveryList.erase(key);
        }

    }
}

void LAN::HostUpdatePlayerList()
{
    u8 cmd[2+sizeof(Players)];
    cmd[0] = Cmd_PlayerList;
    cmd[1] = (u8)NumPlayers;
    memcpy(&cmd[2], Players, sizeof(Players));
    ENetPacket* pkt = enet_packet_create(cmd, 2+sizeof(Players), ENET_PACKET_FLAG_RELIABLE);
    if (pkt) enet_host_broadcast(Host, Chan_Cmd, pkt);
    // The legacy list remains byte-for-byte v1. Ports travel separately and
    // follow that list on the same reliable channel for extension clients.
    u8 ports[35]{Cmd_PeerPorts, u8(PortPeers), u8(PortPeers >> 8)};
    for (int id = 0; id < 16; ++id)
    {
        ports[3 + id * 2] = u8(PeerPorts[id]);
        ports[4 + id * 2] = u8(PeerPorts[id] >> 8);
    }
    for (int id = 1; id < 16; ++id)
    {
        if (!(PortPeers & (1 << id)) || !RemotePeers[id]) continue;
        auto* packet = enet_packet_create(ports, sizeof(ports), ENET_PACKET_FLAG_RELIABLE);
        if (packet && enet_peer_send(RemotePeers[id], Chan_Cmd, packet) != 0)
            enet_packet_destroy(packet);
    }
}

void LAN::ClearPeer(int id)
{
    auto* peer = RemotePeers[id];
    if (peer) peer->data = nullptr;
    RemotePeers[id] = nullptr;
    ConnectedBitmask &= ~(1 << id);
    if (LastHostID == id) LastHostPeer = nullptr;
    // A reused player number must not inherit frames from its previous peer.
    const auto count = RXQueue.size();
    for (size_t i = 0; i < count; ++i)
    {
        auto* packet = RXQueue.front();
        RXQueue.pop();
        if (packet->userData == peer) enet_packet_destroy(packet);
        else RXQueue.push(packet);
    }
}

void LAN::SendLocalReady(ENetPeer* peer)
{
    if (MyPlayer.ID < 0 || !(ConnectedBitmask & (1 << MyPlayer.ID))) return;
    const u8 cmd = Cmd_PlayerConnect;
    auto* packet = enet_packet_create(&cmd, 1, ENET_PACKET_FLAG_RELIABLE);
    if (packet && enet_peer_send(peer, Chan_Cmd, packet) != 0)
        enet_packet_destroy(packet);
}

void LAN::ConnectPeers()
{
    if (IsHost || !Host || !ClientInitReceived || MyPlayer.Status != Player_Client) return;
    if (PortProtocol && !EndpointsReceived) return;
    const u32 tick = static_cast<u32>(Platform::GetMSCount());
    bool connected = true;
    for (int id = 1; id < MaxPlayers; ++id)
    {
        if (id == MyPlayer.ID || Players[id].Status != Player_Client) continue;
        // One direction avoids duplicate connections. A lower ID may receive
        // the connect before its authoritative player list; retry after it has
        // rejected that unknown endpoint, without spinning each frame.
        const bool extendedPeer = PortProtocol && (PortPeers & (1 << id));
        if ((!extendedPeer || id < MyPlayer.ID) && !RemotePeers[id] &&
            (!PeerConnectTicks[id] || static_cast<u32>(tick - PeerConnectTicks[id]) >= 250))
        {
            PeerConnectTicks[id] = tick;
            const u16 port = PortProtocol ? PeerPorts[id] : kLANPort;
            ENetAddress address{Players[id].Address, port};
            if (auto* peer = enet_host_connect(Host, &address, 2, 0))
            {
                RemotePeers[id] = peer;
                peer->data = &Players[id];
            }
        }
        if (extendedPeer && (!RemotePeers[id] || RemotePeers[id]->state != ENET_PEER_STATE_CONNECTED))
            connected = false;
    }
    if (connected && Connection == ClientState::Connecting)
        Connection = ClientState::Connected;
}

void LAN::ReadPeerPorts(const ENetEvent& event)
{
    if (!PortProtocol || !ClientInitReceived || MyPlayer.Status != Player_Client ||
        event.peer != RemotePeers[0] || event.packet->dataLength != 35) return;
    const auto* data = event.packet->data;
    const u16 capable = u16(data[1]) | (u16(data[2]) << 8);
    if (!(capable & 1) || !(capable & (1 << MyPlayer.ID)) || (capable >> MaxPlayers)) return;
    u16 ports[16];
    for (int id = 0; id < 16; ++id)
    {
        ports[id] = u16(data[3 + id * 2]) | (u16(data[4 + id * 2]) << 8);
        if (Players[id].Status != Player_None && !ports[id]) return;
    }
    for (int id = 1; id < 16; ++id)
    {
        if (RemotePeers[id] && (PeerPorts[id] != ports[id] || ((PortPeers ^ capable) & (1 << id))))
        {
            enet_peer_disconnect_now(RemotePeers[id], 0);
            ClearPeer(id);
            PeerConnectTicks[id] = 0;
        }
    }
    memcpy(PeerPorts, ports, sizeof(PeerPorts));
    PortPeers = capable;
    EndpointsReceived = true;
    ConnectPeers();
}

bool LAN::ReadPlayerList(const ENetEvent& event)
{
    if (event.peer != RemotePeers[0] || !ClientInitReceived ||
        event.packet->dataLength != 2 + sizeof(Players)) return false;
    const auto* data = event.packet->data;
    if (data[1] < 2 || data[1] > MaxPlayers) return false;

    Player next[16]{};
    int count = 0;
    for (int i = 0; i < 16; ++i)
    {
        const auto* entry = data + 2 + i * sizeof(Player);
        int status;
        memcpy(&status, entry + offsetof(Player, Status), sizeof(status));
        if (status < Player_None || status > Player_Disconnected) return false;
        // Validate enum bytes before evaluating them. The native wire bool is
        // UI metadata, never trusted as a received bool object.
        memcpy(&next[i], entry, sizeof(Player));
        next[i].IsLocalPlayer = false;
        next[i].Name[31] = '\0';
        if (status == Player_None) continue;
        if (i >= MaxPlayers || next[i].ID != i || (status == Player_Host && i != 0))
            return false;
        ++count;
    }
    if (count != data[1] || next[0].Status != Player_Host ||
        next[MyPlayer.ID].Status != Player_Client) return false;

    for (int i = 1; i < 16; ++i)
    {
        if (next[i].Status == Player_Client && next[i].Address == Players[i].Address) continue;
        if (RemotePeers[i])
        {
            enet_peer_disconnect_now(RemotePeers[i], 0);
            ClearPeer(i);
        }
        PeerPorts[i] = 0;
        PeerConnectTicks[i] = 0;
    }
    memcpy(Players, next, sizeof(Players));
    NumPlayers = count;
    MyPlayer.Status = Player_Client;
    EndpointsReceived = false;
    ConnectPeers();
    return true;
}

void LAN::ProcessHostEvent(ENetEvent& event)
{
    switch (event.type)
    {
    case ENET_EVENT_TYPE_CONNECT:
        {
            if ((NumPlayers >= MaxPlayers) || (NumPlayers >= 16))
            {
                // game is full, reject connection
                enet_peer_disconnect(event.peer, 0);
                break;
            }

            // client connected; assign player number

            int id;
            for (id = 1; id < MaxPlayers; id++)
            {
                if (Players[id].Status == Player_None) break;
            }

            if (id < MaxPlayers)
            {
                u8 cmd[11];
                cmd[0] = Cmd_ClientInit;
                cmd[1] = (u8)kLANMagic;
                cmd[2] = (u8)(kLANMagic >> 8);
                cmd[3] = (u8)(kLANMagic >> 16);
                cmd[4] = (u8)(kLANMagic >> 24);
                cmd[5] = (u8)kProtocolVersion;
                cmd[6] = (u8)(kProtocolVersion >> 8);
                cmd[7] = (u8)(kProtocolVersion >> 16);
                cmd[8] = (u8)(kProtocolVersion >> 24);
                cmd[9] = (u8)id;
                cmd[10] = MaxPlayers;
                ENetPacket* pkt = enet_packet_create(cmd, 11, ENET_PACKET_FLAG_RELIABLE);
                if (!pkt || enet_peer_send(event.peer, Chan_Cmd, pkt) != 0)
                {
                    if (pkt) enet_packet_destroy(pkt);
                    enet_peer_disconnect_now(event.peer, 0);
                    break;
                }

                const bool portSupport = event.data == kPortCapability;
                if (portSupport)
                {
                    const u8 accepted = Cmd_PortSupport;
                    auto* response = enet_packet_create(&accepted, 1, ENET_PACKET_FLAG_RELIABLE);
                    if (!response || enet_peer_send(event.peer, Chan_Cmd, response) != 0)
                    {
                        if (response) enet_packet_destroy(response);
                        enet_peer_disconnect_now(event.peer, 0);
                        break;
                    }
                    PortPeers |= (1 << id);
                }

                Players[id].ID = id;
                Players[id].Status = Player_Connecting;
                Players[id].Address = event.peer->address.host;
                PeerPorts[id] = event.peer->address.port;
                event.peer->data = &Players[id];
                NumPlayers++;


                RemotePeers[id] = event.peer;
                SendLocalReady(event.peer);
            }
            else
            {
                // ???
                enet_peer_disconnect(event.peer, 0);
            }
        }
        break;

    case ENET_EVENT_TYPE_DISCONNECT:
        {
            Player* player = (Player*)event.peer->data;
            if (!player || player->ID < 0 || player->ID >= 16 ||
                RemotePeers[player->ID] != event.peer || player != &Players[player->ID]) break;

            const int id = player->ID;
            ClearPeer(id);
            PeerPorts[id] = 0;
            PortPeers &= ~(1 << id);
            *player = {};
            NumPlayers--;

            // broadcast updated player list
            HostUpdatePlayerList();
        }
        break;

    case ENET_EVENT_TYPE_RECEIVE:
        {
            const std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)> packet(event.packet, enet_packet_destroy);
            if (event.channelID != Chan_Cmd || event.packet->dataLength < 1) break;

            u8* data = (u8*)event.packet->data;
            switch (data[0])
            {
            case Cmd_PlayerInfo: // client sending player info
                {
                    if (event.packet->dataLength != (9+sizeof(Player))) break;

                    u32 magic = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
                    u32 version = data[5] | (data[6] << 8) | (data[7] << 16) | (data[8] << 24);
                    if ((magic != kLANMagic) || (version != kProtocolVersion))
                    {
                        enet_peer_disconnect(event.peer, 0);
                        break;
                    }

                    Player player;
                    memcpy(&player, &data[9], sizeof(Player));
                    player.Name[31] = '\0';

                    Player* hostside = (Player*)event.peer->data;
                    if (!hostside || player.ID <= 0 || player.ID >= MaxPlayers ||
                        hostside != &Players[player.ID] || RemotePeers[player.ID] != event.peer)
                    {
                        enet_peer_disconnect(event.peer, 0);
                        break;
                    }


                    player.Status = Player_Client;
                    player.IsLocalPlayer = false;
                    player.Ping = event.peer->roundTripTime;
                    player.Address = event.peer->address.host;
                    PeerPorts[player.ID] = event.peer->address.port;
                    memcpy(hostside, &player, sizeof(Player));


                    // broadcast updated player list
                    HostUpdatePlayerList();
                }
                break;

            case Cmd_PlayerConnect: // player connected
                {
                    if (event.packet->dataLength != 1) break;
                    Player* player = (Player*)event.peer->data;
                    if (!player || player->ID < 0 || player->ID >= 16 ||
                        RemotePeers[player->ID] != event.peer || player != &Players[player->ID]) break;

                    ConnectedBitmask |= (1 << player->ID);
                }
                break;

            case Cmd_PlayerDisconnect: // player disconnected
                {
                    if (event.packet->dataLength != 1) break;
                    Player* player = (Player*)event.peer->data;
                    if (!player || player->ID < 0 || player->ID >= 16 ||
                        RemotePeers[player->ID] != event.peer || player != &Players[player->ID]) break;

                    ConnectedBitmask &= ~(1 << player->ID);
                }
                break;
            }

        }
        break;
    case ENET_EVENT_TYPE_NONE:
        break;
    }
}

void LAN::ProcessClientEvent(ENetEvent& event)
{
    switch (event.type)
    {
    case ENET_EVENT_TYPE_CONNECT:
        {
            if (event.peer == RemotePeers[0]) break;
            int playerid = -1;
            for (int i = 1; i < 16; ++i)
            {
                if (i == MyPlayer.ID || Players[i].Status != Player_Client) continue;
                if (RemotePeers[i] == event.peer ||
                    (!RemotePeers[i] && (!(PortPeers & (1 << i)) || i > MyPlayer.ID) &&
                     Players[i].Address == event.peer->address.host &&
                     (!PortProtocol || PeerPorts[i] == event.peer->address.port)))
                {
                    playerid = i;
                    break;
                }
            }
            if (playerid < 0)
            {
                enet_peer_disconnect_now(event.peer, 0);
                break;
            }
            RemotePeers[playerid] = event.peer;
            event.peer->data = &Players[playerid];
            SendLocalReady(event.peer);
            ConnectPeers();
        }
        break;
    case ENET_EVENT_TYPE_DISCONNECT:
        {
            if (event.peer == RemotePeers[0])
            {
                Connection = ClientState::Disconnected;
                break;
            }
            auto* player = static_cast<Player*>(event.peer->data);
            if (!player || player->ID < 0 || player->ID >= 16 ||
                RemotePeers[player->ID] != event.peer || player != &Players[player->ID]) break;
            ClearPeer(player->ID);
            // Membership is authoritative on the lobby host. A transient mesh
            // disconnect can be retried; host removal clears the slot and queue.
        }
        break;
    case ENET_EVENT_TYPE_RECEIVE:
        {
            const std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)> packet(event.packet, enet_packet_destroy);
            if (event.channelID != Chan_Cmd || packet->dataLength < 1) break;
            const u8* data = packet->data;
            switch (data[0])
            {
            case Cmd_ClientInit:
                {
                    if (Connection != ClientState::Connecting || ClientInitReceived ||
                        event.peer != RemotePeers[0] || packet->dataLength != 11) break;
                    const u32 magic = u32(data[1]) | (u32(data[2]) << 8) |
                                      (u32(data[3]) << 16) | (u32(data[4]) << 24);
                    const u32 version = u32(data[5]) | (u32(data[6]) << 8) |
                                        (u32(data[7]) << 16) | (u32(data[8]) << 24);
                    if (magic != kLANMagic) break;
                    if (version != kProtocolVersion)
                    {
                        Connection = ClientState::Incompatible;
                        break;
                    }
                    if (data[10] < 2 || data[10] > 16 || data[9] == 0 || data[9] >= data[10])
                    {
                        Connection = ClientState::Failed;
                        break;
                    }
                    MaxPlayers = data[10];
                    MyPlayer.ID = data[9];
                    u8 cmd[9 + sizeof(Player)];
                    memcpy(cmd, data, 9);
                    cmd[0] = Cmd_PlayerInfo;
                    memcpy(cmd + 9, &MyPlayer, sizeof(Player));
                    auto* reply = enet_packet_create(cmd, sizeof(cmd), ENET_PACKET_FLAG_RELIABLE);
                    if (!reply || enet_peer_send(event.peer, Chan_Cmd, reply) != 0)
                    {
                        if (reply) enet_packet_destroy(reply);
                        Connection = ClientState::Failed;
                        break;
                    }
                    ClientInitReceived = true;
                    enet_host_flush(Host);
                }
                break;
            case Cmd_PlayerList:
                {
                    ReadPlayerList(event);
                }
                break;
            case Cmd_PortSupport:
                if (packet->dataLength == 1 && event.peer == RemotePeers[0] &&
                    ClientInitReceived && Connection == ClientState::Connecting)
                    PortProtocol = true;
                break;
            case Cmd_PeerPorts:
                ReadPeerPorts(event);
                break;
            case Cmd_PlayerConnect:
            case Cmd_PlayerDisconnect:
                {
                    if (packet->dataLength != 1) break;
                    auto* player = static_cast<Player*>(event.peer->data);
                    if (!player || player->ID < 0 || player->ID >= 16 ||
                        RemotePeers[player->ID] != event.peer || player != &Players[player->ID]) break;
                    if (data[0] == Cmd_PlayerConnect) ConnectedBitmask |= (1 << player->ID);
                    else ConnectedBitmask &= ~(1 << player->ID);
                }
                break;
            }
        }
        break;
    case ENET_EVENT_TYPE_NONE:
        break;
    }
}

void LAN::ProcessEvent(ENetEvent& event)
{
    if (IsHost)
        ProcessHostEvent(event);
    else
        ProcessClientEvent(event);
}

bool LAN::ValidateMPPacket(const ENetEvent& event) const
{
    if (event.packet->dataLength < sizeof(MPPacketHeader))
        return false;

    MPPacketHeader header;
    memcpy(&header, event.packet->data, sizeof(header));

    // Wifi::TXSendFrame can send the whole 0x2000-byte WiFi RAM/TXBuffer.
    // The existing 2048/1024-byte receive crops are not wire payload limits.
    if (header.Magic != kPacketMagic || header.Length > 0x2000 ||
        header.Length != event.packet->dataLength - sizeof(MPPacketHeader))
        return false;

    const u32 type = header.Type & 0xFFFF;
    const u32 aid = header.Type >> 16;
    if (type > 3)
        return false;
    if (type == 2)
    {
        // A zero-length AID-0 reply is the v1 "no data" notification used by
        // Wifi::FinishRX so the host need not wait for this client's timeout.
        if (aid > 15 || (aid == 0 && header.Length != 0))
            return false;
    }
    else if (aid != 0)
        return false;

    if (header.SenderID >= 16 || header.SenderID == static_cast<u32>(MyPlayer.ID))
        return false;

    // A DS AID is not a LAN player ID. Bind the sender to the established ENet
    // peer instead. Do not require ConnectedBitmask/Player_Client here: reliable
    // control messages can arrive after MP frames on the other ENet channel.
    return event.peer && RemotePeers[header.SenderID] == event.peer &&
           event.peer->data == &Players[header.SenderID] &&
           Players[header.SenderID].ID == static_cast<int>(header.SenderID);
}

// 0 = per-frame processing of events and eventual misc. frame
// 1 = checking if a misc. frame has arrived
// 2 = waiting for a MP frame
void LAN::ProcessLAN(int type, u32 timeout)
{
    if (!Host || PendingStops.load()) return;

    u64 time_last = Platform::GetMSCount();

    // see if we have queued packets already, get rid of the stale ones
    // any incoming packet should be consumed by the core quickly, so if
    // they've been sitting in the queue for more than one frame's time,
    // we can assume they're stale
    while (!RXQueue.empty())
    {
        ENetPacket* enetpacket = RXQueue.front();
        MPPacketHeader* header = (MPPacketHeader*)&enetpacket->data[0];
        u32 packettime = header->Magic;

        if (static_cast<u32>(time_last - packettime) > 16)
        {
            ++Stats.ExpiredPackets;
            RXQueue.pop();
            enet_packet_destroy(enetpacket);
        }
        else
        {
            // we got a packet, depending on what the caller wants we might be able to return now
            if (type == 2) return;
            if (type == 1)
            {
                // if looking for a misc. frame, we shouldn't be receiving a MP frame
                if (header->Type == 0)
                    return;

                RXQueue.pop();
                enet_packet_destroy(enetpacket);
            }

            break;
        }
    }

    time_last = Platform::GetMSCount();

    ENetEvent event;
    // A stream of control packets must yield to UI cancellation/frame work.
    for (unsigned received = 0; received < 64;)
    {
        if (PendingStops.load()) return;
        // Preserve the configured receive deadline while making shutdown
        // observable even when a user has selected a long inactivity wait.
        const u32 wait = std::min(timeout, 25u);
        const u64 waitStarted = wait ? Platform::GetMSCount() : 0;
        const int result = enet_host_service(Host, &event, wait);
        if (wait)
        {
            const u64 waitEnded = Platform::GetMSCount();
            if (waitEnded >= waitStarted)
            {
                const u64 waited = waitEnded - waitStarted;
                ++Stats.WaitSamples;
                Stats.RequestedWaitMS += wait;
                Stats.WaitTimeMS += waited;
                Stats.MaxWaitMS = std::max(Stats.MaxWaitMS, waited);
            }
            else
                ++Stats.ClockRegressions;
        }
        if (PendingStops.load())
        {
            if (result > 0 && event.type == ENET_EVENT_TYPE_RECEIVE)
                enet_packet_destroy(event.packet);
            return;
        }
        if (result <= 0)
        {
            if (result < 0) ++Stats.ServiceErrors;
            if (result == 0 && timeout > wait)
            {
                const u64 time = Platform::GetMSCount();
                const u64 elapsed = time - time_last;
                // A backwards or stationary clock must not extend the wait
                // or turn a zero-result service call into a busy loop.
                if (!elapsed || elapsed >= timeout) return;
                timeout -= static_cast<u32>(elapsed);
                time_last = time;
                continue;
            }
            return;
        }
        ++received; // Empty wait slices do not spend the packet work limit.
        if (event.type == ENET_EVENT_TYPE_RECEIVE && event.channelID == Chan_MP)
        {
            if (!ValidateMPPacket(event))
            {
                ++Stats.RejectedPackets;
                enet_packet_destroy(event.packet);
            }
            else
            {
                // mark this packet with the time it was received
                MPPacketHeader* header = (MPPacketHeader*)event.packet->data;
                header->Magic = (u32)Platform::GetMSCount();

                event.packet->userData = event.peer;
                RXQueue.push(event.packet);
                ++Stats.ReceivedPackets;
                Stats.PeakQueuedPackets = std::max<u64>(Stats.PeakQueuedPackets, RXQueue.size());

                // return now -- if we are receiving MP frames, if we keep going
                // we'll consume too many even if we have no timeout set
                return;
            }
        }
        else
        {
            ProcessEvent(event);
        }

        if (Connection == ClientState::Failed || Connection == ClientState::Incompatible ||
            Connection == ClientState::Disconnected)
        {
            const auto result = Connection;
            EndSession();
            Connection = result;
            return;
        }

        if (type == 2)
        {
            const u64 time = Platform::GetMSCount();
            const u64 elapsed = time - time_last;
            // Keep the full host clock: low32 wrap is ordinary elapsed time,
            // while expiry or a backwards jump must never enlarge the wait.
            if (elapsed >= timeout) return;
            timeout -= static_cast<u32>(elapsed);
            time_last = time;
        }
    }
    ++Stats.WorkLimitReturns;
}

void LAN::Process()
{
    if (PendingStops.load()) return;
    std::lock_guard lock(SessionMutex);
    if (!Active) return;

    ProcessDiscovery();
    ProcessLAN(0);
    ConnectPeers();
    if (Connection == ClientState::Connecting &&
        static_cast<u32>(Platform::GetMSCount() - ConnectionStartTick) >= 5000)
    {
        EndSession();
        Connection = ClientState::TimedOut;
    }

    FrameCount++;
    if (FrameCount >= 60)
    {
        FrameCount = 0;


        for (int i = 0; i < 16; i++)
        {
            if (Players[i].Status == Player_None) continue;
            if (i == MyPlayer.ID) continue;
            if (!RemotePeers[i]) continue;

            Players[i].Ping = RemotePeers[i]->roundTripTime;
        }

    }
}


void LAN::Begin(int inst)
{
    std::lock_guard lock(SessionMutex);
    if (!Host || Connection != ClientState::Connected) return;

    ConnectedBitmask |= (1 << MyPlayer.ID);
    LastHostID = -1;
    LastHostPeer = nullptr;

    u8 cmd = Cmd_PlayerConnect;
    ENetPacket* pkt = enet_packet_create(&cmd, 1, ENET_PACKET_FLAG_RELIABLE);
    if (pkt) enet_host_broadcast(Host, Chan_Cmd, pkt);
}

void LAN::End(int inst)
{
    std::lock_guard lock(SessionMutex);
    if (!Host || Connection != ClientState::Connected) return;

    ConnectedBitmask &= ~(1 << MyPlayer.ID);

    u8 cmd = Cmd_PlayerDisconnect;
    ENetPacket* pkt = enet_packet_create(&cmd, 1, ENET_PACKET_FLAG_RELIABLE);
    if (pkt) enet_host_broadcast(Host, Chan_Cmd, pkt);
}


int LAN::SendPacketGeneric(u32 type, u8* packet, int len, u64 timestamp)
{
    if (PendingStops.load()) return 0;
    std::lock_guard lock(SessionMutex);
    if (PendingStops.load() || !Host || Connection != ClientState::Connected || len < 0 || len > 0x2000 || (!packet && len)) return 0;

    // TODO make the reliable part optional?
    //u32 flags = ENET_PACKET_FLAG_RELIABLE;
    u32 flags = ENET_PACKET_FLAG_UNSEQUENCED;

    ENetPacket* enetpacket = enet_packet_create(nullptr, sizeof(MPPacketHeader)+len, flags);
    if (!enetpacket) return 0;

    MPPacketHeader pktheader;
    pktheader.Magic = 0x4946494E;
    pktheader.SenderID = MyPlayer.ID;
    pktheader.Type = type;
    pktheader.Length = len;
    pktheader.Timestamp = timestamp;
    memcpy(&enetpacket->data[0], &pktheader, sizeof(MPPacketHeader));
    if (len)
        memcpy(&enetpacket->data[sizeof(MPPacketHeader)], packet, len);

    if (((type & 0xFFFF) == 2) && LastHostPeer)
    {
        if (enet_peer_send(LastHostPeer, Chan_MP, enetpacket) != 0)
        {
            enet_packet_destroy(enetpacket);
            return 0;
        }
    }
    else
        enet_host_broadcast(Host, Chan_MP, enetpacket);
    enet_host_flush(Host);

    return len;
}

int LAN::RecvPacketGeneric(u8* packet, bool block, u64* timestamp, u32 capacity)
{
    if (PendingStops.load() || !packet || !capacity) return 0;
    std::lock_guard lock(SessionMutex);
    if (!Host || Connection != ClientState::Connected) return 0;

    ProcessLAN(block ? 2 : 1, block ? std::max(GetRecvTimeout(), 0) : 0);
    if (PendingStops.load() || RXQueue.empty()) return 0;

    ENetPacket* enetpacket = RXQueue.front();
    RXQueue.pop();
    MPPacketHeader* header = (MPPacketHeader*)&enetpacket->data[0];

    u32 len = header->Length;
    if (len)
    {
        if (len > capacity) len = capacity;

        memcpy(packet, &enetpacket->data[sizeof(MPPacketHeader)], len);

        if (header->Type == 1)
        {
            LastHostID = header->SenderID;
            LastHostPeer = (ENetPeer*)enetpacket->userData;
        }
    }

    if (timestamp) *timestamp = header->Timestamp;
    enet_packet_destroy(enetpacket);
    return len;
}


int LAN::SendPacket(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(0, packet, len, timestamp);
}

int LAN::RecvPacket(int inst, u8* packet, u64* timestamp, u32 capacity)
{
    return RecvPacketGeneric(packet, false, timestamp, capacity);
}


int LAN::SendCmd(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(1, packet, len, timestamp);
}

int LAN::SendReply(int inst, u8* packet, int len, u64 timestamp, u16 aid)
{
    return SendPacketGeneric(2 | (aid<<16), packet, len, timestamp);
}

int LAN::SendAck(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(3, packet, len, timestamp);
}

int LAN::RecvHostPacket(int inst, u8* packet, u64* timestamp, u32 capacity)
{
    if (PendingStops.load()) return 0;
    std::lock_guard lock(SessionMutex);
    if (Connection == ClientState::Disconnected) return -1;
    if (LastHostID != -1)
    {
        // check if the host is still connected

        if (!(ConnectedBitmask & (1<<LastHostID)))
            return -1;
    }

    return RecvPacketGeneric(packet, true, timestamp, capacity);
}

u16 LAN::RecvReplies(int inst, u8* packets, u64 timestamp, u16 aidmask)
{
    if (PendingStops.load()) return 0;
    std::lock_guard lock(SessionMutex);
    if (!Host || Connection != ClientState::Connected) return 0;
    ++Stats.ReplyCalls;

    u16 ret = 0;
    auto partialResult = [&]
    {
        if (ret) ++Stats.PartialReplyReturns;
        return ret;
    };
    u16 myinstmask = 1 << MyPlayer.ID;

    if ((myinstmask & ConnectedBitmask) == ConnectedBitmask)
        return 0;

    const u32 timeout = std::max(GetRecvTimeout(), 0);
    u64 started = Platform::GetMSCount();
    // Keep the existing inactivity wait when a new peer replies, but do not
    // renew it for duplicates or unrelated traffic. Also yield under a stream
    // of already available packets, leaving the rest for the next call.
    for (unsigned received = 0; received < 64; ++received)
    {
        if (PendingStops.load()) return partialResult();
        const u64 elapsed = Platform::GetMSCount() - started;
        if (received && timeout && elapsed >= timeout) return partialResult();
        ProcessLAN(2, elapsed < timeout ? timeout - static_cast<u32>(elapsed) : 0);
        if (PendingStops.load() || RXQueue.empty())
        {
            // no more replies available
            return partialResult();
        }

        ENetPacket* enetpacket = RXQueue.front();
        RXQueue.pop();
        MPPacketHeader* header = (MPPacketHeader*)&enetpacket->data[0];
        bool good = true;
        if ((header->Type & 0xFFFF) != 2)
            good = false;
        else if (header->Timestamp < timestamp && timestamp - header->Timestamp > 32)
            good = false;

        if (good)
        {
            u32 len = header->Length;
            if (len)
            {
                if (len > 1024) len = 1024;

                u32 aid = header->Type >> 16;
                memcpy(&packets[(aid-1)*1024], &enetpacket->data[sizeof(MPPacketHeader)], len);

                ret |= (1<<aid);
            }

            const u16 sender = 1 << header->SenderID;
            if (!(myinstmask & sender))
            {
                ++Stats.NewPeerReplies;
                myinstmask |= sender;
                started = Platform::GetMSCount();
            }
            else
                ++Stats.DuplicateReplies;
            if (((myinstmask & ConnectedBitmask) == ConnectedBitmask) ||
                ((ret & aidmask) == aidmask))
            {
                // all the clients have sent their reply
                enet_packet_destroy(enetpacket);
                return ret;
            }
        }

        enet_packet_destroy(enetpacket);
    }
    ++Stats.WorkLimitReturns;
    return partialResult();
}

}
