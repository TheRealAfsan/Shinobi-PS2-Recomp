// LOCAL PATCH (Shinobi mod): Hotsuma's scarf in any colour. Settings [game] scarf_color = original | #RRGGBB (Mod Menu).
//
// How the game draws the scarf (from the translated code): CPlayerMuffler::Draw (0x2b8e60) sets the texture it was
// given when the player was created (MlMultiModelData::GetExtraTexture, kept at this+0xC4) and draws a strip of
// triangles with it. The red is in that texture; the vertex colours only darken and fade it. So the colour is changed
// in the texture itself: HyTexture+0x18 -> surface[0], HyTextureSurface+0x10 -> _hyTextureUnit, whose upload buffer holds
// the palette (CLUT) and then the pixels, laid out as _hyTextureUnit::_reload (0x33a0c0) copies them:
//   +0x48 buffer index, +0x4C[index] buffer, data at buffer+0x10 (or *(buffer+0x10) when +0x54's top byte is 0xF4)
//   palette bytes (+0x68 * +0x6C) / 8, pixel bytes (+0x6A * +0x58 * +0x5C) / 8 (bits per pixel, width, height)
// The original bytes are kept; every change starts from them. Then the game's own _reload(unit, 0, 0) is queued (it
// only releases the unit's graphics memory, so the next draw uploads the new colours), run by the EE scheduler like an
// interrupt handler, so it never interrupts game code half way. Anything that does not look like a scarf texture is
// left alone (and logged once).

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ps2x::pcsx2gs
{
    bool resendPalette(const uint8_t *match1, const uint8_t *match2, const uint8_t *palette); // gs_pcsx2.cpp
    bool rewritePaletteInVram(const std::vector<const uint8_t *> &candidates, const uint8_t *palette); // gs_pcsx2.cpp
}

namespace
{
    constexpr uint32_t kReloadPc = 0x339DF0u; // _unloadVram__14_hyTextureUnitFv

    struct ScarfState
    {
        uint32_t unit = 0u;
        uint32_t data = 0u;
        uint32_t clutBytes = 0u;
        uint32_t pixelBytes = 0u;
        std::vector<uint8_t> original; // palette + pixels as the game loaded them
        std::vector<uint8_t> written;  // what this file last put there
        std::string applied = "original";
        std::vector<uint8_t> pendingGs;  // palette still to be sent to the graphics card
        std::vector<uint8_t> previousGs; // palette last sent there
        bool rejectedLogged = false;
    };

    ScarfState &scarf()
    {
        static ScarfState s;
        return s;
    }

    bool inRam(uint32_t address, uint32_t bytes)
    {
        const uint32_t physical = address & 0x1FFFFFFFu;
        return bytes <= PS2_RAM_SIZE && physical <= PS2_RAM_SIZE - bytes;
    }

