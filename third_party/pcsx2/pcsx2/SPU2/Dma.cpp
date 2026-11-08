// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <vector>
#include "SPU2/defs.h"
#include "SPU2/Debug.h"
#include "SPU2/Dma.h"
#include "SPU2/spu2.h"
#include "R3000A.h"
#include "IopHw.h"
#include "Config.h"

static constexpr int CYCLES_PER_WORD = 24;

#ifdef PCSX2_DEVBUILD

#define safe_fclose(ptr) \
	((void)((((ptr) != nullptr) && (std::fclose(ptr), !!0)), (ptr) = nullptr))

static FILE* DMA4LogFile = nullptr;
static FILE* DMA7LogFile = nullptr;
static FILE* ADMA4LogFile = nullptr;
static FILE* ADMA7LogFile = nullptr;
static FILE* ADMAOutLogFile = nullptr;

static FILE* REGWRTLogFile[2] = {0, 0};

void DMALogOpen()
{
	if (!SPU2::DMALog())
		return;
	DMA4LogFile = EmuFolders::OpenLogFile("SPU2dma4.dat", "wb");
	DMA7LogFile = EmuFolders::OpenLogFile("SPU2dma7.dat", "wb");
	ADMA4LogFile = EmuFolders::OpenLogFile("adma4.raw", "wb");
	ADMA7LogFile = EmuFolders::OpenLogFile("adma7.raw", "wb");
	ADMAOutLogFile = EmuFolders::OpenLogFile("admaOut.raw", "wb");
}

void DMA4LogWrite(void* lpData, u32 ulSize)
{
	if (!SPU2::DMALog())
		return;
	if (!DMA4LogFile)
		return;
	fwrite(lpData, ulSize, 1, DMA4LogFile);
}

void DMA7LogWrite(void* lpData, u32 ulSize)
{
	if (!SPU2::DMALog())
		return;
	if (!DMA7LogFile)
		return;
	fwrite(lpData, ulSize, 1, DMA7LogFile);
}

void ADMAOutLogWrite(void* lpData, u32 ulSize)
{
	if (!SPU2::DMALog())
		return;
	if (!ADMAOutLogFile)
		return;
	fwrite(lpData, ulSize, 1, ADMAOutLogFile);
}

void RegWriteLog(u32 core, u16 value)
{
	if (!SPU2::DMALog())
		return;
	if (!REGWRTLogFile[core])
		return;
	fwrite(&value, 2, 1, REGWRTLogFile[core]);
}

void DMALogClose()
{
	safe_fclose(DMA4LogFile);
	safe_fclose(DMA7LogFile);
	safe_fclose(REGWRTLogFile[0]);
	safe_fclose(REGWRTLogFile[1]);
	safe_fclose(ADMA4LogFile);
	safe_fclose(ADMA7LogFile);
	safe_fclose(ADMAOutLogFile);
}

#endif

void V_Core::LogAutoDMA(FILE* fp)
{
	if (!SPU2::DMALog() || !fp || !DMAPtr)
		return;
	fwrite(DMAPtr + InputDataProgress, 0x400, 1, fp);
}

// LOCAL DEBUG counters (see AutoDMAReadBuffer), read by spu2_port.cpp takeStats.
unsigned long long g_admaBlocks[2][2] = {}, g_admaStale[2][2] = {}, g_admaZero[2][2] = {};

