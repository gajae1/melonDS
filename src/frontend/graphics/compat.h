#ifndef GRAPHICS_COMPAT_H
#define GRAPHICS_COMPAT_H

#include "../../types.h"

#include <assert.h>

#define ALWAYS_INLINE __attribute__((always_inline)) inline

#define AssertMsg(cond, msg) assert(cond && msg)
#define Assert(cond) assert(cond)

#define Panic(msg) assert(false && msg)

#define UnreachableCode() __builtin_unreachable()

#endif