// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using TestSocket = SOCKET;
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
using TestSocket = int;
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "Platform.h"

// Only access control changes in this test's single translation unit. The
// actual stub, framing and command implementations are compiled below.
#define private public
#include "debug/GdbStub.h"
#undef private
using namespace melonDS;
namespace melonDS::Platform { void Log(LogLevel, const char*, ...) {} }

#ifdef _WIN32
static constexpr TestSocket fakeSocket = 0x70000000;
#else
static constexpr TestSocket fakeSocket = 42;
#endif
static std::vector<u8> incoming, sent;
static size_t readOffset = 0;
static int sendChunk = 100000, receiveChunk = 100000;
static bool atEOF = false, zeroSend = false, selectError = false;
static int invalidTimeout = 0;
static int blockedSends = 0;

static void WouldBlock()
{
#ifdef _WIN32
    WSASetLastError(WSAEWOULDBLOCK);
#else
    errno = EAGAIN;
#endif
}

static int Receive(TestSocket socket, char* data, int length, int flags)
{
    if (socket != fakeSocket) return ::recv(socket, data, length, flags);
    if (readOffset == incoming.size())
    {
        if (atEOF) return 0;
        WouldBlock();
        return -1;
    }
    const size_t count = std::min({size_t(length), incoming.size() - readOffset, size_t(receiveChunk)});
    std::memcpy(data, incoming.data() + readOffset, count);
    readOffset += count;
    return int(count);
}
static int Send(TestSocket socket, const char* data, int length, int flags)
{
    if (socket != fakeSocket) return ::send(socket, data, length, flags);
    if (blockedSends > 0) { --blockedSends; WouldBlock(); return -1; }
    if (zeroSend) return 0;
    const int count = std::min(length, sendChunk);
    sent.insert(sent.end(), data, data + count);
    return count;
}
static int Select(int count, fd_set* reads, fd_set* writes, fd_set* errors, const timeval* timeout)
{
    if ((!reads || !FD_ISSET(fakeSocket, reads)) && (!writes || !FD_ISSET(fakeSocket, writes)))
        return ::select(count, reads, writes, errors, const_cast<timeval*>(timeout));
    if (timeout && (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000))
        ++invalidTimeout;
    if (selectError) return -1;
    if (errors) FD_ZERO(errors);
    if (writes && FD_ISSET(fakeSocket, writes)) return 1;
    if (reads && (readOffset < incoming.size() || atEOF)) return 1;
    if (reads) FD_ZERO(reads);
    return 0;
}
static int CloseSocket(TestSocket socket)
{
    if (socket == fakeSocket) return 0;
#ifdef _WIN32
    return ::closesocket(socket);
#else
    return ::close(socket);
#endif
}

#ifndef _WIN32
static int Poll(pollfd* descriptors, nfds_t count, int timeout)
{
    if (count != 1 || descriptors[0].fd != fakeSocket) return ::poll(descriptors, count, timeout);
    if (selectError) return -1;
    descriptors[0].revents = (descriptors[0].events & POLLOUT) ? POLLOUT :
        (readOffset < incoming.size() || atEOF) ? POLLIN : 0;
    return descriptors[0].revents ? 1 : 0;
}
#endif

// Exercise real TCP while restricting the production listener to loopback in
// this test. No external interface is bound and no test changes the default UI.
static int BindLoopback(TestSocket socket, const sockaddr* address, socklen_t length)
{
    auto local = *reinterpret_cast<const sockaddr_in*>(address);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return ::bind(socket, reinterpret_cast<const sockaddr*>(&local), length);
}

#define recv Receive
#define send Send
#define select Select
#define bind BindLoopback
#ifndef _WIN32
#define poll Poll
#endif
#ifdef _WIN32
#define closesocket CloseSocket
#else
#define close CloseSocket
#endif
#include "debug/GdbProto.cpp"
#include "debug/GdbStub.cpp"
#undef recv
#undef send
#undef select
#undef bind
#undef poll
#undef closesocket
#undef close
#include "debug/GdbCmds.cpp"