void V_Core::AutoDMAReadBuffer(int mode) //mode: 0= split stereo; 1 = do not split stereo
{
	u32 spos = InputPosWrite & 0x100; // Starting position passed by TSA
	bool leftbuffer = !(InputPosWrite & 0x80);

	if (InputPosWrite == 0xFFFF) // Data request not made yet
		return;

	AutoDMACtrl &= 0x3;

	int size = std::min(InputDataLeft, (u32)0x200);
	if (!leftbuffer)
		size = std::min(size, 0x100);
#ifdef PCSX2_DEVBUILD
	LogAutoDMA(Index ? ADMA7LogFile : ADMA4LogFile);
#endif
	//ConLog("Refilling ADMA buffer at %x OutPos %x with %x\n", spos, OutPos, size);
	// HACKFIX!! DMAPtr can be invalid after a savestate load, so the savestate just forces it
	// to nullptr and we ignore it here.  (used to work in old VM editions of PCSX2 with fixed
	// addressing, but new PCSX2s have dynamic memory addressing).
	if (DMAPtr == nullptr)
	{
		DMAPtr = (u16*)iopPhysMem(MADR);
		InputDataProgress = 0;
	}

	if (mode)
	{
		if (DMAPtr != nullptr)
			memcpy(GetMemPtr(0x2000 + (Index << 10) + spos), DMAPtr + InputDataProgress, size);
		MADR += size;
		InputDataLeft -= 0x200;
		InputDataProgress += 0x200;
	}
	else
	{
		while (size)
		{
			if (!leftbuffer)
				spos |= 0x200;
			else
				spos &= ~0x200;

			// LOCAL DEBUG (static in one ear): is this block new? Same samples as the previous block of this side = the IOP did
			// not refill its buffer in time (stale); all zero = nothing was there. Counted per core and side.
			if (DMAPtr != nullptr)
			{
				static u64 s_lastHash[2][2] = {};
				const int side = leftbuffer ? 0 : 1;
				u64 h = 1469598103934665603ull;
				bool zero = true;
				for (int k = 0; k < 0x100; ++k)
				{
					h = (h ^ DMAPtr[InputDataProgress + k]) * 1099511628211ull;
					zero = zero && DMAPtr[InputDataProgress + k] == 0;
				}
				++g_admaBlocks[Index][side];
				// LOCAL DEBUG: sharp edges inside this block (and across to the previous block of this side), with the IOP memory
				// address it is read from, so damaged blocks can be matched with the EE -> IOP transfers ([sif-edges]).
				{
					static s32 s_prevLast[2][2] = {};
					// Per IOP address (the second's blocks): how many blocks had edges and how many edges; printed once a second
					// (every 188 right-hand blocks of core 0), for the busiest few, so it covers the moments of the static.
					static std::map<u32, std::pair<u64, u64>> s_byAddress[2];
					static u64 s_blocks[2] = {};
					int edges = 0;
					s32 prev = s_prevLast[Index][side];
					for (int k = 0; k < 0x100; ++k)
					{
						const s32 cur = static_cast<s16>(DMAPtr[InputDataProgress + k]);
						edges += (cur - prev > 8000 || prev - cur > 8000) ? 1 : 0;
						prev = cur;
					}
					s_prevLast[Index][side] = prev;
					const u32 address = static_cast<u32>(reinterpret_cast<const u8*>(DMAPtr + InputDataProgress) - reinterpret_cast<const u8*>(iopPhysMem(0)));
					++s_blocks[side];
					if (edges > 0)
					{
						auto& entry = s_byAddress[side][address];
						++entry.first;
						entry.second += static_cast<u64>(edges);
					}
					if (Index == 0 && side == 1 && s_blocks[1] % 188 == 0)
					{
						for (int sd = 0; sd < 2; ++sd)
						{
							std::vector<std::pair<u32, std::pair<u64, u64>>> top(s_byAddress[sd].begin(), s_byAddress[sd].end());
							std::sort(top.begin(), top.end(), [](const auto& l, const auto& r) { return l.second.second > r.second.second; });
							u64 total = 0, blocksWith = 0;
							for (const auto& e : top)
							{
								total += e.second.second;
								blocksWith += e.second.first;
							}
							std::fprintf(stderr, "[adma-edges] core 0 %s, last second: %llu edges in %llu blocks (%zu IOP addresses)", sd ? "RIGHT" : "left",
								static_cast<unsigned long long>(total), static_cast<unsigned long long>(blocksWith), top.size());
							for (size_t k = 0; k < top.size() && k < 5; ++k)
								std::fprintf(stderr, " | 0x%x: %llu blocks, %llu edges", top[k].first, static_cast<unsigned long long>(top[k].second.first),
									static_cast<unsigned long long>(top[k].second.second));
							std::fprintf(stderr, "\n");
							s_byAddress[sd].clear();
						}
					}
				}
				if (h == s_lastHash[Index][side])
					++g_admaStale[Index][side];
				if (zero)
					++g_admaZero[Index][side];
				s_lastHash[Index][side] = h;
			}
			if (DMAPtr != nullptr)
			{
				// LOCAL PATCH (PS2X_SPU2_COPY_LEFT=1, setting audio.stereo_repair): the streamed music's right-hand block reaches
				// the IOP damaged (static / a late copy of the sound) while the left one is clean, so the left block is played in
				// both ears. Sound effects (voices) stay in stereo.
				static const bool s_copyLeft = []() { const char* e = std::getenv("PS2X_SPU2_COPY_LEFT"); return e && e[0] == '1'; }();
				static u16 s_leftBlock[2][0x100] = {};
				if (s_copyLeft && Index == 0)
				{
					if (leftbuffer)
						memcpy(s_leftBlock[Index], DMAPtr + InputDataProgress, 0x200);
					memcpy(GetMemPtr(0x2000 + (Index << 10) + spos), leftbuffer ? static_cast<const void*>(DMAPtr + InputDataProgress) : static_cast<const void*>(s_leftBlock[Index]), 0x200);
				}
				else
					memcpy(GetMemPtr(0x2000 + (Index << 10) + spos), DMAPtr + InputDataProgress, 0x200);
			}
			InputDataTransferred += 0x200;
			InputDataLeft -= 0x100;
			InputDataProgress += 0x100;
			leftbuffer = !leftbuffer;
			size -= 0x100;
			InputPosWrite += 0x80;
		}
	}
	if (!(InputPosWrite & 0x80))
		InputPosWrite = 0xFFFF;
}

