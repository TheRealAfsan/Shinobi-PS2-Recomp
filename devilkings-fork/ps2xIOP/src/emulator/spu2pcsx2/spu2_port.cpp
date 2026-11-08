// LOCAL: the IOP side of PCSX2's SPU2 core for this runtime. It replaces PCSX2's SPU2/spu2.cpp (host audio stream,
// configuration, save states, capture) and PCSX2's IOP glue (IopDma.cpp psxDma4/psxDma7, spu2DMA4Irq/spu2DMA7Irq/spu2Irq,
// IOP counter 6). Everything that touches the sound chip's state is PCSX2's own code, unmodified; this file only feeds it
// the IOP clock, the DMA registers, IOP RAM and the interrupt lines, and takes its output samples.
//
// Derived in part from PCSX2 (SPU2/spu2.cpp, IopDma.cpp, IopCounters.cpp), GPL-3.0+, PCSX2 Dev Team.

#include "spu2_pcsx2.h"

#include "IopCounters.h"
#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"
#include "R3000A.h"
#include "SPU2/Dma.h"
#include "SPU2/defs.h"
#include "SPU2/regs.h"
#include "SPU2/spu2.h"
#include "common/Console.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ------------------------------------------------------------------ PCSX2 globals the SPU2 core expects

psxRegisters psxRegs{};
psxCounter psxCounters[8]{};
s32 psxNextDeltaCounter = 0;
u32 psxNextStartCounter = 0;
u64 lClocks = 0;
float DCFilterIn[2], DCFilterOut[2];
const StereoOut32 StereoOut32::Empty(0, 0); // PCSX2 spu2.cpp

namespace spu2pcsx2_hw
{
	static u32 s_unbound[8] = {};
	u32* dma4Madr = &s_unbound[0];
	u32* dma4Bcr = &s_unbound[1];
	u32* dma4Chcr = &s_unbound[2];
	u32* dma4Tadr = &s_unbound[3];
	u32* dma7Madr = &s_unbound[4];
	u32* dma7Bcr = &s_unbound[5];
	u32* dma7Chcr = &s_unbound[6];
	u32* dma7Tadr = &s_unbound[7];
	u8* iopRam = nullptr;
} // namespace spu2pcsx2_hw

namespace spu2pcsx2_console
{
	bool enabled()
	{
		static const bool s_on = []() {
			const char* e = std::getenv("PS2X_SPU2_LOG");
			return e && e[0] == '1';
		}();
		return s_on;
	}

	static void print(const char* prefix, const char* fmt, va_list args)
	{
		if (!enabled())
			return;
		char line[1024];
		std::vsnprintf(line, sizeof(line), fmt, args);
		std::fprintf(stderr, "[spu2-pcsx2]%s %s\n", prefix, line);
	}

	void Sink::WriteLn(const char* fmt, ...) const
	{
		va_list args;
		va_start(args, fmt);
		print("", fmt, args);
		va_end(args);
	}

	void Sink::Warning(const char* fmt, ...) const
	{
		va_list args;
		va_start(args, fmt);
		print(" warning:", fmt, args);
		va_end(args);
	}

	void Sink::Error(const char* fmt, ...) const
	{
		va_list args;
		va_start(args, fmt);
		print(" error:", fmt, args);
		va_end(args);
	}
} // namespace spu2pcsx2_console

extern unsigned long long g_admaBlocks[2][2], g_admaStale[2][2], g_admaZero[2][2], g_inputJumps[2][2]; // SPU2/Dma.cpp (local debug)

namespace
{
	ps2x::iop::spu2pcsx2::Hooks s_hooks;
	ps2x::iop::spu2pcsx2::Stats s_stats;
	u64 s_keyOnCycle[2][24] = {};
	u64 s_cycle64 = 0; // full-width IOP cycle of the last call (psxRegs.cycle is its low 32 bits)

	// PCSX2's IOP counter 6 rate: the SPU2 is ticked at least every 12 samples.
	constexpr u32 kSpu2CounterRate = 768u * 12u;

	void setCycle(uint64_t cycle)
	{
		s_cycle64 = cycle;
		psxRegs.cycle = static_cast<u32>(cycle);
	}

	void raise(int irq)
	{
		if (s_hooks.raiseIrq)
			s_hooks.raiseIrq(s_hooks.user, irq);
	}

