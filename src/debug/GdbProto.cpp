
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstring>

#include "../Platform.h"
#include "hexutil.h"
#include "GdbStub.h"

using namespace melonDS;

namespace Gdb
{

ReadResult GdbStub::TryParsePacket(size_t start, size_t& packetStart, size_t& packetSize, size_t& packetContentSize)
{
	for (size_t i = start; i < RecvBufferFilled; )
	{
		const u8 value = RecvBuffer[i++];
		if (value == '+' || value == '-') continue;
		packetStart = i - 1;
		packetSize = packetContentSize = 1;
		if (value == 4) return ReadResult::Eof;
		if (value == 3) return ReadResult::Break;
		if (value != '$') return ReadResult::Wut;

		packetStart = i;
		u8 checksum = 0;
		bool escaped = false;
		for (; i < RecvBufferFilled; ++i)
		{
			const u8 current = RecvBuffer[i];
			if (!escaped && current == '#')
			{
				if (i + 2 >= RecvBufferFilled) return ReadResult::NoPacket;
				packetContentSize = i - packetStart;
				packetSize = packetContentSize + 3;
				if (packetContentSize > GDBPROTO_MAX_PAYLOAD) return ReadResult::Wut;
				unsigned expected;
				const char* digits = reinterpret_cast<const char*>(&RecvBuffer[i + 1]);
				const auto result = std::from_chars(digits, digits + 2, expected, 16);
				return result.ec == std::errc{} && result.ptr == digits + 2 && expected == checksum
					? ReadResult::CmdRecvd : ReadResult::CksumErr;
			}
			if (!escaped && current == '$') return ReadResult::Wut;
			if (i - packetStart >= GDBPROTO_MAX_PAYLOAD) return ReadResult::Wut;
			checksum += current;
			escaped = !escaped && current == '}';
		}
		return ReadResult::NoPacket;
	}
	return ReadResult::NoPacket;
}

void GdbStub::ConsumeReceived(size_t count, size_t offset)
{
	RecvBufferFilled -= u32(count);
	std::memmove(RecvBuffer.data() + offset, RecvBuffer.data() + offset + count, RecvBufferFilled - offset);
}

ReadResult GdbStub::ParseAndSetupPacket()
{
	// Preserve the existing resend rule: consecutive identical commands already
	// in this read get one response; different coalesced commands remain queued.
	size_t consumed = 0, previousStart = 0, previousLength = 0;
	ReadResult previous = ReadResult::NoPacket;
	while (true)
	{
		size_t start = 0, size = 0, length = 0;
		const ReadResult result = TryParsePacket(consumed, start, size, length);
		if (result != ReadResult::CmdRecvd && result != ReadResult::Break)
		{
			if (previous != ReadResult::NoPacket) break;
			if (result == ReadResult::CksumErr) ConsumeReceived(start + size);
			else if (result == ReadResult::NoPacket)
			{
				// ACKs can arrive by themselves or before an incomplete packet.
				while (consumed < RecvBufferFilled &&
					(RecvBuffer[consumed] == '+' || RecvBuffer[consumed] == '-')) ++consumed;
				ConsumeReceived(consumed);
			}
			return result;
		}
		if (previous != ReadResult::NoPacket && (result != previous || length != previousLength ||
			std::memcmp(&RecvBuffer[start], &RecvBuffer[previousStart], length) != 0)) break;
		previous = result;
		previousStart = start;
		previousLength = length;
		consumed = start + size;
	}
	std::memcpy(Cmdbuf.data(), &RecvBuffer[previousStart], previousLength);
	Cmdbuf[previousLength] = 0;
	Cmdlen = ssize_t(previousLength);
	ConsumeReceived(consumed);
	return previous;
}

static bool SocketInterrupted()
{
#ifdef _WIN32
	return WSAGetLastError() == WSAEINTR;
#else
	return errno == EINTR;
#endif
}

static bool SocketWouldBlock()
{
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

int GdbStub::WaitForSocket(SocketHandle socket, bool write, int timeoutMs)
{
	const auto started = std::chrono::steady_clock::now();
	int remaining = timeoutMs;
	while (true)
	{
#ifdef _WIN32
		fd_set ready, errors;
		FD_ZERO(&ready); FD_ZERO(&errors);
		FD_SET(socket, &ready); FD_SET(socket, &errors);
		timeval timeout{remaining / 1000, (remaining % 1000) * 1000};
		const int result = select(0, write ? nullptr : &ready, write ? &ready : nullptr,
			&errors, timeoutMs < 0 ? nullptr : &timeout);
		if (result > 0) return FD_ISSET(socket, &errors) ? -1 : 1;
#else
		pollfd descriptor{socket, short(write ? POLLOUT : POLLIN), 0};
		const int result = poll(&descriptor, 1, remaining);
		if (result > 0)
		{
			if (descriptor.revents & (POLLERR | POLLNVAL)) return -1;
			if (write && (descriptor.revents & POLLHUP)) return -1;
			return 1; // A readable HUP is consumed as EOF by recv().
		}
#endif
		if (result >= 0 || !SocketInterrupted()) return result;
		if (timeoutMs >= 0)
		{
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started).count();
			if (elapsed >= timeoutMs) return 0;
			remaining = timeoutMs - int(elapsed);
		}
	}
}

ReadResult GdbStub::MsgRecv()
{
	const ReadResult buffered = ParseAndSetupPacket();
	if (buffered != ReadResult::NoPacket) return buffered;
	if (RecvBufferFilled == RecvBuffer.size()) return ReadResult::Wut;
	// Accepted sockets are nonblocking: a split packet must not stop emulation.
	const auto received = recv(ConnFd, reinterpret_cast<char*>(&RecvBuffer[RecvBufferFilled]),
		int(RecvBuffer.size() - RecvBufferFilled), 0);
	if (received == 0) return ReadResult::Eof;
	if (received < 0)
		return SocketWouldBlock() || SocketInterrupted() ? ReadResult::NoPacket : ReadResult::Eof;
	RecvBufferFilled += u32(received);
	return ParseAndSetupPacket();
}

int GdbStub::SendAll(const u8* data, size_t length)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (length)
	{
		if (!IsConnected() || std::chrono::steady_clock::now() >= deadline) return -1;
		const auto written = send(ConnFd, reinterpret_cast<const char*>(data), int(length), 0);
		if (written > 0) { data += written; length -= written; continue; }
		if (written == 0) return -1;
		if (SocketInterrupted()) continue;
		if (!SocketWouldBlock()) return -1;
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		if (remaining <= 0 || WaitForSocket(ConnFd, true, int(remaining)) != 1) return -1;
	}
	return 0;
}

