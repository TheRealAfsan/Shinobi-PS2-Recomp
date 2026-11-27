// LOCAL PATCH (cinematic bars mod, graphics level): removes letterbox bands from what the game draws.
//
// Hide cinematic bars first skipped the bands of EventAction::Draw and widened the screen's clipping, but the TATE kill
// camera's bands come from elsewhere. Whatever draws them, they reach the GS as flat, untextured, black shapes as wide
// as the frame that touch its top or bottom edge and are much less tall than it. Every GIF packet is walked; such a
// shape gets all its corners on one line (no height, nothing drawn). A full-screen shape (a fade, the pause screen's
// dimming) is far taller and is left alone. PS2X_BARS_LOG=1 logs what was removed.

#include "runtime/gs/gs_bars.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace ps2x::bars
{
    namespace
    {
        struct Vertex
        {
            uint32_t offset; // where its Y sits in the packet (16 bits)
            int32_t x, y;    // 12.4, XYOFFSET taken off
            uint16_t rawY;   // as written
            bool black;
            bool tex;
            uint32_t rgb;
            // LOCAL PATCH (black strips): where its colour was written in this packet (kind 0: not in it, 1: a register
            // value, 2: a packed RGBAQ) and its texture function then
            uint32_t colOffset;
            uint8_t colKind;
            uint8_t tfx;
        };

        struct Walker
        {
            State st;
            std::vector<Vertex> &verts;
            std::vector<std::pair<uint32_t, uint32_t>> &runs; // first, count
            bool open = false;
            bool black = false;
            uint32_t runFirst = 0;

            void close()
            {
                if (open)
                {
                    // Sprites are checked one by one: a batch of them (a fade, both bands at once) is not one shape.
                    const uint32_t n = static_cast<uint32_t>(verts.size()) - runFirst;
                    // A batch that also covers the middle of the frame is a fade or a full-screen picture, not bands:
                    // it stays one shape (and so is never taken for a band).
                    bool middle = false;
                    for (uint32_t i = 0; primType == 6u && i + 1u < n; i += 2u)
                    {
                        const int32_t a = std::min(verts[runFirst + i].y, verts[runFirst + i + 1u].y);
                        const int32_t b = std::max(verts[runFirst + i].y, verts[runFirst + i + 1u].y);
                        middle = middle || (a <= 100 * 16 && b >= 124 * 16);
                    }
                    if (primType == 6u && n > 2u && !middle)
                        for (uint32_t i = 0; i + 1u < n; i += 2u)
                            runs.emplace_back(runFirst + i, 2u);
                    else
                        runs.emplace_back(runFirst, n);
                }
                open = false;
            }
            bool textured() const { return st.prmodeCont ? st.primTme : st.prmodeTme; }
            int ctx() const { return (st.prmodeCont ? st.primCtxt : st.prmodeCtxt) ? 1 : 0; }

            void vertex(uint32_t yOffset, uint16_t x, uint16_t y)
            {
                if (!open)
                {
                    open = true;
                    runFirst = static_cast<uint32_t>(verts.size());
                }
                verts.push_back({yOffset, static_cast<int32_t>(x) - st.ofx[ctx()], static_cast<int32_t>(y) - st.ofy[ctx()], y,
                                 black && !textured(), textured(), colour, colOffset, colKind, st.tfx[ctx()]});
            }
            uint32_t colour = 0;
            uint32_t colOffset = 0;
            uint8_t colKind = 0;
            uint32_t primType = 0;
            std::vector<uint32_t> runType; // prim type of each run
            std::vector<std::pair<uint32_t, uint64_t>> scissors; // where the value sits, value
            void rgba(uint32_t r, uint32_t g, uint32_t b)
            {
                colour = (r << 16) | (g << 8) | b;
                black = r <= 12u && g <= 12u && b <= 12u;
            }
            void reg(uint8_t address, uint64_t value, uint32_t offset)
            {
                switch (address)
                {
                case 0x00: // PRIM
                    close();
                    st.primCtxt = ((value >> 9) & 1u) != 0;
                    st.primTme = ((value >> 4) & 1u) != 0;
                    primType = static_cast<uint32_t>(value & 7u);
                    break;
                case 0x01: // RGBAQ
                    rgba(static_cast<uint32_t>(value & 0xFFu), static_cast<uint32_t>((value >> 8) & 0xFFu),
                         static_cast<uint32_t>((value >> 16) & 0xFFu));
                    colOffset = offset;
                    colKind = 1u;
                    break;
                case 0x06:
                case 0x07: // TEX0_1/2: TFX
                    st.tfx[address - 0x06u] = static_cast<uint8_t>((value >> 35) & 3u);
                    break;
                case 0x04: // XYZF2
                case 0x05: // XYZ2
                case 0x0C:
                case 0x0D:
                    vertex(offset + 2u, static_cast<uint16_t>(value), static_cast<uint16_t>(value >> 16));
                    break;
                case 0x18:
                case 0x19: // XYOFFSET_1/2
                    st.ofx[address - 0x18u] = static_cast<uint16_t>(value);
                    st.ofy[address - 0x18u] = static_cast<uint16_t>(value >> 32);
                    break;
                case 0x40:
                case 0x41: // SCISSOR_1/2
                    scissors.push_back({offset, value});
                    break;
                case 0x1A:
                    st.prmodeCont = (value & 1u) != 0;
                    break;
                case 0x1B:
                    st.prmodeCtxt = ((value >> 9) & 1u) != 0;
                    st.prmodeTme = ((value >> 4) & 1u) != 0;
                    break;
                case 0x4C:
                case 0x4D: // FRAME_1/2
                    st.width[address - 0x4Cu] = static_cast<uint32_t>((value >> 16) & 0x3Fu) * 64u;
                    break;
                default:
                    break;
                }
            }
            void packed(uint8_t desc, uint64_t lo, uint64_t hi, uint32_t offset)
            {
                switch (desc)
                {
                case 0x00:
                    reg(0x00, lo & 0x7FFu, offset);
                    break;
                case 0x01: // RGBAQ packed: R lo[7:0], G lo[39:32], B hi[7:0]
                    rgba(static_cast<uint32_t>(lo & 0xFFu), static_cast<uint32_t>((lo >> 32) & 0xFFu),
                         static_cast<uint32_t>(hi & 0xFFu));
                    colOffset = offset;
                    colKind = 2u;
                    break;
                case 0x04:
                case 0x05:
                case 0x0C:
                case 0x0D: // XYZ packed: X lo[15:0], Y lo[47:32]
                    vertex(offset + 4u, static_cast<uint16_t>(lo), static_cast<uint16_t>(lo >> 32));
                    break;
                case 0x0E:
                    reg(static_cast<uint8_t>(hi & 0xFFu), lo, offset);
                    break;
                case 0x02:
                case 0x03:
                case 0x0A:
                case 0x0F:
                    break;
                default:
                    reg(desc, lo, offset);
                    break;
                }
            }
            void walk(const uint8_t *data, uint32_t size)
            {
                auto load = [data](uint32_t o) { uint64_t v; std::memcpy(&v, data + o, 8); return v; };
                uint32_t o = 0;
                while (o + 16u <= size)
                {
                    const uint64_t tagLo = load(o), tagHi = load(o + 8u);
                    o += 16u;
                    const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                    const uint32_t flg = static_cast<uint32_t>((tagLo >> 58) & 3u);
                    uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
                    if (nreg == 0u)
                        nreg = 16u;
                    if ((tagLo >> 46) & 1u)
                        reg(0x00, (tagLo >> 47) & 0x7FFu, 0);
                    if (flg == 0u)
                    {
                        for (uint32_t l = 0; l < nloop; ++l)
                            for (uint32_t r = 0; r < nreg; ++r)
                            {
                                if (o + 16u > size)
                                    return;
                                packed(static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu), load(o), load(o + 8u), o);
                                o += 16u;
                            }
                    }
                    else if (flg == 1u)
                    {
                        for (uint32_t l = 0; l < nloop; ++l)
                            for (uint32_t r = 0; r < nreg; ++r)
                            {
                                if (o + 8u > size)
                                    return;
                                reg(static_cast<uint8_t>((tagHi >> (r * 4u)) & 0xFu), load(o), o);
                                o += 8u;
                            }
                        if ((nloop * nreg) & 1u)
                            o += 8u;
                    }
                    else
                        o += std::min(nloop * 16u, size - o);
                }
                close();
            }
        };
    }

    const uint8_t *apply(const uint8_t *data, uint32_t size, const State &state, std::vector<uint8_t> &scratch,
                         bool removeBands)
    {
        thread_local std::vector<Vertex> verts;
        thread_local std::vector<std::pair<uint32_t, uint32_t>> runs;
        verts.clear();
        runs.clear();
        Walker w{state, verts, runs};
        w.walk(data, size);
        if (removeBands)
        {
            static std::vector<uint64_t> s_all;
            for (const auto &sc : w.scissors)
                if (s_all.size() < 60 && std::find(s_all.begin(), s_all.end(), sc.second) == s_all.end())
                {
                    s_all.push_back(sc.second);
                    std::fprintf(stderr, "[scissor] x %u..%u y %u..%u\n", unsigned(sc.second & 0x7FF), unsigned((sc.second >> 16) & 0x7FF),
                                 unsigned((sc.second >> 32) & 0x7FF), unsigned((sc.second >> 48) & 0x7FF));
                }
        }
        static const bool s_log = std::getenv("PS2X_BARS_LOG") != nullptr;
        bool copied = false;
        for (const auto &[first, count] : runs)
        {
            if (count < 2u)
                continue;
            int32_t x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN;
            bool allBlack = true;
            for (uint32_t i = 0; i < count; ++i)
            {
                const Vertex &v = verts[first + i];
                x0 = std::min(x0, v.x);
                x1 = std::max(x1, v.x);
                y0 = std::min(y0, v.y);
                y1 = std::max(y1, v.y);
                allBlack = allBlack && v.black;
            }
            // In pixels of the game's frame (640 wide; 448 or 224 lines).
            const float wPx = (x1 - x0) / 16.0f, top = y0 / 16.0f, bottom = y1 / 16.0f, hPx = bottom - top;
            const bool wide = wPx >= 600.0f;
            // LOCAL PATCH (black strips, user 2026-11-27: "should be black bars there"): with the bars kept (IMAX mode off)
            // the game's stage-intro strips - textured, frame-wide, 640 x 24 at the top / 640 x 45 at the bottom, the same
            // ones IMAX mode removes - came out red / orange. They are drawn black: their colour set to black (the texture
            // is multiplied by it), or, when that cannot be done here (texture not multiplied by the colour, colour not
            // in this packet), the strip is left out. Nothing else is touched in this mode.
            if (!removeBands)
            {
                const float left = x0 / 16.0f;
                const bool strip = verts[first].tex && count <= 4u && left >= -4.0f && left <= 4.0f && wPx >= 630.0f &&
                                   wPx <= 650.0f && hPx >= 12.0f && hPx <= 64.0f &&
                                   ((top >= -2.0f && top <= 2.0f) || (bottom >= 216.0f && bottom <= 240.0f && top >= 160.0f));
                if (!strip)
                    continue;
                bool colourable = true;
                for (uint32_t i = 0; i < count; ++i)
                    colourable = colourable && verts[first + i].colKind != 0u && verts[first + i].tfx == 0u;
                if (!copied)
                {
                    scratch.assign(data, data + size);
                    copied = true;
                }
                for (uint32_t i = 0; i < count; ++i)
                {
                    const Vertex &v = verts[first + i];
                    if (colourable && v.colKind == 1u)
                        std::memset(scratch.data() + v.colOffset, 0, 3); // R, G, B of the register value
                    else if (colourable && v.colKind == 2u)
                    {
                        scratch[v.colOffset] = 0;      // R
                        scratch[v.colOffset + 4u] = 0; // G
                        scratch[v.colOffset + 8u] = 0; // B
                    }
                    else if (!colourable)
                        std::memcpy(scratch.data() + v.offset, &verts[first].rawY, 2); // zero height: left out
                }
                static int s_told = 0;
                if (s_told < 6)
                {
                    ++s_told;
                    std::fprintf(stderr, "[bars] stage strip %.0f x %.0f at y %.0f..%.0f drawn black (%s)\n", wPx, hPx, top,
                                 bottom, colourable ? "colour" : "left out");
                }
                continue;
            }
            // Research: every distinct shape touching the top or bottom edge (any colour, textured or not), once.
            {
                static std::vector<uint64_t> s_edgeSeen;
                const Vertex &v0 = verts[first];
                const bool edge = top <= 6.0f || (bottom >= 200.0f && bottom <= 260.0f) || bottom >= 420.0f;
                const uint64_t key = (static_cast<uint64_t>(static_cast<int>(wPx) & 0xFFF) << 48) |
                                     (static_cast<uint64_t>(static_cast<int>(top) & 0xFFF) << 36) |
                                     (static_cast<uint64_t>(static_cast<int>(bottom) & 0xFFF) << 24) | (v0.rgb & 0xFFFFFFu);
                if (edge && wPx >= 300.0f && hPx < 300.0f && s_edgeSeen.size() < 120 &&
                    std::find(s_edgeSeen.begin(), s_edgeSeen.end(), key) == s_edgeSeen.end())
                {
                    s_edgeSeen.push_back(key);
                    std::fprintf(stderr, "[edge] %.0f x %.0f at x %.0f y %.0f..%.0f colour %06x %s %u corners\n", wPx, hPx,
                                 x0 / 16.0f, top, bottom, v0.rgb, v0.tex ? "textured" : "flat", count);
                }
            }
            const bool band = hPx >= 4.0f && hPx <= 180.0f;
            // The frame is 448 lines, or 224 when the game draws fields (as in the TATE camera): its bottom edge is
            // either.
            // (The TATE camera's bottom band runs 183..233, a little past its 224-line frame.)
            // Any frame-wide black band anywhere on screen, not only at the edges (full-screen fades are taller).
            // The TATE kill camera's strips are textured (640 x 24 at the top, 640 x 45 at the bottom of its 224-line
            // frame); full-screen effects are far taller and are left alone.
            // Only a screen-aligned strip exactly the frame's width (x 0..640, a 2D overlay), never 3D scenery: the
            // level's polygons can be just as wide but sit anywhere and slant.
            const float left = x0 / 16.0f;
            const bool frameWide = left >= -4.0f && left <= 4.0f && wPx >= 630.0f && wPx <= 650.0f;
            const bool texStrip = verts[first].tex && frameWide && count <= 4u && hPx >= 12.0f && hPx <= 64.0f &&
                                  ((top >= -2.0f && top <= 2.0f) || (bottom >= 216.0f && bottom <= 240.0f && top >= 160.0f));
            // Research: for 3 seconds after a TATE band was removed, every distinct shape in the bottom part of the
            // frame (what still makes the bottom black?).
            {
                static int64_t s_lastRemoval = 0;
                static std::vector<uint64_t> s_low;
                const int64_t now = static_cast<int64_t>(std::clock());
                if (texStrip)
                    s_lastRemoval = now;
                const Vertex &v0 = verts[first];
                const uint64_t key = (static_cast<uint64_t>(static_cast<int>(wPx) & 0xFFF) << 48) |
                                     (static_cast<uint64_t>(static_cast<int>(top) & 0xFFF) << 36) |
                                     (static_cast<uint64_t>(static_cast<int>(bottom) & 0xFFF) << 24) | (v0.rgb & 0xFFFFFFu);
                if (s_lastRemoval && now - s_lastRemoval < 3 * CLOCKS_PER_SEC && bottom >= 190.0f && bottom <= 260.0f &&
                    top >= 120.0f && wPx >= 8.0f && s_low.size() < 150 &&
                    std::find(s_low.begin(), s_low.end(), key) == s_low.end())
                {
                    s_low.push_back(key);
                    std::fprintf(stderr, "[low] %.0f x %.0f at x %.0f y %.0f..%.0f colour %06x %s %u corners\n", wPx, hPx,
                                 x0 / 16.0f, top, bottom, v0.rgb, v0.tex ? "textured" : "flat", count);
                }
            }
            if (!texStrip && (!allBlack || !frameWide || !band))
                continue;
            if (!copied)
            {
                scratch.assign(data, data + size);
                copied = true;
            }
            const uint16_t flatY = verts[first].rawY; // every corner on the first one's line: zero height
            for (uint32_t i = 0; i < count; ++i)
                std::memcpy(scratch.data() + verts[first + i].offset, &flatY, 2);
            static std::vector<uint64_t> s_removedShapes;
            const uint64_t rkey = (static_cast<uint64_t>(static_cast<int>(wPx) & 0xFFF) << 36) |
                                  (static_cast<uint64_t>(static_cast<int>(top) & 0xFFF) << 24) |
                                  (static_cast<uint64_t>(static_cast<int>(bottom) & 0xFFF) << 12) | (texStrip ? 1u : 0u);
            const bool newShape = s_removedShapes.size() < 60 &&
                                  std::find(s_removedShapes.begin(), s_removedShapes.end(), rkey) == s_removedShapes.end();
            if (newShape)
                s_removedShapes.push_back(rkey);
            if (s_log || newShape)
                std::fprintf(stderr, "[bars] removed a %.0f x %.0f band at y %.0f..%.0f (%s)\n", wPx, hPx, top, bottom, texStrip ? "textured" : "flat");
        }
        return copied ? scratch.data() : data;
    }
}