	// PCSX2 spu2.cpp: a DC-blocking high-pass filter on the output ("some games pause voices with the volume left on").
	void DCFilter(float* input)
	{
		float output[2];
		output[0] = (input[0] - DCFilterIn[0] + ((0.995f * DCFilterOut[0])));
		output[1] = (input[1] - DCFilterIn[1] + ((0.995f * DCFilterOut[1])));
		DCFilterIn[0] = input[0];
		DCFilterIn[1] = input[1];
		DCFilterOut[0] = output[0];
		DCFilterOut[1] = output[1];
		input[0] = output[0];
		input[1] = output[1];
	}

	void internalReset()
	{
		spu2Mix = MULTI_ISA_SELECT(spu2Mix);
		ReverbDownsample = MULTI_ISA_SELECT(ReverbDownsample);
		ReverbUpsample = MULTI_ISA_SELECT(ReverbUpsample);

		std::memset(spu2regs, 0, 0x010000);
		std::memset(_spu2mem, 0, 0x200000);
		std::memset(_spu2mem + 0x2800, 7, 0x10); // from BIOS reversal. Locks the voices so they don't run free.
		std::memset(_spu2mem + 0xe870, 7, 0x10); // Loop which gets left over by the BIOS, Megaman X7 relies on it being there.
		std::memset(DCFilterIn, 0, sizeof(DCFilterIn));
		std::memset(DCFilterOut, 0, sizeof(DCFilterOut));
		Spdif.Info = 0;
		Cores[0].Init(0);
		Cores[1].Init(1);
	}
} // namespace

// ------------------------------------------------------------------ PCSX2 spu2.cpp replacement

u32 SPU2::GetConsoleSampleRate()
{
	return SAMPLE_RATE;
}

bool SPU2::IsRunningPSXMode()
{
	return false;
}

__forceinline void spu2Output(StereoOut32 out)
{
	if ((s_stats.outputSamples & 0x7Fu) == 0u) // every 64 samples (~1.3 ms): how many voices are stacked, how loud
	{
		for (int c = 0; c < 2; ++c)
		{
			float sum = 0.0f, sumL = 0.0f, sumR = 0.0f;
			uint32_t count = 0, invL = 0, invR = 0;
			for (int v = 0; v < 24; ++v)
			{
				const V_Voice& voice = Cores[c].Voices[v];
				if (voice.ADSR.Phase == V_ADSR::PHASE_STOPPED)
					continue;
				++count;
				const float env = static_cast<float>(std::max(voice.ADSR.Value, 0)) / 32768.0f;
				const float l = static_cast<float>(std::abs(voice.Volume.Left.Value)) / 32768.0f;
				const float r = static_cast<float>(std::abs(voice.Volume.Right.Value)) / 32768.0f;
				sum += std::max(l, r) * env;
				sumL += l * env;
				sumR += r * env;
				invL += voice.Volume.Left.Value < 0 ? 1u : 0u;
				invR += voice.Volume.Right.Value < 0 ? 1u : 0u;
			}
			s_stats.levelPeak[c] = std::max(s_stats.levelPeak[c], sum);
			s_stats.voicesPeak[c] = std::max(s_stats.voicesPeak[c], count);
			s_stats.levelSidePeak[c][0] = std::max(s_stats.levelSidePeak[c][0], sumL);
			s_stats.levelSidePeak[c][1] = std::max(s_stats.levelSidePeak[c][1], sumR);
			s_stats.phaseInvertedVoices[c][0] = std::max(s_stats.phaseInvertedVoices[c][0], invL);
			s_stats.phaseInvertedVoices[c][1] = std::max(s_stats.phaseInvertedVoices[c][1], invR);
		}
	}
	{
		static s32 s_prevOut[2] = {};
		if (std::abs(out.Left - s_prevOut[0]) > 8000)
			++s_stats.outputJumps[0];
		if (std::abs(out.Right - s_prevOut[1]) > 8000)
			++s_stats.outputJumps[1];
		s_prevOut[0] = out.Left;
		s_prevOut[1] = out.Right;
	}
	s_stats.outputSamples += 2u;
	const bool clipL = out.Left > 32767 || out.Left < -32768, clipR = out.Right > 32767 || out.Right < -32768;
	s_stats.outputClipped += (clipL ? 1u : 0u) + (clipR ? 1u : 0u);
	s_stats.clippedLeft += clipL ? 1u : 0u;
	s_stats.clippedRight += clipR ? 1u : 0u;
	float conv[2];
	conv[0] = static_cast<float>(clamp_mix(out.Left)) / INT16_MAX;
	conv[1] = static_cast<float>(clamp_mix(out.Right)) / INT16_MAX;
	DCFilter(conv);
	if (s_hooks.output)
	{
		const auto to16 = [](float v) { return static_cast<int16_t>(std::clamp(v * 32767.0f, -32768.0f, 32767.0f)); };
		s_hooks.output(s_hooks.user, to16(conv[0]), to16(conv[1]));
	}
}

