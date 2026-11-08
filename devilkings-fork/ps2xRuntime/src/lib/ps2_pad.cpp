#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "runtime/ps2_settings.h"

extern std::atomic<bool> g_ps2MouseLookActive; // Pad.cpp, set by the window loop (ps2_runtime.cpp)
extern std::atomic<float> g_ps2MouseStickX;
extern std::atomic<float> g_ps2MouseStickY;

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    // LOCAL PATCH: GLFW only reports a joystick as a gamepad when it has a mapping, and older tables lack the
    // DualSense Edge (054c:0df2). Same layout as the DualSense entry, for both Windows GUID spellings.
    static bool s_mappingsAdded = false;
    if (!s_mappingsAdded && IsWindowReady())
    {
        s_mappingsAdded = true;
        static const char kPs5Layout[] =
            "a:b1,b:b2,x:b0,y:b3,back:b8,guide:b12,start:b9,leftstick:b10,rightstick:b11,"
            "leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
            "leftx:a0,lefty:a1,rightx:a2,righty:a5,lefttrigger:a3,righttrigger:a4,touchpad:b13,platform:Windows,\n";
        const std::string mappings = std::string("030000004c050000f20d000000000000,DualSense Edge,") + kPs5Layout +
                                     "030000004c050000e60c000000000000,PS5 Controller," + kPs5Layout;
        SetGamepadMappings(mappings.c_str());
    }
    // Prefer a PlayStation pad; racing wheels, shifters and pedals also show up as gamepads and must not win.
    // The scan lowercases device names, so it runs at most twice a second; the choice is reused in between.
    static int s_chosenGamepad = -1;
    static std::chrono::steady_clock::time_point s_lastScan{};
    int kGamepad = s_chosenGamepad;
    const auto scanNow = std::chrono::steady_clock::now();
    const bool rescan = s_lastScan.time_since_epoch().count() == 0 || scanNow - s_lastScan > std::chrono::milliseconds(500);
    if (rescan)
    {
        s_lastScan = scanNow;
        kGamepad = -1;
        const auto contains = [](const char *name, const char *part)
        {
            std::string lower(name ? name : "");
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower.find(part) != std::string::npos;
        };
        int fallback = -1;
        for (int i = 0; i < 4 && kGamepad < 0; ++i)
        {
            if (!IsGamepadAvailable(i))
                continue;
            const char *name = GetGamepadName(i);
            if (contains(name, "dualsense") || contains(name, "ps5") || contains(name, "dualshock") ||
                contains(name, "wireless controller") || contains(name, "ps4") || contains(name, "xbox"))
                kGamepad = i;
            else if (fallback < 0 && !contains(name, "shifter") && !contains(name, "wheel") &&
                     !contains(name, "pedal") && !contains(name, "throttle"))
                fallback = i;
        }
        if (kGamepad < 0)
            kGamepad = fallback;
        s_chosenGamepad = kGamepad;
    }
    const bool useGamepad = kGamepad >= 0;
    static int s_loggedPad = -2;
    if (kGamepad != s_loggedPad)
    {
        s_loggedPad = kGamepad;
        std::fprintf(stderr, "[pad] using gamepad %d: %s\n", kGamepad, useGamepad ? GetGamepadName(kGamepad) : "(none, keyboard)");
    }
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    // LOCAL PATCH: while a menu is open (the in-game menu, F1) the controller drives the menu, not the game: all buttons
    // released and both sticks centred.
    if (ps2x::settings::inputBlocked())
    {
        data[2] = 0xFF;
        data[3] = 0xFF;
        data[4] = data[5] = data[6] = data[7] = kPadStickCenter;
        return true;
    }

    if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        // LOCAL PATCH: a round dead zone ([input] stick_deadzone, % of the stick's travel, 15 by default): an analog stick
        // that rests slightly off centre (a worn or drifting DualSense / Xbox pad) would otherwise walk the character or
        // turn the camera on its own. Outside it the stick is stretched to its full range.
        const float zone = static_cast<float>(std::clamp(ps2x::settings::getNumber("input", "stick_deadzone"), 0, 50)) / 100.0f;
        const auto stick = [zone](float x, float y, uint8_t &outX, uint8_t &outY)
        {
            const float length = std::sqrt(x * x + y * y);
            if (length <= zone || length < 0.0001f)
            {
                outX = outY = 128;
                return;
            }
            const float scale = std::min(1.0f, (length - zone) / (1.0f - zone)) / length;
            outX = static_cast<uint8_t>(std::clamp(128.0f + x * scale * 127.0f, 0.0f, 255.0f));
            outY = static_cast<uint8_t>(std::clamp(128.0f + y * scale * 127.0f, 0.0f, 255.0f));
        };
        stick(lx, ly, data[6], data[7]);
        stick(rx, ry, data[4], data[5]);
    }
    // LOCAL PATCH: the keyboard works alongside a connected controller too (it used to be ignored while one was plugged in).
    if (!ps2x::settings::inputBlocked())
    {
        // Keyboard bindings from the settings ([controls], the F1 menu), parsed again when
        // the settings change.
        static uint64_t s_version = 0u;
        static std::vector<std::pair<uint16_t, std::vector<int>>> s_bindings;
        const uint64_t version = ps2x::settings::version();
        if (version != s_version)
        {
            s_bindings.clear();
            for (const ps2x::settings::PadButton &button : ps2x::settings::padButtons())
            {
                std::vector<int> keys;
                const std::string list = ps2x::settings::get("controls", button.key);
                size_t begin = 0;
                while (begin <= list.size())
                {
                    size_t comma = list.find(',', begin);
                    if (comma == std::string::npos)
                        comma = list.size();
                    if (const int code = ps2x::settings::keyCode(list.substr(begin, comma - begin)))
                        keys.push_back(code);
                    begin = comma + 1;
                }
                s_bindings.emplace_back(button.bit, std::move(keys));
            }
            s_version = version;
        }
        // LOCAL PATCH: Enter pressed together with Alt is the window shortcut (Alt+Enter, ps2_runtime.cpp), not Start: it is
        // ignored until it is let go.
        static bool s_altEnter = false;
        const bool enterDown = IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER);
        if (enterDown && (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)))
            s_altEnter = true;
        else if (!enterDown)
            s_altEnter = false;
        for (const auto &[bit, keys] : s_bindings)
        {
            for (int key : keys)
            {
                if (s_altEnter && (key == KEY_ENTER || key == KEY_KP_ENTER))
                    continue;
                if (IsKeyDown(key))
                {
                    clearBit(bit);
                    break;
                }
            }
        }
    }

    // LOCAL PATCH: the camera (the pad's right stick) on keyboard and mouse. The four keys are [controls] camera_up / camera_down /
    // camera_left / camera_right (I K J L by default, rebindable in the launcher and the F1 menu). The mouse, while the window
    // has captured it (ps2_runtime.cpp, F10 switches that on and off), adds its movement; left click is Square, right click Circle.
    if (!ps2x::settings::inputBlocked())
    {
        static uint64_t s_cameraVersion = ~0ull;
        static std::vector<int> s_camera[4];
        const uint64_t version = ps2x::settings::version();
        if (version != s_cameraVersion)
        {
            static const char *const kNames[4] = {"camera_up", "camera_down", "camera_left", "camera_right"};
            for (int axis = 0; axis < 4; ++axis)
            {
                s_camera[axis].clear();
                const std::string list = ps2x::settings::get("controls", kNames[axis]);
                size_t begin = 0;
                while (begin <= list.size())
                {
                    size_t comma = list.find(',', begin);
                    if (comma == std::string::npos)
                        comma = list.size();
                    if (const int code = ps2x::settings::keyCode(list.substr(begin, comma - begin)))
                        s_camera[axis].push_back(code);
                    begin = comma + 1;
                }
            }
            s_cameraVersion = version;
        }
        const auto held = [](const std::vector<int> &keys)
        {
            for (int key : keys)
                if (IsKeyDown(key))
                    return true;
            return false;
        };
        float cx = (held(s_camera[3]) ? 1.0f : 0.0f) - (held(s_camera[2]) ? 1.0f : 0.0f);
        float cy = (held(s_camera[1]) ? 1.0f : 0.0f) - (held(s_camera[0]) ? 1.0f : 0.0f);
        if (g_ps2MouseLookActive.load(std::memory_order_relaxed))
        {
            cx += g_ps2MouseStickX.load(std::memory_order_relaxed);
            cy += g_ps2MouseStickY.load(std::memory_order_relaxed);
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
                clearBit(PAD_SQUARE);
            if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
                clearBit(PAD_CIRCLE);
        }
        if (cx != 0.0f || cy != 0.0f)
        {
            data[4] = static_cast<uint8_t>(std::clamp(128.0f + std::clamp(cx, -1.0f, 1.0f) * 127.0f, 0.0f, 255.0f));
            data[5] = static_cast<uint8_t>(std::clamp(128.0f + std::clamp(cy, -1.0f, 1.0f) * 127.0f, 0.0f, 255.0f));
        }
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
