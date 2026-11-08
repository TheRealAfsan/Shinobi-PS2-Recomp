// LOCAL: stand-in for PCSX2's IopHw.h: the IOP DMA registers of the SPU2 channels (4 = core 0, 7 = core 1) as lvalues.
// They point straight at our IopMemory's register storage (bound by Spu2Pcsx2::bindDmaRegisters), so the sound driver
// polling MADR/CHCR sees exactly what the SPU2 core updates.
#pragma once

#include "common/Pcsx2Types.h"

namespace spu2pcsx2_hw
{
	extern u32* dma4Madr;
	extern u32* dma4Bcr;
	extern u32* dma4Chcr;
	extern u32* dma4Tadr;
	extern u32* dma7Madr;
	extern u32* dma7Bcr;
	extern u32* dma7Chcr;
	extern u32* dma7Tadr;
} // namespace spu2pcsx2_hw

#define HW_DMA4_MADR (*spu2pcsx2_hw::dma4Madr)
#define HW_DMA4_BCR (*spu2pcsx2_hw::dma4Bcr)
#define HW_DMA4_CHCR (*spu2pcsx2_hw::dma4Chcr)
#define HW_DMA4_TADR (*spu2pcsx2_hw::dma4Tadr)
#define HW_DMA7_MADR (*spu2pcsx2_hw::dma7Madr)
#define HW_DMA7_BCR (*spu2pcsx2_hw::dma7Bcr)
#define HW_DMA7_CHCR (*spu2pcsx2_hw::dma7Chcr)
#define HW_DMA7_TADR (*spu2pcsx2_hw::dma7Tadr)

void spu2DMA4Irq();
void spu2DMA7Irq();
void spu2Irq();
