// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MELONDS_PIXEL_CONVERT_H
#define MELONDS_PIXEL_CONVERT_H
#include <stddef.h>
#include "types.h"
namespace melonDS::PixelConvert
{
enum class Backend { Auto, Scalar, AVX2, AVX512 };
using Function = void (*)(u32* pixels, size_t count) noexcept;
// Input needs u32 alignment, not SIMD alignment. Conversion is in-place.
void ExpandScalar(u32* pixels, size_t count) noexcept;
bool IsSupported(Backend backend) noexcept;
// Unsupported explicit requests fall back to Scalar, never to an illegal ISA.
Function Select(Backend backend = Backend::Auto) noexcept;
}
#endif
