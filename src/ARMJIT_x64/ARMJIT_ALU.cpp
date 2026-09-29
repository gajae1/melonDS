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

#include "ARMJIT_Compiler.h"
#include "../ARM.h"
#include "../jit/CPUDetect.h"

using namespace Gen;

namespace melonDS
{

// uses RSCRATCH3 only when op2 is also the destination
void Compiler::Comp_ArithTriOp(void (Compiler::*op)(int, const OpArg&, const OpArg&),
    OpArg rd, OpArg rn, OpArg op2, bool carryUsed, int opFlags)
{
    if (opFlags & opSyncCarry)
    {
        BT(32, R(RCPSR), Imm8(29));
        if (opFlags & opInvertCarry)
            CMC();
    }

    // With no live guest flag from this instruction, ADD and SUB #imm need no
    // host flags: LEA or a folded constant replaces the scratch copy.
    const bool isAdd = op == &Compiler::ADD;
    const bool flaglessLEA = (isAdd || (op == &Compiler::SUB && op2.IsImm()))
        && (!(opFlags & opSetsFlags) || CurInstr.SetFlags == 0);

    if (rd == rn && !(opFlags & opInvertOp2))
        (this->*op)(32, rd, op2);
    else if (flaglessLEA)
    {
        if (op2.IsImm())
        {
            const u32 offset = isAdd ? op2.Imm32() : 0u - op2.Imm32();
            if (rn.IsImm())
                MOV(32, rd, Imm32(rn.Imm32() + offset));
            else
                LEA(32, rd.GetSimpleReg(), MDisp(rn.GetSimpleReg(), (s32)offset));
        }
        else if (rn.IsImm())
            LEA(32, rd.GetSimpleReg(), MDisp(op2.GetSimpleReg(), (s32)rn.Imm32()));
        else
            LEA(32, rd.GetSimpleReg(), MRegSum(rn.GetSimpleReg(), op2.GetSimpleReg()));
    }
    else if (opFlags & opSymmetric && !(opFlags & opInvertOp2) && op2 == rd)
        (this->*op)(32, rd, rn);
    else if (opFlags & opInvertOp2)
    {
        // BIC needs rn & ~op2. A constant operand is inverted at compile time,
        // and a register operand is inverted in rd as long as rd does not still
        // hold a live operand, so neither case needs a scratch copy or write back.
        if (op2.IsImm())
        {
            if (rd != rn)
                MOV(32, rd, rn);
            (this->*op)(32, rd, Imm32(~op2.Imm32()));
        }
        else if (rd != rn && op2 != R(RSCRATCH))
        {
            if (op2 != rd)
                MOV(32, rd, op2);
            NOT(32, rd);
            (this->*op)(32, rd, rn);
        }
        else
        {
            if (op2 != R(RSCRATCH))
            {
                MOV(32, R(RSCRATCH), op2);
                op2 = R(RSCRATCH);
            }
            NOT(32, op2);
            if (rd != rn)
                MOV(32, rd, rn);
            (this->*op)(32, rd, op2);
        }
    }
    else
    {
        // Build the result in rd unless op2 still names it.
        const OpArg dst = op2 == rd ? R(RSCRATCH3) : rd;
        if (dst != rn)
            MOV(32, dst, rn);
        (this->*op)(32, dst, op2);
        if (dst != rd)
            MOV(32, rd, dst);
    }

    if (opFlags & opSetsFlags)
        Comp_RetriveFlags(opFlags & opInvertCarry, opFlags & opRetriveCV, carryUsed);
}

void Compiler::Comp_ArithTriOpReverse(void (Compiler::*op)(int, const Gen::OpArg&, const Gen::OpArg&),
        Gen::OpArg rd, Gen::OpArg rn, Gen::OpArg op2, bool carryUsed, int opFlags)
{
    if (opFlags & opSyncCarry)
    {
        BT(32, R(RCPSR), Imm8(29));
        if (opFlags & opInvertCarry)
            CMC();
    }

    // Reverse subtraction starts from op2. Preserve rn when it is also rd;
    // otherwise compute in rd and avoid copying the temporary result back.
    const OpArg dst = rd == rn ? R(RSCRATCH) : rd;
    if (dst != op2)
        MOV(32, dst, op2);
    (this->*op)(32, dst, rn);
    if (dst != rd)
        MOV(32, rd, dst);

    if (opFlags & opSetsFlags)
        Comp_RetriveFlags(opFlags & opInvertCarry, opFlags & opRetriveCV, carryUsed);
}

void Compiler::Comp_CmpOp(int op, Gen::OpArg rn, Gen::OpArg op2, bool carryUsed)
{
    switch (op)
    {
    case 0: // TST
        if (rn.IsImm())
        {
            MOV(32, R(RSCRATCH3), rn);
            rn = R(RSCRATCH3);
        }
        TEST(32, rn, op2);
    break;
    case 1: // TEQ
        MOV(32, R(RSCRATCH3), rn);
        XOR(32, R(RSCRATCH3), op2);
    break;
    case 2: // CMP
        if (rn.IsImm())
        {
            MOV(32, R(RSCRATCH3), rn);
            rn = R(RSCRATCH3);
        }
        CMP(32, rn, op2);
    break;
    case 3: // CMN
        MOV(32, R(RSCRATCH3), rn);
        ADD(32, R(RSCRATCH3), op2);
    break;
    }

    Comp_RetriveFlags(op == 2, op >= 2, carryUsed);
}

// also calculates cycles
OpArg Compiler::A_Comp_GetALUOp2(bool S, bool& carryUsed)
{
    S = S && (CurInstr.SetFlags & 0x2);

    // Conditional PC writes get a separate failed-condition fetch in CompileBlock.
    // Keep their executed-path fetch out of the shared ConstantCycles total.
    const bool conditionalPCWrite = CurInstr.Cond() < 0xE && (CurInstr.Info.DstRegs & (1 << 15));

    if (CurInstr.Instr & (1 << 25))
    {
        Comp_AddCycles_C(conditionalPCWrite);

        u32 shift = (CurInstr.Instr >> 7) & 0x1E;
        u32 imm = melonDS::ROR(CurInstr.Instr & 0xFF, shift);

        carryUsed = false;
        if (S && shift)
        {
            CPSRDirty = true;
            carryUsed = true;
            if (imm & 0x80000000)
                MOV(32, R(RSCRATCH2), Imm32(1));
            else
                XOR(32, R(RSCRATCH2), R(RSCRATCH2));
        }

        return Imm32(imm);
    }
    else
    {
        int op = (CurInstr.Instr >> 5) & 0x3;
        if (CurInstr.Instr & (1 << 4))
        {
            Comp_AddCycles_CI(1);
            OpArg rm = MapReg(CurInstr.A_Reg(0));
            if (rm.IsImm() && CurInstr.A_Reg(0) == 15)
                rm = Imm32(rm.Imm32() + 4);
            return Comp_RegShiftReg(op, MapReg(CurInstr.A_Reg(8)), rm, S, carryUsed);
        }
        else
        {
            Comp_AddCycles_C(conditionalPCWrite);
            return Comp_RegShiftImm(op, (CurInstr.Instr >> 7) & 0x1F,
                    MapReg(CurInstr.A_Reg(0)), S, carryUsed);
        }
    }
}

void Compiler::A_Comp_CmpOp()
{
    u32 op = (CurInstr.Instr >> 21) & 0xF;

    bool carryUsed;
    OpArg rn = MapReg(CurInstr.A_Reg(16));
    OpArg op2 = A_Comp_GetALUOp2((1 << op) & 0xF303, carryUsed);

    Comp_CmpOp(op - 0x8, rn, op2, carryUsed);
}

void Compiler::A_Comp_Arith()
{
    bool S = CurInstr.Instr & (1 << 20);
    u32 op = (CurInstr.Instr >> 21) & 0xF;

    bool carryUsed;
    OpArg rn = MapReg(CurInstr.A_Reg(16));
    OpArg rd = MapReg(CurInstr.A_Reg(12));
    OpArg op2 = A_Comp_GetALUOp2(S && (1 << op) & 0xF303, carryUsed);

    u32 sFlag = S ? opSetsFlags : 0;
    switch (op)
    {
    case 0x0: // AND
        Comp_ArithTriOp(&Compiler::AND, rd, rn, op2, carryUsed, opSymmetric|sFlag);
        break;
    case 0x1: // EOR
        Comp_ArithTriOp(&Compiler::XOR, rd, rn, op2, carryUsed, opSymmetric|sFlag);
        break;
    case 0x2: // SUB
        Comp_ArithTriOp(&Compiler::SUB, rd, rn, op2, carryUsed, sFlag|opRetriveCV|opInvertCarry);
        break;
    case 0x3: // RSB
        if (op2.IsZero())
        {
            if (rd != rn)
                MOV(32, rd, rn);
            NEG(32, rd);
            if (S)
                Comp_RetriveFlags(true, true, false);
        }
        else
            Comp_ArithTriOpReverse(&Compiler::SUB, rd, rn, op2, carryUsed, sFlag|opRetriveCV|opInvertCarry);
        break;
    case 0x4: // ADD
        Comp_ArithTriOp(&Compiler::ADD, rd, rn, op2, carryUsed, opSymmetric|sFlag|opRetriveCV);
        break;
    case 0x5: // ADC
        Comp_ArithTriOp(&Compiler::ADC, rd, rn, op2, carryUsed, opSymmetric|sFlag|opRetriveCV|opSyncCarry);
        break;
    case 0x6: // SBC
        Comp_ArithTriOp(&Compiler::SBB, rd, rn, op2, carryUsed, sFlag|opRetriveCV|opSyncCarry|opInvertCarry);
        break;
    case 0x7: // RSC
        Comp_ArithTriOpReverse(&Compiler::SBB, rd, rn, op2, carryUsed, sFlag|opRetriveCV|opInvertCarry|opSyncCarry);
        break;
    case 0xC: // ORR
        Comp_ArithTriOp(&Compiler::OR, rd, rn, op2, carryUsed, opSymmetric|sFlag);
        break;
    case 0xE: // BIC
        Comp_ArithTriOp(&Compiler::AND, rd, rn, op2, carryUsed, sFlag|opSymmetric|opInvertOp2);
        break;
    default:
        Log(LogLevel::Error, "this is a JIT bug! %04x\n", op);
        abort();
    }

    if (CurInstr.A_Reg(12) == 15)
        Comp_JumpTo(rd.GetSimpleReg(), S);
}

void Compiler::A_Comp_MovOp()
{
    bool carryUsed;
    bool S = CurInstr.Instr & (1 << 20);
    OpArg op2 = A_Comp_GetALUOp2(S, carryUsed);
    OpArg rd = MapReg(CurInstr.A_Reg(12));

    if (rd != op2)
        MOV(32, rd, op2);

    if (((CurInstr.Instr >> 21) & 0xF) == 0xF)
    {
        NOT(32, rd);
        if (op2.IsImm() && CurInstr.Cond() == 0xE)
            RegCache.PutLiteral(CurInstr.A_Reg(12), ~op2.Imm32());
    }
    else if (op2.IsImm() && CurInstr.Cond() == 0xE)
            RegCache.PutLiteral(CurInstr.A_Reg(12), op2.Imm32());

    if (S)
    {
        if (FlagsNZRequired())
            TEST(32, rd, rd);
        Comp_RetriveFlags(false, false, carryUsed);
    }

    if (CurInstr.A_Reg(12) == 15)
        Comp_JumpTo(rd.GetSimpleReg(), S);
}

void Compiler::A_Comp_CLZ()
{
    Comp_AddCycles_C();
    OpArg rd = MapReg(CurInstr.A_Reg(12));
    OpArg rm = MapReg(CurInstr.A_Reg(0));

    if (cpu_info.bLZCNT)
        LZCNT(32, RSCRATCH, rm); // Includes the defined result of 32 for zero.
    else
    {
        MOV(32, R(RSCRATCH), Imm32(32));
        TEST(32, rm, rm);
        FixupBranch skipZero = J_CC(CC_Z);
        BSR(32, RSCRATCH, rm);
        XOR(32, R(RSCRATCH), Imm8(0x1F)); // 31 - RSCRATCH
        SetJumpTarget(skipZero);
    }
    MOV(32, rd, R(RSCRATCH));
}

void Compiler::Comp_MulOp(bool S, bool add, Gen::OpArg rd, Gen::OpArg rm, Gen::OpArg rs, Gen::OpArg rn)
{
    if (Num == 0)
        Comp_AddCycles_CI(S ? 3 : 1);
    else
    {
        XOR(32, R(RSCRATCH), R(RSCRATCH));
        MOV(32, R(RSCRATCH3), rs);
        TEST(32, R(RSCRATCH3), R(RSCRATCH3));
        FixupBranch zeroBSR = J_CC(CC_Z);
        BSR(32, RSCRATCH2, R(RSCRATCH3));
        NOT(32, R(RSCRATCH3));
        BSR(32, RSCRATCH, R(RSCRATCH3));
        CMP(32, R(RSCRATCH2), R(RSCRATCH));
        CMOVcc(32, RSCRATCH, R(RSCRATCH2), CC_L);
        SHR(32, R(RSCRATCH), Imm8(3));
        SetJumpTarget(zeroBSR); // fortunately that's even right
        // Include all I cycles in the register: the CI helper's constant path
        // only adds the fetch cost, unlike its conditional path.
        ADD(32, R(RSCRATCH), Imm8(add ? 2 : 1));
        Comp_AddCycles_CI(RSCRATCH, 0);
    }

    if (add && rd == rn)
    {
        // The accumulator must survive until the product has been computed.
        MOV(32, R(RSCRATCH), rm);
        IMUL(32, RSCRATCH, rs);
        LEA(32, rd.GetSimpleReg(), MRegSum(RSCRATCH, rn.GetSimpleReg()));
    }
    else
    {
        // Multiplication is commutative: preserve Rs when it already occupies Rd.
        if (rd == rs)
            IMUL(32, rd.GetSimpleReg(), rm);
        else
        {
            if (rd != rm)
                MOV(32, rd, rm);
            IMUL(32, rd.GetSimpleReg(), rs);
        }
        if (add)
            LEA(32, rd.GetSimpleReg(), MRegSum(rd.GetSimpleReg(), rn.GetSimpleReg()));
    }

    if (S)
    {
        if (FlagsNZRequired())
            TEST(32, rd, rd);
        Comp_RetriveFlags(false, false, false);
    }
}

void Compiler::A_Comp_MUL_MLA()
{
    bool S = CurInstr.Instr & (1 << 20);
    bool add = CurInstr.Instr & (1 << 21);
    OpArg rd = MapReg(CurInstr.A_Reg(16));
    OpArg rm = MapReg(CurInstr.A_Reg(0));
    OpArg rs = MapReg(CurInstr.A_Reg(8));
    OpArg rn;
    if (add)
        rn = MapReg(CurInstr.A_Reg(12));

    Comp_MulOp(S, add, rd, rm, rs, rn);
}

void Compiler::A_Comp_Mul_Long()
{
    bool S = CurInstr.Instr & (1 << 20);
    bool add = CurInstr.Instr & (1 << 21);
    bool sign = CurInstr.Instr & (1 << 22);
    OpArg rd = MapReg(CurInstr.A_Reg(16));
    OpArg rm = MapReg(CurInstr.A_Reg(0));
    OpArg rs = MapReg(CurInstr.A_Reg(8));
    OpArg rn = MapReg(CurInstr.A_Reg(12));

    if (Num == 0)
        Comp_AddCycles_CI(S ? 3 : 1);
    else
    {
        XOR(32, R(RSCRATCH), R(RSCRATCH));
        MOV(32, R(RSCRATCH3), rs);
        TEST(32, R(RSCRATCH3), R(RSCRATCH3));
        FixupBranch zeroBSR = J_CC(CC_Z);
        if (sign)
        {
            BSR(32, RSCRATCH2, R(RSCRATCH3));
            NOT(32, R(RSCRATCH3));
            BSR(32, RSCRATCH, R(RSCRATCH3));
            CMP(32, R(RSCRATCH2), R(RSCRATCH));
            CMOVcc(32, RSCRATCH, R(RSCRATCH2), CC_L);
        }
        else
        {
            BSR(32, RSCRATCH, R(RSCRATCH3));
        }

        SHR(32, R(RSCRATCH), Imm8(3));
        SetJumpTarget(zeroBSR); // fortunately that's even right
        ADD(32, R(RSCRATCH), Imm8(add ? 3 : 2)); // MULL m+1 I; MLAL m+2 I.
        Comp_AddCycles_CI(RSCRATCH, 0);
    }

    if (sign)
    {
        MOVSX(64, 32, RSCRATCH2, rm);
        MOVSX(64, 32, RSCRATCH3, rs);
    }
    else
    {
        MOV(32, R(RSCRATCH2), rm);
        MOV(32, R(RSCRATCH3), rs);
    }
    if (add)
    {
        MOV(32, R(RSCRATCH), rd);
        SHL(64, R(RSCRATCH), Imm8(32));
        OR(64, R(RSCRATCH), rn);

        IMUL(64, RSCRATCH2, R(RSCRATCH3));
        ADD(64, R(RSCRATCH2), R(RSCRATCH));
    }
    else
    {
        IMUL(64, RSCRATCH2, R(RSCRATCH3));
        if (S && FlagsNZRequired())
            TEST(64, R(RSCRATCH2), R(RSCRATCH2));
    }

    if (S)
        Comp_RetriveFlags(false, false, false);

    MOV(32, rn, R(RSCRATCH2));
    SHR(64, R(RSCRATCH2), Imm8(32));
    MOV(32, rd, R(RSCRATCH2));
}

void Compiler::Comp_RetriveFlags(bool sign, bool retriveCV, bool carryUsed)
{
    if (CurInstr.SetFlags == 0)
        return;
    if (retriveCV && !(CurInstr.SetFlags & 0x3))
        retriveCV = false;

    bool carryOnly = !retriveCV && carryUsed;
    if (carryOnly && !(CurInstr.SetFlags & 0x2))
    {
        carryUsed = false;
        carryOnly = false;
    }

    CPSRDirty = true;

    if (retriveCV)
    {
        SETcc(CC_O, R(RSCRATCH));
        SETcc(sign ? CC_NC : CC_C, R(RSCRATCH3));
        LEA(32, RSCRATCH2, MComplex(RSCRATCH, RSCRATCH3, SCALE_2, 0));
    }

    if (FlagsNZRequired())
    {
        SETcc(CC_S, R(RSCRATCH));
        SETcc(CC_Z, R(RSCRATCH3));
        LEA(32, RSCRATCH, MComplex(RSCRATCH3, RSCRATCH, SCALE_2, 0));
        int shiftAmount = 30;
        if (retriveCV || carryUsed)
        {
            LEA(32, RSCRATCH, MComplex(RSCRATCH2, RSCRATCH, carryOnly ? SCALE_2 : SCALE_4, 0));
            shiftAmount = carryOnly ? 29 : 28;
        }
        SHL(32, R(RSCRATCH), Imm8(shiftAmount));

        AND(32, R(RCPSR), Imm32(0x3FFFFFFF & ~(carryUsed << 29) & ~((retriveCV ? 3 : 0) << 28)));
        OR(32, R(RCPSR), R(RSCRATCH));
    }
    else if (carryUsed || retriveCV)
    {
        SHL(32, R(RSCRATCH2), Imm8(carryOnly ? 29 : 28));
        AND(32, R(RCPSR), Imm32(0xFFFFFFFF & ~(carryUsed << 29) & ~((retriveCV ? 3 : 0) << 28)));
        OR(32, R(RCPSR), R(RSCRATCH2));
    }
}

// Result defaults to RSCRATCH; RSCRATCH2 retains carry when requested.
OpArg Compiler::Comp_RegShiftReg(int op, Gen::OpArg rs, Gen::OpArg rm, bool S, bool& carryUsed, OpArg result)
{
    S = S && (CurInstr.SetFlags & 0x2);
    carryUsed = S;

    if (S)
    {
        XOR(32, R(RSCRATCH2), R(RSCRATCH2));
        TEST(32, R(RCPSR), Imm32(1 << 29));
        SETcc(CC_NZ, R(RSCRATCH2));
    }

    static_assert(RSCRATCH3 == ECX, "Someone changed RSCRATCH3");
    // Save the old count before shifting a Thumb destination that may be Rs.
    MOV(32, R(ECX), rs);
    if (result != rm) MOV(32, result, rm);
    // ROR masks the count itself; only the low byte matters for zero.
    if (op == 3)
        TEST(8, R(ECX), R(ECX));
    else
        AND(32, R(ECX), Imm32(0xFF));

    FixupBranch zero = J_CC(CC_Z);
    if (op < 3)
    {
        void (Compiler::*shiftOp)(int, const OpArg&, const OpArg&) = NULL;
        if (op == 0)
            shiftOp = &Compiler::SHL;
        else if (op == 1)
            shiftOp = &Compiler::SHR;
        else if (op == 2)
            shiftOp = &Compiler::SAR;

        CMP(32, R(ECX), Imm8(32));
        FixupBranch lt32 = J_CC(CC_L);
        FixupBranch done1;
        if (op < 2)
        {
            FixupBranch eq32 = J_CC(CC_E);
            XOR(32, result, result);
            if (S)
                XOR(32, R(RSCRATCH2), R(RSCRATCH2));
            done1 = J();
            SetJumpTarget(eq32);
        }
        (this->*shiftOp)(32, result, Imm8(31));
        (this->*shiftOp)(32, result, Imm8(1));
        if (S)
            SETcc(CC_C, R(RSCRATCH2));

        FixupBranch done2 = J();

        SetJumpTarget(lt32);
        (this->*shiftOp)(32, result, R(ECX));
        if (S)
            SETcc(CC_C, R(RSCRATCH2));

        if (op < 2)
            SetJumpTarget(done1);
        SetJumpTarget(done2);

    }
    else if (op == 3)
    {
        if (S)
            BT(32, result, Imm8(31));
        ROR(32, result, R(ECX));
        if (S)
            SETcc(CC_C, R(RSCRATCH2));
    }
    SetJumpTarget(zero);

    return result;
}

// Result defaults to RSCRATCH; Thumb may supply its mapped destination.
// RSCRATCH2 still retains the shifter carry before NZ flags are collected.
OpArg Compiler::Comp_RegShiftImm(int op, int amount, OpArg rm, bool S, bool& carryUsed, OpArg result)
{
    carryUsed = true;

    switch (op)
    {
    case 0: // LSL
        if (amount > 0)
        {
            if (result != rm) MOV(32, result, rm);
            SHL(32, result, Imm8(amount));
            if (S)
                SETcc(CC_C, R(RSCRATCH2));

            return result;
        }
        else
        {
            carryUsed = false;
            return rm;
        }
    case 1: // LSR
        if (amount > 0)
        {
            if (result != rm) MOV(32, result, rm);
            SHR(32, result, Imm8(amount));
            if (S)
                SETcc(CC_C, R(RSCRATCH2));
            return result;
        }
        else
        {
            if (S)
            {
                MOV(32, R(RSCRATCH2), rm);
                SHR(32, R(RSCRATCH2), Imm8(31));
            }
            return Imm32(0);
        }
    case 2: // ASR
        if (result != rm) MOV(32, result, rm);
        SAR(32, result, Imm8(amount ? amount : 31));
        if (S)
        {
            if (amount == 0)
                // ASR #32 leaves every result bit equal to the source sign.
                // rm can be an immediate PC value, which BT cannot encode.
                BT(32, result, Imm8(31));
            SETcc(CC_C, R(RSCRATCH2));
        }
        return result;
    case 3: // ROR
        if (result != rm) MOV(32, result, rm);
        if (amount > 0)
            ROR(32, result, Imm8(amount));
        else
        {
            BT(32, R(RCPSR), Imm8(29));
            RCR(32, result, Imm8(1));
        }
        if (S)
            SETcc(CC_C, R(RSCRATCH2));
        return result;
    }

    abort();
}

void Compiler::T_Comp_ShiftImm()
{
    OpArg rd = MapReg(CurInstr.T_Reg(0));
    OpArg rs = MapReg(CurInstr.T_Reg(3));

    int op = (CurInstr.Instr >> 11) & 0x3;
    int amount = (CurInstr.Instr >> 6) & 0x1F;

    Comp_AddCycles_C();

    bool carryUsed;
    OpArg shifted = Comp_RegShiftImm(op, amount, rs, true, carryUsed, rd);

    if (shifted != rd)
        MOV(32, rd, shifted);

    if (FlagsNZRequired())
        TEST(32, rd, rd);
    Comp_RetriveFlags(false, false, carryUsed);
}

void Compiler::T_Comp_AddSub_()
{
    OpArg rd = MapReg(CurInstr.T_Reg(0));
    OpArg rs = MapReg(CurInstr.T_Reg(3));

    int op = (CurInstr.Instr >> 9) & 0x3;

    OpArg rn = op >= 2 ? Imm32((CurInstr.Instr >> 6) & 0x7) : MapReg(CurInstr.T_Reg(6));

    Comp_AddCycles_C();

    // special case for thumb mov being alias to add rd, rn, #0
    if (CurInstr.SetFlags == 0 && rn.IsImm() && rn.Imm32() == 0)
    {
        if (rd != rs)
            MOV(32, rd, rs);
    }
    else if (op & 1)
        Comp_ArithTriOp(&Compiler::SUB, rd, rs, rn, false, opSetsFlags|opInvertCarry|opRetriveCV);
    else
        Comp_ArithTriOp(&Compiler::ADD, rd, rs, rn, false, opSetsFlags|opSymmetric|opRetriveCV);
}

void Compiler::T_Comp_ALU_Imm8()
{
    OpArg rd = MapReg(CurInstr.T_Reg(8));

    u32 op = (CurInstr.Instr >> 11) & 0x3;
    OpArg imm = Imm32(CurInstr.Instr & 0xFF);

    Comp_AddCycles_C();

    switch (op)
    {
    case 0x0:
        MOV(32, rd, imm);
        if (FlagsNZRequired())
            TEST(32, rd, rd);
        Comp_RetriveFlags(false, false, false);
        return;
    case 0x1:
        Comp_CmpOp(2, rd, imm, false);
        return;
    case 0x2:
        Comp_ArithTriOp(&Compiler::ADD, rd, rd, imm, false, opSetsFlags|opSymmetric|opRetriveCV);
        return;
    case 0x3:
        Comp_ArithTriOp(&Compiler::SUB, rd, rd, imm, false, opSetsFlags|opInvertCarry|opRetriveCV);
        return;
    }
}

void Compiler::T_Comp_MUL()
{
    OpArg rd = MapReg(CurInstr.T_Reg(0));
    OpArg rs = MapReg(CurInstr.T_Reg(3));
    // Thumb MUL Rd,Rs is ARM MULS Rd,Rs,Rd: old Rd controls timing.
    Comp_MulOp(true, false, rd, rs, rd, Imm8(-1));
}

void Compiler::T_Comp_ALU()
{
    OpArg rd = MapReg(CurInstr.T_Reg(0));
    OpArg rs = MapReg(CurInstr.T_Reg(3));

    u32 op = (CurInstr.Instr >> 6) & 0xF;

    if ((op >= 0x2 && op <= 0x4) || op == 0x7)
        Comp_AddCycles_CI(1); // shift by reg
    else
        Comp_AddCycles_C();

    switch (op)
    {
    case 0x0: // AND
        Comp_ArithTriOp(&Compiler::AND, rd, rd, rs, false, opSetsFlags|opSymmetric);
        return;
    case 0x1: // EOR
        Comp_ArithTriOp(&Compiler::XOR, rd, rd, rs, false, opSetsFlags|opSymmetric);
        return;
    case 0x2:
    case 0x3:
    case 0x4:
    case 0x7:
        {
            int shiftOp = op == 0x7 ? 3 : op - 0x2;
            bool carryUsed;
            OpArg shifted = Comp_RegShiftReg(shiftOp, rs, rd, true, carryUsed, rd);
            if (FlagsNZRequired())
                TEST(32, shifted, shifted);
            if (rd != shifted) MOV(32, rd, shifted);
            Comp_RetriveFlags(false, false, true);
        }
        return;
    case 0x5: // ADC
        Comp_ArithTriOp(&Compiler::ADC, rd, rd, rs, false, opSetsFlags|opSymmetric|opSyncCarry|opRetriveCV);
        return;
    case 0x6: // SBC
        Comp_ArithTriOp(&Compiler::SBB, rd, rd, rs, false, opSetsFlags|opSyncCarry|opInvertCarry|opRetriveCV);
        return;
    case 0x8: // TST
        Comp_CmpOp(0, rd, rs, false);
        return;
    case 0x9: // NEG
        if (rd != rs)
            MOV(32, rd, rs);
        NEG(32, rd);
        Comp_RetriveFlags(true, true, false);
        return;
    case 0xA: // CMP
        Comp_CmpOp(2, rd, rs, false);
        return;
    case 0xB: // CMN
        Comp_CmpOp(3, rd, rs, false);
        return;
    case 0xC: // ORR
        Comp_ArithTriOp(&Compiler::OR, rd, rd, rs, false, opSetsFlags|opSymmetric);
        return;
    case 0xE: // BIC
        Comp_ArithTriOp(&Compiler::AND, rd, rd, rs, false, opSetsFlags|opSymmetric|opInvertOp2);
        return;
    case 0xF: // MVN
        if (rd != rs)
            MOV(32, rd, rs);
        NOT(32, rd);
        Comp_RetriveFlags(false, false, false);
        return;
    default:
        break;
    }
}

void Compiler::T_Comp_ALU_HiReg()
{
    u32 rd = ((CurInstr.Instr & 0x7) | ((CurInstr.Instr >> 4) & 0x8));
    OpArg rdMapped = MapReg(rd);
    OpArg rs = MapReg((CurInstr.Instr >> 3) & 0xF);

    u32 op = (CurInstr.Instr >> 8) & 0x3;

    Comp_AddCycles_C();

    switch (op)
    {
    case 0x0: // ADD
        Comp_ArithTriOp(&Compiler::ADD, rdMapped, rdMapped, rs, false, opSymmetric);
        break;
    case 0x1: // CMP
        Comp_CmpOp(2, rdMapped, rs, false);
        return; // this is on purpose
    case 0x2: // MOV
        if (rdMapped != rs)
            MOV(32, rdMapped, rs);
        break;
    }

    if (rd == 15)
    {
        OR(32, rdMapped, Imm8(1));
        Comp_JumpTo(rdMapped.GetSimpleReg());
    }
}

void Compiler::T_Comp_AddSP()
{
    Comp_AddCycles_C();

    OpArg sp = MapReg(13);
    OpArg offset = Imm32((CurInstr.Instr & 0x7F) << 2);
    if (CurInstr.Instr & (1 << 7))
        SUB(32, sp, offset);
    else
        ADD(32, sp, offset);
}

void Compiler::T_Comp_RelAddr()
{
    Comp_AddCycles_C();

    OpArg rd = MapReg(CurInstr.T_Reg(8));
    u32 offset = (CurInstr.Instr & 0xFF) << 2;
    if (CurInstr.Instr & (1 << 11))
    {
        OpArg sp = MapReg(13);
        LEA(32, rd.GetSimpleReg(), MDisp(sp.GetSimpleReg(), offset));
    }
    else
        MOV(32, rd, Imm32((R15 & ~2) + offset));
}

}