struct Memory : Gdb::StubCallbacks
{
    struct Write { u32 address; int width; u32 value; };
    std::map<u32, u8> bytes;
    std::vector<Write> writes;
    unsigned reads = 0;
    int GetCPU() const override { return 9; }
    u32 ReadReg(Gdb::Register) override { return 0; }
    void WriteReg(Gdb::Register, u32) override {}
    u32 ReadMem(u32 addr, int width) override
    {
        ++reads;
        u32 value = 0;
        for (int i = 0; i < width / 8; ++i) value |= u32(bytes[addr + i]) << (8 * i);
        return value;
    }
    void WriteMem(u32 addr, int width, u32 value) override
    {
        writes.push_back({addr, width, value});
        for (int i = 0; i < width / 8; ++i) bytes[addr + i] = u8(value >> (8 * i));
    }
    void ResetGdb() override {}
    int RemoteCmd(const u8*, size_t) override { return 0; }
};

static std::vector<u8> Frame(std::string_view payload)
{
    std::vector<u8> frame{'$'};
    u8 sum = 0;
    for (const unsigned char value : payload) { frame.push_back(value); sum += value; }
    const char* hex = "0123456789abcdef";
    frame.insert(frame.end(), {'#', u8(hex[sum >> 4]), u8(hex[sum & 15])});
    return frame;
}
static bool Response(std::string_view payload) { return sent == Frame(payload); }
static void Fill(Gdb::GdbStub& stub, const std::vector<u8>& data)
{
    std::copy(data.begin(), data.end(), stub.RecvBuffer.begin());
    stub.RecvBufferFilled = data.size();
}