void V_Core::StartADMAWrite(u16* pMem, u32 sz)
{
	int size = sz;

	TimeUpdate(psxRegs.cycle);

	if (SPU2::MsgAutoDMA())
	{
		SPU2::ConLog("* SPU2: DMA%c AutoDMA Transfer of %d bytes to %x (%02x %x %04x).OutPos %x\n",
			   GetDmaIndexChar(), size << 1, ActiveTSA, DMABits, AutoDMACtrl, (~Regs.ATTR) & 0xffff, OutPos);
	}

	InputDataProgress = 0;
	TADR = MADR + (size << 1);
	if ((AutoDMACtrl & (Index + 1)) == 0)
	{
		ActiveTSA = 0x2000 + (Index << 10);
		DMAICounter = size * CYCLES_PER_WORD;
		LastClock = psxRegs.cycle;
	}
	else if (size >= 256)
	{
		InputDataLeft = size;
		if (InputPosWrite != 0xFFFF)
		{
#ifdef PCM24_S1_INTERLEAVE
			if ((Index == 1) && ((PlayMode & 8) == 8))
			{
				AutoDMAReadBuffer(Index, 1);
			}
			else
			{
				AutoDMAReadBuffer(Index, 0);
			}
#else
			AutoDMAReadBuffer(0);
#endif
		}
		AdmaInProgress = 1;
	}
	else
	{
		if (SPU2::MsgToConsole())
			SPU2::ConLog("ADMA%c Error Size of %x too small\n", GetDmaIndexChar(), size);
		InputDataLeft = 0;
		DMAICounter = size * CYCLES_PER_WORD;
		LastClock = psxRegs.cycle;
	}
}

void V_Core::PlainDMAWrite(u16* pMem, u32 size)
{
	if (SPU2::MsgToConsole())
	{
		// Don't need this anymore. Target may still be good to know though.
		/*if((uptr)pMem & 15)
		{
			ConLog("* SPU2 DMA Write > Misaligned source. Core: %d  IOP: %p  TSA: 0x%x  Size: 0x%x\n", Index, (void*)pMem, TSA, size);
		}*/

		if (ActiveTSA & 7)
		{
			SPU2::ConLog("* SPU2 DMA Write > Misaligned target. Core: %d  IOP: %p  TSA: 0x%x  Size: 0x%x\n", Index, (void*)DMAPtr, ActiveTSA, ReadSize);
		}
	}

	TimeUpdate(psxRegs.cycle);

	ReadSize = size;
	IsDMARead = false;
	DMAICounter = 0;
	LastClock = psxRegs.cycle;
	Regs.STATX &= ~0x80;
	Regs.STATX |= 0x400;
	TADR = MADR + (size << 1);

	if (SPU2::MsgDMA())
	{
		SPU2::ConLog("* SPU2: DMA%c Write Transfer of %d bytes to %x (%02x %x %04x). IRQE = %d IRQA = %x \n",
			GetDmaIndexChar(), size << 1, ActiveTSA, DMABits, AutoDMACtrl, Regs.ATTR & 0xffff,
			Cores[Index].IRQEnable, Cores[Index].IRQA);
	}

	FinishDMAwrite();
}