void SPU2readDMA4Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[0].DoDMAread(pMem, size);
}

void SPU2writeDMA4Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[0].DoDMAwrite(pMem, size);
}

void SPU2interruptDMA4()
{
	if (Cores[0].DmaMode)
		Cores[0].Regs.STATX |= 0x80;
	Cores[0].Regs.STATX &= ~0x400;
	Cores[0].TSA = Cores[0].ActiveTSA;
}

void SPU2interruptDMA7()
{
	if (Cores[1].DmaMode)
		Cores[1].Regs.STATX |= 0x80;
	Cores[1].Regs.STATX &= ~0x400;
	Cores[1].TSA = Cores[1].ActiveTSA;
}

void SPU2readDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[1].DoDMAread(pMem, size);
}

void SPU2writeDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[1].DoDMAwrite(pMem, size);
}

void SPU2async()
{
	TimeUpdate(psxRegs.cycle);
}

u16 SPU2read(u32 rmem)
{
	u16 ret = 0xDEAD;
	u32 core = 0;
	const u32 mem = rmem & 0xFFFF;
	u32 omem = mem;
	if (mem & 0x400)
	{
		omem ^= 0x400;
		core = 1;
	}
	if (omem == 0x1f9001AC)
	{
		Cores[core].ActiveTSA = Cores[core].TSA;
		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA == Cores[core].ActiveTSA))
				SetIrqCall(i);
		}
		ret = Cores[core].DmaRead();
	}
	else
	{
		TimeUpdate(psxRegs.cycle);
		if (rmem >> 16 == 0x1f80)
			ret = Cores[0].ReadRegPS1(rmem);
		else if (mem >= 0x800)
			ret = spu2Ru16(mem);
		else
			ret = *(regtable[(mem >> 1)]);
	}
	return ret;
}

void SPU2write(u32 rmem, u16 value)
{
	TimeUpdate(psxRegs.cycle);
	if (rmem >> 16 == 0x1f80)
		Cores[0].WriteRegPS1(rmem, value);
	else
		SPU2_FastWrite(rmem, value);
}

// ------------------------------------------------------------------ PCSX2 IOP glue (IopDma.cpp, IopHw)

void spu2DMA4Irq()
{
	SPU2interruptDMA4();
	if (HW_DMA4_CHCR & 0x01000000)
	{
		HW_DMA4_CHCR &= ~0x01000000;
		++s_stats.dmaIrqs[0];
		raise(0x24);
	}
}

void spu2DMA7Irq()
{
	SPU2interruptDMA7();
	if (HW_DMA7_CHCR & 0x01000000)
	{
		HW_DMA7_CHCR &= ~0x01000000;
		++s_stats.dmaIrqs[1];
		raise(0x28);
	}
}

void spu2Irq()
{
	++s_stats.spuIrqs;
	raise(9);
}

// ------------------------------------------------------------------ interface for the IOP

namespace ps2x::iop::spu2pcsx2
{
	void bind(uint8_t* iopRam, uint32_t* const dma4[4], uint32_t* const dma7[4], const Hooks& hooks)
	{
		spu2pcsx2_hw::iopRam = iopRam;
		spu2pcsx2_hw::dma4Madr = dma4[0];
		spu2pcsx2_hw::dma4Bcr = dma4[1];
		spu2pcsx2_hw::dma4Chcr = dma4[2];
		spu2pcsx2_hw::dma4Tadr = dma4[3];
		spu2pcsx2_hw::dma7Madr = dma7[0];
		spu2pcsx2_hw::dma7Bcr = dma7[1];
		spu2pcsx2_hw::dma7Chcr = dma7[2];
		spu2pcsx2_hw::dma7Tadr = dma7[3];
		s_hooks = hooks;
	}

