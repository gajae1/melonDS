// miniaudio 0.11.25, upstream 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d.
// https://github.com/mackron/miniaudio ; distributed under the MIT option in LICENSE.
// Keep the same configuration in the C implementation and its C++ callers.
#pragma once
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#include "miniaudio.h"
