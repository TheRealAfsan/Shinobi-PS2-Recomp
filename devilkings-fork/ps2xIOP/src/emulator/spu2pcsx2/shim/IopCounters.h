// LOCAL: stand-in for PCSX2's IopCounters.h. Counter 6 is PCSX2's "SPU2 counter": the SPU2 core shortens it
// (CounterUpdate) so the IOP calls SPU2async() exactly when a DMA completes; otherwise it fires every 768 * 12 cycles.
// spu2_port.cpp turns it into Spu2Pcsx2::nextEventCycle() for our IOP scheduler.
#pragma once

#include "common/Pcsx2Types.h"

struct psxCounter
{
	u64 count;
	u64 target;
	u32 mode;
	u32 rate;
	u32 interrupt;
	u32 startCycle;
	s32 deltaCycles;
};

extern psxCounter psxCounters[8];
extern s32 psxNextDeltaCounter;
extern u32 psxNextStartCounter;