	void reset(uint64_t cycle)
	{
		setCycle(cycle);
		lClocks = psxRegs.cycle;
		psxCounters[6].rate = kSpu2CounterRate;
		psxCounters[6].startCycle = psxRegs.cycle;
		psxCounters[6].deltaCycles = static_cast<s32>(kSpu2CounterRate);
		psxNextStartCounter = psxRegs.cycle;
		psxNextDeltaCounter = static_cast<s32>(kSpu2CounterRate);
		internalReset();
		s_stats = {};
		std::memset(s_keyOnCycle, 0, sizeof(s_keyOnCycle));
	}

	void write16(uint32_t phys, uint16_t value, uint64_t cycle)
	{
		setCycle(cycle);
		// Key-on bookkeeping for the stats (PCSX2 keys the voices on itself, in TimeUpdate).
		const uint32_t offset = phys - 0x1F900000u;
		if (offset < 0x760u)
		{
			const uint32_t core = (offset >> 10) & 1u;
			const uint32_t local = offset & 0x3FFu;
			if (local == 0x1A0u || local == 0x1A2u)
			{
				const uint32_t first = local == 0x1A0u ? 0u : 16u;
				for (uint32_t bit = 0; bit < 16u && first + bit < 24u; ++bit)
					if ((value >> bit) & 1u)
					{
						++s_stats.keyOns;
						s_keyOnCycle[core][first + bit] = cycle;
						// LOCAL DEBUG (PS2X_IOP_WATCH=1): every voice start, with its sample address in sound RAM, pitch and volumes.
						static const bool s_logKeyOn = []() { const char *e = std::getenv("PS2X_IOP_WATCH"); return e && e[0] == '1'; }();
						if (s_logKeyOn)
						{
							const uint32_t kbase = 0x1F900000u + core * 0x400u;
							const uint32_t kv = first + bit;
							const uint32_t kssa = (static_cast<uint32_t>(SPU2read(kbase + 0x1C0u + kv * 12u)) << 16) | SPU2read(kbase + 0x1C2u + kv * 12u);
							std::fprintf(stderr, "[snd-key] core %u voice %u start=0x%x pitch=0x%x volL=0x%x volR=0x%x\n", core, kv, kssa,
							             SPU2read(kbase + kv * 0x10u + 4u), SPU2read(kbase + kv * 0x10u), SPU2read(kbase + kv * 0x10u + 2u));
						}
					}
			}
		}
		SPU2write(phys, value);
	}

	uint16_t read16(uint32_t phys, uint64_t cycle)
	{
		setCycle(cycle);
		return SPU2read(phys);
	}

	void dmaStart(int core, uint32_t madr, uint32_t bcr, uint32_t chcr, uint64_t cycle)
	{
		setCycle(cycle);
		// PCSX2 IopDma.cpp psxDma4 / psxDma7.
		const u32 size = (bcr >> 16) * (bcr & 0xFFFF); // in 32-bit words
		u16* const mem = reinterpret_cast<u16*>(iopPhysMem(madr));
		const bool toSpu = (chcr & 1u) != 0u; // 0x01000201: IOP -> SPU2, 0x01000200: SPU2 -> IOP
		if (core == 0)
		{
			if (toSpu)
				SPU2writeDMA4Mem(mem, size * 2);
			else
				SPU2readDMA4Mem(mem, size * 2);
		}
		else
		{
			if (toSpu)
				SPU2writeDMA7Mem(mem, size * 2);
			else
				SPU2readDMA7Mem(mem, size * 2);
		}
	}

	void advanceTo(uint64_t cycle)
	{
		setCycle(cycle);
		TimeUpdate(psxRegs.cycle);
	}

	uint64_t nextEventCycle(uint64_t now)
	{
		const u32 target = psxCounters[6].startCycle + static_cast<u32>(psxCounters[6].deltaCycles);
		const s32 remaining = static_cast<s32>(target - static_cast<u32>(now));
		return remaining <= 0 ? now : now + static_cast<uint64_t>(remaining);
	}

