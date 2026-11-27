#pragma once
// LOCAL PATCH (cinematic bars mod): letterbox bands removed from the GIF packets (gs_bars.cpp).
#include <cstdint>
#include <vector>

namespace ps2x::bars
{
    struct State
    {
        bool primCtxt = false;
        bool prmodeCtxt = false;
        bool prmodeCont = true;
        bool primTme = false;
        bool prmodeTme = false;
        uint16_t ofx[2] = {0, 0}; // XYOFFSET, 12.4
        uint16_t ofy[2] = {0, 0};
        uint32_t width[2] = {640, 640};
        uint8_t tfx[2] = {0, 0}; // TEX0 TFX (0 = modulate) - LOCAL PATCH (black strips)
    };

    // The packet to send on: `data` itself, or `scratch` with the bands made zero-height.
    // removeBands false (the bars kept): only the game's stage-intro strips, drawn black (LOCAL PATCH (black strips)).
    const uint8_t *apply(const uint8_t *data, uint32_t size, const State &state, std::vector<uint8_t> &scratch,
                         bool removeBands = true);
}
