// LOCAL PATCH (mod): the close-up, over-the-shoulder camera. Settings [game] close_camera (the launcher's Display tab,
// F3 in game).
//
// Where: CamMngStatus::Flush (0x2e5150) hands the gameplay camera's eye ($s0+0x20) and watch point ($s0+0x10) to
// HyCamera::SetLookAt. Just before that call the two points are moved (ps2xCloseCameraApply), and right after it they
// are put back (ps2xCloseCameraRestore): the camera manager starts from its own values next frame, so the change never
// feeds back into the game's camera (moving them earlier, in CCameraManage::MainOut, made the two fight and flicker).
// Cutscene cameras do not go through here.
//
// What: the eye comes in to a fraction of its distance and a little to the side, the watch point moves a little to the
// same side, so Hotsuma stands off-centre. When the game itself has moved the camera in (locking on with R1 brings it
// closer than usual) the close-up fades out, so the two never add up. Switching eases in and out over half a second.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "runtime/ps2_settings.h"

namespace
{
    constexpr float kDistance = 0.72f; // the eye's distance from the watch point, as a fraction of the game's
    constexpr float kShoulder = 0.08f; // sideways shift of the eye, as a fraction of the game's distance
    constexpr float kWatchSide = 0.6f; // the watch point moves this fraction of the eye's sideways shift
    constexpr float kEaseSeconds = 0.45f;

    float g_amount = 0.0f;     // 0 = the game's camera, 1 = fully close
    float g_usual = 0.0f;      // the game's usual camera distance (follows the longest recent distance slowly)
    std::chrono::steady_clock::time_point g_last{};

    // The points as the game had them, put back after SetLookAt.
    bool g_saved = false;
    uint32_t g_eyeAddress = 0, g_watchAddress = 0;
    float g_eye[3], g_watch[3];

    bool readPoint(const uint8_t *rdram, uint32_t address, float p[3])
    {
        const uint32_t physical = address & 0x1FFFFFFFu;
        if (physical + 12u > 0x2000000u)
            return false;
        std::memcpy(p, rdram + physical, 12u);
        return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
    }

    void writePoint(uint8_t *rdram, uint32_t address, const float p[3])
    {
        std::memcpy(rdram + (address & 0x1FFFFFFFu), p, 12u);
    }
}

void ps2xCloseCameraApply(uint8_t *rdram, uint32_t eyeAddress, uint32_t watchAddress)
{
    g_saved = false;
    static uint64_t s_version = 0;
    static bool s_on = false;
    if (const uint64_t v = ps2x::settings::version(); v != s_version)
    {
        s_version = v;
        s_on = ps2x::settings::getBool("game", "close_camera");
    }
    const auto now = std::chrono::steady_clock::now();
    float dt = g_last.time_since_epoch().count() ? std::chrono::duration<float>(now - g_last).count() : 0.0f;
    g_last = now;
    dt = std::clamp(dt, 0.0f, 0.1f);
    const float target = s_on ? 1.0f : 0.0f;
    if (g_amount < target)
        g_amount = std::min(target, g_amount + dt / kEaseSeconds);
    else if (g_amount > target)
        g_amount = std::max(target, g_amount - dt / kEaseSeconds);

    float eye[3], watch[3];
    if (!readPoint(rdram, eyeAddress, eye) || !readPoint(rdram, watchAddress, watch))
        return;
    const float d[3] = {eye[0] - watch[0], eye[1] - watch[1], eye[2] - watch[2]};
    const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float flat = std::sqrt(d[0] * d[0] + d[2] * d[2]);
    if (dist < 1e-3f || flat < 1e-3f)
        return;
    // The usual distance: jumps up to a longer one, creeps down (over ~10 s) to a shorter one.
    g_usual = dist > g_usual ? dist : g_usual + (dist - g_usual) * std::min(1.0f, dt * 0.1f);
    if (g_amount <= 0.0f)
        return;
    // Fade out when the game has already brought the camera in (lock-on): full effect at 85% of the usual distance or
    // more, none at 60% or less.
    const float zoomed = std::clamp((dist / std::max(g_usual, 1e-3f) - 0.60f) / 0.25f, 0.0f, 1.0f);
    const float ease = g_amount * g_amount * (3.0f - 2.0f * g_amount);
    const float k = ease * zoomed;
    if (k <= 0.0f)
        return;

    const float side[3] = {d[2] / flat, 0.0f, -d[0] / flat}; // across the view, level with the ground
    const float scale = 1.0f + (kDistance - 1.0f) * k;
    const float shift = kShoulder * dist * k;
    float newEye[3], newWatch[3];
    for (int i = 0; i < 3; ++i)
    {
        newEye[i] = watch[i] + d[i] * scale + side[i] * shift;
        newWatch[i] = watch[i] + side[i] * shift * kWatchSide;
    }
    std::memcpy(g_eye, eye, sizeof(g_eye));
    std::memcpy(g_watch, watch, sizeof(g_watch));
    g_eyeAddress = eyeAddress;
    g_watchAddress = watchAddress;
    g_saved = true;
    writePoint(rdram, eyeAddress, newEye);
    writePoint(rdram, watchAddress, newWatch);
}

void ps2xCloseCameraRestore(uint8_t *rdram)
{
    if (!g_saved)
        return;
    g_saved = false;
    writePoint(rdram, g_eyeAddress, g_eye);
    writePoint(rdram, g_watchAddress, g_watch);
}
