// LOCAL: stand-in for PCSX2's R3000A.h: the IOP cycle counter the SPU2 core reads. spu2_port.cpp sets psxRegs.cycle to
// our IOP's current cycle (low 32 bits, wrapping like PCSX2's) before every call into the SPU2 core.
#pragma once

#include "common/Pcsx2Types.h"

struct psxRegisters
{
	u32 cycle;
};

extern psxRegisters psxRegs;
