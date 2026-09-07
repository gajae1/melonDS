// SPDX-License-Identifier: GPL-3.0-or-later
#include "NDS_Header.h"
#include <cstdio>
#include <initializer_list>
using namespace melonDS;
#include "ROMHeaderMethod.inc"
int main()
{
    unsigned cases = 0, failures = 0;
    for (u32 length : {0x200U, 0x400U, 0x1000U, 0xFFFFFFFFU})
        for (u32 offset : {0U, 0x1FFU, 0x200U, 0x400U, 0xF0000200U, 0xFFFFFFFFU})
            for (u32 size : {0U, 1U, 0x100U, 0x10000000U, 0xFFFFFFFFU})
                for (bool arm9 : {false, true})
                {
                    NDSHeader header{};
                    header.ARM9ROMOffset = header.ARM7ROMOffset = 0x200;
                    (arm9 ? header.ARM9ROMOffset : header.ARM7ROMOffset) = offset;
                    (arm9 ? header.ARM9Size : header.ARM7Size) = size;
                    const bool valid = offset >= 0x200 && static_cast<u64>(offset) + size <= length;
                    if (ValidateROM(length, header) != valid) ++failures;
                    ++cases;
                }
    std::printf("NDS header bounds: %u cases, %u failures\n", cases, failures);
    return failures ? 1 : 0;
}
