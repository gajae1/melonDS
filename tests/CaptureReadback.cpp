// SPDX-License-Identifier: GPL-3.0-or-later
// Compile the production method with a recording GL boundary. This tests byte
// ranges and dirty flags, NOT OpenGL drivers, shader output, or game timing.
#include <array>
#include <stdio.h>
#include <string.h>
#include "types.h"

using namespace melonDS;

enum LogLevel { Error };
static void Log(LogLevel, const char*, ...) {}
enum { GL_DITHER, GL_READ_FRAMEBUFFER, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV };
static void glDisable(int) {}
static void glBindFramebuffer(int, int) {}
static unsigned BytesWritten;
static void glReadPixels(int, int, int width, int height, int, int, void* dst)
{
    unsigned size = static_cast<unsigned>(width * height * 2);
    memset(dst, 0xA5, size);
    BytesWritten += size;
}

struct GPUFixture
{
    u8 VRAM[4][128 * 1024] {};
    bool VRAMDirty[4][256] {};
};

class GLRenderer
{
public:
    GPUFixture& GPU;
    int CaptureSyncFB = 0;
    explicit GLRenderer(GPUFixture& gpu) : GPU(gpu) {}
    void DownscaleCapture(int, int, int) {}
    void SyncVRAMCapture(u32 bank, u32 start, u32 len, bool complete);
};

#include "CaptureMethod.inc"

int main()
{
    int failures = 0;
    for (u32 start = 0; start < 4; start++)
    {
        for (u32 len = 0; len < 4; len++)
        {
            GPUFixture gpu;
            GLRenderer renderer(gpu);
            BytesWritten = 0;
            renderer.SyncVRAMCapture(2, start, len, true);
            unsigned blocks = len == 0 ? 1 : len;
            bool okay = BytesWritten == blocks * 32768;
            for (u32 bank = 0; bank < 4; bank++)
            {
                for (u32 offset = 0; offset < 128 * 1024; offset++)
                {
                    bool expected = bank == 2 &&
                        ((offset / 32768 + 4 - start) % 4) < blocks;
                    if (gpu.VRAM[bank][offset] != (expected ? 0xA5 : 0) ||
                        gpu.VRAMDirty[bank][offset / 512] != expected)
                        okay = false;
                }
            }
            if (!okay)
            {
                fprintf(stderr, "Capture start=%u len=%u wrote %u bytes, expected %u\n",
                        start, len, BytesWritten, blocks * 32768);
                failures++;
            }
        }
    }
    printf("Capture range cases: %d/16 passed\n", 16 - failures);
    return failures ? 1 : 0;
}