int GdbStub::SendAck()
{
	const u8 ack = '+';
	return NoAck ? 0 : SendAll(&ack, 1);
}

int GdbStub::SendNak()
{
	const u8 nak = '-';
	return NoAck ? 0 : SendAll(&nak, 1);
}

int GdbStub::WaitAckBlocking(u8* ackp, int to_ms)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(to_ms);
	size_t cursor = 0;
	bool attemptedRead = false;
	while (true)
	{
		while (cursor < RecvBufferFilled)
		{
			if (RecvBuffer[cursor] == '+' || RecvBuffer[cursor] == '-')
			{
				*ackp = RecvBuffer[cursor];
				ConsumeReceived(1, cursor);
				return 0;
			}
			if (attemptedRead && std::chrono::steady_clock::now() >= deadline) return -1;
			size_t start = 0, size = 0, length = 0;
			const ReadResult result = TryParsePacket(cursor, start, size, length);
			if (result == ReadResult::NoPacket) break;
			if (result == ReadResult::CksumErr)
			{
				ConsumeReceived(start + size - cursor, cursor);
				if (SendNak() < 0) return -1;
			}
			else if (result == ReadResult::CmdRecvd && length == size_t(Cmdlen) &&
				std::memcmp(&RecvBuffer[start], Cmdbuf.data(), length) == 0)
			{
				// A retransmission can be split across reads. Acknowledge it,
				// retain the response already sent, and keep looking for its ACK.
				ConsumeReceived(start + size - cursor, cursor);
				if (SendAck() < 0) return -1;
			}
			else if (result == ReadResult::CmdRecvd || result == ReadResult::Break)
				cursor = start + size; // Leave a different command queued for Poll.
			else return -1;
		}
		if (RecvBufferFilled == RecvBuffer.size()) return -1;
		if (attemptedRead && std::chrono::steady_clock::now() >= deadline) return -1;
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		if (WaitForSocket(ConnFd, false, int(std::max<int64_t>(0, remaining))) != 1) return -1;
		const auto received = recv(ConnFd, reinterpret_cast<char*>(&RecvBuffer[RecvBufferFilled]),
			int(RecvBuffer.size() - RecvBufferFilled), 0);
		attemptedRead = true;
		if (received > 0) RecvBufferFilled += u32(received);
		else if (received == 0 || (!SocketWouldBlock() && !SocketInterrupted())) return -1;
		if (std::chrono::steady_clock::now() >= deadline && received <= 0) return -1;
	}
}

int GdbStub::Resp(const u8* data1, size_t len1, const u8* data2, size_t len2, bool noack)
{
	if (len1 > GDBPROTO_MAX_PAYLOAD || len2 > GDBPROTO_MAX_PAYLOAD - len1) return -1;
	// Build separately so RespFmt's input may safely alias RespBuf.
	std::array<u8, GDBPROTO_BUFFER_CAPACITY> packet;
	size_t position = 1;
	u8 checksum = 0;
	packet[0] = '$';
	const auto append = [&](const u8* data, size_t length) {
		for (size_t i = 0; i < length; ++i)
		{
			u8 value = data[i];
			const bool escape = value == '$' || value == '#' || value == '}' || value == '*';
			if (position - 1 + (escape ? 2 : 1) > GDBPROTO_MAX_PAYLOAD) return false;
			if (escape) { packet[position++] = '}'; checksum += '}'; value ^= 0x20; }
			packet[position++] = value;
			checksum += value;
		}
		return true;
	};
	if (!append(data1, len1) || !append(data2, len2)) return -1;
	packet[position++] = '#';
	hexfmt8(&packet[position], checksum);
	position += 2;
	packet[position] = 0;
	std::copy_n(packet.begin(), position + 1, RespBuf.begin());
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		if (SendAll(packet.data(), position) < 0) { Disconnect(); return -1; }
		if (noack) return 0;
		u8 ack = 0;
		if (WaitAckBlocking(&ack, 2000) == 0 && ack == '+') return 0;
	}
	Disconnect();
	return -1;
}

}