static bool Loopback(Memory& memory, Gdb::GdbStub& stub)
{
    stub.Disconnect();
    if (!stub.Init(0)) return false;
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (getsockname(stub.SockFd, reinterpret_cast<sockaddr*>(&address), &length) != 0) return false;
    std::atomic<bool> done = false;
    bool clientOK = true;
    unsigned disconnected = 0;
    std::jthread client([&] {
        const auto write = [](TestSocket socket, const std::vector<u8>& bytes) {
            size_t offset = 0;
            while (offset < bytes.size())
            {
                const auto count = ::send(socket, reinterpret_cast<const char*>(bytes.data() + offset),
                    int(bytes.size() - offset), 0);
                if (count <= 0) return false;
                offset += count;
            }
            return true;
        };
        const auto expect = [](TestSocket socket, const std::vector<u8>& bytes) {
            std::vector<u8> actual(bytes.size());
            size_t offset = 0;
            while (offset < bytes.size())
            {
                fd_set reads;
                FD_ZERO(&reads); FD_SET(socket, &reads);
                timeval timeout{2, 0};
#ifdef _WIN32
                const int count = 0;
#else
                const int count = socket + 1;
#endif
                if (::select(count, &reads, nullptr, nullptr, &timeout) != 1) return false;
                const auto received = ::recv(socket, reinterpret_cast<char*>(actual.data() + offset),
                    int(actual.size() - offset), 0);
                if (received <= 0) return false;
                offset += received;
            }
            if (actual != bytes) std::fprintf(stderr, "Loopback reply mismatch\n");
            return actual == bytes;
        };
        for (int session = 0; session < 2 && clientOK; ++session)
        {
            const TestSocket socket = ::socket(AF_INET, SOCK_STREAM, 0);
            if (socket == TestSocket(-1)) { clientOK = false; break; }
            clientOK = connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
            const auto exchange = [&](std::string_view request, std::string_view reply, bool ack) {
                if (!clientOK) return;
                clientOK = write(socket, Frame(request)) && (!ack || expect(socket, {'+'})) &&
                    expect(socket, Frame(reply)) && (!ack || write(socket, {'+'}));
            };
            if (clientOK) clientOK = write(socket, {'+'}) && expect(socket, {'+'});
            if (session == 0)
            {
                exchange("qSupported:swbreak+", "PacketSize=47B;qXfer:features:read+;swbreak-;hwbreak+;QStartNoAckMode+", true);
                exchange("QStartNoAckMode", "OK", true);
                std::string binary = "X100,5:";
                for (u8 value : std::array<u8, 5>{'$', '#', '}', 0, 0x7f})
                {
                    if (value == '$' || value == '#' || value == '}')
                    { binary += '}'; binary += char(value ^ 0x20); }
                    else binary += char(value);
                }
                const auto packet = Frame(binary);
                if (clientOK)
                {
                    clientOK = write(socket, std::vector<u8>(packet.begin(), packet.begin() + 4));
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    clientOK = clientOK && write(socket, std::vector<u8>(packet.begin() + 4, packet.end())) &&
                        expect(socket, Frame("OK"));
                }
                exchange("m100,5", "24237d007f", false);
                auto invalid = Frame("M100,1:AA");
                invalid.back() = invalid.back() == '0' ? '1' : '0';
                const auto valid = Frame("m100,1");
                invalid.insert(invalid.end(), valid.begin(), valid.end());
                if (clientOK) clientOK = write(socket, invalid) && expect(socket, Frame("24"));
                exchange("vMustReplyEmpty", "", false);
                if (clientOK) clientOK = write(socket, {'$', 'M'}); // Partial command must die with this session.
            }
            else
            {
                // The same listener accepts a new default ACK session after no-ACK mode.
                exchange("m100,5", "24237d007f", true);
                exchange("D", "OK", true);
            }
            CloseSocket(socket);
        }
        done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while ((!done || stub.IsConnected()) && std::chrono::steady_clock::now() < deadline)
    {
        if (stub.Poll(false) == Gdb::StubState::Disconnect) ++disconnected;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.join();
    return clientOK && disconnected == 2 && !stub.IsConnected() && !stub.NoAck &&
        stub.RecvBufferFilled == 0 && memory.bytes[0x100] == '$';
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (argc != 2) return 2;
    const std::string_view mode = argv[1];
    Memory memory;
    Gdb::GdbStub stub(&memory);
    stub.ConnFd = fakeSocket;
    stub.NoAck = true;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };
    const auto command = [&](char kind, std::string body, size_t visible = SIZE_MAX) {
        if (visible == SIZE_MAX) visible = body.size();
        if (kind == 'm' || body.find(':') == std::string::npos) body += '\0';
        body.append(64, 'B'); // Extra allocated bytes do not belong to the request.
        if (kind == 'M') Gdb::GdbStub::Handle_M(&stub, reinterpret_cast<const u8*>(body.data()), visible);
        else if (kind == 'X') Gdb::GdbStub::Handle_X(&stub, reinterpret_cast<const u8*>(body.data()), visible);
        else Gdb::GdbStub::Handle_m(&stub, reinterpret_cast<const u8*>(body.data()), visible);
    };
    const auto qcrc = [&](const std::string& body, size_t visible = SIZE_MAX) {
        if (visible == SIZE_MAX) visible = body.size();
        sent.clear();
        memory.reads = 0;
        std::string request = body;
        request += '\0';
        request.append(64, 'B'); // Extra allocated bytes do not belong to the request.
        Gdb::GdbStub::Handle_q_CRC(&stub, reinterpret_cast<const u8*>(request.data()), visible);
    };

    if (mode == "memory-control")
    {
        command('M', "101,7:01020304050607");
        check(Response("OK") && memory.writes.size() == 3, "Normal hex write did not finish");
        check(memory.writes.size() == 3 && memory.writes[0].width == 8 &&
              memory.writes[1].width == 16 && memory.writes[2].width == 32,
              "Aligned MMIO access widths changed");
        for (u32 i = 0; i < 7; ++i) check(memory.bytes[0x101 + i] == i + 1, "Normal hex write changed bytes");
        sent.clear();
        command('m', "101,7");
        check(Response("01020304050607"), "Memory read response changed");
    }
    else if (mode == "binary-tail")
    {
        command('X', std::string("100,5:\x01\x02\x03\x04" "A", 11));
        check(Response("OK") && memory.bytes[0x104] == 'A', "Binary final byte was interpreted as hex");
        check(memory.writes.size() == 2 && memory.writes[0].width == 32 && memory.writes[1].width == 8,
              "Binary access widths changed");
    }
    else if (mode == "binary-escape")
    {
        const std::array<u8, 5> values{'$', '#', '}', 0, 0x7f};
        std::string body = "100,5:";
        for (u8 value : values)
        {
            if (value == '$' || value == '#' || value == '}') { body += '}'; body += char(value ^ 0x20); }
            else body += char(value);
        }
        command('X', body);
        check(Response("OK"), "Escaped binary write did not finish");
        for (u32 i = 0; i < values.size(); ++i) check(memory.bytes[0x100 + i] == values[i], "Binary escape/NUL changed data");
    }
    else if (mode == "last-address")
    {
        command('M', "fffffffc,4:01020304");
        check(Response("OK") && memory.writes.size() == 1 && memory.bytes[0xffffffff] == 4,
              "Valid final four bytes of the address space were rejected");
    }
    else if (mode == "short-hex" || mode == "missing-colon" || mode == "invalid-hex" ||
             mode == "extra-hex" || mode == "wrap" || mode == "short-binary" || mode == "dangling-escape")
    {
        if (mode == "short-hex") command('M', "100,5:01020304");
        else if (mode == "missing-colon") command('M', "100,4");
        else if (mode == "invalid-hex") command('M', "100,5:01020304ZZ");
        else if (mode == "extra-hex") command('M', "100,1:0102");
        else if (mode == "wrap") command('M', "fffffffe,4:01020304");
        else if (mode == "short-binary") command('X', "100,5:ABCD");
        else command('X', "100,1:}");
        check(memory.writes.empty() && memory.bytes.empty(), "Rejected command partially changed guest memory");
        check(sent.size() > 2 && sent[1] == 'E', "Malformed command did not return an error");
    }
    else if (mode == "extra-binary")
    {
        for (const char* body : {"100,1:AB", "100,1:A}", "100,1:#"})
        {
            sent.clear();
            command('X', body);
            check(memory.writes.empty() && Response("E01"), "Invalid binary tail changed guest memory");
        }
    }
    else if (mode == "receive-limit")
    {
        const std::string body(Gdb::GDBPROTO_BUFFER_CAPACITY - 5, 'q');
        auto frame = Frame(body);
        frame.insert(frame.begin(), '+'); // Fill the physical receive buffer exactly.
        Fill(stub, frame);
        size_t start = 0, size = 0, content = 0;
        check(stub.TryParsePacket(0, start, size, content) == Gdb::ReadResult::CmdRecvd,
              "Full-buffer scanner lost a valid packet");
        const volatile u32& storedLength = stub.RecvBufferFilled;
        check(storedLength == frame.size(), "Scanner terminator overwrote the stored receive length");
        check(stub.ParseAndSetupPacket() == Gdb::ReadResult::CmdRecvd &&
              stub.Cmdlen == ssize_t(body.size()) && stub.RecvBufferFilled == 0,
              "Full receive buffer damaged length or lost its complete packet");
    }
    else if (mode == "checksum" || mode == "checksum-recovery")
    {
        auto frame = Frame("m100,1");
        if (mode == "checksum") frame.back() = 'z';
        else frame.back() = frame.back() == '0' ? '1' : '0';
        Fill(stub, frame);
        check(stub.ParseAndSetupPacket() == Gdb::ReadResult::CksumErr,
              "Invalid checksum was not reported");
        check(stub.RecvBufferFilled == 0, "Bad checksum packet blocks a later retransmission");
    }
    else if (mode == "coalesced")
    {
        auto first = Frame("m100,1"), second = Frame("m200,2");
        first.insert(first.end(), second.begin(), second.end());
        Fill(stub, first);
        check(stub.ParseAndSetupPacket() == Gdb::ReadResult::CmdRecvd && stub.Cmdlen == 6 &&
              std::memcmp(stub.Cmdbuf.data(), "m100,1", 6) == 0, "First coalesced command was lost");
        check(stub.ParseAndSetupPacket() == Gdb::ReadResult::CmdRecvd && stub.Cmdlen == 6 &&
              std::memcmp(stub.Cmdbuf.data(), "m200,2", 6) == 0, "Second coalesced command was lost");
    }
    else if (mode == "stream-boundaries")
    {
        const auto frame = Frame("m100,1");
        incoming.assign(frame.begin(), frame.begin() + 3);
        check(stub.MsgRecv() == Gdb::ReadResult::NoPacket && stub.RecvBufferFilled == 3,
              "Partial receive did not retain its prefix");
        check(stub.MsgRecv() == Gdb::ReadResult::NoPacket, "Would-block was treated as disconnection");
        incoming.insert(incoming.end(), frame.begin() + 3, frame.end());
        check(stub.MsgRecv() == Gdb::ReadResult::CmdRecvd && stub.Cmdlen == 6,
              "Split receive did not complete the command");
        auto duplicates = frame;
        duplicates.insert(duplicates.end(), frame.begin(), frame.end());
        Fill(stub, duplicates);
        check(stub.ParseAndSetupPacket() == Gdb::ReadResult::CmdRecvd && stub.RecvBufferFilled == 0,
              "Consecutive resends produced extra responses");
        auto corrupt = frame;
        corrupt.back() = 'z';
        auto goodBadGood = frame;
        goodBadGood.insert(goodBadGood.end(), corrupt.begin(), corrupt.end());
        goodBadGood.insert(goodBadGood.end(), frame.begin(), frame.end());
        Fill(stub, goodBadGood);
        check(stub.ParseAndSetupPacket() == Gdb::ReadResult::CmdRecvd &&
              stub.ParseAndSetupPacket() == Gdb::ReadResult::CksumErr &&
              stub.ParseAndSetupPacket() == Gdb::ReadResult::CmdRecvd,
              "A corrupt coalesced packet hid a valid command or retransmission");
        Fill(stub, Frame(std::string(Gdb::GDBPROTO_MAX_PAYLOAD + 1, 'q')));
        check(stub.ParseAndSetupPacket() == Gdb::ReadResult::Wut, "Oversize input was accepted");
        stub.RecvBufferFilled = 0;
        Fill(stub, Frame("c"));
        check(stub.Poll(false) == Gdb::StubState::Continue, "Buffered command required fresh socket data");
    }
    else if (mode == "response-extra")
    {
        const std::string maximum(Gdb::GDBPROTO_MAX_PAYLOAD, 'q');
        check(stub.RespFmt("%sX", maximum.c_str()) < 0 && sent.empty(), "Formatted overflow was sent");
        const std::string escaping(Gdb::GDBPROTO_MAX_PAYLOAD / 2 + 1, '#');
        check(stub.RespStr(escaping.c_str()) < 0 && sent.empty(), "Escaped overflow was sent");
        check(stub.Resp(nullptr, SIZE_MAX, nullptr, 1) < 0 && sent.empty(), "Response length wrapped");
        blockedSends = 1;
        check(stub.RespStr("OK") == 0 && Response("OK"), "Would-block send lost response");
        sent.clear();
        incoming = {'-','-','-'};
        stub.NoAck = false;
        check(stub.RespStr("OK") < 0 && !stub.IsConnected(), "ACK retry exhaustion was accepted");
    }
    else if (mode == "loopback")
    {
        check(Loopback(memory, stub), "Actual TCP session/escape/split/reconnect check failed");
    }
    else if (mode == "q-xfer")
    {
        const std::string xml = Gdb::TARGET_XML_ARM9;
        const std::string request = "0,47b";
        check(Gdb::DoQResponse(&stub, reinterpret_cast<const u8*>(request.c_str()), xml.data(), xml.size()) == 0 &&
              Response("m" + xml.substr(0, Gdb::GDBPROTO_MAX_PAYLOAD - 1)),
              "Target XML chunk could not reserve its response prefix");
    }
    else if (mode == "split-resend")
    {
        const auto original = Frame("m0,1");
        auto buffered = original;
        buffered.insert(buffered.end(), original.begin(), original.begin() + 4);
        Fill(stub, buffered);
        check(stub.ParseAndSetupPacket() == Gdb::ReadResult::CmdRecvd,
              "Original command before partial resend was lost");
        incoming.assign(original.begin() + 4, original.end());
        incoming.push_back('+');
        receiveChunk = 2;
        stub.NoAck = false;
        check(stub.RespStr("00") == 0 && stub.IsConnected() && stub.RecvBufferFilled == 0,
              "Partial resend prevented reading the real ACK");
        auto expected = Frame("00");
        expected.push_back('+');
        check(sent == expected, "Duplicate command was re-executed or response was resent unnecessarily");
        sent.clear();
        Fill(stub, Frame("c"));
        incoming = {'+'}; readOffset = 0;
        u8 ack = 0;
        check(stub.WaitAckBlocking(&ack, 1) == 0 && ack == '+' &&
              stub.Poll(false) == Gdb::StubState::Continue,
              "A different queued command was lost while waiting for ACK");
    }
    else if (mode == "listener-error")
    {
        stub.Disconnect();
        stub.SockFd = fakeSocket;
        selectError = true;
        check(stub.Enter(true) == Gdb::StubState::Disconnect && stub.SockFd == Gdb::InvalidSocket,
              "Listener error did not release the listener and leave the paused loop");
    }
    else if (mode == "response-limit")
    {
        stub.Cmdbuf.fill(0xa5);
        const std::string body(Gdb::GDBPROTO_BUFFER_CAPACITY - 4, 'q');
        check(stub.RespStr(body.c_str()) < 0 && sent.empty(), "Oversize response was sent");
        check(stub.Cmdbuf[0] == 0xa5, "Response terminator damaged command storage");
    }
    else if (mode == "response-chunks" || mode == "response-zero")
    {
        sendChunk = 3;
        zeroSend = mode == "response-zero";
        const int result = stub.RespStr("0123456789");
        if (zeroSend) check(result < 0, "Zero-byte send was reported as complete");
        else check(result == 0 && Response("0123456789"), "Partial socket sends lost response bytes");
    }
    else if (mode == "response-escape")
    {
        const std::array<u8, 4> body{'$', '#', '}', '*'};
        check(stub.Resp(body.data(), body.size()) == 0 && Response("}\x04}\x03}]}\x0a"),
              "Response did not escape reserved binary/RLE bytes");
    }
    else if (mode == "response-format")
    {
        const std::string body(Gdb::GDBPROTO_BUFFER_CAPACITY - 5, 'q');
        check(stub.RespFmt("%s", body.c_str()) == 0 && Response(body),
              "Formatting a maximum response truncated data or transmitted a NUL");
    }
    else if (mode == "memory-limit")
    {
        const unsigned length = (Gdb::GDBPROTO_BUFFER_CAPACITY - 5) / 2;
        char body[32]; std::snprintf(body, sizeof(body), "100,%x", length);
        command('m', body);
        check(Response(std::string(length * 2, '0')), "Largest advertised memory response failed");
    }
    else if (mode == "disconnect")
    {
        stub.RecvBufferFilled = 3; stub.Cmdlen = 2;
        stub.Disconnect();
        check(!stub.IsConnected() && !stub.NoAck && stub.RecvBufferFilled == 0 && stub.Cmdlen == 0,
              "Disconnected session retained protocol state");
    }
    else if (mode == "eof" || mode == "ack-eof")
    {
        atEOF = true;
        if (mode == "eof") check(stub.MsgRecv() == Gdb::ReadResult::Eof, "Socket EOF was treated as no packet");
        else { u8 ack = 0; check(stub.WaitAckBlocking(&ack, 0) < 0, "EOF was accepted as an acknowledgment"); }
    }
    else if (mode == "poll-wait")
    {
        incoming = Frame("c");
        check(stub.Poll(true) == Gdb::StubState::Continue && invalidTimeout == 0,
              "Blocking poll supplied an invalid timeout or lost the command");
    }
    else if (mode == "poll-error")
    {
        selectError = true;
        check(stub.Poll(false) == Gdb::StubState::Disconnect && !stub.IsConnected(),
              "Socket readiness error did not disconnect");
    }
    else if (mode == "crc-control")
    {
        // Official libiberty xcrc32 vectors verified by the parent: MSB-first
        // CRC (poly 0x04C11DB7, seed 0xFFFFFFFF, no reflect, no final XOR).
        // Untouched stub memory reads as zero bytes.
        const struct { const char* body; unsigned reads; const char* value; } zeros[] = {
            {"1000,0", 0, "Cffffffff"}, {"1000,1", 1, "C4e08bfb4"},
            {"1000,7f", 127, "Ca6ad09f1"}, {"1000,80", 128, "C46a0eabc"},
            {"1000,81", 129, "C8eea81c5"},
        };
        for (const auto& vector : zeros)
        {
            qcrc(vector.body);
            check(Response(vector.value) && memory.reads == vector.reads,
                  "qCRC zero-data vector or byte read count differs");
        }
        for (u32 i = 0; i < 129; ++i) memory.bytes[0x2000 + i] = u8(i * 37 + 11);
        const struct { const char* body; unsigned reads; const char* value; } mixed[] = {
            {"2000,1", 1, "C654374d5"}, {"2000,7f", 127, "Ce86523b0"},
            {"2000,80", 128, "C30a1f0e4"}, {"2000,81", 129, "C3785a21f"},
        };
        for (const auto& vector : mixed)
        {
            qcrc(vector.body);
            check(Response(vector.value) && memory.reads == vector.reads,
                  "qCRC known-byte vector or byte read count differs");
        }
        for (u32 i = 0; i < 9; ++i) memory.bytes[0x3000 + i] = "123456789"[i];
        qcrc("3000,9");
        check(Response("C376e6e7") && memory.reads == 9,
              "qCRC published check string '123456789' differs");
    }
    else if (mode == "crc-chunks")
    {
        // 128-byte chunk boundaries must not disturb the running checksum and
        // long requests must not gain a silent cap.
        const struct { const char* body; unsigned reads; const char* value; } zeros[] = {
            {"1000,1000", 4096, "C77ffc71c"},
        };
        for (const auto& vector : zeros)
        {
            qcrc(vector.body);
            check(Response(vector.value) && memory.reads == vector.reads,
                  "qCRC chunk-boundary vector or byte read count differs");
        }
        for (u32 i = 0; i < 4096; ++i) memory.bytes[0x2000 + i] = u8(i * 37 + 11);
        qcrc("2000,1000");
        check(Response("C692a24da") && memory.reads == 4096,
              "qCRC 32-chunk request lost data or gained a silent cap");
    }
    else if (mode == "crc-wrap")
    {
        qcrc("ffffff00,100");
        check(Response("Ce55e964f") && memory.reads == 256,
              "Range ending exactly at 2^32 was rejected or miscounted");
        qcrc("ffffff00,101");
        check(Response("E01") && memory.reads == 0,
              "Range crossing the 2^32 boundary was accepted");
        qcrc("ffffffff,1");
        check(Response("C4e08bfb4") && memory.reads == 1,
              "Final byte of the address space was mishandled");
    }
    else if (mode == "crc-malformed")
    {
        for (const char* body : {"100", "100,", ",4", "100,10ZZ", "100, 4",
                                 "100000000,4", "100,100000000", "0x100,4"})
        {
            qcrc(body);
            check(Response("E01") && memory.reads == 0,
                  "Malformed qCRC range was accepted or read guest bytes");
        }
        qcrc("100,10", 3);
        check(Response("E01") && memory.reads == 0,
              "Bytes beyond the declared request length were parsed");
        qcrc("100,000010");
        check(Response("C552d22c8") && memory.reads == 16,
              "Valid zero-padded hex length was mishandled");
    }
    else return 2;
    std::printf("GDB %s: %d failures\n", argv[1], failures);
    return failures ? 1 : 0;
}
