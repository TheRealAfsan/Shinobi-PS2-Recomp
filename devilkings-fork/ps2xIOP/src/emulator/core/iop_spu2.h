#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace ps2x::iop::detail
{
    // SPU2 sound processor: two cores of 24 ADPCM voices with ADSR envelopes,
    // AutoDMA PCM input and a 48 kHz stereo output that the host drains.
    // Register storage stays with IopMemory; this class mirrors every 16-bit
    // write and overrides the registers the hardware updates by itself.
    // LOCAL PATCH: a volume register in sweep mode (bit 15) changes the volume by itself, like an envelope.
    struct VolumeSweep
    {
        int32_t level = 0;      // magnitude, 0..0x7FFF
        uint32_t wait = 0;      // samples until the next step
    };

    class Spu2
    {
    public:
        static constexpr uint32_t kBase = 0x1F900000u;
        static constexpr uint32_t kEnd = 0x1F900800u;
        static constexpr uint32_t kIopCyclesPerSample = 768u;

        Spu2();

        void reset();

        [[nodiscard]] static bool isRegister(uint32_t phys) noexcept { return phys >= kBase && phys < kEnd; }

        void write16(uint32_t phys, uint16_t value);
        // Returns true and sets value when the register is produced by the SPU.
        [[nodiscard]] bool read16(uint32_t phys, uint16_t &value) const;

        // DMA from IOP RAM: AutoDMA PCM (ADMA) or a block into SPU RAM at TSA.
        void dmaWrite(uint32_t core, const uint8_t *data, uint32_t bytes, bool autoDma);

        // Generate output samples up to the given IOP cycle.
        void advanceTo(uint64_t cycle);

        // Host side (any thread): copy up to frames stereo frames, returns count.
        size_t readOutput(int16_t *destination, size_t frames);

        // LOCAL PATCH: PCSX2's SPU2 core instead of this class's own model (PS2X_SPU2=legacy keeps the old one).
        // IopMemory binds it to IOP RAM, the DMA 4/7 registers and the IOP cycle counter; DMA starts on those channels
        // go to pcsx2DmaStart instead of dmaWrite, and the interrupts it raises are collected for the IOP scheduler.
        [[nodiscard]] static bool pcsx2Wanted();
        void attachPcsx2(uint8_t *iopRam, uint32_t *const dma4[4], uint32_t *const dma7[4], const uint64_t *cycle);
        [[nodiscard]] bool pcsx2() const noexcept { return m_pcsx2; }
        void pcsx2DmaStart(uint32_t core, uint32_t madr, uint32_t bcr, uint32_t chcr);
        [[nodiscard]] bool hasPendingIrqs() const noexcept { return !m_pendingIrqs.empty(); }
        [[nodiscard]] std::vector<int> takePendingIrqs()
        {
            std::vector<int> out;
            out.swap(m_pendingIrqs);
            return out;
        }
        // The latest IOP cycle the scheduler may run to before calling serviceEvents (UINT64_MAX with the old model).
        [[nodiscard]] uint64_t nextEventCycle(uint64_t now) const;
        void serviceEvents(uint64_t now);

    private:
        struct Voice
        {
            bool active = false;
            uint32_t startAddress = 0;   // SSA, halfwords
            uint32_t loopAddress = 0;    // LSAX, halfwords
            uint32_t nextAddress = 0;    // NAX, halfwords (current block)
            bool loopWritten = false;
            uint32_t counter = 0;        // 12-bit pitch fraction
            std::array<int16_t, 28> block{};
            uint32_t blockIndex = 28;
            int32_t history1 = 0;
            int32_t history2 = 0;
            int16_t previous = 0;
            int16_t current = 0;
            std::array<int16_t, 4> history{}; // the last four decoded samples, oldest first (Gaussian interpolation)
            bool blockEnd = false;
            bool blockRepeat = false;
            enum class Phase : uint8_t
            {
                Off,
                Attack,
                Decay,
                Sustain,
                Release
            } phase = Phase::Off;
            int32_t level = 0;           // ENVX 0..0x7FFF
            uint32_t envelopeWait = 0;
            int16_t volumeLeft = 0;
            int16_t volumeRight = 0;
            VolumeSweep sweepLeft;
            VolumeSweep sweepRight;
        };

        struct Core
        {
            std::array<Voice, 24> voices{};
            uint32_t endx = 0;
            std::deque<int16_t> admaLeft;
            std::deque<int16_t> admaRight;
            // LOCAL PATCH: the AutoDMA buffer in IOP RAM is read sample by sample as it is mixed (the SPU2 fetches it
            // just in time, while the driver is still decoding into the ring ahead of the DMA position); snapshotting
            // it at DMA start played the previous lap of the ring as an echo.
            const uint8_t *admaSource = nullptr;
            uint32_t admaSourceFrames = 0;
            uint32_t admaSourcePos = 0;
            bool masterVolumeWritten = false;
            VolumeSweep masterSweepLeft{0x7FFF, 0u};
            VolumeSweep masterSweepRight{0x7FFF, 0u};
            uint32_t keyOnCount = 0;
        };

        [[nodiscard]] uint16_t reg(uint32_t offset) const noexcept { return m_regs[(offset >> 1u) & 0x3FFu]; }
        [[nodiscard]] uint32_t coreReg(uint32_t core, uint32_t local) const noexcept { return reg(core * 0x400u + local); }
        [[nodiscard]] uint32_t voiceAddress(uint32_t core, uint32_t voice, uint32_t field) const noexcept;
        [[nodiscard]] uint32_t transferAddress(uint32_t core) const noexcept;
        void setTransferAddress(uint32_t core, uint32_t halfword);

        void keyOn(uint32_t core, uint32_t voice);
        void keyOff(uint32_t core, uint32_t voice);
        void decodeBlock(Voice &voice);
        void advanceSample(uint32_t core, uint32_t voiceIndex, Voice &voice);
        void stepEnvelope(uint32_t core, uint32_t voiceIndex, Voice &voice);
        void mixSample();
        void pushOutput(int16_t left, int16_t right);
        void onPcsx2Sample(int16_t left, int16_t right);
        [[nodiscard]] uint64_t cycleNow() const noexcept { return m_cycle ? *m_cycle : 0u; }

        std::array<uint16_t, 0x400> m_regs{};
        std::vector<uint16_t> m_ram;
        std::array<Core, 2> m_cores{};
        uint64_t m_nextSampleCycle = 0;
        bool m_clockStarted = false;

        std::mutex m_outputMutex;
        std::vector<int16_t> m_output;
        size_t m_outputRead = 0;
        size_t m_outputCount = 0;
        bool m_playing = false;     // jitter buffer: handing samples to the host (see readOutput)
        size_t m_fadeInLeft = 0;    // frames of fade-in still to apply
        double m_resamplePos = 0.0; // read position past the ring's head, in frames (dynamic rate control)

        bool m_pcsx2 = false;
        const uint64_t *m_cycle = nullptr;
        std::vector<int> m_pendingIrqs;
        uint64_t m_pcsx2Samples = 0;
    };
}
