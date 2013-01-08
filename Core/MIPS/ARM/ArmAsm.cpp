// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ArmEmitter.h"
#include "ArmABI.h"

#include "../../MemMap.h"

#include "../MIPS.h"
#include "../../CoreTiming.h"
#include "MemoryUtil.h"

#include "ArmJit.h"
#include "../JitCommon/JitCommon.h"
#include "../../Core.h"
#include "ArmAsm.h"

using namespace ArmGen;

//static int temp32; // unused?

//TODO - make an option
//#if _DEBUG
static bool enableDebug = false; 
//#else
//		bool enableDebug = false; 
//#endif

//static bool enableStatistics = false; //unused?

//The standard ARM calling convention allocates the 16 ARM registers as:

// r15 is the program counter.
// r14 is the link register. (The BL instruction, used in a subroutine call, stores the return address in this register).
// r13 is the stack pointer. (The Push/Pop instructions in "Thumb" operating mode use this register only).
// r12 is the Intra-Procedure-call scratch register.
// r4 to r11: used to hold local variables.
// r0 to r3: used to hold argument values passed to a subroutine, and also hold results returned from a subroutine.

//	 R2, R3, R4, R5, R6, R7, R8, R10, R11	 // omitting R9?


// STATIC ALLOCATION ARM:
// R11 : Memory base pointer.
// R10 : MIPS state
// R9 : 
extern volatile CoreState coreState;

void Jit()
{
	MIPSComp::jit->Compile(currentMIPS->pc);
}

void ShowPC(u32 sp) {
	if (currentMIPS) {
		WARN_LOG(HLE, "PC : %08x  ArmSP : %08x", currentMIPS->pc, sp);
	} else {
		ERROR_LOG(HLE, "Universe corrupt?");
	}
}

void DisassembleArm(const u8 *data, int size);

// PLAN: no more block numbers - crazy opcodes just contain offset within
// dynarec buffer
// At this offset - 4, there is an int specifying the block number.

void ArmAsmRoutineManager::QuickCallFunction(ARMReg reg, void *func) {
	PUSH(1, _LR);
	ARMABI_MOVI2R(reg, (u32)(func));
	BL(reg);
	POP(1, _LR);
}

void ArmAsmRoutineManager::Generate(MIPSState *mips, MIPSComp::Jit *jit)
{
	enterCode = AlignCode16();

	INFO_LOG(HLE, "Base: %08x", (u32)Memory::base);

	SetCC(CC_AL);

	PUSH(9, R4, R5, R6, R7, R8, R9, R10, R11, _LR);

	// Fixed registers, these are always kept when in Jit context.
	// R13 cannot be used as it's the stack pointer.
	ARMABI_MOVI2R(R11, (u32)Memory::base);
	ARMABI_MOVI2R(R10, (u32)mips);
	ARMABI_MOVI2R(R7, (u32)jit->GetBlockCache()->GetCodePointers());

	outerLoop = GetCodePtr();
		QuickCallFunction(R0, (void *)&CoreTiming::Advance);
		FixupBranch skipToRealDispatch = B(); //skip the sync and compare first time

		dispatcherCheckCoreState = GetCodePtr();

		// TODO: critical
		ARMABI_MOVI2R(R0, (u32)&coreState);
		LDR(R0, R0);
		CMP(R0, 0);
		FixupBranch badCoreState = B_CC(CC_NEQ);
		FixupBranch skipToRealDispatch2 = B(); //skip the sync and compare first time

		// At this point : flags = EQ. Fine for the next check, no need to jump over it.
		dispatcher = GetCodePtr();

			// The result of slice decrementation should be in flags if somebody jumped here
			// IMPORTANT - We jump on negative, not carry!!!
			FixupBranch bail = B_CC(CC_MI);

			SetJumpTarget(skipToRealDispatch);
			SetJumpTarget(skipToRealDispatch2);

			dispatcherNoCheck = GetCodePtr();

			// Debug
			// MOV(R0, R13);
			// QuickCallFunction(R1, (void *)&ShowPC);

			ARMABI_MOVI2R(R7, (u32)jit->GetBlockCache()->GetCodePointers());
			ARMABI_MOVI2R(R11, (u32)Memory::base);
			ARMABI_MOVI2R(R10, (u32)mips);

			LDR(R0, R10, offsetof(MIPSState, pc));

			ARMABI_MOVI2R(R1, Memory::MEMVIEW32_MASK);  // can be done with single MOVN instruction
			AND(R0, R0, R1);
			ADD(R0, R0, R11);  // TODO: Optimize (can merge with next instr)
			LDR(R0, R0);
			AND(R1, R0, Operand2(0xFC, 4));   // rotation is to the right, in 2-bit increments.
			BIC(R0, R0, Operand2(0xFC, 4));
			CMP(R1, Operand2(MIPS_EMUHACK_OPCODE >> 24, 4));
			FixupBranch notfound = B_CC(CC_NEQ);  // TODO : No need for a branch really, can use CCs.
				// IDEA - we have 24 bits, why not just use offsets from base of code?
				if (enableDebug)
				{
					//ADD(32, M(&mips->debugCount), Imm8(1));
				}
				// grab from list and jump to it
				ADD(R0, R7, Operand2(2, ST_LSL, R0));
				LDR(R0, R0);
				B(R0);

			SetJumpTarget(notfound);

			//Ok, no block, let's jit
			ARMABI_CallFunction((void *)&Jit);

			B(dispatcherNoCheck); // no point in special casing this

		SetJumpTarget(bail);

		// TODO: critical
		ARMABI_MOVI2R(R0, (u32)&coreState);
		LDR(R0, R0);
		CMP(R0, 0);
		B_CC(CC_EQ, outerLoop);

	SetJumpTarget(badCoreState);

	breakpointBailout = GetCodePtr();

	POP(9, R4, R5, R6, R7, R8, R9, R10, R11, _PC);  // Returns

	INFO_LOG(HLE, "THE DISASM ========================");
	DisassembleArm(enterCode, GetCodePtr() - enterCode);
	INFO_LOG(HLE, "END OF THE DISASM ========================");

	// Don't forget to zap the instruction cache!
	FlushIcache();
}