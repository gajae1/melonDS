# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute current production A64 blocks in Unicorn 2.1.4, with ARM-doc oracles.

Unicorn executes A64 instructions; guest timing is the emitted W28 result, not
Unicorn's instruction count. This is not native A64 or ARM7TDMI silicon timing.
"""
import json
import struct
import subprocess
import sys
import unicorn
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X29, UC_ARM64_REG_W27, UC_ARM64_REG_W28, UC_ARM64_REG_PC

# CTest passes the host emitter executable. Unicorn only executes its synthetic
# A64 blocks; it supplies no Nintendo DS timing model or native ABI validation.
export = subprocess.run([sys.argv[1], 'export-mul-cycles'],
                        capture_output=True, text=True, timeout=30)
if export.returncode:
    raise SystemExit(export.stderr)
cases = [json.loads(line) for line in export.stdout.splitlines() if line.startswith('{')]
assert cases
lines = [f'Unicorn {unicorn.__version__}: actual production CompileBlock A64 bytes; guest cycle register W28']
failures = 0
counts = {}

def signed(value):
    return value if value < 0x80000000 else value - 0x100000000

for c in cases:
    initial = c['initial']
    expected = initial.copy()
    op, num = c['op'], c['num']
    thumb, long_op = op == 6, 2 <= op <= 5
    instruction = c['instrs'][c['prefix']]
    cond = instruction >> 28
    z = bool(c['cpsr'] & (1 << 30))
    taken = thumb or cond == 14 or (z if cond == 0 else not z)
    rd, rs, rm, rn = (c[name] for name in ('rd', 'rs', 'rm', 'rn'))
    flags = c['cpsr']
    m = c['m']
    if taken:
        a, b = (initial[rs], initial[rd]) if thumb else (initial[rm], initial[rs])
        product = signed(a) * signed(b) if op >= 4 and long_op else a * b
        if op == 1:
            product += initial[rn]
        elif long_op and op & 1:
            product += initial[rd] | (initial[rn] << 32)
        product &= (1 << (64 if long_op else 32)) - 1
        expected[rd] = product & 0xFFFFFFFF
        if long_op:
            expected[rn] = product >> 32
        if c['s']:
            nz = (((product >> (63 if long_op else 31)) & 1) << 31) | (int(product == 0) << 30)
            mask = (c['flags'] & 12) << 28
            flags = (flags & ~mask) | (nz & mask)
    if c['prefix']:
        expected[4] = (expected[4] + 1) & 0xFFFFFFFF
    if c['consumeZ'] and flags & (1 << 30):
        expected[6] = (expected[6] + 1) & 0xFFFFFFFF
    expected[15] += (len(c['instrs']) - 1) * (2 if thumb else 4)
    fetch = c['ns'] if num else (0 if initial[15] & 2 else 1)
    sequential = c['seq'] if num else 1
    internal = m + (1 if op in (1, 2, 4) else 2 if op in (3, 5) else 0) if num else (3 if c['s'] else 1)
    cycles = (fetch + internal if taken else sequential) + (c['prefix'] + c['consumeZ']) * sequential

    # Both backends must meet the same documented guest-cycle expectations.
    # ARM7 multiply C is architecturally undefined; retain the backend's policy.
    interpreter_mask = 0xFFFFFFFF
    if c['s']:
        interpreter_mask &= ~0xC0000000 | ((c['flags'] & 12) << 28)
        if num: interpreter_mask &= ~(1 << 29)  # architecturally undefined ARM7 multiply C
    interp_ok = (c['interpreter'] == expected and c['interpreterCycles'] == cycles
                 and ((c['interpreterCPSR'] ^ flags) & interpreter_mask) == 0)
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    code = struct.pack('<' + 'I' * len(c['words']), *c['words'])
    page = c['base'] & ~0xFFF
    size = ((c['base'] - page + len(code) + 0xFFF) // 0x1000) * 0x1000
    uc.mem_map(page, size)
    uc.mem_write(c['base'], code)
    exit_page = c['return'] & ~0xFFF
    if not page <= exit_page < page + size: uc.mem_map(exit_page, 0x1000)
    cpu = 0x10000000
    uc.mem_map(cpu, 0x10000)
    uc.mem_write(cpu + c['regOffset'], struct.pack('<16I', *initial))
    uc.mem_write(cpu + c['cpsrOffset'], struct.pack('<I', c['cpsr']))
    uc.mem_write(cpu + c['cyclesOffset'], struct.pack('<I', 7))
    uc.reg_write(UC_ARM64_REG_X29, cpu)
    uc.reg_write(UC_ARM64_REG_W27, c['cpsr'])
    uc.reg_write(UC_ARM64_REG_W28, 7)
    uc.emu_start(c['base'], c['return'], count=1024)
    actual = list(struct.unpack('<16I', uc.mem_read(cpu + c['regOffset'], 64)))
    actual_flags = uc.reg_read(UC_ARM64_REG_W27)
    actual_cycles = (uc.reg_read(UC_ARM64_REG_W28) - 7) & 0xFFFFFFFF
    returned = uc.reg_read(UC_ARM64_REG_PC) == c['return']
    ok = actual == expected and actual_flags == flags and actual_cycles == cycles and interp_ok and returned
    failures += not ok
    category = f'ARM{7 if num else 9}/' + ('Thumb-MUL' if thumb else ['MUL', 'MLA', 'UMULL', 'UMLAL', 'SMULL', 'SMLAL'][op])
    counts.setdefault(category, [0, 0])
    counts[category][0] += 1
    counts[category][1] += not ok
    changed = [f'r{i}={actual[i]:08X}/{expected[i]:08X}' for i in range(16) if actual[i] != expected[i]]
    lines.append(f"{'PASS' if ok else 'FAIL'} {c['name']}: cycles={actual_cycles}/{cycles} "
                 f"CPSR={actual_flags:08X}/{flags:08X} regs={'OK' if not changed else ','.join(changed)} "
                 f"interpreter={'OK' if interp_ok else 'UNEXPECTED'} returned={returned}")

for category, (checked, failed) in counts.items():
    lines.append(f'{category}: {checked} cases, {failed} failures')
lines.append(f'Production A64 multiply: {len(cases)} cases, {failures} failures; interpreter timing checked')
print('\n'.join(lines if failures else lines[-15:]))
raise SystemExit(bool(failures))
