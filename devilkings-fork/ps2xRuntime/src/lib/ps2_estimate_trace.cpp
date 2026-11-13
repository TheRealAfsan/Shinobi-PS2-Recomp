// LOCAL PATCH (diagnostics): the stage-clear results screen (Estimate / CEstimateSub) and the game's file loads, to find
// why "Quit" there does not leave it. Lines "[results] ..." in game/captures/last_run.log. Functions the game calls every
// frame are logged only when the screen's state bytes (Estimate + 0x38..0x3F) change; file loads are logged with names.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
    std::mutex g_mutex;

    std::string guestString(const uint8_t *rdram, uint32_t address)
    {
        std::string out;
        const uint32_t p = address & 0x1FFFFFFFu;
        if (!address || p >= 0x2000000u)
            return out;
        for (uint32_t i = 0; i < 96u && p + i < 0x2000000u; ++i)
        {
            const char c = static_cast<char>(rdram[p + i]);
            if (!c)
                break;
            out += (c >= 32 && c < 127) ? c : '?';
        }
        return out;
    }

    uint64_t stateOf(const uint8_t *rdram, uint32_t estimate)
    {
        const uint32_t p = estimate & 0x1FFFFFFFu;
        if (!estimate || p + 0x40u > 0x2000000u)
            return 0;
        uint64_t v;
        std::memcpy(&v, rdram + p + 0x38u, 8u);
        return v;
    }
}

void ps2xEstimateTrace(const uint8_t *rdram, const char *what, uint32_t a0, uint32_t a1, uint32_t a2)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string w = what;
    if (w == "CheckMenu" || w == "CheckSubMenu" || w == "EstimateMain" || w == "CheckLoad")
    {
        static uint64_t s_last[4] = {~0ull, ~0ull, ~0ull, ~0ull};
        const int slot = w == "CheckMenu" ? 0 : w == "CheckSubMenu" ? 1 : w == "EstimateMain" ? 2 : 3;
        const uint64_t st = stateOf(rdram, a0);
        if (st == s_last[slot])
            return;
        s_last[slot] = st;
        std::fprintf(stderr, "[results] %s estimate=0x%08x state(+0x38)=%016llx\n", what, a0,
                     static_cast<unsigned long long>(st));
        return;
    }
    if (w == "FileLoadRequest")
    {
        std::fprintf(stderr, "[results] FileLoadRequest \"%s\" \"%s\" out=0x%08x\n", guestString(rdram, a0).c_str(),
                     guestString(rdram, a1).c_str(), a2);
        return;
    }
    if (w == "CheckFileLoadEnd")
    {
        static uint32_t s_last = ~0u;
        if (a0 == s_last)
            return;
        s_last = a0;
        std::fprintf(stderr, "[results] CheckFileLoadEnd id=%u\n", a0);
        return;
    }
    std::fprintf(stderr, "[results] %s a0=0x%08x a1=0x%08x a2=0x%08x state(+0x38)=%016llx\n", what, a0, a1, a2,
                 static_cast<unsigned long long>(stateOf(rdram, a0)));
}
