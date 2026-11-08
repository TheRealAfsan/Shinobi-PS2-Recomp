#include "iop_spu2.h"
#include "iop_spu2_gauss.h"
#include "../spu2pcsx2/spu2_pcsx2.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// LOCAL PATCH: audio health counters, read once a second by the performance recorder (ps2_runtime.cpp).
std::atomic<uint64_t> g_ps2AudioCalls{0};      // host audio callbacks
std::atomic<uint64_t> g_ps2AudioUnderruns{0};  // callbacks the ring could not fill (silence inserted)
std::atomic<uint64_t> g_ps2AudioDropped{0};    // samples dropped because the ring was full
std::atomic<uint32_t> g_ps2AudioBuffered{0};   // frames waiting in the ring after the last callback

namespace ps2x::iop::detail
{
    // LOCAL DEBUG (PS2X_SPU2_TRACE=1): finds where a sound is played twice ("overlapping / playing twice"; audio_echo.py
    // shows a ~30 ms repeat in the output). Separate recordings of the voices and of the AutoDMA stream (both cores
    // summed, before master volume: captures/spu2_voices.raw, captures/spu2_adma.raw, 48 kHz s16 stereo), a log of
    // every AutoDMA transfer, re-key counts (same voice, same start address, within 50 ms), and once a second the core
    // routing registers the game set (MMIX, ADMAS, ADMA input volume AVOL, core input volume BVOL).
    namespace spu2trace
    {
        bool enabled()
        {
            static const bool s_on = []()
            {
                const char *e = std::getenv("PS2X_SPU2_TRACE");
                return e && e[0] == '1';
            }();
            return s_on;
        }
        uint64_t g_samples = 0; // output samples mixed so far
        uint64_t g_keyOns = 0, g_reKeys = 0, g_admaStarts = 0, g_admaLeftover = 0;
        uint64_t g_lastKey[2][24] = {};
        uint32_t g_lastKeyAddr[2][24] = {};
        uint32_t g_admaLogged = 0;
        FILE *open(const char *name)
        {
            return enabled() ? std::fopen(name, "wb") : nullptr;
        }
    }

    namespace
    {
        constexpr uint32_t kRamHalfwords = 1024u * 1024u; // 2 MB
        constexpr size_t kOutputCapacityFrames = 48000u / 4u; // 250 ms
        constexpr uint32_t kVoiceAddressBase = 0x1C0u;
        constexpr uint32_t kVoiceAddressStride = 0xCu;
        constexpr uint32_t kKeyOn = 0x1A0u;
        constexpr uint32_t kKeyOff = 0x1A4u;
        constexpr uint32_t kTransferAddress = 0x1A8u;
        constexpr uint32_t kDataPort = 0x1ACu;
        constexpr uint32_t kVoiceMixLeft = 0x188u;
        constexpr uint32_t kVoiceMixRight = 0x190u;
        constexpr uint32_t kEndx = 0x340u;
        constexpr uint32_t kCoreVolumeBase = 0x760u;
        constexpr uint32_t kCoreVolumeStride = 0x28u;

        constexpr int32_t kFilterPositive[5] = {0, 60, 115, 98, 122};
        constexpr int32_t kFilterNegative[5] = {0, 0, -52, -55, -60};

        int16_t clamp16(int32_t value)
        {
            return static_cast<int16_t>(std::clamp(value, -32768, 32767));
        }

        // LOCAL PATCH: volume registers. Fixed mode (bit 15 clear) holds a 15-bit signed value. Sweep mode (bit 15 set)
        // moves the volume by itself, one step per sample at the register's rate, with the envelope's rate law:
        // bit 14 exponential, bit 13 decrease, bit 12 inverted phase, bits 0-6 rate (shift = rate >> 2, step from rate & 3).
        // It used to keep the previous volume, so a sound the game faded out stayed at full volume - and a game that waits
        // for the current volume (VOLX) to reach zero before keying the voice off never did: sounds piled up over time.
        int16_t stepVolume(uint16_t value, VolumeSweep &sweep)
        {
            if ((value & 0x8000u) == 0u)
            {
                const int16_t fixed = static_cast<int16_t>(static_cast<uint16_t>(value << 1u));
                sweep.level = std::min(std::abs(static_cast<int32_t>(fixed)), 0x7FFF);
                sweep.wait = 0u;
                return fixed;
            }
            const bool exponential = (value & 0x4000u) != 0u;
            const bool decrease = (value & 0x2000u) != 0u;
            const bool negative = (value & 0x1000u) != 0u;
            const int32_t rate = value & 0x7F;
            const int32_t shift = rate >> 2;
            const int32_t stepBits = rate & 3;
            if (sweep.wait > 0u)
                --sweep.wait;
            else
            {
                const int32_t step = decrease ? -8 + stepBits : 7 - stepBits;
                uint32_t cycles = 1u << std::max(0, shift - 11);
                int32_t delta = step << std::max(0, 11 - shift);
                if (exponential && !decrease && sweep.level > 0x6000)
                    cycles *= 4u;
                if (exponential && decrease)
                    delta = (delta * sweep.level) >> 15;
                sweep.level = std::clamp(sweep.level + delta, 0, 0x7FFF);
                sweep.wait = cycles - 1u;
            }
            return static_cast<int16_t>(negative ? -sweep.level : sweep.level);
        }
    }

    Spu2::Spu2()
        : m_ram(kRamHalfwords, 0u),
          m_output(kOutputCapacityFrames * 2u, 0)
    {
    }