    uint32_t rd32(const uint8_t *rdram, uint32_t address)
    {
        if (!inRam(address, 4u))
            return 0u;
        uint32_t value;
        std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), sizeof(value));
        return value;
    }

    int16_t rd16(const uint8_t *rdram, uint32_t address)
    {
        if (!inRam(address, 2u))
            return 0;
        int16_t value;
        std::memcpy(&value, rdram + (address & 0x1FFFFFFFu), sizeof(value));
        return value;
    }

    // "#rrggbb" -> rgb; false for "original" or anything else.
    bool parseColour(const std::string &text, uint8_t rgb[3])
    {
        if (text.size() != 7u || text[0] != '#')
            return false;
        for (int i = 0; i < 3; ++i)
        {
            const std::string part = text.substr(1u + 2u * i, 2u);
            char *end = nullptr;
            const long value = std::strtol(part.c_str(), &end, 16);
            if (!end || *end != '\0' || value < 0 || value > 255)
                return false;
            rgb[i] = static_cast<uint8_t>(value);
        }
        return true;
    }

    // One colour of the scarf: keeps its brightness (relative to the brightest part of the scarf, `reference`) and how
    // coloured it is: the red shading becomes the same shading in the new colour, near-white highlights stay light.
    void recolour(uint8_t &r, uint8_t &g, uint8_t &b, const uint8_t target[3], float reference)
    {
        const float maxc = static_cast<float>(std::max({r, g, b}));
        const float minc = static_cast<float>(std::min({r, g, b}));
        if (maxc <= 0.0f)
            return;
        const float saturation = (maxc - minc) / maxc;
        const float shade = std::min(maxc / reference, 1.5f);
        const float grey = maxc;
        const float out[3] = {static_cast<float>(target[0]) * shade, static_cast<float>(target[1]) * shade,
                              static_cast<float>(target[2]) * shade};
        auto mix = [&](float coloured) {
            const float v = grey + (coloured - grey) * saturation;
            return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
        };
        r = mix(out[0]);
        g = mix(out[1]);
        b = mix(out[2]);
    }

    // Colours in 32-bit (RGBA8888, alpha 0x80 = opaque on the PS2) or 16-bit (RGBA5551) entries.
    void recolourEntries(uint8_t *bytes, uint32_t count, uint32_t bits, const uint8_t target[3])
    {
        auto read = [&](uint32_t i, uint8_t &r, uint8_t &g, uint8_t &b) -> bool {
            if (bits == 32u)
            {
                r = bytes[i * 4u + 0u];
                g = bytes[i * 4u + 1u];
                b = bytes[i * 4u + 2u];
                return bytes[i * 4u + 3u] != 0u;
            }
            if (bits == 24u)
            {
                r = bytes[i * 3u + 0u];
                g = bytes[i * 3u + 1u];
                b = bytes[i * 3u + 2u];
                return true;
            }
            uint16_t v;
            std::memcpy(&v, bytes + i * 2u, 2u);
            r = static_cast<uint8_t>(((v >> 0) & 31u) * 255u / 31u);
            g = static_cast<uint8_t>(((v >> 5) & 31u) * 255u / 31u);
            b = static_cast<uint8_t>(((v >> 10) & 31u) * 255u / 31u);
            return true;
        };
        // Reference brightness: the brightest well-coloured entries (95th percentile), so the main red maps onto the
        // chosen colour itself rather than onto a darker version of it.
        std::vector<float> levels;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint8_t r, g, b;
            if (read(i, r, g, b))
                levels.push_back(static_cast<float>(std::max({r, g, b})));
        }
        if (levels.empty())
            return;
        std::sort(levels.begin(), levels.end());
        const float reference = std::max(1.0f, levels[static_cast<size_t>(0.95 * (levels.size() - 1u))]);
        for (uint32_t i = 0; i < count; ++i)
        {
            uint8_t r, g, b;
            if (!read(i, r, g, b))
                continue;
            recolour(r, g, b, target, reference);
            if (bits == 32u)
            {
                bytes[i * 4u + 0u] = r;
                bytes[i * 4u + 1u] = g;
                bytes[i * 4u + 2u] = b;
            }
            else if (bits == 24u)
            {
                bytes[i * 3u + 0u] = r;
                bytes[i * 3u + 1u] = g;
                bytes[i * 3u + 2u] = b;
            }
            else
            {
                uint16_t v;
                std::memcpy(&v, bytes + i * 2u, 2u);
                v = static_cast<uint16_t>((v & 0x8000u) | ((r * 31u / 255u) & 31u) | (((g * 31u / 255u) & 31u) << 5) |
                                          (((b * 31u / 255u) & 31u) << 10));
                std::memcpy(bytes + i * 2u, &v, 2u);
            }
        }
    }
}

namespace
{
    // Once, when the texture is not understood: the unit, its buffer and its palette object, for working out the layout.
    void dumpUnit(const uint8_t *rdram, uint32_t unit, uint32_t buffer)
    {
        auto words = [&](const char *name, uint32_t address, uint32_t count) {
            if (!address)
                return;
            std::fprintf(stderr, "[scarf] %s 0x%08x:", name, address);
            for (uint32_t i = 0; i < count; ++i)
                std::fprintf(stderr, "%s%08x", (i % 8u) ? " " : "\n[scarf]   ", rd32(rdram, address + 4u * i));
            std::fputc('\n', stderr);
        };
        words("unit", unit, 40u);
        words("buffer", buffer, 16u);
        const uint32_t palette = rd32(rdram, unit + 0x64u);
        words("palette", palette, 8u);
        if (palette)
            words("palette data", rd32(rdram, palette + 0x10u), 16u);
    }
}

