// SPDX-License-Identifier: GPL-3.0-or-later
#include <bit>
#include <stdio.h>

#ifdef _MSVC_LANG
constexpr long LanguageVersion = _MSVC_LANG;
#else
constexpr long LanguageVersion = __cplusplus;
#endif

static_assert(LanguageVersion > 202302L, "The regression suite must compile in C++26 mode");
static_assert(__cpp_lib_bitops >= 201907L, "Standard bit operations are required");
static_assert(std::countr_zero(0ULL) == 64);
static_assert(std::countl_zero(0ULL) == 64);

int main()
{
    printf("C++ language mode: %ld; standard bit operations available\n", LanguageVersion);
}