    void Spu2::reset()
    {
        if (m_pcsx2)
            spu2pcsx2::reset(cycleNow());
        m_pendingIrqs.clear();
        m_pcsx2Samples = 0u;
        m_regs.fill(0u);
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        for (Core &core : m_cores)
            core = Core{};
        m_clockStarted = false;
        m_nextSampleCycle = 0u;
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_outputRead = 0u;
        m_outputCount = 0u;
        m_playing = false;
        m_fadeInLeft = 0u;
        m_resamplePos = 0.0;
    }

    uint32_t Spu2::voiceAddress(uint32_t core, uint32_t voice, uint32_t field) const noexcept
    {
        const uint32_t local = kVoiceAddressBase + voice * kVoiceAddressStride + field;
        return ((coreReg(core, local) & 0xFu) << 16u) | coreReg(core, local + 2u);
    }

    uint32_t Spu2::transferAddress(uint32_t core) const noexcept
    {
        return ((coreReg(core, kTransferAddress) & 0xFu) << 16u) | coreReg(core, kTransferAddress + 2u);
    }

    void Spu2::setTransferAddress(uint32_t core, uint32_t halfword)
    {
        halfword &= kRamHalfwords - 1u;
        m_regs[(core * 0x400u + kTransferAddress) >> 1u] = static_cast<uint16_t>(halfword >> 16u);
        m_regs[(core * 0x400u + kTransferAddress + 2u) >> 1u] = static_cast<uint16_t>(halfword);
    }

    void Spu2::write16(uint32_t phys, uint16_t value)
    {
        if (m_pcsx2)
        {
            spu2pcsx2::write16(phys, value, cycleNow());
            return;
        }
        const uint32_t offset = phys - kBase;
        m_regs[(offset >> 1u) & 0x3FFu] = value;

        if (offset >= kCoreVolumeBase)
        {
            const uint32_t core = (offset - kCoreVolumeBase) / kCoreVolumeStride;
            const uint32_t local = (offset - kCoreVolumeBase) % kCoreVolumeStride;
            if (core < 2u && local < 4u)
                m_cores[core].masterVolumeWritten = true;
            return;
        }

        const uint32_t core = offset >> 10u;
        const uint32_t local = offset & 0x3FFu;
        if (local >= kVoiceAddressBase && local < kVoiceAddressBase + 24u * kVoiceAddressStride)
        {
            const uint32_t voice = (local - kVoiceAddressBase) / kVoiceAddressStride;
            const uint32_t field = (local - kVoiceAddressBase) % kVoiceAddressStride;
            if (field == 4u || field == 6u)
            {
                Voice &target = m_cores[core].voices[voice];
                target.loopAddress = voiceAddress(core, voice, 4u);
                target.loopWritten = true;
            }
            return;
        }

        switch (local)
        {
        case kKeyOn:
        case kKeyOn + 2u:
        {
            const uint32_t first = local == kKeyOn ? 0u : 16u;
            for (uint32_t bit = 0; bit < 16u && first + bit < 24u; ++bit)
                if ((value >> bit) & 1u)
                    keyOn(core, first + bit);
            break;
        }
        case kKeyOff:
        case kKeyOff + 2u:
        {
            const uint32_t first = local == kKeyOff ? 0u : 16u;
            for (uint32_t bit = 0; bit < 16u && first + bit < 24u; ++bit)
                if ((value >> bit) & 1u)
                    keyOff(core, first + bit);
            break;
        }
        case kDataPort:
        {
            const uint32_t address = transferAddress(core);
            m_ram[address & (kRamHalfwords - 1u)] = value;
            setTransferAddress(core, address + 1u);
            break;
        }
        default:
            break;
        }
    }

    bool Spu2::read16(uint32_t phys, uint16_t &value) const
    {
        if (m_pcsx2)
        {
            value = spu2pcsx2::read16(phys, cycleNow());
            return true;
        }
        const uint32_t offset = phys - kBase;
        if (offset >= kCoreVolumeBase)
            return false;
        const uint32_t core = offset >> 10u;
        const uint32_t local = offset & 0x3FFu;

        if (local < 24u * 0x10u)
        {
            const Voice &voice = m_cores[core].voices[local >> 4u];
            switch (local & 0xFu)
            {
            case 0xAu:
                value = static_cast<uint16_t>(voice.level);
                return true;
            case 0xCu:
                value = static_cast<uint16_t>(voice.volumeLeft);
                return true;
            case 0xEu:
                value = static_cast<uint16_t>(voice.volumeRight);
                return true;
            default:
                return false;
            }
        }
        if (local >= kVoiceAddressBase && local < kVoiceAddressBase + 24u * kVoiceAddressStride)
        {
            const Voice &voice = m_cores[core].voices[(local - kVoiceAddressBase) / kVoiceAddressStride];
            const uint32_t field = (local - kVoiceAddressBase) % kVoiceAddressStride;
            if (field == 8u)
            {
                value = static_cast<uint16_t>((voice.nextAddress >> 16u) & 0xFu);
                return true;
            }
            if (field == 0xAu)
            {
                value = static_cast<uint16_t>(voice.nextAddress);
                return true;
            }
            return false;
        }
        if (local == kEndx)
        {
            value = static_cast<uint16_t>(m_cores[core].endx);
            return true;
        }
        if (local == kEndx + 2u)
        {
            value = static_cast<uint16_t>(m_cores[core].endx >> 16u);
            return true;
        }
        return false;
    }

