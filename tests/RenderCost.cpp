// SPDX-License-Identifier: GPL-3.0-or-later
// Production timer ownership with non-index GL names and delayed GPU results.
#include "PlatformOGL.h"
#include "RenderCost.h"
#include <cstdio>
#include <map>
#include <set>

namespace
{
std::set<GLuint> Names;
std::map<GLuint, GLuint64> Pending;
bool Ready = false, Error = false;
unsigned Calls = 0, Results = 0;
GLuint64 Clock = 0;
void APIENTRY Generate(GLsizei n, GLuint* names)
{
    ++Calls;
    for (int i = 0; i < n; ++i) Names.insert(names[i] = (i + 1) * 7);
}
void APIENTRY Delete(GLsizei n, const GLuint* names)
{
    ++Calls;
    for (int i = 0; i < n; ++i) { Names.erase(names[i]); Pending.erase(names[i]); }
}
void APIENTRY Counter(GLuint query, GLenum target)
{
    ++Calls;
    Error |= target != GL_TIMESTAMP || !Names.contains(query) || Pending.contains(query);
    Pending[query] = Clock += 100;
}
void APIENTRY Bits(GLenum, GLenum, GLint* result) { ++Calls; *result = 64; }
void APIENTRY Available(GLuint query, GLenum, GLuint* result)
{
    ++Calls;
    Error |= !Names.contains(query);
    *result = Ready;
}
void APIENTRY Result(GLuint query, GLenum, GLuint64* result)
{
    ++Calls;
    ++Results;
    Error |= !Ready || !Names.contains(query) || !Pending.contains(query);
    *result = Pending[query];
    Pending.erase(query);
}
}

int main()
{
    GLAD_GL_VERSION_3_3 = 1;
    glad_glGenQueries = Generate;
    glad_glDeleteQueries = Delete;
    glad_glQueryCounter = Counter;
    glad_glGetQueryiv = Bits;
    glad_glGetQueryObjectuiv = Available;
    glad_glGetQueryObjectui64v = Result;
    melonDS::RenderCostGpuSpans gpu;
    melonDS::RenderCostStage stage, nested;
    gpu.End(gpu.Begin(stage)); gpu.Poll();
    if (Calls) return 1;
    gpu.Enabled = true;
    for (int i = 0; i < 3; ++i)
    {
        Ready = false;
        gpu.End(gpu.Begin(stage)); gpu.Poll();
        if (Results != unsigned(i * 2)) return 2;
        Ready = true;
        gpu.Poll();
        if (Error || stage.Count != i + 1 || stage.Samples[i] != 100) return 3;
    }
    // Nested spans own their own timestamp pairs. No active query target can
    // accidentally be ended by a skipped or nested instrumentation boundary.
    const int outer = gpu.Begin(stage), inner = gpu.Begin(nested);
    gpu.End(inner); gpu.End(outer); gpu.Poll();
    if (stage.Samples[3] != 300 || nested.Samples[0] != 100 || Error) return 4;
    Ready = false;
    for (int i = 0; i < melonDS::RenderCostGpuSpans::MaxPending; ++i)
        gpu.End(gpu.Begin(stage));
    const unsigned calls = Calls, results = Results;
    gpu.End(gpu.Begin(stage));
    if (Calls != calls || stage.IntervalDropped != 1) return 5;
    gpu.Poll();
    if (Results != results || Error) return 6;
    Ready = true; gpu.Poll();
    if (Results != results + 2 * melonDS::RenderCostGpuSpans::MaxPending || Error) return 7;
    gpu.Shutdown();
    if (!Names.empty() || !Pending.empty()) return 8;
    GLAD_GL_VERSION_3_3 = 0;
    const unsigned before = Calls;
    gpu.End(gpu.Begin(stage)); gpu.Poll();
    if (Calls != before || std::strcmp(gpu.State(), "unsup")) return 9;
    gpu.Shutdown(); GLAD_GL_VERSION_3_3 = 1;
    gpu.End(gpu.Begin(stage)); gpu.Poll(); gpu.Shutdown();
    if (Error || !Names.empty()) return 10;
    std::puts("timer disabled/reuse/nesting/exhaustion/async/recreation: PASS");
}
