#include "iop_cpu_execute.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <vector>

namespace ps2x::iop::detail
{
    IopCpuCore::IopCpuCore(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopCpuCore::raiseException(IopCpuState &cpu, uint32_t code, uint32_t faultPc, bool delaySlot, std::optional<uint32_t> badAddress) const
    {
        uint32_t cause = cpu.cop0[13] & ~0x7Cu;
        cause |= (code & 0x1Fu) << 2u;
        if (delaySlot)
        {
            cause |= 0x80000000u;
            cpu.cop0[14] = faultPc - 4u;
        }
        else
        {
            cause &= ~0x80000000u;
            cpu.cop0[14] = faultPc;
        }
        cpu.cop0[13] = cause;
        if (badAddress)
            cpu.cop0[8] = *badAddress;
        const uint32_t status = cpu.cop0[12];
        cpu.cop0[12] = (status & ~0x3Fu) | ((status << 2u) & 0x3Fu);
        cpu.pc = (status & (1u << 22u)) ? 0xBFC00180u : 0x80000080u;
        cpu.branchPending = false;
        cpu.pendingLoad = false;
        cpu.exception = true;
    }

    bool IopCpuCore::executeInstruction(IopCpuState &cpu)
    {
        return executeInstructionInline(cpu);
    }

    // LOCAL DEBUG: see PS2X_IOP_WATCH in iop_cpu_execute.h. Per second: for each side (left / right blocks), the instruction
    // addresses that wrote into the AutoDMA stream blocks, with how many writes, bytes and how many distinct blocks.
    static uint64_t g_stagingBytes = 0;
    uint64_t iopStagingTakeBytes()
    {
        const uint64_t v = g_stagingBytes;
        g_stagingBytes = 0;
        return v;
    }

    void iopWatchWrite(uint32_t pc, uint32_t address, uint32_t value, uint32_t bytes)
    {
        static const bool s_on = []() { const char *e = std::getenv("PS2X_IOP_WATCH"); return e && e[0] == '1'; }();
        if (!s_on)
            return;
        struct Entry
        {
            uint64_t writes = 0, bytes = 0, nonZero = 0;
            std::map<uint32_t, uint64_t> blocks; // block address (0x200 granularity) -> writes
        };
        static std::map<uint32_t, Entry> s_perPc[2][2]; // [region: 0 = stream blocks, 1 = mixer source ring][side]
        static auto s_start = std::chrono::steady_clock::now();
        if (pc == 0x6c618u || pc == 0x6c61cu)
        {
            // CRI_ADXI mixer entry (mix(int32 *acc, int16 *src), 256 samples): which accumulator / source buffers it is called with.
            static std::map<uint32_t, uint64_t> s_mix[2];
            static auto s_mixStart = std::chrono::steady_clock::now();
            ++s_mix[pc == 0x6c61cu ? 1 : 0][value & 0x1FFFFFu];
            const auto tnow = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(tnow - s_mixStart).count() >= 1.0)
            {
                s_mixStart = tnow;
                for (int w = 0; w < 2; ++w)
                {
                    std::vector<std::pair<uint64_t, uint32_t>> top;
                    for (const auto &kv : s_mix[w])
                        top.emplace_back(kv.second, kv.first);
                    std::sort(top.begin(), top.end(), [](const auto &l, const auto &r) { return l.first > r.first; });
                    std::fprintf(stderr, "[iop-mix] %s buffers (calls/s):", w ? "SOURCE" : "accumulator");
                    for (size_t k = 0; k < top.size() && k < 14u; ++k)
                        std::fprintf(stderr, " 0x%x:%llu", top[k].second, static_cast<unsigned long long>(top[k].first));
                    std::fputc(10, stderr);
                    s_mix[w].clear();
                }
            }
            return;
        }
        const uint32_t phys = address & 0x1FFFFFu;
        if (phys - 0x7a600u < 0x200u)
            g_stagingBytes += bytes;
        const int side = static_cast<int>((phys >> 9u) & 1u);
        const int region = (phys - 0x19f000u) < 0x2000u ? 1 : 0;
        Entry &e = s_perPc[region][side][pc];
        ++e.writes;
        e.bytes += bytes;
        e.nonZero += value != 0u ? 1u : 0u;
        ++e.blocks[phys & ~0x1FFu];
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - s_start).count() < 1.0)
            return;
        s_start = now;
        for (int rg = 0; rg < 2; ++rg)
        for (int sd = 0; sd < 2; ++sd)
        {
            std::vector<std::pair<uint32_t, Entry *>> top;
            uint64_t total = 0;
            for (auto &kv : s_perPc[rg][sd])
            {
                top.emplace_back(kv.first, &kv.second);
                total += kv.second.bytes;
            }
            std::sort(top.begin(), top.end(), [](const auto &l, const auto &r) { return l.second->bytes > r.second->bytes; });
            std::fprintf(stderr, "[iop-watch%s] %s %s, last second: %llu bytes written by %zu instruction addresses", rg ? "-src" : "",
                         sd ? "RIGHT" : "left", rg ? "mixer source blocks" : "stream blocks",
                         static_cast<unsigned long long>(total), top.size());
            for (size_t k = 0; k < top.size() && k < (rg ? 8u : 5u); ++k)
                std::fprintf(stderr, " | pc 0x%08x: %llu writes, %llu bytes, %zu blocks", top[k].first,
                             static_cast<unsigned long long>(top[k].second->writes), static_cast<unsigned long long>(top[k].second->bytes),
                             top[k].second->blocks.size());
            std::fprintf(stderr, "\n");
            s_perPc[rg][sd].clear();
        }
    }

}
