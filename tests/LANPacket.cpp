// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone target: this TU includes the real LAN implementation. Link ENet and
// PlatformSync.cpp, but not LAN.cpp/net-utils again. ENet owns the actual packets;
// only its event delivery and outbound transport are replaced. No sockets open.
#include "net/LAN.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string_view>
#include <span>
#include <vector>

namespace
{
std::deque<ENetEvent> Events;
unsigned FreedPackets = 0;
ENetPeer* SentPeer = nullptr;
std::vector<melonDS::u8> SentPacket;
melonDS::u64 Tick = 1000;
ENetPeer ClientPeer{};
unsigned HostsCreated = 0, HostsDestroyed = 0;

ENetHost* MakeHost(const ENetAddress*, size_t, size_t, enet_uint32, enet_uint32)
{
    ++HostsCreated;
    return new ENetHost{};
}
void DestroyHost(ENetHost* host)
{
    ++HostsDestroyed;
    delete host;
    for (auto& event : Events)
        if (event.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(event.packet);
    Events.clear();
}
ENetPeer* ConnectHost(ENetHost*, const ENetAddress* address, size_t, enet_uint32)
{
    ClientPeer = {};
    ClientPeer.address = *address;
    ClientPeer.state = ENET_PEER_STATE_CONNECTED;
    return &ClientPeer;
}
void DisconnectPeer(ENetPeer* peer, enet_uint32) { peer->state = ENET_PEER_STATE_DISCONNECTED; }

int InjectHostService(ENetHost*, ENetEvent* event, enet_uint32)
{
    if (Events.empty()) return 0;
    *event = Events.front();
    Events.pop_front();
    return 1;
}

int CapturePeerSend(ENetPeer* peer, enet_uint8, ENetPacket* packet)
{
    SentPeer = peer;
    SentPacket.assign(packet->data, packet->data + packet->dataLength);
    enet_packet_destroy(packet);
    return 0;
}

void CaptureBroadcast(ENetHost*, enet_uint8 channel, ENetPacket* packet)
{
    CapturePeerSend(nullptr, channel, packet);
}

void NoFlush(ENetHost*) {}
}

#define enet_host_service InjectHostService
#define enet_peer_send CapturePeerSend
#define enet_host_broadcast CaptureBroadcast
#define enet_host_flush NoFlush
#define enet_host_create MakeHost
#define enet_host_destroy DestroyHost
#define enet_host_connect ConnectHost
#define enet_peer_disconnect DisconnectPeer
#define enet_peer_disconnect_now DisconnectPeer
#include "../src/net/LAN.cpp"
#undef enet_host_service
#undef enet_peer_send
#undef enet_host_broadcast
#undef enet_host_flush
#undef enet_host_create
#undef enet_host_destroy
#undef enet_host_connect
#undef enet_peer_disconnect
#undef enet_peer_disconnect_now

namespace melonDS::Platform
{
// Keep packets younger than one frame; waiting for input is deterministic too.
u64 GetMSCount() { return Tick; }
}

namespace melonDS
{
// Friend access is limited to a socket-free session fixture and state snapshots.
// Packets enter through ENet events and leave through the public MPInterface API.
struct LANPacketTest
{
    struct State
    {
        size_t Queued;
        u16 Connected;
        int LastHost;
        ENetPeer* LastPeer;
        bool operator==(const State&) const = default;
    };

    LAN Net;
    ENetHost Host{};
    std::array<ENetPeer, 16> Peers{};

    LANPacketTest()
    {
        Net.Active = true;
        Net.Connection = LAN::ClientState::Connected;
        Net.Host = &Host;
        Net.MyPlayer = {};
        Net.MyPlayer.ID = 0;
        Net.MaxPlayers = 16;
        Net.NumPlayers = 4;
        Net.ConnectedBitmask = 0x0007;
        for (int id : {1, 2, 15})
        {
            Net.Players[id].ID = id;
            Net.Players[id].Status = LAN::Player_Client;
            Net.RemotePeers[id] = &Peers[id];
            Peers[id].data = &Net.Players[id];
            Peers[id].state = ENET_PEER_STATE_CONNECTED;
        }
        Net.LastHostID = 2;
        Net.LastHostPeer = &Peers[2];
        FreedPackets = 0;
        Tick = 1000;
        SentPeer = nullptr;
        SentPacket.clear();
    }

