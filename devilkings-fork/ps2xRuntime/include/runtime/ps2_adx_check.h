#pragma once
// LOCAL DEBUG: self-check of Shinobi's ADX stereo decoder (ADX_DecodeSteFloatAsSte, 0x265d78) for the right-ear static.
// PS2X_ADX_CHECK=1 (launcher "Record diagnostics"): the decoder's start and end call these (inserted into the translated
// function in src/runner, see scripts/patch_adx_check.py). At the end the checker decodes the same frames again with a plain
// C++ reference (once with the host's float rules, once with the EE's: round toward zero, no denormals) and compares them with
// what the translated function wrote, and counts sharp edges in the decoder's own output, per channel. One "[adx-check]" line a
// second plus "[adx-bad]" lines for the first mismatches. Cheap when the variable is not set (one flag test).
#include <cstdint>

namespace ps2x::adxcheck
{
    [[nodiscard]] bool enabled();
    // Fresh entry of the function: a0 = ADX data (left frame, right frame, left frame ...), a1 = frame count (both channels),
    // a2 / t0 = left / right output, a3 / t1 = left / right history (2 x int16 each), t2 / t3 = the two filter coefficients.
    void begin(uint8_t *rdram, uint32_t src, int32_t frames, uint32_t outLeft, uint32_t histLeft, uint32_t outRight,
               uint32_t histRight, uint32_t coefA, uint32_t coefB);
    // The function's return (not its pause points): histLeft / histRight are its $t5 / $t6 (the history pointers).
    void end(uint8_t *rdram, uint32_t histLeft, uint32_t histRight);
}
