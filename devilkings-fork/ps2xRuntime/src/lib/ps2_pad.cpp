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

// LOCAL PATCH (DualSense): src/lib/ps2_dualsense.cpp
struct Ps2xDualSenseInput
{
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
    uint8_t hat;
    bool square, cross, circle, triangle, l1, r1, l2Button, r2Button, create, options, l3, r3, ps;
};
bool ps2xDualSenseRead(Ps2xDualSenseInput &out);
// LOCAL PATCH (haptics trace): src/lib/ps2_haptics_trace.cpp
void ps2xHapticTraceL2(int pad, int value, bool down);

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
        // LOCAL PATCH: every connected controller is read, not only the chosen one. Steam Input, the EA app and similar tools
        // can take the real DualSense / Xbox pad and hand its input to a virtual controller with another number, so the pad
        // the scan picked sees nothing. Buttons from all of them count; each stick takes the one pushed furthest.
        // Wheels, shifters and pedals stay out (they report gamepad axes of their own).
        static std::vector<int> s_pads;
        if (rescan)
        {
            s_pads.clear();
            for (int i = 0; i < 4; ++i)
            {
                if (!IsGamepadAvailable(i))
                    continue;
                std::string lower(GetGamepadName(i) ? GetGamepadName(i) : "");
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find("shifter") == std::string::npos && lower.find("wheel") == std::string::npos &&
                    lower.find("pedal") == std::string::npos && lower.find("throttle") == std::string::npos)
                    s_pads.push_back(i);
            }
            if (s_pads.empty())
                s_pads.push_back(kGamepad);
            static size_t s_loggedCount = 0u;
            if (s_pads.size() != s_loggedCount)
            {
                s_loggedCount = s_pads.size();
                std::fprintf(stderr, "[pad] reading %zu controller(s):", s_pads.size());
                for (int i : s_pads)
                    std::fprintf(stderr, " %d (%s)", i, GetGamepadName(i) ? GetGamepadName(i) : "?");
                std::fputc('\n', stderr);
            }
        }
        static const std::pair<int, uint16_t> kButtons[] = {
            {GAMEPAD_BUTTON_LEFT_FACE_UP, PAD_UP},         {GAMEPAD_BUTTON_LEFT_FACE_DOWN, PAD_DOWN},
            {GAMEPAD_BUTTON_LEFT_FACE_LEFT, PAD_LEFT},     {GAMEPAD_BUTTON_LEFT_FACE_RIGHT, PAD_RIGHT},
            {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, PAD_CROSS},   {GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, PAD_CIRCLE},
            {GAMEPAD_BUTTON_RIGHT_FACE_LEFT, PAD_SQUARE},  {GAMEPAD_BUTTON_RIGHT_FACE_UP, PAD_TRIANGLE},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_1, PAD_L1},       {GAMEPAD_BUTTON_RIGHT_TRIGGER_1, PAD_R1},
            {GAMEPAD_BUTTON_LEFT_TRIGGER_2, PAD_L2},       {GAMEPAD_BUTTON_RIGHT_TRIGGER_2, PAD_R2},
            {GAMEPAD_BUTTON_MIDDLE_RIGHT, PAD_START},      {GAMEPAD_BUTTON_MIDDLE_LEFT, PAD_SELECT},
            {GAMEPAD_BUTTON_LEFT_THUMB, PAD_L3},           {GAMEPAD_BUTTON_RIGHT_THUMB, PAD_R3}};
        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
        for (int pad : s_pads)
        {
            for (const auto &[button, bit] : kButtons)
                if (IsGamepadButtonDown(pad, button))
                    clearBit(bit);
            {
                // LOCAL PATCH (haptics trace): how far L2 is pressed (raylib: -1 released .. 1 fully in).
                const float l2 = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_TRIGGER);
                ::ps2xHapticTraceL2(pad, static_cast<int>(std::clamp((l2 + 1.0f) * 127.5f, 0.0f, 255.0f)),
                                  IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_TRIGGER_2));
            }
            const float plx = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X), ply = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y);
            const float prx = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_X), pry = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y);
            if (plx * plx + ply * ply > lx * lx + ly * ly)
            {
                lx = plx;
                ly = ply;
            }
            if (prx * prx + pry * pry > rx * rx + ry * ry)
            {
                rx = prx;
                ry = pry;
            }
        }
        // LOCAL PATCH: a DualSense read directly (ps2_dualsense.cpp, for haptics): its buttons and sticks count too. Over
        // Bluetooth the usual controller APIs stop seeing it once haptics start, so this keeps it working.
        {
            Ps2xDualSenseInput ds{};
            if (::ps2xDualSenseRead(ds))
            {
                const bool hatUp = ds.hat == 7 || ds.hat == 0 || ds.hat == 1;
                const bool hatRight = ds.hat >= 1 && ds.hat <= 3;
                const bool hatDown = ds.hat >= 3 && ds.hat <= 5;
                const bool hatLeft = ds.hat >= 5 && ds.hat <= 7;
                const std::pair<bool, uint16_t> pressed[] = {
                    {hatUp, PAD_UP},         {hatDown, PAD_DOWN},       {hatLeft, PAD_LEFT},     {hatRight, PAD_RIGHT},
                    {ds.cross, PAD_CROSS},   {ds.circle, PAD_CIRCLE},   {ds.square, PAD_SQUARE}, {ds.triangle, PAD_TRIANGLE},
                    {ds.l1, PAD_L1},         {ds.r1, PAD_R1},           {ds.l2Button, PAD_L2},   {ds.r2Button, PAD_R2},
                    {ds.options, PAD_START}, {ds.create, PAD_SELECT},   {ds.l3, PAD_L3},         {ds.r3, PAD_R3}};
                for (const auto &[down, bit] : pressed)
                    if (down)
                        clearBit(bit);
                // LOCAL PATCH: with the L2 trigger resistance on, L2 counts as pressed only past the stiff part (the
                // effect gives way at about 70% of its travel): a light touch no longer fires the ninja magic. The
                // controller's own L2 bit (set at a light touch, here or through the usual APIs) is overridden.
                if (ps2x::settings::getBool("input", "trigger_resistance"))
                {
                    if (ds.l2 >= 190)
                        clearBit(PAD_L2);
                    else
                        btns |= PAD_L2;
                }
                const float dlx = (ds.lx - 127.5f) / 127.5f, dly = (ds.ly - 127.5f) / 127.5f;
                const float drx = (ds.rx - 127.5f) / 127.5f, dry = (ds.ry - 127.5f) / 127.5f;
                if (dlx * dlx + dly * dly > lx * lx + ly * ly)
                {
                    lx = dlx;
                    ly = dly;
                }
                if (drx * drx + dry * dry > rx * rx + ry * ry)
                {
                    rx = drx;
                    ry = dry;
                }
            }
        }
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