void V_Core::FinishDMAwrite()
{
	if (DMAPtr == nullptr)
	{
		DMAPtr = (u16*)iopPhysMem(MADR);
	}

	DMAICounter = ReadSize;

#ifdef PCSX2_DEVBUILD
	if (Index == 0)
		DMA4LogWrite(DMAPtr, ReadSize << 1);
	else
		DMA7LogWrite(DMAPtr, ReadSize << 1);
#endif

	u32 buff1end = ActiveTSA + std::min(ReadSize, (u32)0x100 + std::abs(DMAICounter / CYCLES_PER_WORD));
	u32 buff2end = 0;
	if (buff1end > 0x100000)
	{
		buff2end = buff1end - 0x100000;
		buff1end = 0x100000;
	}

	const int cacheIdxStart = ActiveTSA / pcm_WordsPerBlock;
	const int cacheIdxEnd = (buff1end + pcm_WordsPerBlock - 1) / pcm_WordsPerBlock;
	PcmCacheEntry* cacheLine = &pcm_cache_data[cacheIdxStart];
	PcmCacheEntry& cacheEnd = pcm_cache_data[cacheIdxEnd];

	do
	{
		cacheLine->Validated = false;
		cacheLine++;
	} while (cacheLine != &cacheEnd);

	//ConLog( "* SPU2: Cache Clear Range!  TSA=0x%x, TDA=0x%x (low8=0x%x, high8=0x%x, len=0x%x)\n",
	//	ActiveTSA, buff1end, flagTSA, flagTDA, clearLen );


	// First Branch needs cleared:
	// It starts at TSA and goes to buff1end.

	const u32 buff1size = (buff1end - ActiveTSA);
	memcpy(GetMemPtr(ActiveTSA), DMAPtr, buff1size * 2);

	u32 TDA;

	if (buff2end > 0)
	{
		// second branch needs copied:
		// It starts at the beginning of memory and moves forward to buff2end

		// endpoint cache should be irrelevant, since it's almost certainly dynamic
		// memory below 0x2800 (registers and such)
		//const u32 endpt2 = (buff2end + roundUp) / indexer_scalar;
		//memset( pcm_cache_flags, 0, endpt2 );
		const u32 start = ActiveTSA;
		TDA = buff1end;

		DMAPtr += TDA - ActiveTSA;
		ReadSize -= TDA - ActiveTSA;
		ActiveTSA = 0;
		// Emulation Grayarea: Should addresses wrap around to zero, or wrap around to
		// 0x2800?  Hard to know for sure (almost no games depend on this)
		memcpy(GetMemPtr(0), DMAPtr, buff2end * 2);
		TDA = (buff2end) & 0xfffff;

		// Flag interrupt?  If IRQA occurs between start and dest, flag it.
		// Important: Test both core IRQ settings for either DMA!
		// Note: Because this buffer wraps, we use || instead of &&

		for (int i = 0; i < 2; i++)
		{
			// Start is exclusive and end is inclusive... maybe? The end is documented to be inclusive,
			// which suggests that memory access doesn't trigger interrupts, incrementing registers does
			// (which would mean that if TSA=IRQA an interrupt doesn't fire... I guess?)
			// Chaos Legion uses interrupt addresses set to the beginning of the two buffers in a double
			// buffer scheme and sets LSA of one of the voices to the start of the opposite buffer.
			// However it transfers to the same address right after setting IRQA, which by our previous
			// understanding would trigger the interrupt early causing it to switch buffers again immediately
			// and an interrupt never fires again, leaving the voices looping the same samples forever.

			if (Cores[i].IRQEnable && (Cores[i].IRQA > start || Cores[i].IRQA < TDA))
			{
				//ConLog("DMAwrite Core %d: IRQ Called (IRQ passed). IRQA = %x Cycles = %d\n", i, Cores[i].IRQA, Cycles );
				SetIrqCallDMA(i);
			}
		}
	}
	else
	{
		// Buffer doesn't wrap/overflow!
		// Just set the TDA and check for an IRQ...

		TDA = buff1end;

		// Flag interrupt?  If IRQA occurs between start and dest, flag it.
		// Important: Test both core IRQ settings for either DMA!
		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA > ActiveTSA && Cores[i].IRQA < TDA))
			{
				//ConLog("DMAwrite Core %d: IRQ Called (IRQ passed). IRQA = %x Cycles = %d\n", i, Cores[i].IRQA, Cycles );
				SetIrqCallDMA(i);
			}
		}
	}

	DMAPtr += TDA - ActiveTSA;
	ReadSize -= TDA - ActiveTSA;

	DMAICounter = (DMAICounter - ReadSize) * CYCLES_PER_WORD;

	CounterUpdate(DMAICounter);

	ActiveTSA = TDA;
	ActiveTSA &= 0xfffff;
	TSA = ActiveTSA;
}

