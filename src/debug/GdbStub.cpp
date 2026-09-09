
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
// actually do the define for unix platforms rather than windows so that
// it's clearer what the code does
#define closesocket(x) close(x)
#endif


#include "../Platform.h"
#include "GdbStub.h"

using namespace melonDS;
using Platform::Log;
using Platform::LogLevel;

static int SocketSetBlocking(Gdb::SocketHandle fd, bool block)
{
#if MOCKTEST
	return 0;
#endif

	if (fd == Gdb::InvalidSocket) return -1;

#ifdef _WIN32
	unsigned long mode = block ? 0 : 1;
	return ioctlsocket(fd, FIONBIO, &mode);
#else
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) return -1;
	flags = block ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
	return fcntl(fd, F_SETFL, flags);
#endif
}

namespace Gdb
{

GdbStub::GdbStub(StubCallbacks* cb)
	: Cb(cb), Port(0)
	, SockFd(InvalidSocket), ConnFd(InvalidSocket)
	, Stat(TgtStatus::None), CurBkpt(0), CurWatchpt(0), StatFlag(false), NoAck(false)
	, ServerSA((void*)new struct sockaddr_in())
	, ClientSA((void*)new struct sockaddr_in())
{ }

bool GdbStub::Init(int port)
{
	Close();
	Port = port;
	Log(LogLevel::Info, "[GDB] initializing GDB stub for core %d on port %d\n",
		Cb->GetCPU(), Port);

#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
#else
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		Log(LogLevel::Error, "[GDB] winsock could not be initialized (%d).\n", WSAGetLastError());
		return false;
	}
	WinsockInitialized = true;
#endif

