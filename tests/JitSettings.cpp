// SPDX-License-Identifier: GPL-3.0-or-later
// This isolates argument forwarding, not native JIT emission or execution.
#include <stdio.h>
#include "JitArgs.inc"
;

class ARMJIT
{
public:
    bool LiteralOptimizations;
    bool BranchOptimizations;
    bool FastMemory;
    JITArgs Received;
    void SetJITArgs(JITArgs args) noexcept { Received = args; }
    void SetMaxBlockSize(int size) noexcept;
};

#include "JitMethod.inc"

int main()
{
    int failures = 0;
    for (unsigned flags = 0; flags < 8; flags++)
    {
        ARMJIT jit {};
        jit.LiteralOptimizations = (flags & 1) != 0;
        jit.BranchOptimizations = (flags & 2) != 0;
        jit.FastMemory = (flags & 4) != 0;
        jit.SetMaxBlockSize(16);
        const auto& args = jit.Received;
        if (args.MaxBlockSize != 16 ||
            args.LiteralOptimizations != jit.LiteralOptimizations ||
            args.BranchOptimizations != jit.BranchOptimizations ||
            args.FastMemory != jit.FastMemory)
        {
            fprintf(stderr, "JIT setter changed unrelated flags: %u\n", flags);
            failures++;
        }
    }
    printf("JIT settings cases: %d/8 passed\n", 8 - failures);
    return failures ? 1 : 0;
}