void V_Core::FinishDMAread()
{
	u32 buff1end = ActiveTSA + std::min(ReadSize, (u32)0x100 + std::abs(DMAICounter / CYCLES_PER_WORD));
	u32 buff2end = 0;

	if (buff1end > 0x100000)
	{
		buff2end = buff1end - 0x100000;
		buff1end = 0x100000;
	}

	if (DMAPtr == nullptr)
	{
		DMAPtr = (u16*)iopPhysMem(MADR);
	}

	const u32 buff1size = (buff1end - ActiveTSA);
	memcpy(DMARPtr, GetMemPtr(ActiveTSA), buff1size * 2);
	// Note on TSA's position after our copy finishes:
	// IRQA should be measured by the end of the writepos+0x20.  But the TDA
	// should be written back at the precise endpoint of the xfer.
	u32 TDA;

	if (buff2end > 0)
	{
		const u32 start = ActiveTSA;
		TDA = buff1end;

		DMARPtr += TDA - ActiveTSA;
		ReadSize -= TDA - ActiveTSA;
		ActiveTSA = 0;

		// second branch needs cleared:
		// It starts at the beginning of memory and moves forward to buff2end
		memcpy(DMARPtr, GetMemPtr(0), buff2end * 2);

		TDA = (buff2end) & 0xfffff;

		// Flag interrupt?  If IRQA occurs between start and dest, flag it.
		// Important: Test both core IRQ settings for either DMA!
		// Note: Because this buffer wraps, we use || instead of &&

		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA > start || Cores[i].IRQA < TDA))
			{
				SetIrqCallDMA(i);
			}
		}
	}
	else
	{
		// Buffer doesn't wrap/overflow!
		// Just set the TDA and check for an IRQ...

		TDA = buff1end;

		// Flag interrupt?  If IRQA occurs between start and dest, flag it.
		// Important: Test both core IRQ settings for either DMA!

		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA > ActiveTSA && Cores[i].IRQA < TDA))
			{
				SetIrqCallDMA(i);
			}
		}
	}

	DMARPtr += TDA - ActiveTSA;
	ReadSize -= TDA - ActiveTSA;

	// DMA Reads are done AFTER the delay, so to get the timing right we need to scheule one last DMA to catch IRQ's
	if (ReadSize)
		DMAICounter = std::min(ReadSize, (u32)0x100) * CYCLES_PER_WORD;
	else
		DMAICounter = CYCLES_PER_WORD;

	CounterUpdate(DMAICounter);

	ActiveTSA = TDA;
	ActiveTSA &= 0xfffff;
	TSA = ActiveTSA;
}

void V_Core::DoDMAread(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);

	DMARPtr = pMem;
	ActiveTSA = TSA & 0xfffff;
	ReadSize = size;
	IsDMARead = true;
	LastClock = psxRegs.cycle;
	DMAICounter = (std::min(ReadSize, (u32)0x100) * CYCLES_PER_WORD);
	Regs.STATX &= ~0x80;
	Regs.STATX |= 0x400;
	//Regs.ATTR |= 0x30;
	TADR = MADR + (size << 1);

	CounterUpdate(DMAICounter);

	if (SPU2::MsgDMA())
	{
		SPU2::ConLog("* SPU2: DMA%c Read Transfer of %d bytes from %x (%02x %x %04x). IRQE = %d IRQA = %x \n",
			GetDmaIndexChar(), size << 1, ActiveTSA, DMABits, AutoDMACtrl, Regs.ATTR & 0xffff,
			Cores[Index].IRQEnable, Cores[Index].IRQA);
	}
}

void V_Core::DoDMAwrite(u16* pMem, u32 size)
{
	DMAPtr = pMem;

	if (size < 2)
	{
		Regs.STATX &= ~0x80;
		//Regs.ATTR |= 0x30;
		DMAICounter = 1 * CYCLES_PER_WORD;
		LastClock = psxRegs.cycle;
		return;
	}

	if (IsDevBuild)
	{
		DebugCores[Index].lastsize = size;
		DebugCores[Index].dmaFlag = 2;
	}

	if (SPU2::MsgToConsole())
	{
		if (TSA > 0xfffff)
		{
			SPU2::ConLog("* SPU2: Transfer Start Address out of bounds. TSA is %x\n", TSA);
		}
	}

	ActiveTSA = TSA & 0xfffff;

	const bool adma_enable = ((AutoDMACtrl & (Index + 1)) == (Index + 1));

	if (adma_enable)
	{
		StartADMAWrite(pMem, size);
	}
	else
	{
		PlainDMAWrite(pMem, size);
		Regs.STATX &= ~0x80;
		Regs.STATX |= 0x400;
	}
}