	void serviceEvents(uint64_t now)
	{
		setCycle(now);
		// PCSX2 IopCounters.cpp, counter 6: when it runs out the SPU2 is updated and the counter restarts at its rate
		// (the SPU2 core shortens it again with CounterUpdate when a transfer is due sooner).
		const s32 elapsed = static_cast<s32>(psxRegs.cycle - psxCounters[6].startCycle);
		if (elapsed >= psxCounters[6].deltaCycles)
		{
			psxCounters[6].startCycle = psxRegs.cycle;
			psxCounters[6].deltaCycles = static_cast<s32>(psxCounters[6].rate);
			SPU2async();
		}
	}

	Stats takeStats(uint64_t cycle)
	{
		Stats result = s_stats;
		constexpr uint64_t kLongCycles = 36864000ull * 20ull; // 20 s of IOP clock
		for (int c = 0; c < 2; ++c)
		{
			for (int v = 0; v < 24; ++v)
			{
				const int phase = Cores[c].Voices[v].ADSR.Phase;
				if (phase == V_ADSR::PHASE_STOPPED)
					continue;
				++result.activeVoices[c];
				const V_Voice& voice = Cores[c].Voices[v];
				const float vol = static_cast<float>(std::max(std::abs(voice.Volume.Left.Value), std::abs(voice.Volume.Right.Value))) / 32768.0f;
				const float level = vol * static_cast<float>(std::max(voice.ADSR.Value, 0)) / 32768.0f;
				result.levelSum[c] += level;
				result.loudest[c] = std::max(result.loudest[c], level);
				if (phase == V_ADSR::PHASE_RELEASE)
					++result.releasingVoices[c];
				if (s_keyOnCycle[c][v] != 0 && cycle - s_keyOnCycle[c][v] > kLongCycles)
					++result.longVoices[c];
			}
		}
		for (int c = 0; c < 2; ++c)
		{
			result.masterVol[c] = Cores[c].MasterVol.Left.Value;
			result.fxVol[c] = Cores[c].FxVol.Left;
			result.extVol[c] = Cores[c].ExtVol.Left;
			result.inpVol[c] = Cores[c].InpVol.Left;
			result.fxEnable[c] = Cores[c].FxEnable;
			result.effectsStart[c] = Cores[c].EffectsStartA;
			result.effectsEnd[c] = Cores[c].EffectsEndA;
			result.masterVolR[c] = Cores[c].MasterVol.Right.Value;
			result.fxVolR[c] = Cores[c].FxVol.Right;
			result.extVolR[c] = Cores[c].ExtVol.Right;
			result.inpVolR[c] = Cores[c].InpVol.Right;
		}
		s_stats.keyOns = 0;
		s_stats.spuIrqs = 0;
		s_stats.dmaIrqs[0] = s_stats.dmaIrqs[1] = 0;
		s_stats.outputClipped = 0;
		s_stats.outputSamples = 0;
		s_stats.levelPeak[0] = s_stats.levelPeak[1] = 0.0f;
		s_stats.voicesPeak[0] = s_stats.voicesPeak[1] = 0u;
		for (int c = 0; c < 2; ++c)
			for (int side = 0; side < 2; ++side)
			{
				result.inputJumps[c][side] = g_inputJumps[c][side];
				g_inputJumps[c][side] = 0;
			}
		result.outputJumps[0] = s_stats.outputJumps[0];
		result.outputJumps[1] = s_stats.outputJumps[1];
		s_stats.outputJumps[0] = s_stats.outputJumps[1] = 0;
		s_stats.clippedLeft = s_stats.clippedRight = 0u;
		for (int c = 0; c < 2; ++c)
			for (int side = 0; side < 2; ++side)
			{
				result.admaBlocks[c][side] = g_admaBlocks[c][side];
				result.admaStale[c][side] = g_admaStale[c][side];
				result.admaZero[c][side] = g_admaZero[c][side];
				g_admaBlocks[c][side] = g_admaStale[c][side] = g_admaZero[c][side] = 0;
				s_stats.levelSidePeak[c][side] = 0.0f;
				s_stats.phaseInvertedVoices[c][side] = 0u;
			}
		return result;
	}
} // namespace ps2x::iop::spu2pcsx2