	int r;
	struct sockaddr_in* server = (struct sockaddr_in*)ServerSA;
	SockFd = socket(AF_INET, SOCK_STREAM, 0);
	if (SockFd == InvalidSocket)
	{
		Log(LogLevel::Error, "[GDB] err: can't create a socket fd\n");
		goto err;
	}
	{
		// Make sure the port can be reused immediately after melonDS stops and/or restarts
		int enable = 1;
#ifdef _WIN32
		setsockopt(SockFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&enable, sizeof(enable));
#else
		setsockopt(SockFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#endif
	}
	if (SocketSetBlocking(SockFd, false) < 0) goto err;

	server->sin_family = AF_INET;
	server->sin_addr.s_addr = htonl(INADDR_ANY);
	server->sin_port = htons(Port);

	r = bind(SockFd, (const sockaddr*)server, sizeof(*server));
	if (r < 0)
	{
		Log(LogLevel::Error, "[GDB] err: can't bind to address <any> and port %d\n", Port);
		goto err;
	}

	r = listen(SockFd, 5);
	if (r < 0)
	{
		Log(LogLevel::Error, "[GDB] err: can't listen to SockFd\n");
		goto err;
	}

	return true;

err:
	Close();
	return false;
}

void GdbStub::Close()
{
	Disconnect();
	if (SockFd != InvalidSocket) closesocket(SockFd);
	SockFd = InvalidSocket;
#ifdef _WIN32
	if (WinsockInitialized) WSACleanup();
	WinsockInitialized = false;
#endif
}

void GdbStub::Disconnect()
{
	if (IsConnected()) closesocket(ConnFd);
	ConnFd = InvalidSocket;
	NoAck = false;
	RecvBufferFilled = 0;
	Cmdlen = 0;
	Stat = TgtStatus::None;
	StatFlag = false;
}

GdbStub::~GdbStub()
{
	Close();
	delete (struct sockaddr_in*)ServerSA;
	delete (struct sockaddr_in*)ClientSA;
}

SubcmdHandler GdbStub::Handlers_v[] = {
	{ .MainCmd = 'v', .SubStr = "Attach;"       , .Handler = GdbStub::Handle_v_Attach },
	{ .MainCmd = 'v', .SubStr = "Kill;"         , .Handler = GdbStub::Handle_v_Kill },
	{ .MainCmd = 'v', .SubStr = "Run"           , .Handler = GdbStub::Handle_v_Run },
	{ .MainCmd = 'v', .SubStr = "Stopped"       , .Handler = GdbStub::Handle_v_Stopped },
	{ .MainCmd = 'v', .SubStr = "MustReplyEmpty", .Handler = GdbStub::Handle_v_MustReplyEmpty },
	{ .MainCmd = 'v', .SubStr = "Cont?"         , .Handler = GdbStub::Handle_v_ContQuery },
	{ .MainCmd = 'v', .SubStr = "Cont"          , .Handler = GdbStub::Handle_v_Cont },

	{ .MainCmd = 'v', .SubStr = NULL, .Handler = NULL }
};

SubcmdHandler GdbStub::Handlers_q[] = {
	{ .MainCmd = 'q', .SubStr = "HostInfo"   , .Handler = GdbStub::Handle_q_HostInfo },
	{ .MainCmd = 'q', .SubStr = "ProcessInfo", .Handler = GdbStub::Handle_q_HostInfo },
	{ .MainCmd = 'q', .SubStr = "Rcmd,"      , .Handler = GdbStub::Handle_q_Rcmd },
	{ .MainCmd = 'q', .SubStr = "Supported:" , .Handler = GdbStub::Handle_q_Supported },
	{ .MainCmd = 'q', .SubStr = "CRC:"       , .Handler = GdbStub::Handle_q_CRC },
	{ .MainCmd = 'q', .SubStr = "C"          , .Handler = GdbStub::Handle_q_C },
	{ .MainCmd = 'q', .SubStr = "fThreadInfo", .Handler = GdbStub::Handle_q_fThreadInfo },
	{ .MainCmd = 'q', .SubStr = "sThreadInfo", .Handler = GdbStub::Handle_q_sThreadInfo },
	{ .MainCmd = 'q', .SubStr = "Attached"   , .Handler = GdbStub::Handle_q_Attached },
	{ .MainCmd = 'q', .SubStr = "Xfer:features:read:target.xml:", .Handler = GdbStub::Handle_q_features },

	{ .MainCmd = 'q', .SubStr = NULL, .Handler = NULL },
};

SubcmdHandler GdbStub::Handlers_Q[] = {
	{ .MainCmd = 'Q', .SubStr = "StartNoAckMode", .Handler = GdbStub::Handle_Q_StartNoAckMode },

	{ .MainCmd = 'Q', .SubStr = NULL, .Handler = NULL },
};

ExecResult GdbStub::Handle_q(GdbStub* stub, const u8* cmd, ssize_t len)
{
	return stub->SubcmdExec(cmd, len, Handlers_q);
}

ExecResult GdbStub::Handle_v(GdbStub* stub, const u8* cmd, ssize_t len)
{
	return stub->SubcmdExec(cmd, len, Handlers_v);
}

ExecResult GdbStub::Handle_Q(GdbStub* stub, const u8* cmd, ssize_t len)
{
	return stub->SubcmdExec(cmd, len, Handlers_Q);
}

CmdHandler GdbStub::Handlers_top[] = {
	{ .Cmd = 'g', .Handler = GdbStub::Handle_g },
	{ .Cmd = 'G', .Handler = GdbStub::Handle_G },
	{ .Cmd = 'm', .Handler = GdbStub::Handle_m },
	{ .Cmd = 'M', .Handler = GdbStub::Handle_M },
	{ .Cmd = 'X', .Handler = GdbStub::Handle_X },
	{ .Cmd = 'c', .Handler = GdbStub::Handle_c },
	{ .Cmd = 's', .Handler = GdbStub::Handle_s },
	{ .Cmd = 'p', .Handler = GdbStub::Handle_p },
	{ .Cmd = 'P', .Handler = GdbStub::Handle_P },
	{ .Cmd = 'H', .Handler = GdbStub::Handle_H },
	{ .Cmd = 'T', .Handler = GdbStub::Handle_H },

	{ .Cmd = '?', .Handler = GdbStub::Handle_Question },
	{ .Cmd = '!', .Handler = GdbStub::Handle_Exclamation },
	{ .Cmd = 'D', .Handler = GdbStub::Handle_D },
	{ .Cmd = 'r', .Handler = GdbStub::Handle_r },
	{ .Cmd = 'R', .Handler = GdbStub::Handle_R },
	{ .Cmd = 'k', .Handler = GdbStub::Handle_k },

	{ .Cmd = 'z', .Handler = GdbStub::Handle_z },
	{ .Cmd = 'Z', .Handler = GdbStub::Handle_Z },

	{ .Cmd = 'q', .Handler = GdbStub::Handle_q },
	{ .Cmd = 'v', .Handler = GdbStub::Handle_v },
	{ .Cmd = 'Q', .Handler = GdbStub::Handle_Q },

	{ .Cmd = 0, .Handler = NULL }
};


StubState GdbStub::HandlePacket()
{
	ExecResult r = CmdExec(Handlers_top);
	if (!IsConnected()) return StubState::Disconnect;

	if (r == ExecResult::MustBreak)
	{
		if (Stat == TgtStatus::None || Stat == TgtStatus::Running)
			Stat = TgtStatus::BreakReq;
		return StubState::Break;
	}
	else if (r == ExecResult::InitialBreak)
	{
		Stat = TgtStatus::BreakReq;
		return StubState::Attach;
	/*}
	else if (r == ExecResult::Detached)
	{
		Stat = TgtStatus::None;
		return StubState::Disconnect;*/
	}
	else if (r == ExecResult::Continue)
	{
		Stat = TgtStatus::Running;
		return StubState::Continue;
	}
	else if (r == ExecResult::Step)
	{
		return StubState::Step;
	}
	else if (r == ExecResult::Ok || r == ExecResult::UnkCmd)
	{
		return StubState::None;
	}
	else
	{
		Disconnect();
		return StubState::Disconnect;
	}
}

StubState GdbStub::Poll(bool wait)
{
	if (!IsConnected())
	{
		if (SockFd == InvalidSocket) return StubState::NoConn;
		const int ready = WaitForSocket(SockFd, false, wait ? -1 : 0);
		if (ready < 0) { Close(); return StubState::Disconnect; }
		if (ready == 0) return StubState::NoConn;
		auto* client = static_cast<sockaddr_in*>(ClientSA);
		socklen_t length = sizeof(*client);
#ifdef __linux__
		ConnFd = accept4(SockFd, reinterpret_cast<sockaddr*>(client), &length, SOCK_CLOEXEC);
#else
		ConnFd = accept(SockFd, reinterpret_cast<sockaddr*>(client), &length);
#endif
		if (!IsConnected()) return StubState::NoConn;
		if (SocketSetBlocking(ConnFd, false) < 0)
		{
			Disconnect();
			return StubState::Disconnect;
		}
		u8 ack = 0;
		if (WaitAckBlocking(&ack, 1000) < 0 || ack != '+' || SendAck() < 0)
		{
			Log(LogLevel::Error, "[GDB] initial handshake failed\n");
			Disconnect();
			return StubState::Disconnect;
		}
		Stat = TgtStatus::Running;
		StatFlag = false;
	}

	if (StatFlag)
	{
		StatFlag = false;
		Handle_Question(this, nullptr, 0);
		if (!IsConnected()) return StubState::Disconnect;
	}

	// Process a coalesced command even when the socket has no new bytes.
	ReadResult result = ParseAndSetupPacket();
	if (result == ReadResult::NoPacket)
	{
		const int ready = WaitForSocket(ConnFd, false, wait ? -1 : 0);
		if (ready < 0) { Disconnect(); return StubState::Disconnect; }
		if (ready == 0) return StubState::None;
		result = MsgRecv();
	}
	switch (result)
	{
	case ReadResult::NoPacket:
		return StubState::None;
	case ReadResult::Break:
		return StubState::Break;
	case ReadResult::Wut:
	case ReadResult::Eof:
		Disconnect();
		return StubState::Disconnect;
	case ReadResult::CksumErr:
		if (SendNak() < 0) { Disconnect(); return StubState::Disconnect; }
		return StubState::None;
	case ReadResult::CmdRecvd:
		return HandlePacket();
	}
	return StubState::None;
}

ExecResult GdbStub::SubcmdExec(const u8* cmd, ssize_t len, const SubcmdHandler* handlers)
{
	for (size_t i = 0; handlers[i].Handler != nullptr; ++i)
	{
		const size_t prefix = strlen(handlers[i].SubStr);
		if (len >= 0 && size_t(len) >= prefix && !memcmp(cmd, handlers[i].SubStr, prefix))
			return handlers[i].Handler(this, cmd + prefix, len - ssize_t(prefix));
	}
	Resp(nullptr, 0);
	return ExecResult::UnkCmd;
}

ExecResult GdbStub::CmdExec(const CmdHandler* handlers)
{
	if (SendAck() < 0) return ExecResult::NetErr;
	for (size_t i = 0; handlers[i].Handler != nullptr; ++i)
	{
		if (Cmdlen > 0 && handlers[i].Cmd == Cmdbuf[0])
			return handlers[i].Handler(this, &Cmdbuf[1], Cmdlen - 1);
	}
	Resp(nullptr, 0);
	return ExecResult::UnkCmd;
}

void GdbStub::SignalStatus(TgtStatus stat, u32 arg)
{
	//Log(LogLevel::Debug, "[GDB] SIGNAL STATUS %d!\n", stat);

	this->Stat = stat;
	StatFlag = true;

	if (stat == TgtStatus::Bkpt) CurBkpt = arg;
	else if (stat == TgtStatus::Watchpt) CurWatchpt = arg;
}


StubState GdbStub::Enter(bool stay, TgtStatus stat, u32 arg, bool wait_for_conn)
{
	if (stat != TgtStatus::NoEvent) SignalStatus(stat, arg);

	StubState st;
	bool do_next = true;
	do
	{
		bool was_conn = IsConnected();
		st = Poll(wait_for_conn);
		bool has_conn = IsConnected();

		if (has_conn && !was_conn) stay = true;

		switch (st)
		{
		case StubState::Break:
			Log(LogLevel::Info, "[GDB] break execution\n");
			SignalStatus(TgtStatus::BreakReq, ~(u32)0);
			break;
		case StubState::Continue:
			Log(LogLevel::Info, "[GDB] continue execution\n");
			do_next = false;
			break;
		case StubState::Step:
			Log(LogLevel::Info, "[GDB] single-step\n");
			do_next = false;
			break;
		case StubState::Disconnect:
			Log(LogLevel::Info, "[GDB] disconnect\n");
			SignalStatus(TgtStatus::None, ~(u32)0);
			do_next = false;
			break;
		default: break;
		}
	}
	while (do_next && stay);

	if (st != StubState::None && st != StubState::NoConn)
	{
		Log(LogLevel::Debug, "[GDB] enter exit: %d\n", st);
	}
	return st;
}

void GdbStub::AddBkpt(u32 addr, int kind)
{
	BpWp np;
	np.addr = addr ^ (addr & 1); // clear lowest bit to not break on thumb mode weirdnesses
	np.len = 0;
	np.kind = kind;

	{
		// already in the map
		auto search = BpList.find(np.addr);
		if (search != BpList.end()) return;
	}

	BpList.insert({np.addr, np});

	Log(LogLevel::Debug, "[GDB] added bkpt:\n");
	size_t i = 0;
	for (auto search = BpList.begin(); search != BpList.end(); ++search, ++i)
	{
		Log(LogLevel::Debug, "\t[%zu]: addr=%08x, kind=%d\n", i, search->first, search->second.kind);
	}
}
void GdbStub::AddWatchpt(u32 addr, u32 len, int kind)
{
	BpWp np;
	np.addr = addr;
	np.len = len;
	np.kind = kind;

	for (auto search = WpList.begin(); search != WpList.end(); ++search)
	{
		if (search->addr > addr)
		{
			WpList.insert(search, np);
			return;
		}
		else if (search->addr == addr && search->kind == kind)
		{
			if (search->len < len) search->len = len;
			return;
		}
	}

	WpList.push_back(np);
}

void GdbStub::DelBkpt(u32 addr, int kind)
{
	addr = addr ^ (addr & 1);

	auto search = BpList.find(addr);
	if (search != BpList.end())
	{
		BpList.erase(search);
	}
}
void GdbStub::DelWatchpt(u32 addr, u32 len, int kind)
{
	(void)len; (void)kind;

	for (auto search = WpList.begin(); search != WpList.end(); ++search)
	{
		if (search->addr == addr && search->kind == kind)
		{
			WpList.erase(search);
			return;
		}
		else if (search->addr > addr) return;
	}
}

void GdbStub::DelAllBpWp()
{
	BpList.erase(BpList.begin(), BpList.end());
	WpList.erase(WpList.begin(), WpList.end());
}

StubState GdbStub::CheckBkpt(u32 addr, bool enter, bool stay)
{
	addr ^= (addr & 1); // clear lowest bit to not break on thumb mode weirdnesses

	auto search = BpList.find(addr);
	if (search == BpList.end()) return StubState::CheckNoHit;

	if (enter)
	{
		StubState r = Enter(stay, TgtStatus::Bkpt, addr);
		Log(LogLevel::Debug, "[GDB] ENTER st=%d\n", r);
		return r;
	}
	else
	{
		SignalStatus(TgtStatus::Bkpt, addr);
		return StubState::None;
	}
}
StubState GdbStub::CheckWatchpt(u32 addr, int kind, bool enter, bool stay)
{
	for (auto search = WpList.begin(); search != WpList.end(); ++search)
	{
		if (search->addr > addr) break;

		if (addr >= search->addr && addr < search->addr + search->len && search->kind == kind)
		{
			if (enter) return Enter(stay, TgtStatus::Watchpt, addr);
			else
			{
				SignalStatus(TgtStatus::Watchpt, addr);
				return StubState::None;
			}
		}
	}

	return StubState::CheckNoHit;
}

int GdbStub::Resp(const u8* data1, size_t len1, const u8* data2, size_t len2)
{
	return Resp(data1, len1, data2, len2, NoAck);
}
int GdbStub::RespC(const char* data1, size_t len1, const u8* data2, size_t len2)
{
	return Resp((const u8*)data1, len1, data2, len2, NoAck);
}
#if defined(__GCC__) || defined(__clang__)
__attribute__((__format__(printf, 2/*includes implicit this*/, 3)))
#endif
int GdbStub::RespFmt(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int r = vsnprintf((char*)&RespBuf[1], GDBPROTO_MAX_PAYLOAD + 1, fmt, args);
	va_end(args);

	if (r < 0) return r;

	if (size_t(r) > GDBPROTO_MAX_PAYLOAD) return -1;

	return Resp(&RespBuf[1], r);
}

int GdbStub::RespStr(const char* str)
{
	return Resp((const u8*)str, strlen(str));
}

}

