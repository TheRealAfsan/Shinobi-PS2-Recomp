// LOCAL: stand-in for PCSX2's IopMem.h: IOP main RAM (2 MB) as seen by SPU2 DMA.
#pragma once

#include "common/Pcsx2Types.h"

namespace spu2pcsx2_hw
{
	extern u8* iopRam;
}

inline u8* iopPhysMem(u32 addr)
{
	return spu2pcsx2_hw::iopRam + (addr & 0x1fffff);
}
