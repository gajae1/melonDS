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

#ifndef MELONDS_UTILS_H
#define MELONDS_UTILS_H

#include <bit>
#include <memory>
#include "types.h"
#include <utility>

namespace melonDS
{
/// Ensures that the given array is a power of 2 in length.
///
/// @return If \c len is a power of 2, returns \c data and \c len unchanged
/// without copying anything.
/// If \c data is \c nullptr, returns <tt>{nullptr, 0}</tt>.
/// Otherwise, return a copy of \c data with zero-padding to the next power of 2.
/// Null/empty inputs or lengths above 2^31 return {nullptr, 0}.
/// Rejected inputs do not consume the owning pointer. On success it is consumed,
/// including when the buffer is already a power of two.
std::pair<std::unique_ptr<u8[]>, u32> PadToPowerOf2(std::unique_ptr<u8[]>&& data, u32 len) noexcept;

std::pair<std::unique_ptr<u8[]>, u32> PadToPowerOf2(const u8* data, u32 len) noexcept;

std::unique_ptr<u8[]> CopyToUnique(const u8* data, u32 len) noexcept;

template <typename T>
constexpr T GetMSBit(T val) noexcept
{
    return std::bit_floor(val);
}

// convenience function for updating part of a register
template <typename T>
void UpdateRegister(T& reg, T val, T mask)
{
    reg = (reg & ~mask) | (val & mask);
}

}

#endif // MELONDS_UTILS_H
