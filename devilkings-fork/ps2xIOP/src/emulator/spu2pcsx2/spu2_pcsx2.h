// LOCAL: PCSX2's SPU2 core (third_party/pcsx2/pcsx2/SPU2, GPL-3.0+, PCSX2 v2.8.0, compiled unmodified) driven by our IOP.
// This header is the whole interface; it does not expose any PCSX2 type. See README.md in this folder.
#pragma once

#include <cstdint>

namespace ps2x::iop::spu2pcsx2
{
    struct Hooks
    {
        void *user = nullptr;
        // An IOP interrupt to deliver: 0x24 (DMA 4 = SPU2 core 0 done), 0x28 (DMA 7 = core 1 done), 9 (SPU2 IRQ).
        void (*raiseIrq)(void *user, int irq) = nullptr;
        // One 48 kHz stereo output sample.
        void (*output)(void *user, int16_t left, int16_t right) = nullptr;
    };

    // DMA register storage of channels 4 and 7 in this order: MADR, BCR, CHCR, TADR.
    void bind(uint8_t *iopRam, uint32_t *const dma4[4], uint32_t *const dma7[4], const Hooks &hooks);
    void reset(uint64_t cycle);

    void write16(uint32_t phys, uint16_t value, uint64_t cycle);
    [[nodiscard]] uint16_t read16(uint32_t phys, uint64_t cycle);
    // CHCR of channel 4 (core 0) or 7 (core 1) written with the start bit: run the transfer as PCSX2's IOP DMA does.
    void dmaStart(int core, uint32_t madr, uint32_t bcr, uint32_t chcr, uint64_t cycle);

    // Mix up to `cycle` (PCSX2's TimeUpdate).
    void advanceTo(uint64_t cycle);
    // PCSX2 wakes the IOP when its SPU2 counter (IOP counter 6) runs out: when a DMA completes, else every 768 * 12
    // cycles. The IOP scheduler must not run past nextEventCycle() without calling serviceEvents().
    [[nodiscard]] uint64_t nextEventCycle(uint64_t now);
    void serviceEvents(uint64_t now);

    struct Stats
    {
        uint64_t keyOns = 0;
        uint64_t spuIrqs = 0;
        uint64_t dmaIrqs[2] = {0, 0};
        uint32_t activeVoices[2] = {0, 0};
        uint32_t releasingVoices[2] = {0, 0};
        uint32_t longVoices[2] = {0, 0}; // active for more than 20 s without a new key-on (stuck notes)
        // LOCAL DEBUG (loudness / static when many sounds play): per core, as of now. levelSum = the playing voices'
        // volume x envelope added up, in full-scale voices (2.0 = two voices at full volume; > ~1 can clip);
        // loudest = the biggest single voice. Volumes on the 0..0x7FFF scale, sign kept.
        float levelSum[2] = {0, 0};
        float loudest[2] = {0, 0};
        float levelPeak[2] = {0, 0}; // highest levelSum seen since the last call (checked every 1 ms)
        uint32_t voicesPeak[2] = {0, 0};
        int32_t masterVol[2] = {0, 0}, fxVol[2] = {0, 0}, extVol[2] = {0, 0}, inpVol[2] = {0, 0};
        bool fxEnable[2] = {false, false};
        uint32_t effectsStart[2] = {0, 0}, effectsEnd[2] = {0, 0};
        // Output samples (L or R) that went past full scale before the final cut-off, since the last call.
        uint64_t outputClipped = 0;
        uint64_t outputSamples = 0;
        // LOCAL DEBUG (static in the right ear only): the same per side. levelSide = playing voices' volume x envelope
        // per side (peak this second); right-side volumes of the master / effects / external / input stages.
        uint64_t clippedLeft = 0, clippedRight = 0;
        float levelSidePeak[2][2] = {{0, 0}, {0, 0}}; // [core][0 = left, 1 = right]
        int32_t masterVolR[2] = {0, 0}, fxVolR[2] = {0, 0}, extVolR[2] = {0, 0}, inpVolR[2] = {0, 0};
        uint32_t phaseInvertedVoices[2][2] = {{0, 0}, {0, 0}}; // voices whose volume is negative (phase-inverted), per side
        // Streamed audio (AutoDMA) blocks since the last call, [core][0 = left, 1 = right]: delivered, identical to the previous
        // block of that side (stale: the IOP did not refill in time), and all zero.
        uint64_t admaBlocks[2][2] = {{0, 0}, {0, 0}}, admaStale[2][2] = {{0, 0}, {0, 0}}, admaZero[2][2] = {{0, 0}, {0, 0}};
        // Sharp edges (jump > 8000) since the last call, [side]: of the streamed input (core0 and core1) and of the final output.
        uint64_t inputJumps[2][2] = {{0, 0}, {0, 0}}; // [core][side]
        uint64_t outputJumps[2] = {0, 0};
    };
    // Counters since the last call, voice counts as of now.
    [[nodiscard]] Stats takeStats(uint64_t cycle);
}