    void Spu2::dmaWrite(uint32_t core, const uint8_t *data, uint32_t bytes, bool autoDma)
    {
        if (core > 1u || data == nullptr)
            return;
        if (autoDma)
        {
            // AutoDMA blocks are 512 bytes of left samples then 512 of right.
            Core &target = m_cores[core];
            if (spu2trace::enabled())
            {
                const uint32_t leftover = target.admaSource && target.admaSourcePos < target.admaSourceFrames
                                              ? target.admaSourceFrames - target.admaSourcePos
                                              : 0u;
                ++spu2trace::g_admaStarts;
                spu2trace::g_admaLeftover += leftover;
                if (spu2trace::g_admaLogged < 400u)
                {
                    ++spu2trace::g_admaLogged;
                    std::fprintf(stderr, "[spu2-adma] core %u t=%llu samples src=%p bytes=%u leftover=%u queued=%zu same-src=%d\n", core,
                                 static_cast<unsigned long long>(spu2trace::g_samples), static_cast<const void *>(data), bytes,
                                 leftover, target.admaLeft.size(), target.admaSource == data ? 1 : 0);
                }
            }
            // Anything the previous transfer has not played yet is kept (as it stands now) ahead of the new one.
            while (target.admaSource && target.admaSourcePos < target.admaSourceFrames)
            {
                const uint32_t pos = target.admaSourcePos++;
                int16_t left;
                int16_t right;
                std::memcpy(&left, target.admaSource + (pos / 256u) * 1024u + (pos % 256u) * 2u, sizeof(left));
                std::memcpy(&right, target.admaSource + (pos / 256u) * 1024u + 512u + (pos % 256u) * 2u, sizeof(right));
                target.admaLeft.push_back(left);
                target.admaRight.push_back(right);
            }
            // PS2X_SPU2_SNAPSHOT=1 restores the old behaviour (copy the whole buffer at DMA start) for A/B tests.
            static const bool s_snapshot = []()
            {
                const char *value = std::getenv("PS2X_SPU2_SNAPSHOT");
                return value && value[0] == '1';
            }();
            if (s_snapshot)
            {
                for (uint32_t block = 0; block + 1024u <= bytes; block += 1024u)
                {
                    for (uint32_t i = 0; i < 256u; ++i)
                    {
                        int16_t left;
                        int16_t right;
                        std::memcpy(&left, data + block + i * 2u, sizeof(left));
                        std::memcpy(&right, data + block + 512u + i * 2u, sizeof(right));
                        target.admaLeft.push_back(left);
                        target.admaRight.push_back(right);
                    }
                }
            }
            else
            {
                target.admaSource = data;
                target.admaSourceFrames = (bytes / 1024u) * 256u;
                target.admaSourcePos = 0u;
            }
            // Never let a stalled consumer grow the queue without bound.
            while (target.admaLeft.size() > 48000u)
            {
                target.admaLeft.pop_front();
                target.admaRight.pop_front();
            }
            return;
        }

        uint32_t address = transferAddress(core);
        for (uint32_t i = 0; i + 1u < bytes; i += 2u)
        {
            uint16_t halfword;
            std::memcpy(&halfword, data + i, sizeof(halfword));
            m_ram[address & (kRamHalfwords - 1u)] = halfword;
            ++address;
        }
        setTransferAddress(core, address);
    }

    void Spu2::keyOn(uint32_t core, uint32_t voiceIndex)
    {
        ++m_cores[core].keyOnCount;
        if (spu2trace::enabled() && core < 2u && voiceIndex < 24u)
        {
            const uint32_t start = voiceAddress(core, voiceIndex, 0u);
            ++spu2trace::g_keyOns;
            if (spu2trace::g_lastKey[core][voiceIndex] != 0u && spu2trace::g_samples - spu2trace::g_lastKey[core][voiceIndex] < 2400u &&
                spu2trace::g_lastKeyAddr[core][voiceIndex] == start)
                ++spu2trace::g_reKeys;
            spu2trace::g_lastKey[core][voiceIndex] = spu2trace::g_samples ? spu2trace::g_samples : 1u;
            spu2trace::g_lastKeyAddr[core][voiceIndex] = start;
        }
        Voice &voice = m_cores[core].voices[voiceIndex];
        voice = Voice{};
        voice.active = true;
        voice.startAddress = voiceAddress(core, voiceIndex, 0u);
        voice.nextAddress = voice.startAddress;
        voice.loopAddress = voiceAddress(core, voiceIndex, 4u);
        voice.phase = Voice::Phase::Attack;
        voice.blockIndex = 28u;
        m_cores[core].endx &= ~(1u << voiceIndex);
        decodeBlock(voice);
    }

    void Spu2::keyOff(uint32_t core, uint32_t voiceIndex)
    {
        Voice &voice = m_cores[core].voices[voiceIndex];
        if (voice.active && voice.phase != Voice::Phase::Off)
        {
            voice.phase = Voice::Phase::Release;
            voice.envelopeWait = 0u;
        }
    }