    ~LANPacketTest()
    {
        while (!Net.RXQueue.empty())
        {
            enet_packet_destroy(Net.RXQueue.front());
            Net.RXQueue.pop();
        }
        for (auto& event : Events) enet_packet_destroy(event.packet);
        Events.clear();
        Net.Active = false;
        Net.Host = nullptr;
    }

    State Snapshot() const
    {
        return {Net.RXQueue.size(), Net.ConnectedBitmask,
                Net.LastHostID, Net.LastHostPeer};
    }

    void Unbind(int id) { Net.RemotePeers[id] = nullptr; }
    void MismatchPlayer(int id) { Net.Players[id].ID = 16; }
    void ConnectingPlayer(int id) { Net.Players[id].Status = LAN::Player_Connecting; }
    void ReadyPlayers(u16 mask) { Net.ConnectedBitmask = mask; }
    void SetHost(bool host) { Net.IsHost = host; }
};
}

namespace
{
using namespace melonDS;
constexpr u64 Timestamp = 123456;
constexpr u8 Sentinel = 0xA5;
using Output = std::array<u8, 15 * 1024 + 64>;

void ENET_CALLBACK PacketFreed(ENetPacket*) { ++FreedPackets; }

ENetPacket* Packet(u32 type, u32 sender, size_t size, u32 declared = ~0u)
{
    // MP frame data retains the existing 24-byte native v1 header.
    static_assert(sizeof(MPPacketHeader) == 24);
    MPPacketHeader header{0x4946494E, sender, type,
                          declared == ~0u ? static_cast<u32>(size) : declared,
                          Timestamp};
    auto* packet = enet_packet_create(nullptr, sizeof(header) + size,
                                      ENET_PACKET_FLAG_UNSEQUENCED);
    std::memcpy(packet->data, &header, sizeof(header));
    std::fill_n(packet->data + sizeof(header), size, 0x3C);
    packet->freeCallback = PacketFreed;
    return packet;
}

void Inject(ENetPacket* packet, ENetPeer* peer)
{
    ENetEvent event{};
    event.type = ENET_EVENT_TYPE_RECEIVE;
    event.channelID = 1; // v1 Chan_MP
    event.peer = peer;
    event.packet = packet;
    Events.push_back(event);
}

bool Unchanged(const Output& out)
{
    return std::all_of(out.begin(), out.end(), [](u8 b) { return b == Sentinel; });
}

bool Copied(const Output& out, size_t offset, size_t size)
{
    for (size_t i = 0; i < out.size(); ++i)
    {
        u8 expected = i >= 32 + offset && i < 32 + offset + size ? 0x3C : Sentinel;
        if (out[i] != expected) return false;
    }
    return true;
}

bool Reject(LANPacketTest& f, ENetPacket* packet, ENetPeer* peer)
{
    const auto before = f.Snapshot();
    Inject(packet, peer);
    f.Net.Process();
    // On RED, stop before consuming the wrongly admitted frame: its declared
    // body/AID may cause an invalid copy in the old implementation.
    if (!(f.Snapshot() == before) || FreedPackets != 1) return false;

    Output out;
    out.fill(Sentinel);
    u64 stamp = 0xBADC0FFEE;
    const int count = f.Net.RecvHostPacket(0, out.data() + 32, &stamp);
    const u16 replies = f.Net.RecvReplies(0, out.data() + 32, Timestamp, 0x8002);
    return count == 0 && replies == 0 && stamp == 0xBADC0FFEE && Unchanged(out)
        && f.Snapshot() == before && FreedPackets == 1;
}

bool Frame(u32 type, size_t size)
{
    LANPacketTest f;
    // DS MP host need not be LAN session host (ID 0). Sender and AID differ.
    Inject(Packet(type, 1, size), &f.Peers[1]);
    Output out;
    out.fill(Sentinel);
    u64 stamp = 0;
    const int count = type == 0 ? f.Net.RecvPacket(0, out.data() + 32, &stamp)
                                : f.Net.RecvHostPacket(0, out.data() + 32, &stamp);
    if (count != static_cast<int>(std::min(size, size_t{2048}))
        || stamp != Timestamp || !Copied(out, 0, count) || FreedPackets != 1)
        return false;
    // Routing changes only on CMD, not on a regular packet or ACK.
    std::array<u8, 40> reply{};
    f.Net.SendReply(0, reply.data(), reply.size(), Timestamp, 15);
    return SentPeer == &f.Peers[type == 1 ? 1 : 2]
        && SentPacket.size() == sizeof(MPPacketHeader) + reply.size();
}

bool ReceiveCapacity()
{
    LANPacketTest f;
    Output out;
    u64 stamp = 0;
    for (u32 capacity : {17u, 0x2000u})
    {
        out.fill(Sentinel);
        Inject(Packet(1, 1, 0x2000), &f.Peers[1]);
        const unsigned freed = FreedPackets;
        if (f.Net.RecvPacket(0, nullptr, &stamp) != 0 ||
            f.Net.RecvPacket(0, out.data() + 32, &stamp, 0) != 0 ||
            FreedPackets != freed || !Unchanged(out)) return false;
        if (f.Net.RecvHostPacket(0, out.data() + 32, &stamp, capacity) != int(capacity) ||
            stamp != Timestamp || !Copied(out, 0, capacity) || FreedPackets != freed + 1) return false;
    }
    out.fill(Sentinel);
    Inject(Packet(0, 1, 40), &f.Peers[1]);
    return f.Net.RecvPacket(0, out.data() + 32, &stamp) == 40 && Copied(out, 0, 40);
}

bool Replies()
{
    LANPacketTest f;
    // AID 15 belongs to sender 1 here; a client without data uses AID 0.
    Inject(Packet(2, 2, 0), &f.Peers[2]);
    Inject(Packet(2 | (15u << 16), 1, 0x2000), &f.Peers[1]);
    Output out;
    out.fill(Sentinel);
    const auto ret = f.Net.RecvReplies(0, out.data() + 32, Timestamp, 0x8000);
    return ret == 0x8000 && Copied(out, 14 * 1024, 1024) && FreedPackets == 2;
}

bool BlankReplyCompletes()
{
    LANPacketTest f;
    f.ReadyPlayers(0x0003);
    Inject(Packet(2, 1, 0), &f.Peers[1]);
    Inject(Packet(0, 2, 40), &f.Peers[2]);
    Output out;
    out.fill(Sentinel);
    const auto ret = f.Net.RecvReplies(0, out.data() + 32, Timestamp, 0x0002);
    return ret == 0 && Unchanged(out) && FreedPackets == 1 && Events.size() == 1;
}

bool ReadyNotificationCanLag()
{
    LANPacketTest f;
    // Reliable Chan_Cmd readiness/player-info can lag behind Chan_MP traffic.
    f.ReadyPlayers(1);
    f.ConnectingPlayer(1);
    Inject(Packet(0, 1, 40), &f.Peers[1]);
    Output out;
    out.fill(Sentinel);
    return f.Net.RecvPacket(0, out.data() + 32, nullptr) == 40
        && Copied(out, 0, 40) && FreedPackets == 1;
}

bool EmptyControl(bool host)
{
    LANPacketTest f;
    f.SetHost(host);
    auto* packet = enet_packet_create(nullptr, 0, 0);
    packet->freeCallback = PacketFreed;
    Inject(packet, &f.Peers[1]);
    Events.back().channelID = 0;
    const auto before = f.Snapshot();
    f.Net.Process();
    return FreedPackets == 1 && f.Snapshot() == before;
}

bool ControlFloodYields()
{
    LANPacketTest f;
    for (int i = 0; i < 2000; ++i)
    {
        auto* packet = enet_packet_create(nullptr, 0, 0);
        packet->freeCallback = PacketFreed;
        Inject(packet, &f.Peers[1]);
        Events.back().channelID = 0;
    }
    const auto before = f.Snapshot();
    f.Net.Process();
    return FreedPackets > 0 && !Events.empty() && f.Snapshot() == before;
}

void Control(std::span<const u8> bytes, ENetPeer* peer = &ClientPeer)
{
    auto* packet = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
    packet->freeCallback = PacketFreed;
    Inject(packet, peer);
    Events.back().channelID = 0;
}

std::array<u8, 11> Init(u8 id = 1, u8 max = 2, u8 version = 1)
{
    return {1, 'L', 'A', 'N', 'P', version, 0, 0, 0, id, max};
}

void PlayerList()
{
    std::array<u8, 2 + 16 * sizeof(LAN::Player)> bytes{};
    bytes[0] = 3;
    bytes[1] = 2;
    LAN::Player players[16]{};
    players[0].Status = LAN::Player_Host;
    std::strcpy(players[0].Name, "Host");
    players[1].ID = 1;
    players[1].Status = LAN::Player_Client;
    std::strcpy(players[1].Name, "Client");
    std::memcpy(bytes.data() + 2, players, sizeof(players));
    Control(bytes);
}

bool HandshakeAndRejoin()
{
    LAN net;
    Tick = 1000;
    FreedPackets = 0;
    const auto destroyed = HostsDestroyed;
    if (!net.StartClient("Client", "127.0.0.1") || net.GetClientState() != LAN::ClientState::Connecting)
        return false;
    Control({});
    Control(Init());
    net.Process();
    if (FreedPackets != 2 || net.GetClientState() != LAN::ClientState::Connecting ||
        SentPacket.size() != 9 + sizeof(LAN::Player)) return false;
    PlayerList();
    net.Process();
    if (FreedPackets != 3 || net.GetClientState() != LAN::ClientState::Connected ||
        net.GetPlayerList().size() != 2) return false;
    net.EndSession();
    if (HostsDestroyed != destroyed + 1 || !net.GetPlayerList().empty() ||
        net.GetNumPlayers() || net.GetMaxPlayers()) return false;
    if (!net.StartClient("Again", "127.0.0.1")) return false;
    net.EndSession(); // Cancel before the first packet.
    return net.GetClientState() == LAN::ClientState::Idle && HostsDestroyed == destroyed + 2;
}

bool OptionalPortsAndLegacyRejoin()
{
    LAN net;
    Tick = 1000;
    if (!net.StartClient("Client", "127.0.0.1")) return false;
    Control(Init());
    const std::array<u8, 1> accepted{6};
    Control(accepted);
    PlayerList();
    net.Process();
    // A capable host promised endpoints. A legacy list alone is not completion.
    if (net.GetClientState() != LAN::ClientState::Connecting) return false;
    std::array<u8, 35> ports{7, 3, 0, 0x98, 0x1B, 0x40, 0x9C}; // 7064, 40000
    ENetPeer stranger{};
    Control(ports, &stranger);
    auto invalid = ports;
    invalid[5] = invalid[6] = 0;
    Control(invalid);
    Control(std::span(ports).first(ports.size() - 1));
    net.Process();
    if (net.GetClientState() != LAN::ClientState::Connecting) return false;
    Control(ports);
    net.Process();
    if (net.GetClientState() != LAN::ClientState::Connected) return false;
    net.EndSession();

    if (!net.StartClient("Legacy host", "127.0.0.1")) return false;
    Control(Init());
    PlayerList(); // No extension acknowledgement from a legacy host.
    net.Process();
    return net.GetClientState() == LAN::ClientState::Connected;
}

bool HandshakeFailure(LAN::ClientState expected, bool timeout, u8 id = 1, u8 max = 2, u8 version = 1)
{
    LAN net;
    Tick = 0xFFFFFF00;
    FreedPackets = 0;
    const auto destroyed = HostsDestroyed;
    if (!net.StartClient("Client", "127.0.0.1")) return false;
    if (timeout) Tick += 5000;
    else Control(Init(id, max, version));
    net.Process();
    return net.GetClientState() == expected && HostsDestroyed == destroyed + 1 &&
        net.GetPlayerList().empty() && FreedPackets == (timeout ? 0u : 1u);
}

int Failures = 0;
int Cases = 0;
void Check(std::string_view name, bool pass)
{
    ++Cases;
    if (!pass) ++Failures;
    std::printf("%s %.*s\n", pass ? "PASS" : "FAIL", int(name.size()), name.data());
}
}

int main()
{
    Check("normal-regular", Frame(0, 40));
    Check("normal-command-route", Frame(1, 40));
    Check("normal-ack-keeps-route", Frame(3, 44));
    Check("full-wifi-frame-preserves-v1-crop", Frame(1, 0x2000));
    Check("caller-capacity-preserves-next-datagram", ReceiveCapacity());
    Check("blank-and-aid15-reply-preserve-v1-slot-crop", Replies());
    Check("blank-reply-completes-without-timeout", BlankReplyCompletes());
    Check("ready-notification-may-lag-other-channel", ReadyNotificationCanLag());
    Check("host-empty-control-releases-packet", EmptyControl(true));
    Check("client-empty-control-releases-packet", EmptyControl(false));
    Check("control-flood-yields-to-cancellation", ControlFloodYields());
    Check("handshake-awaits-player-list-and-cancel-rejoin", HandshakeAndRejoin());
    Check("optional-ports-validate-host-and-reset-for-legacy", OptionalPortsAndLegacyRejoin());
    Check("handshake-rejects-unknown-protocol", HandshakeFailure(LAN::ClientState::Incompatible, false, 1, 2, 99));
    Check("handshake-rejects-host-id", HandshakeFailure(LAN::ClientState::Failed, false, 0));
    Check("handshake-rejects-invalid-capacity", HandshakeFailure(LAN::ClientState::Failed, false, 1, 0));
    Check("handshake-timeout-across-clock-wrap", HandshakeFailure(LAN::ClientState::TimedOut, true));

    for (size_t length : {size_t{0}, size_t{23}})
    {
        LANPacketTest f;
        auto* packet = enet_packet_create(nullptr, length, 0);
        packet->freeCallback = PacketFreed;
        Check(length == 0 ? "empty-header" : "truncated-header",
              Reject(f, packet, &f.Peers[1]));
    }

    struct BadFrame { const char* Name; u32 Type; u32 Sender; size_t Size; u32 Length; };
    const BadFrame malformed[] = {
        {"truncated-payload", 1, 1, 39, 40},
        {"undeclared-trailing-payload", 1, 1, 41, 40},
        {"oversize-wifi-payload", 1, 1, 0x2001, 0x2001},
        {"oversize-declared-payload", 1, 1, 40, 0xFFFFFFFE},
        {"unknown-type", 4, 1, 40, 40},
        {"non-reply-high-type-bits", 0x10001, 1, 40, 40},
        {"data-reply-aid0", 2, 1, 40, 40},
        {"data-reply-aid16", 2 | (16u << 16), 1, 40, 40},
        {"blank-reply-invalid-aid", 2 | (0xFFFFu << 16), 1, 0, 0},
        {"sender-out-of-range", 1, 16, 40, 40},
        {"sender-overflow", 1, 0xFFFFFFFF, 40, 40},
        {"sender-is-local", 1, 0, 40, 40},
        {"sender-peer-mismatch", 1, 2, 40, 40},
    };
    for (const auto& bad : malformed)
    {
        LANPacketTest f;
        Check(bad.Name, Reject(f, Packet(bad.Type, bad.Sender, bad.Size, bad.Length), &f.Peers[1]));
    }
    {
        LANPacketTest f;
        auto* packet = Packet(1, 1, 40);
        packet->data[0] = 0;
        Check("wrong-magic", Reject(f, packet, &f.Peers[1]));
    }
    {
        LANPacketTest f;
        Check("null-peer", Reject(f, Packet(1, 1, 40), nullptr));
    }
    {
        LANPacketTest f;
        f.Unbind(1);
        Check("unbound-peer", Reject(f, Packet(1, 1, 40), &f.Peers[1]));
    }
    {
        LANPacketTest f;
        f.Peers[1].data = f.Peers[2].data;
        Check("peer-player-association-mismatch", Reject(f, Packet(1, 1, 40), &f.Peers[1]));
    }
    {
        LANPacketTest f;
        f.MismatchPlayer(1);
        Check("player-slot-id-mismatch", Reject(f, Packet(1, 1, 40), &f.Peers[1]));
    }

    std::printf("LANPacket: %d/%d passed; no sockets opened\n", Cases - Failures, Cases);
    return Failures ? 1 : 0;
}
