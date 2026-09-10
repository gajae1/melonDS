
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <array>
#include <charconv>
#include <span>
#include <string_view>

#include "../Platform.h"
#include "hexutil.h"

#include "GdbStub.h"

using namespace melonDS;
using Platform::Log;
using Platform::LogLevel;

namespace Gdb
{

enum class GdbSignal : int
{
	INT  = 2,
	TRAP = 5,
	EMT  = 7, // "emulation trap"
	SEGV = 11,
	ILL  = 4
};

// 12: llvm::MachO::CPU_TYPE_ARM
// 5: llvm::MachO::CPU_SUBTYPE_ARM_V4T
// 7: llvm::MachO::CPU_SUBTYPE_ARM_V5TEJ
const char* TARGET_INFO_ARM7 = "cputype:12;cpusubtype:5;triple:arm-none-eabi;ostype:none;vendor:none;endian:little;ptrsize:4;";
const char* TARGET_INFO_ARM9 = "cputype:12;cpusubtype:7;triple:arm-none-eabi;ostype:none;vendor:none;endian:little;ptrsize:4;";


#define TARGET_XML__CORE_REGS \
	"<reg name=\"r0\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r1\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r2\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r3\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r4\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r5\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r6\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r7\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r8\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r9\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r10\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r11\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"r12\" bitsize=\"32\" type=\"uint32\"/>" \
	"<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>" \
	"<reg name=\"lr\" bitsize=\"32\" type=\"code_ptr\"/>" \
	"<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>" \
	/* 16 regs */ \

#define TARGET_XML__MODE_REGS \
	"<reg name=\"cpsr\" bitsize=\"32\" regnum=\"25\"/>" \
	"<reg name=\"sp_usr\" bitsize=\"32\" regnum=\"26\" type=\"data_ptr\"/>" \
	"<reg name=\"lr_usr\" bitsize=\"32\" regnum=\"27\" type=\"code_ptr\"/>" \
	"<reg name=\"r8_fiq\" bitsize=\"32\" type=\"uint32\" regnum=\"28\"/>" \
	"<reg name=\"r9_fiq\" bitsize=\"32\" type=\"uint32\" regnum=\"29\"/>" \
	"<reg name=\"r10_fiq\" bitsize=\"32\" type=\"uint32\" regnum=\"30\"/>" \
	"<reg name=\"r11_fiq\" bitsize=\"32\" type=\"uint32\" regnum=\"31\"/>" \
	"<reg name=\"r12_fiq\" bitsize=\"32\" type=\"uint32\" regnum=\"32\"/>" \
	"<reg name=\"sp_fiq\" bitsize=\"32\" regnum=\"33\" type=\"data_ptr\"/>" \
	"<reg name=\"lr_fiq\" bitsize=\"32\" regnum=\"34\" type=\"code_ptr\"/>" \
	"<reg name=\"sp_irq\" bitsize=\"32\" regnum=\"35\" type=\"data_ptr\"/>" \
	"<reg name=\"lr_irq\" bitsize=\"32\" regnum=\"36\" type=\"code_ptr\"/>" \
	"<reg name=\"sp_svc\" bitsize=\"32\" regnum=\"37\" type=\"data_ptr\"/>" \
	"<reg name=\"lr_svc\" bitsize=\"32\" regnum=\"38\" type=\"code_ptr\"/>" \
	"<reg name=\"sp_abt\" bitsize=\"32\" regnum=\"39\" type=\"data_ptr\"/>" \
	"<reg name=\"lr_abt\" bitsize=\"32\" regnum=\"40\" type=\"code_ptr\"/>" \
	"<reg name=\"sp_und\" bitsize=\"32\" regnum=\"41\" type=\"data_ptr\"/>" \
	"<reg name=\"lr_und\" bitsize=\"32\" regnum=\"42\" type=\"code_ptr\"/>" \
	"<reg name=\"spsr_fiq\" bitsize=\"32\" regnum=\"43\"/>" \
	"<reg name=\"spsr_irq\" bitsize=\"32\" regnum=\"44\"/>" \
	"<reg name=\"spsr_svc\" bitsize=\"32\" regnum=\"45\"/>" \
	"<reg name=\"spsr_abt\" bitsize=\"32\" regnum=\"46\"/>" \
	"<reg name=\"spsr_und\" bitsize=\"32\" regnum=\"47\"/>" \
	/* 23 regs */ \


const char* TARGET_XML_ARM7 =
	"<target version=\"1.0\">"
	"<architecture>armv4t</architecture>"
	"<osabi>none</osabi>"
	"<feature name=\"org.gnu.gdb.arm.core\">"
	TARGET_XML__CORE_REGS
	TARGET_XML__MODE_REGS
	// 39 regs total
	"</feature>"
	"</target>";


const char* TARGET_XML_ARM9 =
	"<target version=\"1.0\">"
	"<architecture>armv5te</architecture>"
	"<osabi>none</osabi>"
	"<feature name=\"org.gnu.gdb.arm.core\">"
	TARGET_XML__CORE_REGS
	TARGET_XML__MODE_REGS
	// 39 regs total
	"</feature>"
	"</target>";
	// TODO: CP15?


static int DoQResponse(GdbStub* stub, const u8* query, const char* data, const size_t len)
{
	size_t qaddr, qlen;

	Log(LogLevel::Debug, "[GDB qresp] query='%s'\n", query);
	if (sscanf((const char*)query, "%zx,%zx", &qaddr, &qlen) != 2)
	{
		return stub->RespStr("E01");
	}
	else if (qaddr >  len)
	{
		return stub->RespStr("E01");
	}
	else if (qaddr == len)
	{
		return stub->RespStr("l");
	}

	size_t bleft = len - qaddr;
	size_t outlen = qlen;
	if (outlen > bleft) outlen = bleft;
	// The XML chunk shares packet data space with its continuation marker.
	if (outlen > GDBPROTO_MAX_PAYLOAD - 1) outlen = GDBPROTO_MAX_PAYLOAD - 1;
	Log(LogLevel::Debug, "[GDB qresp] qaddr=%zu qlen=%zu left=%zu outlen=%zu\n",
			qaddr, qlen, bleft, outlen);

	return stub->RespC("m", 1, (const u8*)&data[qaddr], outlen);
}

__attribute__((__aligned__(4)))
static u8 tempdatabuf[1024];

ExecResult GdbStub::Handle_g(GdbStub* stub, const u8* cmd, ssize_t len)
{
	u8* regstrbuf = tempdatabuf;

	for (size_t i = 0; i < GDB_ARCH_N_REG; ++i)
	{
		u32 v = stub->Cb->ReadReg(static_cast<Register>(i));
		hexfmt32(&regstrbuf[i*4*2], v);
	}

	stub->Resp(regstrbuf, GDB_ARCH_N_REG*4*2);

	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_G(GdbStub* stub, const u8* cmd, ssize_t len)
{
	if (len != GDB_ARCH_N_REG*4*2)
	{
		Log(LogLevel::Error, "[GDB] REG WRITE ERR: BAD LEN: %zd != %d!\n", len, GDB_ARCH_N_REG*4*2);
		stub->RespStr("E01");
		return ExecResult::Ok;
	}

	for (int i = 0; i < GDB_ARCH_N_REG; ++i)
	{
		u32 v = unhex32(&cmd[i*4*2]);
		stub->Cb->WriteReg(static_cast<Register>(i), v);
	}

	stub->RespStr("OK");

	return ExecResult::Ok;
}

// Parse the entire range header before touching guest memory. A range ending
// exactly at 2^32 is valid; a range crossing that boundary is not.
static bool ParseMemoryRange(const u8* cmd, ssize_t len, bool hasBody,
                             u32& address, u32& length, size_t& bodyOffset)
{
	if (len < 0 || size_t(len) > GDBPROTO_MAX_PAYLOAD) return false;
	std::string_view input(reinterpret_cast<const char*>(cmd), size_t(len));
	const size_t comma = input.find(',');
	const size_t end = hasBody ? input.find(':', comma) : input.size();
	if (comma == input.npos || end == input.npos) return false;
	const auto parse = [](std::string_view part, u32& value) {
		const auto result = std::from_chars(part.data(), part.data() + part.size(), value, 16);
		return result.ec == std::errc{} && result.ptr == part.data() + part.size();
	};
	if (!parse(input.substr(0, comma), address) ||
		!parse(input.substr(comma + 1, end - comma - 1), length)) return false;
	if (u64(address) + length > (u64{1} << 32)) return false;
	bodyOffset = hasBody ? end + 1 : end;
	return true;
}

static size_t MemoryAccessBytes(u32 address, size_t remaining)
{
	if ((address & 1) || remaining < 2) return 1;
	if ((address & 2) || remaining < 4) return 2;
	return 4;
}

static void WriteMemory(StubCallbacks* cb, u32 address, std::span<const u8> data)
{
	while (!data.empty())
	{
		const size_t count = MemoryAccessBytes(address, data.size());
		u32 value = 0;
		for (size_t i = 0; i < count; ++i) value |= u32(data[i]) << (8 * i);
		cb->WriteMem(address, int(count * 8), value);
		address += u32(count);
		data = data.subspan(count);
	}
}

ExecResult GdbStub::Handle_m(GdbStub* stub, const u8* cmd, ssize_t len)
{
	u32 address, length;
	size_t offset;
	if (!ParseMemoryRange(cmd, len, false, address, length, offset))
	{
		stub->RespStr("E01");
		return ExecResult::Ok;
	}
	if (length > GDBPROTO_MAX_PAYLOAD / 2)
	{
		stub->RespStr("E02");
		return ExecResult::Ok;
	}
	std::array<u8, GDBPROTO_MAX_PAYLOAD> response;
	for (size_t done = 0; done < length; )
	{
		const size_t count = MemoryAccessBytes(address, length - done);
		const u32 value = stub->Cb->ReadMem(address, int(count * 8));
		for (size_t i = 0; i < count; ++i) hexfmt8(&response[(done + i) * 2], u8(value >> (i * 8)));
		address += u32(count);
		done += count;
	}
	stub->Resp(response.data(), length * 2);
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_M(GdbStub* stub, const u8* cmd, ssize_t len)
{
	u32 address, length;
	size_t offset;
	if (!ParseMemoryRange(cmd, len, true, address, length, offset) ||
		u64(length) * 2 != size_t(len) - offset)
	{
		stub->RespStr("E01");
		return ExecResult::Ok;
	}
	std::array<u8, GDBPROTO_MAX_PAYLOAD / 2> data;
	for (size_t i = 0; i < length; ++i)
	{
		unsigned value;
		const char* byte = reinterpret_cast<const char*>(cmd + offset + i * 2);
		const auto result = std::from_chars(byte, byte + 2, value, 16);
		if (result.ec != std::errc{} || result.ptr != byte + 2)
		{
			stub->RespStr("E01");
			return ExecResult::Ok;
		}
		data[i] = u8(value);
	}
	WriteMemory(stub->Cb, address, std::span(data.data(), length));
	stub->RespStr("OK");
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_X(GdbStub* stub, const u8* cmd, ssize_t len)
{
	u32 address, length;
	size_t offset;
	if (!ParseMemoryRange(cmd, len, true, address, length, offset) || length > size_t(len) - offset)
	{
		stub->RespStr("E01");
		return ExecResult::Ok;
	}
	std::array<u8, GDBPROTO_MAX_PAYLOAD> data;
	size_t decoded = 0;
	bool valid = true;
	while (offset < size_t(len))
	{
		if (decoded == length) { valid = false; break; }
		u8 value = cmd[offset++];
		if (value == '}')
		{
			if (offset == size_t(len)) { valid = false; break; }
			value = cmd[offset++] ^ 0x20;
		}
		else if (value == '$' || value == '#') { valid = false; break; }
		data[decoded++] = value;
	}
	if (!valid || decoded != length)
	{
		stub->RespStr("E01");
		return ExecResult::Ok;
	}
	WriteMemory(stub->Cb, address, std::span(data.data(), length));
	stub->RespStr("OK");
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_c(GdbStub* stub, const u8* cmd, ssize_t len)
{
	u32 addr = ~(u32)0;

	if (len > 0)
	{
		if (len <= 8)
		{
			if (sscanf((const char*)cmd, "%08X", &addr) != 1)
			{
				stub->RespStr("E01");
			} // else: ok
		}
		else
		{
			stub->RespStr("E01");
		}
	} // else: continue at current

	if (~addr)
	{
		stub->Cb->WriteReg(Register::pc, addr);
	}

	return ExecResult::Continue;
}

ExecResult GdbStub::Handle_s(GdbStub* stub, const u8* cmd, ssize_t len) {
	u32 addr = ~(u32)0;

	if (len > 0)
	{
		if (len <= 8)
		{
			if (sscanf((const char*)cmd, "%08X", &addr) != 1) {
				stub->RespStr("E01");
				return ExecResult::Ok;
			} // else: ok
		}
		else
		{
			stub->RespStr("E01");
			return ExecResult::Ok;
		}
	} // else: continue at current

	if (~addr != 0)
	{
		stub->Cb->WriteReg(Register::pc, addr);
	}

	return ExecResult::Step;
}

ExecResult GdbStub::Handle_p(GdbStub* stub, const u8* cmd, ssize_t len)
{
	int reg;
	if (sscanf((const char*)cmd, "%x", &reg) != 1 || reg < 0 || reg >= GDB_ARCH_N_REG)
	{
		stub->RespStr("E01");
		return ExecResult::Ok;
	}

	u32 v = stub->Cb->ReadReg(static_cast<Register>(reg));
	hexfmt32(tempdatabuf, v);
	stub->Resp(tempdatabuf, 4*2);

	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_P(GdbStub* stub, const u8* cmd, ssize_t len)
{
	int reg, dataoff;

	if (sscanf((const char*)cmd, "%x=%n", &reg, &dataoff) != 1 || reg < 0
			|| reg >= GDB_ARCH_N_REG || dataoff + 4*2 > len)
	{
		stub->RespStr("E01");
		return ExecResult::Ok;
	}

	u32 v = unhex32(&cmd[dataoff]);
	stub->Cb->WriteReg(static_cast<Register>(reg), v);

	stub->RespStr("OK");

	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_H(GdbStub* stub, const u8* cmd, ssize_t len)
{
	u8 operation = cmd[0];
	u32 thread_id;
	sscanf((const char*)&cmd[1], "%u", &thread_id);

	(void)operation;
	if (thread_id <= 1)
	{
		stub->RespStr("OK");
	}
	else
	{
		stub->RespStr("E01");
	}

	return ExecResult::Ok;
}


ExecResult GdbStub::Handle_Question(GdbStub* stub, const u8* cmd, ssize_t len)
{
	// "request reason for target halt" (which must also halt)

	TgtStatus st = stub->Stat;
	u32 arg = ~(u32)0;
	int typ = 0;

	switch (st)
	{
	case TgtStatus::None: // no target!
		stub->RespStr("W00");
		break;

	case TgtStatus::Running: // will break very soon due to retval
	case TgtStatus::BreakReq:
		stub->RespFmt("S%02X", GdbSignal::INT);
		break;

	case TgtStatus::SingleStep:
		stub->RespFmt("S%02X", GdbSignal::TRAP);
		break;

	case TgtStatus::Bkpt:
		arg = stub->CurBkpt;
		typ = 1;
		goto bkpt_rest;
	case TgtStatus::Watchpt:
		arg = stub->CurWatchpt;
		typ = 2;
	bkpt_rest:
		if (!~arg)
		{
			stub->RespFmt("S%02X", GdbSignal::TRAP);
		}
		else
		{
			switch (typ)
			{
			case 1:
				stub->RespFmt("S%02X", GdbSignal::TRAP);
				//stub->RespFmt("T%02Xhwbreak:"/*"%08X"*/";", GdbSignal::TRAP/*, arg*/);
				break;
			case 2:
				stub->RespFmt("S%02X", GdbSignal::TRAP);
				//stub->RespFmt("T%02Xwatch:"/*"%08X"*/";", GdbSignal::TRAP/*, arg*/);
				break;
			default:
				stub->RespFmt("S%02X", GdbSignal::TRAP);
				break;
			}
		}
		break;
	case TgtStatus::BkptInsn:
		stub->RespFmt("T%02Xswbreak:%08X;", GdbSignal::TRAP,
				stub->Cb->ReadReg(Register::pc));
		break;

		// these three should technically be a SIGBUS but gdb etc don't really
		// like that (plus it sounds confusing)
	case TgtStatus::FaultData:
	case TgtStatus::FaultIAcc:
		stub->RespFmt("S%02X", GdbSignal::SEGV);
		break;
	case TgtStatus::FaultInsn:
		stub->RespFmt("S%02X", GdbSignal::ILL);
		break;
	default: break;
	}

	return ExecResult::InitialBreak;
}

ExecResult GdbStub::Handle_Exclamation(GdbStub* stub, const u8* cmd, ssize_t len)
{
	stub->RespStr("OK");
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_D(GdbStub* stub, const u8* cmd, ssize_t len)
{
	stub->RespStr("OK");
	return ExecResult::Detached;
}

ExecResult GdbStub::Handle_r(GdbStub* stub, const u8* cmd, ssize_t len)
{
	stub->Cb->ResetGdb();
	return ExecResult::Ok;
}
ExecResult GdbStub::Handle_R(GdbStub* stub, const u8* cmd, ssize_t len)
{
	stub->Cb->ResetGdb();
	return ExecResult::Ok;
}
ExecResult GdbStub::Handle_k(GdbStub* stub, const u8* cmd, ssize_t len)
{
	return ExecResult::Detached;
}


ExecResult GdbStub::Handle_z(GdbStub* stub, const u8* cmd, ssize_t len)
{
	int typ;
	u32 addr, kind;

	if (sscanf((const char*)cmd, "%d,%x,%u", &typ, &addr, &kind) != 3)
	{
		stub->RespStr("E01");
		return ExecResult::Ok;
	}

	switch (typ)
	{
	case 0: case 1: // remove breakpoint (we cheat & always insert a hardware breakpoint)
		stub->DelBkpt(addr, kind);
		break;
	case 2: case 3: case 4: // watchpoint. currently not distinguishing between reads & writes oops
		stub->DelWatchpt(addr, kind, typ);
		break;
	default:
		stub->RespStr("E02");
		return ExecResult::Ok;
	}

	stub->RespStr("OK");
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_Z(GdbStub* stub, const u8* cmd, ssize_t len)
{
	int typ;
	u32 addr, kind;

	if (sscanf((const char*)cmd, "%d,%x,%u", &typ, &addr, &kind) != 3)
	{
		stub->RespStr("E01");
		return ExecResult::Ok;
	}

	switch (typ)
	{
	case 0: case 1: // insert breakpoint (we cheat & always insert a hardware breakpoint)
		stub->AddBkpt(addr, kind);
		break;
	case 2: case 3: case 4: // watchpoint. currently not distinguishing between reads & writes oops
		stub->AddWatchpt(addr, kind, typ);
		break;
	default:
		stub->RespStr("E02");
		return ExecResult::Ok;
	}

	stub->RespStr("OK");
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_q_HostInfo(GdbStub* stub, const u8* cmd, ssize_t len)
{
	const char* resp = "";

	switch (stub->Cb->GetCPU())
	{
	case 7: resp = TARGET_INFO_ARM7; break;
	case 9: resp = TARGET_INFO_ARM9; break;
	default: break;
	}

	stub->RespStr(resp);
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_q_Rcmd(GdbStub* stub, const u8* cmd, ssize_t len)
{

	memset(tempdatabuf, 0, sizeof tempdatabuf);
	for (ssize_t i = 0; i < len/2; ++i)
	{
		tempdatabuf[i] = unhex8(&cmd[i*2]);
	}

	int r = stub->Cb->RemoteCmd(tempdatabuf, len/2);

	if (r) stub->RespFmt("E%02X", r&0xff);
	else stub->RespStr("OK");

	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_q_Supported(GdbStub* stub,
		const u8* cmd, ssize_t len) {
	// TODO: support Xfer:memory-map:read::
	//       but NWRAM is super annoying with that
	stub->RespFmt("PacketSize=%X;qXfer:features:read+;swbreak-;hwbreak+;QStartNoAckMode+", unsigned(GDBPROTO_MAX_PAYLOAD));
	return ExecResult::Ok;
}

// GDB qCRC (libiberty xcrc32): MSB-first polynomial 0x04C11DB7, initial value
// 0xFFFFFFFF, no reflection and no final XOR. The shared melonDS CRC32 is the
// reflected/zlib variant used by other subsystems and must stay unchanged.
constexpr u32 GdbCrcPolynomial = 0x04C11DB7;

constexpr auto MakeGdbCrcTable()
{
	std::array<u32, 256> table {};
	for (u32 index = 0; index < 256; index++)
	{
		u32 entry = index << 24;
		for (int bit = 0; bit < 8; bit++)
			entry = (entry & 0x80000000) ? (entry << 1) ^ GdbCrcPolynomial : entry << 1;
		table[index] = entry;
	}
	return table;
}

constexpr auto GdbCrcTable = MakeGdbCrcTable();

static u32 GdbCrcUpdate(const u8* data, u32 len, u32 crc)
{
	for (u32 i = 0; i < len; i++)
		crc = (crc << 8) ^ GdbCrcTable[(crc >> 24) ^ data[i]];
	return crc;
}

ExecResult GdbStub::Handle_q_CRC(GdbStub* stub,
		const u8* cmd, ssize_t llen)
{
	static u8 crcbuf[128];

	u32 addr, len;
	size_t offset;
	if (!ParseMemoryRange(cmd, llen, false, addr, len, offset))
	{
		stub->RespStr("E01");
		return ExecResult::Ok;
	}

	// A counted remainder keeps a range ending exactly at 2^32 finite; an
	// end-address comparison would wrap and terminate early instead.
	u32 val = 0xFFFFFFFF;
	u32 remaining = len;
	u32 caddr = addr;

	while (remaining)
	{
		// calc partial CRC in 128-byte chunks
		u32 clen = remaining < sizeof(crcbuf) ? remaining : u32(sizeof(crcbuf));

		for (u32 i = 0; i < clen; ++i)
		{
			crcbuf[i] = stub->Cb->ReadMem(caddr, 8);
			++caddr;
		}

		val = GdbCrcUpdate(crcbuf, clen, val);
		remaining -= clen;
	}

	stub->RespFmt("C%x", val);

	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_q_C(GdbStub* stub, const u8* cmd, ssize_t len)
{
	stub->RespStr("QC1"); // current thread ID is 1
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_q_fThreadInfo(GdbStub* stub, const u8* cmd, ssize_t len)
{
	stub->RespStr("m1"); // one thread
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_q_sThreadInfo(GdbStub* stub, const u8* cmd, ssize_t len)
{
	stub->RespStr("l"); // end of thread list
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_q_features(GdbStub* stub, const u8* cmd, ssize_t len)
{
	const char* resp;

	Log(LogLevel::Debug, "[GDB] CPU type = %d\n", stub->Cb->GetCPU());
	switch (stub->Cb->GetCPU())
	{
	case 7: resp = TARGET_XML_ARM7; break;
	case 9: resp = TARGET_XML_ARM9; break;
	default: resp = ""; break;
	}

	DoQResponse(stub, cmd, resp, strlen(resp));
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_q_Attached(GdbStub* stub, const u8* cmd, ssize_t len)
{
	stub->RespStr("1"); // always "attach to a process"
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_v_Attach(GdbStub* stub, const u8* cmd, ssize_t len)
{

	TgtStatus st = stub->Stat;

	if (st == TgtStatus::None)
	{
		// no target
		stub->RespStr("E01");
		return ExecResult::Ok;
	}

	stub->RespStr("T05thread:1;");

	if (st == TgtStatus::Running) return ExecResult::MustBreak;
	else return ExecResult::Ok;
}

ExecResult GdbStub::Handle_v_Kill(GdbStub* stub, const u8* cmd, ssize_t len)
{
	TgtStatus st = stub->Stat;

	stub->Cb->ResetGdb();

	stub->RespStr("OK");

	return (st != TgtStatus::Running && st != TgtStatus::None) ? ExecResult::Detached : ExecResult::Ok;
}

ExecResult GdbStub::Handle_v_Run(GdbStub* stub, const u8* cmd, ssize_t len)
{
	TgtStatus st = stub->Stat;

	stub->Cb->ResetGdb();

	// TODO: handle cmdline for homebrew?

	return (st != TgtStatus::Running && st != TgtStatus::None) ? ExecResult::Continue : ExecResult::Ok;
}

ExecResult GdbStub::Handle_v_Stopped(GdbStub* stub, const u8* cmd, ssize_t len)
{
	TgtStatus st = stub->Stat;

	static bool notified = true;

	// not sure if i understand this correctly
	if (st != TgtStatus::Running)
	{
		if (notified) stub->RespStr("OK");
		else stub->RespStr("W00");

		notified = !notified;
	}
	else stub->RespStr("OK");

	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_v_MustReplyEmpty(GdbStub* stub, const u8* cmd, ssize_t len)
{
	printf("must reply empty\n");
	stub->Resp(NULL, 0);
	return ExecResult::Ok;
}

ExecResult GdbStub::Handle_v_Cont(GdbStub* stub, const u8* cmd, ssize_t len)
{
	if (len < 2)
	{
		printf("insufficient length");
		stub->RespStr("E01");
		return ExecResult::Ok;
	}

	switch (cmd[1])
	{
	case 'c':
		stub->RespStr("OK");
		return ExecResult::Continue;
	case 's':
		stub->RespStr("OK");
		return ExecResult::Step;
	case 't':
		stub->RespStr("OK");
		return ExecResult::MustBreak;
	default:
		printf("invalid continue %c %s\n", cmd[1], cmd);
		stub->RespStr("E01");
		return ExecResult::Ok;
	}
}

ExecResult GdbStub::Handle_v_ContQuery(GdbStub* stub, const u8* cmd, ssize_t len)
{
	stub->RespStr("vCont;c;s;t");
	return ExecResult::Ok;
}


ExecResult GdbStub::Handle_Q_StartNoAckMode(GdbStub* stub, const u8* cmd, ssize_t len)
{
	stub->NoAck = true;
	stub->RespStr("OK");
	return ExecResult::Ok;
}

}
