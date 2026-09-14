# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production A64 MPU guard bytes; model only the C++ helper boundary.

CoreExecution checks the real DataAbort handler. Here Unicorn checks the
emitted call inputs, register spills, permission decision and exception reload.
This is not native ARM64 ABI/device or ARM9 silicon timing acceptance.
"""
import json
import struct
import subprocess
import sys

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X4,
    UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X21, UC_ARM64_REG_X29, UC_ARM64_REG_X30,
    UC_ARM64_REG_W27, UC_ARM64_REG_W28, UC_ARM64_REG_PC)

mode = sys.argv[2] if len(sys.argv) > 2 else "export-mpu"
block = mode == "export-mpu-block"
export = subprocess.run([sys.argv[1], mode], capture_output=True, text=True, timeout=30)
assert export.returncode == 0, export.stderr
cases = [json.loads(line) for line in export.stdout.splitlines() if line.startswith("{")]
assert len(cases) == (38 if block else 8)
checked = 0
for case in cases:
    for permission, second in ([(3,3), (0,3), (3,0), (1,1), (2,2)] if block else [(p,p) for p in range(4)]):
        for address in ((0x02001004, 0x02001FFD, 0xFFFFFFFD) if block else (0x02001000, 0xFFFFFFFF)):
            uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
            mapped = set()
            def pages(start, size):
                for page in range(start & ~4095, (start + size + 4095) & ~4095, 4096):
                    if page not in mapped:
                        uc.mem_map(page, 4096)
                        mapped.add(page)
            def put32(address, value):
                uc.mem_write(address, struct.pack("<I", value))
            def get32(address):
                return struct.unpack("<I", uc.mem_read(address, 4))[0]
            cpu, table = 0x10000000, 0x20000000
            pages(cpu, 0x10000)
            pages(table, 0x100000)
            code = struct.pack("<" + "I" * len(case["words"]), *case["words"])
            pages(case["base"], len(code))
            call = case["fallback"] if block else case["abort"]
            pages(call, 4)
            pages(case["return"], 4)
            uc.mem_write(case["base"], code)
            uc.mem_write(cpu + case["mapOffset"], struct.pack("<Q", table))
            uc.mem_write(table + (address >> 12), bytes([permission]))
            last = ((address & ~3) + (case["count"]-1)*4) & 0xFFFFFFFF if block else address
            different = (last >> 12) != (address >> 12)
            if different: uc.mem_write(table + (last >> 12), bytes([second]))
            offset = (-4*case["count"]+(0 if case["pre"] else 4)) if case["down"] else (4 if case["pre"] else 0)
            base_reg = (address-offset) & 0xFFFFFFFF
            rn = 13 if block and case["thumb"] and case["down"] else 2
            regs = cpu + case["regOffset"]
            put32(regs, 0x11111111)
            put32(regs + 4, 0xABCDEF01)
            put32(cpu + case["cpsrOffset"], 0xAAAAAAAA)
            uc.reg_write(UC_ARM64_REG_X0, address)
            uc.reg_write(UC_ARM64_REG_X4, 0xCAFEBABE)
            uc.reg_write(UC_ARM64_REG_X19, 0x12345678)
            uc.reg_write(UC_ARM64_REG_X20, 0xBADAC0DE)
            uc.reg_write(UC_ARM64_REG_X21, base_reg)
            uc.reg_write(UC_ARM64_REG_X29, cpu)
            uc.reg_write(UC_ARM64_REG_W27, 0x6800005F | (0x20 if case["thumb"] else 0))
            uc.reg_write(UC_ARM64_REG_W28, 7)
            calls = []
            def helper(machine, pc, size, unused):
                if pc != call:
                    return
                assert machine.reg_read(UC_ARM64_REG_X0) == cpu
                assert get32(regs) == 0x12345678 and get32(regs + 4) == 0xABCDEF01
                assert get32(regs + 60) == 0x02000C00 + (4 if case["thumb"] else 8)
                assert get32(cpu + case["cyclesOffset"]) == 20
                assert get32(cpu + case["cpsrOffset"]) == (0x6800005F | (0x20 if case["thumb"] else 0))
                if block:
                    assert get32(regs + 4*rn) == base_reg
                    assert get32(cpu + case["instrOffset"]) == case["instr"]
                    assert get32(cpu + case["codeCyclesOffset"]) == 19
                calls.append(pc)
                put32(cpu + case["cyclesOffset"], 31)
                put32(cpu + case["cpsrOffset"], 0x680000D7)
                put32(regs + 60, 0xFFFF0014)
                machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_X30))
            uc.hook_add(UC_HOOK_CODE, helper)
            uc.emu_start(case["base"], case["return"], count=256)
            mask = 2 if case["store"] else 1
            denied = case["num"] == 0 and (not (permission & mask) or (different and not (second & mask)))
            assert len(calls) == int(denied)
            assert uc.reg_read(UC_ARM64_REG_PC) == case["return"]
            if denied:
                assert uc.reg_read(UC_ARM64_REG_W27) == 0x680000D7
                assert uc.reg_read(UC_ARM64_REG_W28) == 31
                assert get32(regs + 16) == get32(regs + 20) == 0
            else:
                assert get32(regs + 16) == (1 if block else address) and get32(regs + 20) == 0xCAFEBABE
                assert uc.reg_read(UC_ARM64_REG_X21) == base_reg
                assert uc.reg_read(UC_ARM64_REG_W28) == 7
            checked += 1
print(f"Production A64 MPU guards: {checked} permission/register/cycle-boundary cases passed")
