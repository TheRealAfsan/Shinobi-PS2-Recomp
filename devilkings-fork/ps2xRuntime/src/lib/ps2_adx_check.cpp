// LOCAL DEBUG: see runtime/ps2_adx_check.h.
#include "runtime/ps2_adx_check.h"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace ps2x::adxcheck
{
    namespace
    {
        constexpr uint32_t kTableA = 0x463EB0u; // float per byte: value of the high nibble
        constexpr uint32_t kTableB = 0x4642B0u; // float per byte: value of the low nibble
        constexpr uint32_t kMask = 0x01FFFFFFu;

        inline uint8_t *at(uint8_t *rdram, uint32_t address) { return rdram + (address & kMask); }
        inline uint16_t rd16(uint8_t *rdram, uint32_t address)
        {
            uint16_t v;
            std::memcpy(&v, at(rdram, address), 2);
            return v;
        }
        inline float rdF(uint8_t *rdram, uint32_t address)
        {
            float v;
            std::memcpy(&v, at(rdram, address), 4);
            return v;
        }

        // ---- the EE FPU's arithmetic: results rounded toward zero, denormals flushed, no infinities
        inline float eeRound(double exact)
        {
            float f = static_cast<float>(exact);
            if (std::fabs(static_cast<double>(f)) > std::fabs(exact))
                f = std::nextafterf(f, 0.0f);
            if (std::fabs(f) < FLT_MIN)
                return 0.0f;
            if (std::isinf(f))
                return std::signbit(f) ? -FLT_MAX : FLT_MAX;
            return f;
        }
        inline float eeMul(float a, float b) { return eeRound(static_cast<double>(a) * static_cast<double>(b)); }
        inline float eeAdd(float a, float b) { return eeRound(static_cast<double>(a) + static_cast<double>(b)); }
        inline int32_t cvtW(float v)
        {
            if (!(std::fabs(v) < 2147483648.0f))
                return v < 0 ? INT32_MIN : INT32_MAX;
            return static_cast<int32_t>(v);
        }

        struct Snapshot
        {
            uint32_t src = 0, outLeft = 0, outRight = 0, histLeft = 0, histRight = 0;
            int32_t frames = 0;
            int16_t coefA = 0, coefB = 0;
            int16_t hist[2][2] = {};
        };
        std::unordered_map<uint32_t, Snapshot> g_open; // by left history pointer (one per stream)

        struct ChannelState
        {
            int16_t last1 = 0, last2 = 0; // the last two output samples of the previous call (= the history it left)
            bool have = false;
        };
        std::unordered_map<uint32_t, ChannelState> g_state; // by history pointer

        struct Totals
        {
            uint64_t calls = 0, pairs = 0, samples = 0, endMarkers = 0, unmatched = 0;
            uint64_t edges[2] = {}, edgeAtFrameStart[2] = {};
            uint64_t bigDiffHost[2] = {}, bigDiffEe[2] = {};
            int maxDiffHost[2] = {}, maxDiffEe[2] = {};
            uint64_t histBreaks[2] = {};
            uint32_t maxScale[2] = {};
            uint64_t edgePos[2][32] = {};
        } g_t;

        bool g_enabled = [] { const char *e = std::getenv("PS2X_ADX_CHECK"); return e && e[0] == '1'; }();
        uint32_t g_badLogged = 0;
        std::chrono::steady_clock::time_point g_windowStart{};

        // One channel, reference decode. `useEe` selects the EE float rules. Returns the samples written.
        void decodeChannel(uint8_t *rdram, const Snapshot &s, int channel, bool useEe, std::vector<int16_t> &out, int pairs)
        {
            const float A = static_cast<float>(static_cast<int32_t>(s.coefA) * 16) * (1.0f / 65536.0f);
            const float B = static_cast<float>(static_cast<int32_t>(s.coefB) * 16) * (1.0f / 65536.0f);
            float y1 = static_cast<float>(static_cast<int32_t>(s.hist[channel][0]) * 65536);
            float y2 = static_cast<float>(static_cast<int32_t>(s.hist[channel][1]) * 65536);
            out.clear();
            for (int k = 0; k < pairs; ++k)
            {
                const uint32_t frame = s.src + static_cast<uint32_t>(k) * 36u + (channel ? 18u : 0u);
                const uint16_t be = static_cast<uint16_t>((*at(rdram, frame) << 8) | *at(rdram, frame + 1));
                const float scale = static_cast<float>((be & 0x1FFF) + 1);
                for (int b = 0; b < 16; ++b)
                {
                    const uint8_t byte = *at(rdram, frame + 2u + static_cast<uint32_t>(b));
                    for (int half = 0; half < 2; ++half)
                    {
                        const float d = rdF(rdram, (half == 0 ? kTableA : kTableB) + static_cast<uint32_t>(byte) * 4u);
                        float y;
                        if (useEe)
                            y = eeAdd(eeAdd(eeMul(A, y1), eeMul(B, y2)), eeMul(d, scale));
                        else
                            y = (A * y1 + B * y2) + d * scale;
                        y2 = y1;
                        y1 = y;
                        out.push_back(static_cast<int16_t>(cvtW(y) >> 16));
                    }
                }
            }
        }

        void summary(double seconds)
        {
            std::fprintf(stderr,
                         "[adx-check] %.0fs calls %llu pairs %llu samples %llu end-markers %llu no-snapshot %llu | decoder output edges "
                         "L %llu R %llu (at a frame start: L %llu R %llu) | vs host-float ref: max diff L %d R %d, >64 L %llu R %llu | "
                         "vs EE-float ref: max diff L %d R %d, >64 L %llu R %llu | history broken between calls L %llu R %llu | max "
                         "scale L %u R %u\n",
                         seconds, static_cast<unsigned long long>(g_t.calls), static_cast<unsigned long long>(g_t.pairs),
                         static_cast<unsigned long long>(g_t.samples), static_cast<unsigned long long>(g_t.endMarkers),
                         static_cast<unsigned long long>(g_t.unmatched), static_cast<unsigned long long>(g_t.edges[0]),
                         static_cast<unsigned long long>(g_t.edges[1]), static_cast<unsigned long long>(g_t.edgeAtFrameStart[0]),
                         static_cast<unsigned long long>(g_t.edgeAtFrameStart[1]), g_t.maxDiffHost[0], g_t.maxDiffHost[1],
                         static_cast<unsigned long long>(g_t.bigDiffHost[0]), static_cast<unsigned long long>(g_t.bigDiffHost[1]),
                         g_t.maxDiffEe[0], g_t.maxDiffEe[1], static_cast<unsigned long long>(g_t.bigDiffEe[0]),
                         static_cast<unsigned long long>(g_t.bigDiffEe[1]), static_cast<unsigned long long>(g_t.histBreaks[0]),
                         static_cast<unsigned long long>(g_t.histBreaks[1]), g_t.maxScale[0], g_t.maxScale[1]);
            // Where in the 32-sample frame the right channel's edges fall (a spike at 0 = a frame-boundary problem).
            uint64_t total = 0;
            int top = 0;
            for (int i = 0; i < 32; ++i)
            {
                total += g_t.edgePos[1][i];
                if (g_t.edgePos[1][i] > g_t.edgePos[1][top])
                    top = i;
            }
            if (total)
                std::fprintf(stderr, "[adx-check]   right-channel edges by position in the 32-sample frame: most at %d (%llu of %llu)\n",
                             top, static_cast<unsigned long long>(g_t.edgePos[1][top]), static_cast<unsigned long long>(total));
            g_t = Totals{};
        }
    }

    bool enabled() { return g_enabled; }

    void begin(uint8_t *rdram, uint32_t src, int32_t frames, uint32_t outLeft, uint32_t histLeft, uint32_t outRight,
               uint32_t histRight, uint32_t coefA, uint32_t coefB)
    {
        if (!g_enabled)
            return;
        Snapshot s;
        s.src = src;
        s.frames = frames;
        s.outLeft = outLeft;
        s.outRight = outRight;
        s.histLeft = histLeft;
        s.histRight = histRight;
        s.coefA = static_cast<int16_t>(coefA & 0xFFFFu);
        s.coefB = static_cast<int16_t>(coefB & 0xFFFFu);
        s.hist[0][0] = static_cast<int16_t>(rd16(rdram, histLeft));
        s.hist[0][1] = static_cast<int16_t>(rd16(rdram, histLeft + 2));
        s.hist[1][0] = static_cast<int16_t>(rd16(rdram, histRight));
        s.hist[1][1] = static_cast<int16_t>(rd16(rdram, histRight + 2));
        g_open[histLeft] = s;

        // Is the history the previous call left (its last two output samples) still there? A different value means something
        // else changed it between the two calls.
        const uint32_t pointers[2] = {histLeft, histRight};
        for (int c = 0; c < 2; ++c)
        {
            auto it = g_state.find(pointers[c]);
            if (it != g_state.end() && it->second.have &&
                (it->second.last1 != s.hist[c][0] || it->second.last2 != s.hist[c][1]))
            {
                ++g_t.histBreaks[c];
                if (g_badLogged < 40)
                {
                    ++g_badLogged;
                    std::fprintf(stderr, "[adx-bad] history of the %s channel changed between calls: left by the last call %d,%d, "
                                         "found %d,%d (pointer 0x%08x)\n",
                                 c ? "RIGHT" : "left", it->second.last1, it->second.last2, s.hist[c][0], s.hist[c][1], pointers[c]);
                }
            }
        }
    }

    void end(uint8_t *rdram, uint32_t histLeft, uint32_t histRight)
    {
        if (!g_enabled)
            return;
        auto found = g_open.find(histLeft);
        if (found == g_open.end())
        {
            ++g_t.unmatched;
            return;
        }
        const Snapshot s = found->second;
        g_open.erase(found);
        ++g_t.calls;

        // How many frame pairs were decoded: until an end marker (scale with bit 15) in either channel's frame.
        const int available = s.frames / 2;
        int pairs = 0;
        for (; pairs < available; ++pairs)
        {
            const uint32_t frame = s.src + static_cast<uint32_t>(pairs) * 36u;
            const uint8_t lh = *at(rdram, frame), rh = *at(rdram, frame + 18);
            if ((lh & 0x80) || (rh & 0x80))
            {
                ++g_t.endMarkers;
                break;
            }
            const uint32_t ls = static_cast<uint32_t>(((lh << 8) | *at(rdram, frame + 1)) & 0x1FFF) + 1u;
            const uint32_t rs = static_cast<uint32_t>(((rh << 8) | *at(rdram, frame + 19)) & 0x1FFF) + 1u;
            g_t.maxScale[0] = std::max(g_t.maxScale[0], ls);
            g_t.maxScale[1] = std::max(g_t.maxScale[1], rs);
        }
        g_t.pairs += static_cast<uint64_t>(pairs);
        if (pairs > 0)
        {
            const uint32_t outs[2] = {s.outLeft, s.outRight};
            const uint32_t hists[2] = {histLeft, histRight};
            std::vector<int16_t> host, ee;
            for (int c = 0; c < 2; ++c)
            {
                decodeChannel(rdram, s, c, false, host, pairs);
                decodeChannel(rdram, s, c, true, ee, pairs);
                ChannelState &state = g_state[hists[c]];
                int16_t previous = state.last1;
                bool havePrevious = state.have;
                for (size_t i = 0; i < host.size(); ++i)
                {
                    const int16_t ours = static_cast<int16_t>(rd16(rdram, outs[c] + static_cast<uint32_t>(i) * 2u));
                    const int dh = std::abs(static_cast<int>(ours) - host[i]);
                    const int de = std::abs(static_cast<int>(ours) - ee[i]);
                    g_t.maxDiffHost[c] = std::max(g_t.maxDiffHost[c], dh);
                    g_t.maxDiffEe[c] = std::max(g_t.maxDiffEe[c], de);
                    g_t.bigDiffHost[c] += dh > 64 ? 1u : 0u;
                    g_t.bigDiffEe[c] += de > 64 ? 1u : 0u;
                    if (havePrevious && std::abs(static_cast<int>(ours) - previous) > 8000)
                    {
                        ++g_t.edges[c];
                        ++g_t.edgePos[c][i % 32];
                        g_t.edgeAtFrameStart[c] += (i % 32) == 0 ? 1u : 0u;
                        if (g_badLogged < 40)
                        {
                            ++g_badLogged;
                            const size_t frame = i / 32;
                            const uint32_t fa = s.src + static_cast<uint32_t>(frame) * 36u + (c ? 18u : 0u);
                            const uint32_t scale = static_cast<uint32_t>(((*at(rdram, fa) << 8) | *at(rdram, fa + 1)) & 0x1FFF) + 1u;
                            std::fprintf(stderr,
                                         "[adx-bad] %s channel edge at pair %zu sample %zu: previous %d -> ours %d (host ref %d, EE ref "
                                         "%d), frame scale %u, filter coefficients %d,%d\n",
                                         c ? "RIGHT" : "left", frame, i % 32, previous, ours, host[i], ee[i], scale, s.coefA, s.coefB);
                        }
                    }
                    previous = ours;
                    havePrevious = true;
                }
                g_t.samples += host.size();
                if (!host.empty())
                {
                    state.last1 = static_cast<int16_t>(rd16(rdram, outs[c] + static_cast<uint32_t>(host.size() - 1) * 2u));
                    state.last2 = host.size() > 1
                                      ? static_cast<int16_t>(rd16(rdram, outs[c] + static_cast<uint32_t>(host.size() - 2) * 2u))
                                      : state.last1;
                    state.have = true;
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (g_windowStart.time_since_epoch().count() == 0)
            g_windowStart = now;
        const double seconds = std::chrono::duration<double>(now - g_windowStart).count();
        if (seconds >= 1.0)
        {
            summary(seconds);
            g_windowStart = now;
        }
    }
}
