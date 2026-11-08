#include "iop_sysclib.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    IopSysclib::IopSysclib(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    uint64_t iopStagingTakeBytes();

    namespace
    {
        // LOCAL DEBUG (right-ear static, PS2X_IOP_WATCH=1): host-side memcpy / bcopy / memmove / memset into the sound chip's
        // AutoDMA stream blocks (left blocks at ..500/900/d00/100, right at ..700/b00/f00/300). Once a second: how many such
        // calls, their sizes, and how many had overlapping source and destination (a forward byte copy corrupts those).
        void watchCopy(const char *what, uint32_t destination, uint32_t source, uint32_t count, bool isFill)
        {
            static const bool s_on = []() { const char *e = std::getenv("PS2X_IOP_WATCH"); return e && e[0] == '1'; }();
            if (!s_on || count == 0u)
                return;
            const uint32_t d = destination & 0x1FFFFFu;
            // The mixer's source ring (0x19f000..0x1a1000): once a second, which sources are copied into which blocks.
            if (d - 0x19f000u < 0x2000u)
            {
                static std::map<std::pair<uint32_t, uint32_t>, uint64_t> s_ring;
                static auto s_ringStart = std::chrono::steady_clock::now();
                ++s_ring[{d & 0x1FFFFFu, isFill ? 0xFFFFFFFFu : (source & 0x1FFFFFu)}];
                const auto rnow = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(rnow - s_ringStart).count() >= 1.0)
                {
                    s_ringStart = rnow;
                    std::fprintf(stderr, "[iop-copy-src] %s copies into the mixer source ring (dst <- src x count, size %u):", what, count);
                    size_t shown = 0;
                    for (const auto &kv : s_ring)
                        if (shown++ < 16u)
                            std::fprintf(stderr, " 0x%x<-0x%x x%llu", kv.first.first, kv.first.second, static_cast<unsigned long long>(kv.second));
                    std::fputc(10, stderr);
                    s_ring.clear();
                }
            }
            if (d + count <= 0x7b500u || d >= 0x7c500u)
                return;
            struct Stat
            {
                uint64_t calls = 0, bytes = 0, overlaps = 0;
            };
            static Stat s_stat[2][4]; // [right?][memcpy, bcopy, memmove, memset]
            static auto s_start = std::chrono::steady_clock::now();
            const int side = static_cast<int>((d >> 9u) & 1u);
            const int kind = what[0] == 'c' ? 0 : what[0] == 'b' ? 1 : what[0] == 'v' ? 2 : 3;
            Stat &s = s_stat[side][kind];
            ++s.calls;
            s.bytes += count;
            // Where the copied data comes from (IOP RAM, 0x200 granularity), per side, printed with the once-a-second summary.
            static std::map<uint32_t, uint64_t> s_sources[2];
            if (!isFill)
                ++s_sources[side][source & 0x1FFE00u];
            // Bytes the IOP stored into the staging window (0x7a600..0x7a7ff) since the previous copy out of it: 512 is normal.
            static std::map<uint64_t, uint64_t> s_staged[2];
            if (!isFill && (source & 0x1FFFFFu) - 0x7a600u < 0x200u)
                ++s_staged[side][iopStagingTakeBytes()];
            const uint32_t sr = source & 0x1FFFFFu;
            if (!isFill && d < sr + count && sr < d + count)
            {
                ++s.overlaps;
                static int s_listed = 0;
                if (s_listed < 30)
                {
                    ++s_listed;
                    std::fprintf(stderr, "[iop-copy] OVERLAPPING %s into the stream blocks: dst=0x%x src=0x%x count=%u (%s)\n", what, d, sr,
                                 count, d > sr ? "dst above src: a forward copy corrupts it" : "dst below src");
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - s_start).count() >= 1.0)
            {
                s_start = now;
                static const char *names[4] = {"memcpy", "bcopy", "memmove", "memset"};
                for (int sd = 0; sd < 2; ++sd)
                    for (int k = 0; k < 4; ++k)
                        if (s_stat[sd][k].calls)
                            std::fprintf(stderr, "[iop-copy] %s into %s blocks, last second: %llu calls, %llu bytes, %llu overlapping\n", names[k],
                                         sd ? "RIGHT" : "left", static_cast<unsigned long long>(s_stat[sd][k].calls),
                                         static_cast<unsigned long long>(s_stat[sd][k].bytes), static_cast<unsigned long long>(s_stat[sd][k].overlaps));
                for (int sd = 0; sd < 2; ++sd)
                {
                    std::vector<std::pair<uint64_t, uint32_t>> top;
                    for (const auto& kv : s_sources[sd])
                        top.emplace_back(kv.second, kv.first);
                    std::sort(top.begin(), top.end(), [](const auto& l, const auto& r) { return l.first > r.first; });
                    std::fprintf(stderr, "[iop-copy] %s blocks are copied from (IOP RAM, 0x200 granularity):", sd ? "RIGHT" : "left");
                    for (size_t k = 0; k < top.size() && k < 6; ++k)
                        std::fprintf(stderr, " 0x%x x%llu", top[k].second, static_cast<unsigned long long>(top[k].first));
                    std::fputc('\n', stderr);
                    s_sources[sd].clear();
                }
                for (int sd = 0; sd < 2; ++sd)
                {
                    std::fprintf(stderr, "[iop-copy] %s: bytes written into the staging window before each copy:", sd ? "RIGHT" : "left");
                    for (const auto& kv : s_staged[sd])
                        std::fprintf(stderr, " %llu x%llu", static_cast<unsigned long long>(kv.first), static_cast<unsigned long long>(kv.second));
                    std::fputc(10, stderr);
                    s_staged[sd].clear();
                }
                std::memset(s_stat, 0, sizeof(s_stat));
            }
        }
        // sysclib ctype flags: upper 0x01, lower 0x02, digit 0x04, space 0x08,
        // punct 0x10, control 0x20, hex digit 0x40, blank 0x80. IRXs test e.g.
        // look_ctype_table(c) & 0x04 for isdigit when parsing module arguments.
        uint8_t sysclibCtypeFlags(uint8_t c)
        {
            uint8_t flags = 0u;
            if (c >= 'A' && c <= 'Z')
                flags |= 0x01u;
            if (c >= 'a' && c <= 'z')
                flags |= 0x02u;
            if (c >= '0' && c <= '9')
                flags |= 0x04u;
            if (c == ' ' || (c >= 0x09u && c <= 0x0Du))
                flags |= 0x08u;
            if (c > ' ' && c < 0x7Fu && !(flags & 0x07u))
                flags |= 0x10u;
            if (c < 0x20u || c == 0x7Fu)
                flags |= 0x20u;
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                flags |= 0x40u;
            if (c == ' ')
                flags |= 0x80u;
            return flags;
        }
    }

    bool IopSysclib::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        const auto compare = [&](uint32_t lhs, uint32_t rhs, uint32_t count) -> int32_t
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t left = m_memory.read8(lhs + i);
                const uint8_t right = m_memory.read8(rhs + i);
                if (left != right)
                    return static_cast<int32_t>(left) - static_cast<int32_t>(right);
            }
            return 0;
        };
        const auto copy = [&](uint32_t destination, uint32_t source, uint32_t count)
        {
            // LOCAL PATCH: source read in full before anything is written (memmove behaviour). The byte-by-byte forward copy this
            // replaces smeared the data when the destination overlapped the source from above.
            std::vector<uint8_t> temporary(count);
            for (uint32_t i = 0; i < count; ++i)
                temporary[i] = m_memory.read8(source + i);
            for (uint32_t i = 0; i < count; ++i)
                m_memory.write8(destination + i, temporary[i]);
        };
        const auto appendString = [&](uint32_t destination, uint32_t source, std::optional<uint32_t> maxAppend = std::nullopt)
        {
            uint32_t destinationOffset = 0;
            while (m_memory.read8(destination + destinationOffset) != 0u && destinationOffset < (1u << 20))
                ++destinationOffset;

            uint32_t sourceOffset = 0;
            while (sourceOffset < (1u << 20) && (!maxAppend || sourceOffset < *maxAppend))
            {
                const uint8_t character = m_memory.read8(source + sourceOffset);
                m_memory.write8(destination + destinationOffset + sourceOffset, character);
                ++sourceOffset;
                if (character == 0u)
                    return;
            }
            m_memory.write8(destination + destinationOffset + sourceOffset, 0u);
        };

        switch (ordinal)
        {
        case 4: // setjmp - enough for callers which only test the initial return.
            setV0(0);
            return true;
        case 5: // longjmp, TODO bc w can do it without the BIOS jmp_buf ABI.
            setV0(a1 == 0u ? 1u : a1);
            return true;
        case 6:
            setV0(static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(a0))));
            return true;
        case 7:
            setV0(static_cast<uint32_t>(std::tolower(static_cast<unsigned char>(a0))));
            return true;
        case 8: // look_ctype_table
            setV0(sysclibCtypeFlags(static_cast<uint8_t>(a0)));
            return true;
        case 9: // get_ctype_table
            if (m_ctypeTable == 0u)
            {
                m_ctypeTable = m_memory.allocate(256u);
                for (uint32_t c = 0; m_ctypeTable != 0u && c < 256u; ++c)
                    m_memory.write8(m_ctypeTable + c, sysclibCtypeFlags(static_cast<uint8_t>(c)));
            }
            setV0(m_ctypeTable);
            return true;
        case 10: // memchr
            for (uint32_t i = 0; i < a2; ++i)
            {
                if (m_memory.read8(a0 + i) == static_cast<uint8_t>(a1))
                {
                    setV0(a0 + i);
                    return true;
                }
            }
            setV0(0);
            return true;
        case 11:
            setV0(static_cast<uint32_t>(compare(a0, a1, a2)));
            return true;
        case 12:
            watchCopy("copy", a0, a1, a2, false);
            copy(a0, a1, a2);
            setV0(a0);
            return true;
        case 13:
        {
            watchCopy("vmove", a0, a1, a2, false);
            std::vector<uint8_t> temporary(a2);
            for (uint32_t i = 0; i < a2; ++i)
                temporary[i] = m_memory.read8(a1 + i);
            (void)m_memory.writeRam(a0, temporary.data(), temporary.size());
            setV0(a0);
            return true;
        }
        case 14:
            watchCopy("set", a0, 0u, a2, true);
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write8(a0 + i, static_cast<uint8_t>(a1));
            setV0(a0);
            return true;
        case 15: // bcmp
            setV0(static_cast<uint32_t>(compare(a0, a1, a2)));
            return true;
        case 16: // bcopy(src,dst,n)
            watchCopy("bcopy", a1, a0, a2, false);
            copy(a1, a0, a2);
            setV0(0);
            return true;
        case 17:
            for (uint32_t i = 0; i < a1; ++i)
                m_memory.write8(a0 + i, 0u);
            setV0(0);
            return true;
        case 18: // prnt
            setV0(0);
            return true;
        case 19: // sprintf: preserve useful literal formats even before full vararg formatting.
        case 42: // vsprintf fallback: copy format literal.
        {
            const std::string format = m_memory.readString(a1, 4096u);
            for (size_t i = 0; i <= format.size(); ++i)
            {
                m_memory.write8(a0 + static_cast<uint32_t>(i), i < format.size() ? static_cast<uint8_t>(format[i]) : 0u);
            }
            setV0(static_cast<uint32_t>(format.size()));
            return true;
        }
        case 20:
            appendString(a0, a1);
            setV0(a0);
            return true;
        case 21: // strchr
        case 25: // index
        {
            const uint8_t needle = static_cast<uint8_t>(a1);
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t character = m_memory.read8(a0 + i);
                if (character == needle)
                {
                    setV0(a0 + i);
                    return true;
                }
                if (character == 0u)
                    break;
            }
            setV0(0);
            return true;
        }
        case 22: // strcmp
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t left = m_memory.read8(a0 + i);
                const uint8_t right = m_memory.read8(a1 + i);
                if (left != right)
                {
                    setV0(static_cast<uint32_t>(static_cast<int32_t>(left) - static_cast<int32_t>(right)));
                    return true;
                }
                if (left == 0u)
                    break;
            }
            setV0(0);
            return true;
        case 23: // strcpy
        {
            uint32_t i = 0;
            for (;; ++i)
            {
                const uint8_t character = m_memory.read8(a1 + i);
                m_memory.write8(a0 + i, character);
                if (character == 0u)
                    break;
            }
            setV0(a0);
            return true;
        }
        case 24: // strcspn
        {
            const std::string reject = m_memory.readString(a1, 4096u);
            uint32_t count = 0;
            for (; count < (1u << 20); ++count)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + count));
                if (character == 0 || reject.find(character) != std::string::npos)
                    break;
            }
            setV0(count);
            return true;
        }
        case 26: // rindex
        case 32: // strrchr
        {
            const uint8_t needle = static_cast<uint8_t>(a1);
            uint32_t found = 0u;
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t character = m_memory.read8(a0 + i);
                if (character == needle)
                    found = a0 + i;
                if (character == 0u)
                    break;
            }
            setV0(found);
            return true;
        }
        case 27:
            setV0(static_cast<uint32_t>(m_memory.readString(a0, 1u << 20).size()));
            return true;
        case 28:
            appendString(a0, a1, a2);
            setV0(a0);
            return true;
        case 29: // strncmp
            for (uint32_t i = 0; i < a2; ++i)
            {
                const uint8_t left = m_memory.read8(a0 + i);
                const uint8_t right = m_memory.read8(a1 + i);
                if (left != right)
                {
                    setV0(static_cast<uint32_t>(static_cast<int32_t>(left) - static_cast<int32_t>(right)));
                    return true;
                }
                if (left == 0u)
                    break;
            }
            setV0(0);
            return true;
        case 30: // strncpy
        {
            bool ended = false;
            for (uint32_t i = 0; i < a2; ++i)
            {
                const uint8_t character = ended ? 0u : m_memory.read8(a1 + i);
                if (character == 0u)
                    ended = true;
                m_memory.write8(a0 + i, character);
            }
            setV0(a0);
            return true;
        }
        case 31: // strpbrk
        {
            const std::string accept = m_memory.readString(a1, 4096u);
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + i));
                if (character == 0)
                    break;
                if (accept.find(character) != std::string::npos)
                {
                    setV0(a0 + i);
                    return true;
                }
            }
            setV0(0);
            return true;
        }
        case 33: // strspn
        {
            const std::string accept = m_memory.readString(a1, 4096u);
            uint32_t count = 0;
            for (; count < (1u << 20); ++count)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + count));
                if (character == 0 || accept.find(character) == std::string::npos)
                    break;
            }
            setV0(count);
            return true;
        }
        case 34: // strstr
        {
            const std::string needle = m_memory.readString(a1, 4096u);
            if (needle.empty())
            {
                setV0(a0);
                return true;
            }
            const std::string haystack = m_memory.readString(a0, 1u << 20);
            const size_t position = haystack.find(needle);
            setV0(position == std::string::npos
                      ? 0u
                      : a0 + static_cast<uint32_t>(position));
            return true;
        }
        case 35: // strtok state is intentionally not shared across modules yet.
            setV0(0);
            return true;
        case 36:
        case 38: // strtol / strtoul
        {
            const std::string value = m_memory.readString(a0, 4096u);
            char *end = nullptr;
            const int base = static_cast<int>(a2);
            const unsigned long parsed = ordinal == 36
                                             ? static_cast<unsigned long>(std::strtol(value.c_str(), &end, base))
                                             : std::strtoul(value.c_str(), &end, base);
            if (a1 != 0u)
            {
                m_memory.write32(a1, a0 + static_cast<uint32_t>(end - value.c_str()));
            }
            setV0(static_cast<uint32_t>(parsed));
            return true;
        }
        case 37: // atob
            setV0(0);
            return true;
        case 40: // _wmemcopy, count is 32-bit words
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write32(a0 + i * 4u, m_memory.read32(a1 + i * 4u));
            setV0(a0);
            return true;
        case 41:
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write32(a0 + i * 4u, a1);
            setV0(a0);
            return true;
        case 43:
            setV0(0);
            return true;
        default:
            return false;
        }
    }
}
