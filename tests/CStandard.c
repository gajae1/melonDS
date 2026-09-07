// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L
#error C23 dialect is required (draft mode accepted for older local verification compilers).
#endif
static_assert(sizeof(unsigned char) == 1);
int main(void)
{
    const bool enabled = true;
    typeof_unqual(enabled) copy = enabled;
    copy = false;
    printf("C23 mode: __STDC_VERSION__=%ld\n", (long)__STDC_VERSION__);
    return copy;
}