    void Spu2::decodeBlock(Voice &voice)
    {
        const uint32_t address = voice.nextAddress & (kRamHalfwords - 1u);
        const uint16_t header = m_ram[address];
        uint32_t shift = header & 0xFu;
        if (shift > 12u)
            shift = 9u;
        const uint32_t filter = std::min<uint32_t>((header >> 4u) & 0x7u, 4u);
        const uint32_t flags = header >> 8u;

        if ((flags & 0x4u) != 0u && !voice.loopWritten)
            voice.loopAddress = voice.nextAddress;
        voice.blockEnd = (flags & 0x1u) != 0u;
        voice.blockRepeat = (flags & 0x2u) != 0u;

        for (uint32_t i = 0; i < 28u; ++i)
        {
            const uint16_t word = m_ram[(address + 1u + i / 4u) & (kRamHalfwords - 1u)];
            const int32_t nibble = (word >> ((i % 4u) * 4u)) & 0xF;
            int32_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12u)) >> shift;
            sample += (voice.history1 * kFilterPositive[filter] + voice.history2 * kFilterNegative[filter] + 32) >> 6;
            sample = clamp16(sample);
            voice.history2 = voice.history1;
            voice.history1 = sample;
            voice.block[i] = static_cast<int16_t>(sample);
        }
        voice.blockIndex = 0u;
    }

    void Spu2::advanceSample(uint32_t core, uint32_t voiceIndex, Voice &voice)
    {
        const uint32_t pitch = std::min<uint32_t>(coreReg(core, voiceIndex * 0x10u + 4u), 0x3FFFu);
        voice.counter += pitch;
        while (voice.counter >= 0x1000u && voice.active)
        {
            voice.counter -= 0x1000u;
            if (voice.blockIndex >= 28u)
            {
                if (voice.blockEnd)
                {
                    m_cores[core].endx |= 1u << voiceIndex;
                    voice.nextAddress = voice.loopAddress;
                    if (!voice.blockRepeat)
                    {
                        voice.active = false;
                        voice.phase = Voice::Phase::Off;
                        voice.level = 0;
                        break;
                    }
                }
                else
                {
                    voice.nextAddress += 8u;
                }
                decodeBlock(voice);
            }
            voice.previous = voice.current;
            voice.current = voice.block[voice.blockIndex++];
            voice.history[0] = voice.history[1];
            voice.history[1] = voice.history[2];
            voice.history[2] = voice.history[3];
            voice.history[3] = voice.current;
        }
    }

    void Spu2::stepEnvelope(uint32_t core, uint32_t voiceIndex, Voice &voice)
    {
        if (voice.envelopeWait > 0u)
        {
            --voice.envelopeWait;
            return;
        }
        const uint16_t adsr1 = static_cast<uint16_t>(coreReg(core, voiceIndex * 0x10u + 6u));
        const uint16_t adsr2 = static_cast<uint16_t>(coreReg(core, voiceIndex * 0x10u + 8u));
        const int32_t sustainLevel = std::min<int32_t>(((adsr1 & 0xF) + 1) * 0x800, 0x7FFF);

        bool exponential = false;
        bool decrease = false;
        int32_t shift = 0;
        int32_t step = 0;
        switch (voice.phase)
        {
        case Voice::Phase::Attack:
            exponential = (adsr1 & 0x8000u) != 0u;
            shift = (adsr1 >> 10u) & 0x1F;
            step = 7 - ((adsr1 >> 8u) & 0x3);
            break;
        case Voice::Phase::Decay:
            exponential = true;
            decrease = true;
            shift = ((adsr1 >> 4u) & 0xF) << 2;
            step = -8;
            break;
        case Voice::Phase::Sustain:
            exponential = (adsr2 & 0x8000u) != 0u;
            decrease = (adsr2 & 0x4000u) != 0u;
            shift = (adsr2 >> 8u) & 0x1F;
            step = decrease ? -8 + ((adsr2 >> 6u) & 0x3) : 7 - ((adsr2 >> 6u) & 0x3);
            break;
        case Voice::Phase::Release:
            exponential = (adsr2 & 0x20u) != 0u;
            decrease = true;
            shift = (adsr2 & 0x1F) << 2;
            step = -8;
            break;
        case Voice::Phase::Off:
            return;
        }

        uint32_t cycles = 1u << std::max(0, shift - 11);
        int32_t delta = step << std::max(0, 11 - shift);
        if (exponential && !decrease && voice.level > 0x6000)
            cycles *= 4u;
        if (exponential && decrease)
            delta = (delta * voice.level) >> 15;
        voice.level = std::clamp(voice.level + delta, 0, 0x7FFF);
        voice.envelopeWait = cycles - 1u;

        switch (voice.phase)
        {
        case Voice::Phase::Attack:
            if (voice.level >= 0x7FFF)
            {
                voice.phase = Voice::Phase::Decay;
                voice.envelopeWait = 0u;
            }
            break;
        case Voice::Phase::Decay:
            if (voice.level <= sustainLevel)
            {
                voice.phase = Voice::Phase::Sustain;
                voice.envelopeWait = 0u;
            }
            break;
        case Voice::Phase::Release:
            if (voice.level <= 0)
            {
                voice.active = false;
                voice.phase = Voice::Phase::Off;
            }
            break;
        default:
            break;
        }
    }

    void Spu2::mixSample()
    {
        int32_t inputLeft = 0;
        int32_t inputRight = 0;
        int32_t traceVoicesLeft = 0, traceVoicesRight = 0, traceAdmaLeft = 0, traceAdmaRight = 0;
        for (uint32_t core = 0; core < 2u; ++core)
        {
            Core &state = m_cores[core];
            const uint32_t mixLeft = coreReg(core, kVoiceMixLeft) | (coreReg(core, kVoiceMixLeft + 2u) << 16u);
            const uint32_t mixRight = coreReg(core, kVoiceMixRight) | (coreReg(core, kVoiceMixRight + 2u) << 16u);
            int32_t left = 0;
            int32_t right = 0;
            for (uint32_t v = 0; v < 24u; ++v)
            {
                Voice &voice = state.voices[v];
                if (!voice.active)
                    continue;
                stepEnvelope(core, v, voice);
                advanceSample(core, v, voice);
                if (!voice.active)
                    continue;
                // LOCAL PATCH: the SPU2's own 4-tap Gaussian interpolation (PCSX2's table). A straight line between two samples,
                // as before, lets through high-frequency hash that the hardware filters out: heard as a fuzzy, gritty tone,
                // worst on pitched-up sounds. PS2X_SPU2_INTERP=linear restores the old interpolation.
                static const bool s_linear = []() { const char *v = std::getenv("PS2X_SPU2_INTERP"); return v && std::strcmp(v, "linear") == 0; }();
                int32_t interpolated;
                if (s_linear)
                {
                    interpolated = voice.previous +
                                   (((static_cast<int32_t>(voice.current) - voice.previous) * static_cast<int32_t>(voice.counter)) >> 12);
                }
                else
                {
                    const auto &taps = kSpu2GaussTable[(voice.counter & 0x0FF0u) >> 4];
                    interpolated = ((taps[0] * voice.history[0]) >> 15) + ((taps[1] * voice.history[1]) >> 15) +
                                   ((taps[2] * voice.history[2]) >> 15) + ((taps[3] * voice.history[3]) >> 15);
                    interpolated = std::clamp(interpolated, -32768, 32767);
                }
                const int32_t sample = (interpolated * voice.level) >> 15;
                voice.volumeLeft = stepVolume(static_cast<uint16_t>(coreReg(core, v * 0x10u)), voice.sweepLeft);
                voice.volumeRight = stepVolume(static_cast<uint16_t>(coreReg(core, v * 0x10u + 2u)), voice.sweepRight);
                if ((mixLeft >> v) & 1u)
                    left += (sample * voice.volumeLeft) >> 15;
                if ((mixRight >> v) & 1u)
                    right += (sample * voice.volumeRight) >> 15;
            }
            traceVoicesLeft += left;
            traceVoicesRight += right;
            if (!state.admaLeft.empty())
            {
                left += state.admaLeft.front();
                right += state.admaRight.front();
                traceAdmaLeft += state.admaLeft.front();
                traceAdmaRight += state.admaRight.front();
                state.admaLeft.pop_front();
                state.admaRight.pop_front();
            }
            else if (state.admaSource && state.admaSourcePos < state.admaSourceFrames)
            {
                const uint32_t pos = state.admaSourcePos++;
                int16_t sampleLeft;
                int16_t sampleRight;
                std::memcpy(&sampleLeft, state.admaSource + (pos / 256u) * 1024u + (pos % 256u) * 2u, sizeof(sampleLeft));
                std::memcpy(&sampleRight, state.admaSource + (pos / 256u) * 1024u + 512u + (pos % 256u) * 2u, sizeof(sampleRight));
                left += sampleLeft;
                right += sampleRight;
                traceAdmaLeft += sampleLeft;
                traceAdmaRight += sampleRight;
            }
            // Core 0 output feeds core 1's input.
            left += inputLeft;
            right += inputRight;

            int32_t masterLeft = 0x7FFF;
            int32_t masterRight = 0x7FFF;
            if (state.masterVolumeWritten)
            {
                const uint32_t base = kCoreVolumeBase + core * kCoreVolumeStride;
                masterLeft = stepVolume(reg(base), state.masterSweepLeft);
                masterRight = stepVolume(reg(base + 2u), state.masterSweepRight);
            }
            inputLeft = clamp16((left * masterLeft) >> 15);
            inputRight = clamp16((right * masterRight) >> 15);
        }

        if (spu2trace::enabled())
        {
            static FILE *s_voices = spu2trace::open("captures/spu2_voices.raw");
            static FILE *s_adma = spu2trace::open("captures/spu2_adma.raw");
            if (s_voices)
            {
                const int16_t pair[2] = {static_cast<int16_t>(clamp16(traceVoicesLeft)), static_cast<int16_t>(clamp16(traceVoicesRight))};
                std::fwrite(pair, sizeof(pair), 1, s_voices);
            }
            if (s_adma)
            {
                const int16_t pair[2] = {static_cast<int16_t>(clamp16(traceAdmaLeft)), static_cast<int16_t>(clamp16(traceAdmaRight))};
                std::fwrite(pair, sizeof(pair), 1, s_adma);
            }
            if ((++spu2trace::g_samples % 48000u) == 0u)
            {
                // Per core: MMIX (0x198), ADMAS (0x1B0), AVOL L/R (ADMA input volume, 0x768/0x76A + core*0x28),
                // BVOL L/R (core input volume, 0x76C/0x76E + core*0x28), master MVOL L/R.
                std::fprintf(stderr, "[spu2] t=%llus keyons=%llu rekeys<50ms=%llu adma-starts=%llu adma-leftover=%llu",
                             static_cast<unsigned long long>(spu2trace::g_samples / 48000u),
                             static_cast<unsigned long long>(spu2trace::g_keyOns), static_cast<unsigned long long>(spu2trace::g_reKeys),
                             static_cast<unsigned long long>(spu2trace::g_admaStarts),
                             static_cast<unsigned long long>(spu2trace::g_admaLeftover));
                for (uint32_t c = 0; c < 2u; ++c)
                {
                    const uint32_t vb = kCoreVolumeBase + c * kCoreVolumeStride;
                    std::fprintf(stderr, " | core%u MMIX=%04x ADMAS=%04x AVOL=%04x/%04x BVOL=%04x/%04x MVOL=%04x/%04x", c,
                                 coreReg(c, 0x198u), coreReg(c, 0x1B0u), reg(vb + 8u), reg(vb + 10u), reg(vb + 12u), reg(vb + 14u),
                                 reg(vb), reg(vb + 2u));
                }
                std::fprintf(stderr, "\n");
                spu2trace::g_keyOns = spu2trace::g_reKeys = spu2trace::g_admaStarts = spu2trace::g_admaLeftover = 0;
            }
        }

        pushOutput(static_cast<int16_t>(inputLeft), static_cast<int16_t>(inputRight));
    }

    void Spu2::pushOutput(int16_t left, int16_t right)
    {
        // LOCAL DEBUG (PS2X_SPU2_DUMP=<file>): raw 48 kHz s16 stereo of everything mixed, for offline analysis.
        {
            static FILE *dump = []() -> FILE *
            {
                const char *path = std::getenv("PS2X_SPU2_DUMP");
                return path ? std::fopen(path, "wb") : nullptr;
            }();
            if (dump)
            {
                const int16_t pair[2] = {left, right};
                std::fwrite(pair, sizeof(pair), 1, dump);
            }
        }
        std::lock_guard<std::mutex> lock(m_outputMutex);
        const size_t capacity = m_output.size() / 2u;
        if (m_outputCount == capacity)
        {
            m_outputRead = (m_outputRead + 1u) % capacity;
            --m_outputCount;
            g_ps2AudioDropped.fetch_add(1u, std::memory_order_relaxed);
        }
        const size_t write = (m_outputRead + m_outputCount) % capacity;
        m_output[write * 2u] = left;
        m_output[write * 2u + 1u] = right;
        ++m_outputCount;
    }

    // ------------------------------------------------------------------ PCSX2 SPU2 core (PS2X_SPU2=pcsx2, default)

    bool Spu2::pcsx2Wanted()
    {
        static const bool s_wanted = []()
        {
            const char *e = std::getenv("PS2X_SPU2");
            const bool legacy = e && std::strcmp(e, "legacy") == 0;
            std::fprintf(stderr, "[spu2] sound chip: %s\n", legacy ? "legacy (iop_spu2.cpp)" : "PCSX2 SPU2 core (v2.8.0)");
            return !legacy;
        }();
        return s_wanted;
    }

    void Spu2::attachPcsx2(uint8_t *iopRam, uint32_t *const dma4[4], uint32_t *const dma7[4], const uint64_t *cycle)
    {
        m_pcsx2 = pcsx2Wanted();
        m_cycle = cycle;
        if (!m_pcsx2)
            return;
        spu2pcsx2::Hooks hooks;
        hooks.user = this;
        hooks.raiseIrq = [](void *user, int irq) { static_cast<Spu2 *>(user)->m_pendingIrqs.push_back(irq); };
        hooks.output = [](void *user, int16_t left, int16_t right) { static_cast<Spu2 *>(user)->onPcsx2Sample(left, right); };
        spu2pcsx2::bind(iopRam, dma4, dma7, hooks);
        spu2pcsx2::reset(cycleNow());
    }

    void Spu2::onPcsx2Sample(int16_t left, int16_t right)
    {
        pushOutput(left, right);
        // PS2X_SPU2_TRACE=1: once a second (48000 samples) the chip's activity, in the same [spu2] line the session
        // tools read, plus voice counts: "long" voices have been playing for over 20 s since their key-on (stuck sounds).
        static const bool s_trace = []() { const char *e = std::getenv("PS2X_SPU2_TRACE"); return e && e[0] == '1'; }();
        if (!s_trace || ++m_pcsx2Samples % 48000u != 0u)
            return;
        const spu2pcsx2::Stats stats = spu2pcsx2::takeStats(cycleNow());
        std::fprintf(stderr,
                     "[spu2] t=%llus keyons=%llu rekeys<50ms=0 adma-starts=0 adma-leftover=0 | pcsx2: irq=%llu dma4=%llu dma7=%llu | "
                     "core0 voices=%u releasing=%u long=%u | core1 voices=%u releasing=%u long=%u\n",
                     static_cast<unsigned long long>(m_pcsx2Samples / 48000u), static_cast<unsigned long long>(stats.keyOns),
                     static_cast<unsigned long long>(stats.spuIrqs), static_cast<unsigned long long>(stats.dmaIrqs[0]),
                     static_cast<unsigned long long>(stats.dmaIrqs[1]), stats.activeVoices[0], stats.releasingVoices[0],
                     stats.longVoices[0], stats.activeVoices[1], stats.releasingVoices[1], stats.longVoices[1]);
        // Loudness: where the static comes from when many sounds play (see spu2_pcsx2.h Stats).
        std::fprintf(stderr,
                     "[spu2-mix] t=%llus out clipped %.2f%% | core0 level now %.2f peak %.2f (voices peak %u, loudest %.2f) "
                     "MVOL %04x FX %d vol %04x ESA %05x EEA %05x | core1 level now %.2f peak %.2f (voices peak %u, loudest %.2f) "
                     "MVOL %04x FX %d vol %04x ESA %05x EEA %05x EXT %04x INP %04x/%04x\n",
                     static_cast<unsigned long long>(m_pcsx2Samples / 48000u),
                     stats.outputSamples ? 100.0 * static_cast<double>(stats.outputClipped) / static_cast<double>(stats.outputSamples) : 0.0,
                     stats.levelSum[0], stats.levelPeak[0], stats.voicesPeak[0], stats.loudest[0], stats.masterVol[0] & 0xFFFF,
                     stats.fxEnable[0] ? 1 : 0, stats.fxVol[0] & 0xFFFF, stats.effectsStart[0], stats.effectsEnd[0], stats.levelSum[1],
                     stats.levelPeak[1], stats.voicesPeak[1], stats.loudest[1], stats.masterVol[1] & 0xFFFF, stats.fxEnable[1] ? 1 : 0,
                     stats.fxVol[1] & 0xFFFF, stats.effectsStart[1], stats.effectsEnd[1], stats.extVol[1] & 0xFFFF,
                     stats.inpVol[0] & 0xFFFF, stats.inpVol[1] & 0xFFFF);
        // Left / right (static heard in the right ear only): where the two sides differ.
        std::fprintf(stderr,
                     "[spu2-lr] t=%llus clipped L %llu R %llu | core0 voices L %.2f R %.2f (phase-inverted L %u R %u) MVOL %d/%d "
                     "FX %d/%d INP %d/%d | core1 voices L %.2f R %.2f (phase-inverted L %u R %u) MVOL %d/%d FX %d/%d EXT %d/%d "
                     "INP %d/%d\n",
                     static_cast<unsigned long long>(m_pcsx2Samples / 48000u), static_cast<unsigned long long>(stats.clippedLeft),
                     static_cast<unsigned long long>(stats.clippedRight), stats.levelSidePeak[0][0], stats.levelSidePeak[0][1],
                     stats.phaseInvertedVoices[0][0], stats.phaseInvertedVoices[0][1], stats.masterVol[0], stats.masterVolR[0],
                     stats.fxVol[0], stats.fxVolR[0], stats.inpVol[0], stats.inpVolR[0], stats.levelSidePeak[1][0],
                     stats.levelSidePeak[1][1], stats.phaseInvertedVoices[1][0], stats.phaseInvertedVoices[1][1], stats.masterVol[1],
                     stats.masterVolR[1], stats.fxVol[1], stats.fxVolR[1], stats.extVol[1], stats.extVolR[1], stats.inpVol[1],
                     stats.inpVolR[1]);
        // Streamed audio blocks (music / voices): a "stale" block repeats the previous one = refilled too late.
        std::fprintf(stderr,
                     "[spu2-adma] t=%llus core0 blocks L %llu R %llu stale L %llu R %llu zero L %llu R %llu | core1 blocks L %llu "
                     "R %llu stale L %llu R %llu zero L %llu R %llu\n",
                     static_cast<unsigned long long>(m_pcsx2Samples / 48000u),
                     static_cast<unsigned long long>(stats.admaBlocks[0][0]), static_cast<unsigned long long>(stats.admaBlocks[0][1]),
                     static_cast<unsigned long long>(stats.admaStale[0][0]), static_cast<unsigned long long>(stats.admaStale[0][1]),
                     static_cast<unsigned long long>(stats.admaZero[0][0]), static_cast<unsigned long long>(stats.admaZero[0][1]),
                     static_cast<unsigned long long>(stats.admaBlocks[1][0]), static_cast<unsigned long long>(stats.admaBlocks[1][1]),
                     static_cast<unsigned long long>(stats.admaStale[1][0]), static_cast<unsigned long long>(stats.admaStale[1][1]),
                     static_cast<unsigned long long>(stats.admaZero[1][0]), static_cast<unsigned long long>(stats.admaZero[1][1]));
        // Sharp edges: stream input vs final output, per side (which of the two carries the right-side static).
        std::fprintf(stderr, "[spu2-edges] t=%llus stream core0 L %llu R %llu | stream core1 L %llu R %llu | output L %llu R %llu\n",
                     static_cast<unsigned long long>(m_pcsx2Samples / 48000u), static_cast<unsigned long long>(stats.inputJumps[0][0]),
                     static_cast<unsigned long long>(stats.inputJumps[0][1]), static_cast<unsigned long long>(stats.inputJumps[1][0]),
                     static_cast<unsigned long long>(stats.inputJumps[1][1]), static_cast<unsigned long long>(stats.outputJumps[0]),
                     static_cast<unsigned long long>(stats.outputJumps[1]));
    }

    void Spu2::pcsx2DmaStart(uint32_t core, uint32_t madr, uint32_t bcr, uint32_t chcr)
    {
        spu2pcsx2::dmaStart(static_cast<int>(core), madr, bcr, chcr, cycleNow());
    }

    uint64_t Spu2::nextEventCycle(uint64_t now) const
    {
        return m_pcsx2 ? spu2pcsx2::nextEventCycle(now) : UINT64_MAX;
    }

    void Spu2::serviceEvents(uint64_t now)
    {
        if (m_pcsx2)
            spu2pcsx2::serviceEvents(now);
    }

    void Spu2::advanceTo(uint64_t cycle)
    {
        if (m_pcsx2)
        {
            spu2pcsx2::advanceTo(cycle);
            return;
        }
        static const bool trace = std::getenv("PS2X_SPU2_TRACE") != nullptr;
        if (trace && m_clockStarted && cycle / (kIopCyclesPerSample * 48000u * 2u) != m_nextSampleCycle / (kIopCyclesPerSample * 48000u * 2u))
        {
            for (uint32_t c = 0; c < 2u; ++c)
            {
                uint32_t active = 0u;
                for (const Voice &v : m_cores[c].voices)
                    active += v.active ? 1u : 0u;
                const uint32_t vb = kCoreVolumeBase + c * kCoreVolumeStride;
                std::fprintf(stderr, "[spu2] core%u active=%u keyons=%u mvol=%04x/%04x evol=%04x/%04x avol=%04x/%04x bvol=%04x/%04x mmix=%04x vmix=%04x%04x/%04x%04x attr=%04x admaq=%zu\n",
                             c, active, m_cores[c].keyOnCount, reg(vb), reg(vb + 2u), reg(vb + 4u), reg(vb + 6u), reg(vb + 8u), reg(vb + 10u),
                             reg(vb + 12u), reg(vb + 14u), coreReg(c, 0x198u), coreReg(c, kVoiceMixLeft + 2u), coreReg(c, kVoiceMixLeft),
                             coreReg(c, kVoiceMixRight + 2u), coreReg(c, kVoiceMixRight), coreReg(c, 0x19Au), m_cores[c].admaLeft.size());
            }
        }
        if (!m_clockStarted)
        {
            m_clockStarted = true;
            m_nextSampleCycle = cycle;
            return;
        }
        uint32_t produced = 0u;
        while (m_nextSampleCycle + kIopCyclesPerSample <= cycle)
        {
            mixSample();
            m_nextSampleCycle += kIopCyclesPerSample;
            if (++produced >= 48000u)
            {
                // Over a second behind (host stall): resynchronise.
                m_nextSampleCycle = cycle;
                break;
            }
        }
    }

    size_t Spu2::readOutput(int16_t *destination, size_t frames)
    {
        // LOCAL PATCH: jitter buffer. The samples are mixed as the emulated IOP runs, which is paced by the game's CPU thread; a
        // stall of that thread (a 60 ms hitch is common in a fight) leaves the host's audio callback (about 21 ms per call)
        // with nothing to play: silence is inserted, and the burst that follows overflows the ring and drops the oldest
        // samples. Both are heard as clicks and fuzz. So playback only starts once PS2X_AUDIO_PREFILL_MS (default 60, 0 = off)
        // of sound is waiting, and after an underrun it waits again; the edges are faded over a few ms so they do not click.
        static const size_t kPrefillFrames = []() -> size_t
        {
            const char *value = std::getenv("PS2X_AUDIO_PREFILL_MS");
            const int ms = value ? std::atoi(value) : 60;
            return ms <= 0 ? 0u : static_cast<size_t>(std::min(ms, 200)) * 48u;
        }();
        constexpr size_t kFadeFrames = 96; // 2 ms
        std::lock_guard<std::mutex> lock(m_outputMutex);
        const size_t capacity = m_output.size() / 2u;
        g_ps2AudioCalls.fetch_add(1u, std::memory_order_relaxed);
        g_ps2AudioBuffered.store(static_cast<uint32_t>(m_outputCount), std::memory_order_relaxed);
        if (frames > m_outputCount)
            g_ps2AudioUnderruns.fetch_add(1u, std::memory_order_relaxed); // counted before the jitter buffer holds it back
        if (kPrefillFrames != 0u && !m_playing)
        {
            if (m_outputCount < kPrefillFrames)
                return 0u; // the caller fills with silence
            m_playing = true;
            m_fadeInLeft = kFadeFrames;
        }
        size_t count = 0;
        if (kPrefillFrames == 0u)
        {
            count = std::min(frames, m_outputCount);
            for (size_t i = 0; i < count; ++i)
            {
                destination[i * 2u] = m_output[m_outputRead * 2u];
                destination[i * 2u + 1u] = m_output[m_outputRead * 2u + 1u];
                m_outputRead = (m_outputRead + 1u) % capacity;
            }
            m_outputCount -= count;
        }
        else
        {
            // Dynamic rate control. The mixer runs a little faster than the host's audio clock (measured: 166 samples a second
            // had to be thrown away because the ring stayed full, and every dropped sample is a click, the static in the
            // sound). So the host reads slightly faster or slower than 48 kHz, by linear interpolation, to hold the ring at about
            // 100 ms: at most 1% (17 cents), normally a few tenths of a percent, which cannot be heard as pitch.
            const double target = std::max(static_cast<double>(kPrefillFrames) * 1.6, 4800.0);
            const double error = (static_cast<double>(m_outputCount) - target) / target;
            const double ratio = 1.0 + std::clamp(error * 0.02, -0.01, 0.01);
            while (count < frames)
            {
                const size_t index = static_cast<size_t>(m_resamplePos);
                if (index + 1u >= m_outputCount)
                    break; // not enough queued to interpolate: the ring ran dry
                const double fraction = m_resamplePos - static_cast<double>(index);
                const size_t a = (m_outputRead + index) % capacity;
                const size_t b = (a + 1u) % capacity;
                for (size_t channel = 0; channel < 2u; ++channel)
                {
                    const double s0 = m_output[a * 2u + channel];
                    const double s1 = m_output[b * 2u + channel];
                    destination[count * 2u + channel] = static_cast<int16_t>(std::lround(s0 + (s1 - s0) * fraction));
                }
                m_resamplePos += ratio;
                ++count;
            }
            const size_t consumed = std::min(static_cast<size_t>(m_resamplePos), m_outputCount);
            m_resamplePos -= static_cast<double>(consumed);
            m_outputRead = (m_outputRead + consumed) % capacity;
            m_outputCount -= consumed;
        }
        if (kPrefillFrames != 0u)
        {
            if (m_fadeInLeft != 0u)
            {
                const size_t n = std::min(m_fadeInLeft, count);
                for (size_t i = 0; i < n; ++i)
                {
                    const int32_t gain = static_cast<int32_t>((kFadeFrames - m_fadeInLeft + i) * 256u / kFadeFrames);
                    destination[i * 2u] = static_cast<int16_t>(destination[i * 2u] * gain / 256);
                    destination[i * 2u + 1u] = static_cast<int16_t>(destination[i * 2u + 1u] * gain / 256);
                }
                m_fadeInLeft -= n;
            }
            if (count < frames)
            {
                // Ran dry: fade out what there is and wait for the buffer to fill again.
                const size_t n = std::min(kFadeFrames, count);
                for (size_t i = 0; i < n; ++i)
                {
                    const size_t at = count - n + i;
                    const int32_t gain = static_cast<int32_t>((n - i) * 256u / n);
                    destination[at * 2u] = static_cast<int16_t>(destination[at * 2u] * gain / 256);
                    destination[at * 2u + 1u] = static_cast<int16_t>(destination[at * 2u + 1u] * gain / 256);
                }
                m_playing = false;
                m_resamplePos = 0.0;
            }
        }
        return count;
    }
}