// Called from CPlayerMuffler::Draw (src/runner/Draw__14CPlayerMufflerFv_0x2b8e60.cpp) with the scarf object, on the
// game thread, before the scarf's texture is set.
void ps2xScarfColourHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t muffler)
{
    ScarfState &s = scarf();
    std::string wanted = ps2x::settings::get("game", "scarf_color");
    uint8_t target[3] = {0, 0, 0};
    if (!parseColour(wanted, target))
        wanted = "original";

    // HyEnv::SetTexture(env, texture, 0) (0x32da50, called right after this hook) takes the unit as
    // *(*(texture + 0x18) + 0): the table at +0x18 holds the units themselves (HyTextureSurface + 0x10 is the same unit).
    const uint32_t texture = rd32(rdram, muffler + 0xC4u);
    const uint32_t units = texture ? rd32(rdram, texture + 0x18u) : 0u;
    const uint32_t unit = units ? rd32(rdram, units) : 0u;
    const uint32_t word54 = unit ? rd32(rdram, unit + 0x54u) : 0u;
    const uint32_t index = unit ? rd32(rdram, unit + 0x48u) : 0u;
    const uint32_t buffer = unit && index < 4u ? rd32(rdram, unit + 0x4Cu + 4u * index) : 0u;
    // word54 can be 0 for a texture the game loaded once (no reload source): its data is still at buffer + 0x10, which
    // is where _hyTextureUnit::_sendVram (0x339c40) reads it from whenever the unit is (re)loaded into graphics memory.
    if (!unit || !buffer)
    {
        static bool s_missingLogged = false;
        if (!s_missingLogged && wanted != "original")
        {
            s_missingLogged = true;
            std::fprintf(stderr, "[scarf] no texture data found: muffler 0x%08x texture 0x%08x units 0x%08x unit 0x%08x "
                                 "word54 0x%08x index %u buffer 0x%08x\n",
                         muffler, texture, units, unit, word54, index, buffer);
        }
        return;
    }
    uint32_t data = (word54 & 0xFF000000u) == 0xF4000000u ? rd32(rdram, buffer + 0x10u) : buffer + 0x10u;
    int32_t clutA = rd16(rdram, unit + 0x68u), clutB = rd16(rdram, unit + 0x6Cu);
    int32_t bpp = rd16(rdram, unit + 0x6Au);
    uint32_t width = rd32(rdram, unit + 0x58u), height = rd32(rdram, unit + 0x5Cu);
    uint32_t clutBytes = static_cast<uint32_t>(std::max(0, clutA * clutB) / 8);
    uint32_t pixelBytes = static_cast<uint32_t>((static_cast<uint64_t>(bpp > 0 ? bpp : 0) * width * height) >> 3);

    // Palette layout: entry size is the one of the two fields that is 16, 24 or 32 bits; the other is the count.
    uint32_t clutBits = 0u, clutCount = 0u;

    // LOCAL PATCH: the scarf is a Ninja "PVRT" texture kept as loaded from the disc (seen in game: 'PVRT', chunk size
    // 0x1408, 64x64; the unit's own format fields are 0 then). Its layout follows from the chunk size: palette of
    // RGBA8888 entries first (the first entries are 0x80000000, opaque black), then the pixels.
    if (rd32(rdram, buffer) == 0x54525650u) // "PVRT"
    {
        const uint32_t size = rd32(rdram, buffer + 4u) >= 8u ? rd32(rdram, buffer + 4u) - 8u : 0u;
        const uint32_t w = static_cast<uint16_t>(rd16(rdram, buffer + 0xCu));
        const uint32_t h = static_cast<uint16_t>(rd16(rdram, buffer + 0xEu));
        data = buffer + 0x10u;
        width = w;
        height = h;
        clutA = clutB = 0;
        if (size == w * h + 1024u)
        {
            bpp = 8; clutCount = 256u; clutBits = 32u;
        }
        else if (size == w * h / 2u + 64u)
        {
            bpp = 4; clutCount = 16u; clutBits = 32u;
        }
        else if (size == w * h * 4u)
        {
            bpp = 32; clutCount = 0u; clutBits = 0u;
        }
        else if (size == w * h * 2u)
        {
            bpp = 16; clutCount = 0u; clutBits = 0u;
        }
        else
            bpp = 0;
        clutBytes = clutCount * clutBits / 8u;
        pixelBytes = static_cast<uint32_t>((static_cast<uint64_t>(bpp) * w * h) >> 3);
    }
    else if (clutBytes)
    {
        if (clutA == 16 || clutA == 24 || clutA == 32)
        {
            clutBits = static_cast<uint32_t>(clutA);
            clutCount = static_cast<uint32_t>(clutB);
        }
        if ((clutB == 24 || clutB == 32) || (clutB == 16 && clutA == 256))
        {
            clutBits = static_cast<uint32_t>(clutB);
            clutCount = static_cast<uint32_t>(clutA);
        }
    }
    const bool sane = width >= 1u && width <= 1024u && height >= 1u && height <= 1024u &&
                      (bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32) && pixelBytes > 0u &&
                      clutBytes <= 1024u && (clutBytes == 0u || (clutBits != 0u && clutCount * clutBits / 8u == clutBytes)) &&
                      (clutBytes != 0u || bpp >= 16) && inRam(data, clutBytes + pixelBytes);
    if (!sane)
    {
        if (!s.rejectedLogged && wanted != "original")
        {
            s.rejectedLogged = true;
            dumpUnit(rdram, unit, buffer);
            std::fprintf(stderr, "[scarf] texture not recognised, left as it is: %ux%u, %d bits, palette %dx%d, data 0x%08x\n",
                         width, height, bpp, clutA, clutB, data);
        }
        return;
    }

    uint8_t *bytes = rdram + (data & 0x1FFFFFFFu);
    const uint32_t total = clutBytes + pixelBytes;
    // A new scarf texture (a stage was loaded) or data that is no longer what this file wrote: what is there now is the
    // game's original.
    const bool same = s.unit == unit && s.data == data && s.clutBytes == clutBytes && s.pixelBytes == pixelBytes &&
                      s.written.size() == total && std::memcmp(bytes, s.written.data(), total) == 0;
    if (!same)
    {
        s.unit = unit;
        s.data = data;
        s.clutBytes = clutBytes;
        s.pixelBytes = pixelBytes;
        s.original.assign(bytes, bytes + total);
        s.written = s.original;
        s.applied = "original";
        std::fprintf(stderr, "[scarf] scarf texture %ux%u, %d bits per pixel, palette %u x %u bits\n", width, height, bpp,
                     clutCount, clutBits);
    }
    // The palette the graphics card still has to get (see below): tried again every frame until the game's own upload
    // of it has been seen.
    if (!s.pendingGs.empty() && clutBytes == 1024u)
    {
        if (ps2x::pcsx2gs::resendPalette(s.original.data(), s.previousGs.empty() ? nullptr : s.previousGs.data(),
                                         s.pendingGs.data()))
        {
            std::fprintf(stderr, "[scarf] new palette sent to the graphics card\n");
            s.previousGs = s.pendingGs;
            s.pendingGs.clear();
        }
    }
    if (wanted == s.applied)
        return;

    std::vector<uint8_t> image = s.original;
    if (wanted != "original")
    {
        if (clutBytes)
            recolourEntries(image.data(), clutCount, clutBits, target);
        else
            recolourEntries(image.data() + clutBytes, width * height, static_cast<uint32_t>(bpp), target);
    }
    const std::vector<uint8_t> before(s.written.begin(), s.written.begin() + std::min<size_t>(s.written.size(), 1024u));
    std::memcpy(bytes, image.data(), total);
    s.written = image;
    s.applied = wanted;
    // The game keeps the scarf's palette in graphics memory and only sends it again when a stage loads, so the new
    // colours are also sent there straight away: where the game's upload of it was seen (gs_pcsx2.cpp), else where a
    // search of graphics memory finds the colours it has now (the ones just replaced, the game's own, or the last sent).
    if (clutBytes == 1024u)
    {
        s.pendingGs.assign(image.begin(), image.begin() + 1024);
        bool sent = ps2x::pcsx2gs::resendPalette(s.original.data(), s.previousGs.empty() ? nullptr : s.previousGs.data(),
                                                 s.pendingGs.data());
        if (!sent)
        {
            std::vector<const uint8_t *> candidates;
            if (before.size() == 1024u)
                candidates.push_back(before.data());
            candidates.push_back(s.original.data());
            if (s.previousGs.size() == 1024u)
                candidates.push_back(s.previousGs.data());
            sent = ps2x::pcsx2gs::rewritePaletteInVram(candidates, s.pendingGs.data());
        }
        if (sent)
        {
            s.previousGs = s.pendingGs;
            s.pendingGs.clear();
        }
        else
            std::fprintf(stderr, "[scarf] waiting to see the game upload the scarf palette\n");
    }

    // Have the game upload it again: _hyTextureUnit::_unloadVram (0x339df0) releases the unit's graphics memory, so the
    // next draw sends it again from the data just changed (_reload refuses units with no reload source, word54 = 0).
    GuestInvocation call{};
    call.kind = GuestInvocationKind::HleCall;
    call.context.pc = kReloadPc;
    SET_GPR_U32(&call.context, 4, unit);
    SET_GPR_U32(&call.context, 28, GPR_U32(ctx, 28));
    SET_GPR_U32(&call.context, 29, 0u);
    SET_GPR_U32(&call.context, 31, 0u);
    runtime->eeScheduler().queueInvocation(std::move(call));
    std::fprintf(stderr, "[scarf] colour %s\n", wanted.c_str());
}
