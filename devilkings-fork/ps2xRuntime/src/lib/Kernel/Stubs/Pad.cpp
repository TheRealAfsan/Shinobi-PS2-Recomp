#include <atomic>
#include <cmath>
#include <algorithm>
#include "Common.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_settings.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Pad.h"

extern std::atomic<bool> g_ps2StageDetected; // ps2_vu1_core.cpp: a stage is running (stops scripted START presses)

// The window loop (ps2_runtime.cpp) sets these: the mouse is captured and turns the camera. The stick values are -1..1.
std::atomic<bool> g_ps2MouseLookActive{false};
std::atomic<float> g_ps2MouseStickX{0.0f};
std::atomic<float> g_ps2MouseStickY{0.0f};

// LOCAL PATCH (haptics trace): src/lib/ps2_haptics_trace.cpp
void ps2xHapticTrace(const char *tag, uint32_t a, uint32_t b, uint32_t c);

namespace ps2_stubs
{
    namespace
    {
        // LOCAL DEBUG (PS2X_PAD_TRACE): log each pad call with its arguments and return value for the
        // first 80 calls, so a game's controller state machine can be followed.
        struct PadTraceGuard
        {
            const char *name;
            R5900Context *ctx;
            uint32_t a0, a1, a2, a3;
            PadTraceGuard(const char *n, R5900Context *c)
                : name(n), ctx(c), a0(getRegU32(c, 4)), a1(getRegU32(c, 5)), a2(getRegU32(c, 6)), a3(getRegU32(c, 7))
            {
            }
            ~PadTraceGuard()
            {
                static const bool enabled = std::getenv("PS2X_PAD_TRACE") != nullptr;
                static uint32_t shown = 0u;
                if (!enabled || shown >= 80u)
                    return;
                ++shown;
                std::fprintf(stderr, "[pad] %-22s a0=%x a1=%x a2=%x a3=%x -> v0=%d\n", name, a0, a1, a2, a3,
                             static_cast<int32_t>(getRegU32(ctx, 2)));
            }
        };

        constexpr uint8_t kPadModeDigital = 0x41;
        constexpr uint8_t kPadModeDualShock = 0x73;
        constexpr uint8_t kPadAnalogCenter = 0x80;
        constexpr int32_t kPadTypeDigital = 4;
        constexpr int32_t kPadTypeDualShock = 7;
        constexpr int32_t kPadStateDisconnected = 0;
        constexpr int32_t kPadStateExecCmd = 5;
        constexpr int32_t kPadStateStable = 6;
        constexpr size_t kPadPortCount = 2;
        constexpr size_t kPadSlotCount = 1;

        constexpr uint16_t kPadBtnSelect = 1u << 0;
        constexpr uint16_t kPadBtnL3 = 1u << 1;
        constexpr uint16_t kPadBtnR3 = 1u << 2;
        constexpr uint16_t kPadBtnStart = 1u << 3;
        constexpr uint16_t kPadBtnUp = 1u << 4;
        constexpr uint16_t kPadBtnRight = 1u << 5;
        constexpr uint16_t kPadBtnDown = 1u << 6;
        constexpr uint16_t kPadBtnLeft = 1u << 7;
        constexpr uint16_t kPadBtnL2 = 1u << 8;
        constexpr uint16_t kPadBtnR2 = 1u << 9;
        constexpr uint16_t kPadBtnL1 = 1u << 10;
        constexpr uint16_t kPadBtnR1 = 1u << 11;
        constexpr uint16_t kPadBtnTriangle = 1u << 12;
        constexpr uint16_t kPadBtnCircle = 1u << 13;
        constexpr uint16_t kPadBtnCross = 1u << 14;
        constexpr uint16_t kPadBtnSquare = 1u << 15;

        struct PadInputState
        {
            uint16_t buttons = 0xFFFF; // active-low
            uint8_t rx = kPadAnalogCenter;
            uint8_t ry = kPadAnalogCenter;
            uint8_t lx = kPadAnalogCenter;
            uint8_t ly = kPadAnalogCenter;
        };

        struct PadPortState
        {
            bool open = false;
            bool analogMode = false;  // real pads power up DIGITAL (CURID=4, mode 0x41)
            bool pressureEnabled = false;
            bool lastUsedOverride = false;
            bool lastUsedBackend = false;
            bool lastReadOk = false;
            uint16_t buttonMask = 0xFFFFu;
            uint32_t dmaAddr = 0u;
            uint32_t reqState = 0u;
            uint32_t transientState = 0u;
            PadInputState lastInput{};
            uint8_t lastData[32]{};
            uint32_t readCount = 0u;
            uint32_t lastReadDataAddr = 0u;
        };

        std::mutex g_padOverrideMutex;
        std::mutex g_padStateMutex;
        bool g_padOverrideEnabled = false;
        PadInputState g_padOverrideState{};
        PadPortState g_padPorts[kPadPortCount]{};
        int g_padReadLogCount = 0;

        uint8_t axisToByte(float axis)
        {
            axis = std::clamp(axis, -1.0f, 1.0f);
            const float mapped = (axis + 1.0f) * 127.5f;
            return static_cast<uint8_t>(std::lround(mapped));
        }

        void setButton(PadInputState &state, uint16_t mask, bool pressed)
        {
            if (pressed)
            {
                state.buttons = static_cast<uint16_t>(state.buttons & ~mask);
            }
        }

        // PS2X_PAD_SCRIPT="from-to:BTN+BTN;..." presses buttons on port 0 for
        // the given VSync tick ranges, e.g. "1700-1730:LEFT;1760-1790:CROSS".
        // Lets automated runs drive menus without host keyboard focus.
        // PS2X_PAD_SCRIPT_CLOCK=frames counts the game's frames (display swaps)
        // instead: on a slower host, loading takes more VSyncs, but the presses
        // still come at the same point of the game.
        struct PadScriptStep
        {
            uint64_t from = 0u;
            uint64_t to = 0u;
            uint16_t mask = 0u;
            int8_t stickX = 0; // LOCAL PATCH: left stick, -1 / +1 = LSLEFT / LSRIGHT (full deflection)
            int8_t stickY = 0; // -1 / +1 = LSUP / LSDOWN
            int8_t rstickX = 0; // right stick (the camera): RSLEFT / RSRIGHT
            int8_t rstickY = 0; // RSUP / RSDOWN
        };

        const std::vector<PadScriptStep> &padScript()
        {
            static const std::vector<PadScriptStep> steps = []()
            {
                std::vector<PadScriptStep> parsed;
                const char *env = std::getenv("PS2X_PAD_SCRIPT");
                // LOCAL PATCH: PS2X_SKIP_INTRO=1 (or [game] skip_intro=1 in settings.ini) taps CROSS through the
                // memory-card dialogs and START through the logos, intro movie and cutscenes, so the title screen
                // comes up on its own. It stops tapping after tick 9600, before play begins (START would pause).
                static std::string s_skipScript;
                // LOCAL PATCH (Shinobi): only with PS2X_SKIP_INTRO=script. The timed taps were made for Devil Kings: in
                // Shinobi they start after the logos are over and can still be pressing START once play has begun (pause).
                // [game] skip_intro now shortens Shinobi's logos directly (ps2xLogoSkipHook, ps2_runtime.cpp).
                if (!env)
                {
                    const char *skipEnv = std::getenv("PS2X_SKIP_INTRO");
                    const bool skip = skipEnv && std::strcmp(skipEnv, "script") == 0;
                    if (!skip)
                    {
                        return parsed;
                    }
                    for (int t = 2500; t <= 4500; t += 150)
                        s_skipScript += std::to_string(t) + "-" + std::to_string(t + 20) + ":CROSS;";
                    for (int t = 3000; t <= 9600; t += 70)
                        s_skipScript += std::to_string(t) + "-" + std::to_string(t + 15) + ":START;";
                    env = s_skipScript.c_str();
                }
                static const std::pair<const char *, uint16_t> kNames[] = {
                    {"UP", kPadBtnUp}, {"DOWN", kPadBtnDown}, {"LEFT", kPadBtnLeft}, {"RIGHT", kPadBtnRight},
                    {"CROSS", kPadBtnCross}, {"CIRCLE", kPadBtnCircle}, {"SQUARE", kPadBtnSquare},
                    {"TRIANGLE", kPadBtnTriangle}, {"START", kPadBtnStart}, {"SELECT", kPadBtnSelect},
                    {"L1", kPadBtnL1}, {"R1", kPadBtnR1}, {"L2", kPadBtnL2}, {"R2", kPadBtnR2},
                    {"L3", kPadBtnL3}, {"R3", kPadBtnR3}};
                std::string text(env);
                size_t pos = 0u;
                while (pos < text.size())
                {
                    const size_t end = std::min(text.find(';', pos), text.size());
                    const std::string item = text.substr(pos, end - pos);
                    pos = end + 1u;
                    const size_t dash = item.find('-');
                    const size_t colon = item.find(':');
                    if (dash == std::string::npos || colon == std::string::npos || colon < dash)
                    {
                        continue;
                    }
                    PadScriptStep step{};
                    step.from = std::strtoull(item.substr(0, dash).c_str(), nullptr, 10);
                    step.to = std::strtoull(item.substr(dash + 1u, colon - dash - 1u).c_str(), nullptr, 10);
                    size_t namePos = colon + 1u;
                    while (namePos < item.size())
                    {
                        const size_t nameEnd = std::min(item.find('+', namePos), item.size());
                        const std::string name = item.substr(namePos, nameEnd - namePos);
                        if (name == "LSLEFT") step.stickX = -1;
                        else if (name == "LSRIGHT") step.stickX = 1;
                        else if (name == "LSUP") step.stickY = -1;
                        else if (name == "LSDOWN") step.stickY = 1;
                        else if (name == "RSLEFT") step.rstickX = -1;
                        else if (name == "RSRIGHT") step.rstickX = 1;
                        else if (name == "RSUP") step.rstickY = -1;
                        else if (name == "RSDOWN") step.rstickY = 1;
                        for (const auto &[label, mask] : kNames)
                        {
                            if (name == label)
                            {
                                step.mask = static_cast<uint16_t>(step.mask | mask);
                            }
                        }
                        namePos = nameEnd + 1u;
                    }
                    parsed.push_back(step);
                }
                return parsed;
            }();
            return steps;
        }

        void applyPadScript(PadInputState &state, int port, PS2Runtime *runtime)
        {
            if (port != 0 || !runtime || padScript().empty())
            {
                return;
            }
            static const bool s_gameFrames = []()
            {
                const char *clock = std::getenv("PS2X_PAD_SCRIPT_CLOCK");
                return clock && std::string(clock) == "frames";
            }();
            const uint64_t tick = s_gameFrames ? ps2x::settings::gameFrames() : runtime->eeScheduler().currentVSyncTick();
            // LOCAL PATCH: once a stage is running (VU1 load detector in ps2_vu1_core.cpp) the script stops pressing
            // buttons altogether, so a test never pauses the game or attacks on top of a human player. Set
            // PS2X_PAD_SCRIPT_IN_STAGE=1 to keep a scripted run going in the stage (START is still never pressed there).
            static const bool s_scriptInStage = []()
            {
                const char *value = std::getenv("PS2X_PAD_SCRIPT_IN_STAGE");
                return value && value[0] == '1';
            }();
            const bool stageRunning = ::g_ps2StageDetected.load(std::memory_order_relaxed);
            if (stageRunning && !s_scriptInStage)
                return;
            for (const PadScriptStep &step : padScript())
            {
                if (tick >= step.from && tick <= step.to)
                {
                    setButton(state, stageRunning ? static_cast<uint16_t>(step.mask & ~kPadBtnStart) : step.mask, true);
                    if (step.stickX != 0)
                        state.lx = step.stickX < 0 ? 0u : 255u;
                    if (step.stickY != 0)
                        state.ly = step.stickY < 0 ? 0u : 255u;
                    if (step.rstickX != 0)
                        state.rx = step.rstickX < 0 ? 0u : 255u;
                    if (step.rstickY != 0)
                        state.ry = step.rstickY < 0 ? 0u : 255u;
                }
            }
        }

        int findFirstGamepad()
        {
            for (int i = 0; i < 4; ++i)
            {
                if (IsGamepadAvailable(i))
                {
                    return i;
                }
            }
            return -1;
        }

        void applyGamepadState(PadInputState &state)
        {
            if (!IsWindowReady())
            {
                return;
            }

            const int gamepad = findFirstGamepad();
            if (gamepad < 0)
            {
                return;
            }

            // Raylib mapping (PS2 -> raylib buttons/axes):
            // D-Pad -> LEFT_FACE_*, Cross/Circle/Square/Triangle -> RIGHT_FACE_*
            // L1/R1 -> TRIGGER_1, L2/R2 -> TRIGGER_2, L3/R3 -> THUMB
            // Select/Start -> MIDDLE_LEFT/MIDDLE_RIGHT
            // LOCAL PATCH: a round dead zone on both sticks (stick_deadzone, % in [input]) so a resting or slightly worn
            // stick (a DualSense / Xbox pad that reads 0.05 off centre) does not walk the character or turn the camera.
            {
                const float zone = static_cast<float>(std::clamp(ps2x::settings::getNumber("input", "stick_deadzone"), 0, 50)) / 100.0f;
                auto stick = [zone](float x, float y, uint8_t &outX, uint8_t &outY)
                {
                    const float length = std::sqrt(x * x + y * y);
                    if (length <= zone || length <= 0.0001f)
                    {
                        outX = 128u;
                        outY = 128u;
                        return;
                    }
                    const float scaled = std::min(1.0f, (length - zone) / (1.0f - zone));
                    outX = axisToByte(x / length * scaled);
                    outY = axisToByte(y / length * scaled);
                };
                stick(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X), GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y), state.lx, state.ly);
                stick(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X), GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y), state.rx, state.ry);
            }

            setButton(state, kPadBtnUp, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_UP));
            setButton(state, kPadBtnDown, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN));
            setButton(state, kPadBtnLeft, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT));
            setButton(state, kPadBtnRight, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT));

            setButton(state, kPadBtnCross, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
            setButton(state, kPadBtnCircle, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
            setButton(state, kPadBtnSquare, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT));
            setButton(state, kPadBtnTriangle, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP));

            setButton(state, kPadBtnL1, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1));
            setButton(state, kPadBtnR1, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1));
            setButton(state, kPadBtnL2, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2));
            setButton(state, kPadBtnR2, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2));

            setButton(state, kPadBtnL3, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_THUMB));
            setButton(state, kPadBtnR3, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_THUMB));

            setButton(state, kPadBtnSelect, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_LEFT));
            setButton(state, kPadBtnStart, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT));
        }

        void applyKeyboardState(PadInputState &state, bool allowAnalog)
        {
            if (!IsWindowReady())
            {
                return;
            }

            // Keyboard mapping (PS2 -> keys):
            // D-Pad: arrows, Square/Cross/Circle/Triangle: Z/X/C/V
            // L1/R1: Q/E, L2/R2: 1/3, Start/Select: Enter/RightShift
            // L3/R3: LeftCtrl/RightCtrl, Analog left: WASD
            setButton(state, kPadBtnUp, IsKeyDown(KEY_UP));
            setButton(state, kPadBtnDown, IsKeyDown(KEY_DOWN));
            setButton(state, kPadBtnLeft, IsKeyDown(KEY_LEFT));
            setButton(state, kPadBtnRight, IsKeyDown(KEY_RIGHT));

            setButton(state, kPadBtnSquare, IsKeyDown(KEY_Z));
            setButton(state, kPadBtnCross, IsKeyDown(KEY_X));
            setButton(state, kPadBtnCircle, IsKeyDown(KEY_C));
            setButton(state, kPadBtnTriangle, IsKeyDown(KEY_V));

            setButton(state, kPadBtnL1, IsKeyDown(KEY_Q));
            setButton(state, kPadBtnR1, IsKeyDown(KEY_E));
            setButton(state, kPadBtnL2, IsKeyDown(KEY_ONE));
            setButton(state, kPadBtnR2, IsKeyDown(KEY_THREE));

            // Not while Alt is held: Alt+Enter is the window shortcut (ps2_runtime.cpp).
            setButton(state, kPadBtnStart, IsKeyDown(KEY_ENTER) && !IsKeyDown(KEY_LEFT_ALT) && !IsKeyDown(KEY_RIGHT_ALT));
            setButton(state, kPadBtnSelect, IsKeyDown(KEY_RIGHT_SHIFT));
            setButton(state, kPadBtnL3, IsKeyDown(KEY_LEFT_CONTROL));
            setButton(state, kPadBtnR3, IsKeyDown(KEY_RIGHT_CONTROL));

            if (!allowAnalog)
            {
                return;
            }

            float ax = 0.0f;
            float ay = 0.0f;
            if (IsKeyDown(KEY_D))
                ax += 1.0f;
            if (IsKeyDown(KEY_A))
                ax -= 1.0f;
            if (IsKeyDown(KEY_S))
                ay += 1.0f;
            if (IsKeyDown(KEY_W))
                ay -= 1.0f;

            if (ax != 0.0f || ay != 0.0f)
            {
                state.lx = axisToByte(ax);
                state.ly = axisToByte(ay);
            }

            // LOCAL PATCH: the camera. I J K L is the right stick; the mouse (when the window set g_ps2MouseLookActive, see
            // ps2_runtime.cpp) adds to it. Left click = Square, right click = Circle while the mouse is captured.
            float cx = 0.0f;
            float cy = 0.0f;
            if (IsKeyDown(KEY_L))
                cx += 1.0f;
            if (IsKeyDown(KEY_J))
                cx -= 1.0f;
            if (IsKeyDown(KEY_K))
                cy += 1.0f;
            if (IsKeyDown(KEY_I))
                cy -= 1.0f;
            if (g_ps2MouseLookActive.load(std::memory_order_relaxed))
            {
                cx += g_ps2MouseStickX.load(std::memory_order_relaxed);
                cy += g_ps2MouseStickY.load(std::memory_order_relaxed);
                setButton(state, kPadBtnSquare, IsKeyDown(KEY_Z) || IsMouseButtonDown(MOUSE_BUTTON_LEFT));
                setButton(state, kPadBtnCircle, IsKeyDown(KEY_C) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT));
            }
            if (cx != 0.0f || cy != 0.0f)
            {
                state.rx = axisToByte(cx);
                state.ry = axisToByte(cy);
            }
        }

        void resetPadStateLocked()
        {
            for (PadPortState &portState : g_padPorts)
            {
                portState = PadPortState{};
            }
        }

        PadPortState *lookupPadPortStateLocked(int port, int slot)
        {
            if (port < 0 || port >= static_cast<int>(kPadPortCount))
            {
                return nullptr;
            }
            if (slot < 0 || slot >= static_cast<int>(kPadSlotCount))
            {
                return nullptr;
            }
            return &g_padPorts[port];
        }

        void initializePadPortLocked(PadPortState &portState, uint32_t dmaAddr)
        {
            portState.open = true;
            portState.analogMode = false;  // real pads open DIGITAL
            portState.pressureEnabled = false;
            portState.buttonMask = 0xFFFFu;
            portState.dmaAddr = dmaAddr;
            portState.reqState = 0u;
            portState.transientState = 0u;
        }

        void queueExecCmdStateLocked(PadPortState &portState)
        {
            // LOCAL CHANGE: the one-shot "executing command" (5) answer is no longer queued. Shinobi's
            // pad state machine (_izPadPort::_process) ends with a scePadGetState check that accepts only
            // 6 or 2 and otherwise restarts from scratch inside the same call; every restart runs
            // scePadSetMainMode, which queued another 5 for that very check, so it never returned.
            // Real hardware leaves ExecCmd for a few milliseconds only. The pad stays stable instead.
            (void)portState;
            (void)kPadStateExecCmd;
        }

        uint8_t pressureValue(const PadInputState &state, const PadPortState &portState, uint16_t mask)
        {
            if (!portState.pressureEnabled)
            {
                return 0u;
            }
            if ((portState.buttonMask & mask) == 0u)
            {
                return 0u;
            }
            return ((state.buttons & mask) == 0u) ? 0xFFu : 0u;
        }

        void fillPadStatus(uint8_t *data, const PadInputState &state, const PadPortState &portState)
        {
            std::memset(data, 0, 32);
            data[1] = portState.analogMode ? kPadModeDualShock : kPadModeDigital;
            data[2] = static_cast<uint8_t>(state.buttons & 0xFFu);
            data[3] = static_cast<uint8_t>((state.buttons >> 8) & 0xFFu);
            data[4] = state.rx;
            data[5] = state.ry;
            data[6] = state.lx;
            data[7] = state.ly;
            data[8] = pressureValue(state, portState, kPadBtnRight);
            data[9] = pressureValue(state, portState, kPadBtnLeft);
            data[10] = pressureValue(state, portState, kPadBtnUp);
            data[11] = pressureValue(state, portState, kPadBtnDown);
            data[12] = pressureValue(state, portState, kPadBtnTriangle);
            data[13] = pressureValue(state, portState, kPadBtnCircle);
            data[14] = pressureValue(state, portState, kPadBtnCross);
            data[15] = pressureValue(state, portState, kPadBtnSquare);
            data[16] = pressureValue(state, portState, kPadBtnL1);
            data[17] = pressureValue(state, portState, kPadBtnL2);
            data[18] = pressureValue(state, portState, kPadBtnR1);
            data[19] = pressureValue(state, portState, kPadBtnR2);
        }

        bool readPadPortData(int port, int slot, PS2Runtime *runtime, uint8_t *outData, uint32_t dataAddr)
        {
            if (!outData)
            {
                return false;
            }

            PadPortState portState;
            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                const PadPortState *sharedPortState = lookupPadPortStateLocked(port, slot);
                if (!sharedPortState || !sharedPortState->open)
                {
                    return false;
                }
                portState = *sharedPortState;
            }

            PadInputState state;
            bool useOverride = false;
            {
                std::lock_guard<std::mutex> lock(g_padOverrideMutex);
                if (g_padOverrideEnabled)
                {
                    state = g_padOverrideState;
                    useOverride = true;
                }
            }

            bool usedBackend = false;
            if (!useOverride)
            {
                uint8_t backendData[32]{};
                if (runtime && runtime->padBackend().readState(port, slot, backendData, sizeof(backendData)))
                {
                    state.buttons = static_cast<uint16_t>(backendData[2] | (backendData[3] << 8));
                    state.rx = backendData[4];
                    state.ry = backendData[5];
                    state.lx = backendData[6];
                    state.ly = backendData[7];
                    usedBackend = true;
                }
                else if (padScript().empty())
                {
                    // A scripted run (PS2X_PAD_SCRIPT) ignores the host keyboard and gamepad,
                    // so it plays the same whatever window has the focus.
                    applyGamepadState(state);
                    applyKeyboardState(state, portState.analogMode);
                }
                applyPadScript(state, port, runtime);
            }

            fillPadStatus(outData, state, portState);

            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                if (PadPortState *sharedPortState = lookupPadPortStateLocked(port, slot))
                {
                    sharedPortState->lastInput = state;
                    std::memcpy(sharedPortState->lastData, outData, sizeof(sharedPortState->lastData));
                    sharedPortState->lastUsedOverride = useOverride;
                    sharedPortState->lastUsedBackend = usedBackend;
                    sharedPortState->lastReadOk = true;
                    sharedPortState->lastReadDataAddr = dataAddr;
                    ++sharedPortState->readCount;
                }
            }

            return true;
        }
    }

    void PadSyncCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void scePadEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        setReturnS32(ctx, 1);
    }

    void scePadEnterPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PadTraceGuard padTrace("scePadEnterPressMode", ctx);
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->pressureEnabled = true;
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadExitPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->pressureEnabled = false;
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadGetButtonMask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint16_t mask = portState ? portState->buttonMask : 0xFFFFu;
        setReturnS32(ctx, static_cast<int32_t>(mask));
    }

    void scePadGetDmaStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint32_t dmaAddr = portState ? portState->dmaAddr : getRegU32(ctx, 6);
        setReturnU32(ctx, dmaAddr);
    }

    void scePadGetFrameCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        static std::atomic<uint32_t> frameCount{0};
        setReturnU32(ctx, frameCount++);
    }

    void scePadGetModVersion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Arbitrary non-zero module version.
        setReturnS32(ctx, 0x0200);
    }

    void scePadGetPortMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 2);
    }

    void scePadGetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PadTraceGuard padTrace("scePadGetReqState", ctx);
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        setReturnS32(ctx, static_cast<int32_t>(portState ? portState->reqState : 0u));
    }

    void scePadGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Most games use one slot unless multitap is active.
        setReturnS32(ctx, 1);
    }

    void scePadGetState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PadTraceGuard padTrace("scePadGetState", ctx);
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        int32_t state = kPadStateDisconnected;
        if (portState && portState->open)
        {
            if (portState->transientState != 0u)
            {
                state = static_cast<int32_t>(portState->transientState);
                portState->transientState = 0u;
            }
            else
            {
                state = kPadStateStable;
            }
        }
        setReturnS32(ctx, state);
    }

    void scePadInfoAct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t act = static_cast<int32_t>(getRegU32(ctx, 6));
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        if (act < 0)
        {
            setReturnS32(ctx, 2); // small + large motors
            return;
        }
        setReturnS32(ctx, (act < 2) ? 1 : 0);
    }

    void scePadInfoComb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // No combined modes reported.
        setReturnS32(ctx, 0);
    }

    void scePadInfoMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PadTraceGuard padTrace("scePadInfoMode", ctx);
        (void)rdram;
        (void)runtime;

        const int32_t infoMode = static_cast<int32_t>(getRegU32(ctx, 6)); // a2
        const int32_t index = static_cast<int32_t>(getRegU32(ctx, 7));    // a3
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const int32_t currentId = portState->analogMode ? kPadTypeDualShock : kPadTypeDigital;
        switch (infoMode)
        {
        case 1: // PAD_MODECURID
            setReturnS32(ctx, currentId);
            return;
        case 2: // PAD_MODECUREXID
            setReturnS32(ctx, currentId);
            return;
        case 3: // PAD_MODECUROFFS: index of the current mode in PAD_MODETABLE
            setReturnS32(ctx, portState->analogMode ? 1 : 0);
            return;
        case 4: // PAD_MODETABLE: a DualShock 2 offers digital and analog modes
            if (index == -1)
            {
                setReturnS32(ctx, 2);
            }
            else if (index == 0)
            {
                setReturnS32(ctx, kPadTypeDigital);
            }
            else if (index == 1)
            {
                setReturnS32(ctx, kPadTypeDualShock);
            }
            else
            {
                setReturnS32(ctx, 0);
            }
            return;
        default:
            setReturnS32(ctx, 0);
            return;
        }
    }

    void scePadInfoPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PadTraceGuard padTrace("scePadInfoPressMode", ctx);
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        setReturnS32(ctx, (portState && portState->open) ? 1 : 0);
    }

    void scePadInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        setReturnS32(ctx, 1);
    }

    void scePadInit2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        scePadInit(rdram, ctx, runtime);
    }

    void scePadPortClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = false;
        portState->pressureEnabled = false;
        portState->reqState = 0u;
        portState->transientState = 0u;
        setReturnS32(ctx, 1);
    }

    void scePadPortOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t dmaAddr = getRegU32(ctx, 6);
        uint8_t *dmaStr = getMemPtr(rdram, dmaAddr);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || (dmaAddr != 0u && !dmaStr))
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = true;
        portState->analogMode = false;     // real pads open DIGITAL
        portState->pressureEnabled = false;
        portState->buttonMask = 0xFFFFu;
        portState->dmaAddr = dmaAddr;
        portState->reqState = 0u;
        portState->transientState = 0u;
        if (dmaStr)
        {
            ps2TraceGuestRangeWrite(rdram, dmaAddr, 32u, "scePadPortOpen", ctx);
            std::memset(dmaStr, 0, 32);
        }
        setReturnS32(ctx, 1);
    }

    void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PadTraceGuard padTrace("scePadRead", ctx);
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const int slot = static_cast<int>(getRegU32(ctx, 5));
        const uint32_t dataAddr = getRegU32(ctx, 6);
        uint8_t *data = getMemPtr(rdram, dataAddr);
        if (!data)
        {
            setReturnS32(ctx, 0);
            return;
        }

        ps2TraceGuestRangeWrite(rdram, dataAddr, 32u, "scePadRead", ctx);
        if (!readPadPortData(port, slot, runtime, data, dataAddr))
        {
            setReturnS32(ctx, 0);
            return;
        }

        // LOCAL DEBUG (PS2X_IOP_WATCH=1): every change of the pad buttons goes into the log, in order with the sound commands
        // and the per-second stream-edge lines, so a static burst can be matched to the button that started it.
        {
            static const bool s_log = []() { const char *e = std::getenv("PS2X_IOP_WATCH"); return e && e[0] == '1'; }();
            static uint32_t s_lastButtons = 0u;
            if (s_log && port == 0)
            {
                const uint32_t buttons = (static_cast<uint32_t>(static_cast<uint8_t>(data[2] ^ 0xFFu)) << 8) |
                                         static_cast<uint32_t>(static_cast<uint8_t>(data[3] ^ 0xFFu));
                if (buttons != s_lastButtons)
                {
                    std::fprintf(stderr, "[pad] buttons 0x%04x -> 0x%04x (pressed 0x%04x, released 0x%04x)\n", s_lastButtons, buttons,
                                 buttons & ~s_lastButtons, s_lastButtons & ~buttons);
                    s_lastButtons = buttons;
                }
            }
        }

        PS2_IF_AGRESSIVE_LOGS({
            if (g_padReadLogCount < 48)
            {
                const int gamepad = findFirstGamepad();
                const bool gamepadStartPressed =
                    (gamepad >= 0) && IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT);
                const bool startPressed = (data[2] != 0xFFu || data[3] != 0xFFu ||
                                           IsKeyDown(KEY_ENTER) || gamepadStartPressed);
                if (startPressed)
                {
                    const uint32_t guestButtons =
                        (static_cast<uint32_t>(static_cast<uint8_t>(data[2] ^ 0xFFu)) << 8) |
                        static_cast<uint32_t>(static_cast<uint8_t>(data[3] ^ 0xFFu));
                    std::printf("[padread] port=%d slot=%d data2=0x%02x data3=0x%02x guestButtons=0x%04x enter=%d gamepadStart=%d\n",
                                port, slot, data[2], data[3], guestButtons,
                                IsKeyDown(KEY_ENTER) ? 1 : 0, gamepadStartPressed ? 1 : 0);
                    ++g_padReadLogCount;
                }
            }
        });

        setReturnS32(ctx, 1);
    }

    void scePadReqIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = (state == 0) ? "COMPLETE" : "BUSY";
        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        setReturnS32(ctx, 0);
    }

    void scePadSetActAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void scePadSetActDirect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        // LOCAL PATCH (haptics trace): what the game sends to the two motors (small: on/off, large: 0..255).
        {
            const uint32_t address = getRegU32(ctx, 6) & 0x1FFFFFFFu;
            if (address && address < PS2_RAM_SIZE - 6u)
            {
                static uint32_t s_last = 0xFFFFFFFFu;
                const uint32_t motors = static_cast<uint32_t>(rdram[address]) | (static_cast<uint32_t>(rdram[address + 1u]) << 8);
                if (motors != s_last)
                {
                    s_last = motors;
                    ::ps2xHapticTrace("PAD_MOTORS", rdram[address], rdram[address + 1u], getRegU32(ctx, 4));
                }
            }
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetButtonInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            portState->buttonMask = static_cast<uint16_t>(getRegU32(ctx, 6));
            portState->reqState = 0u;
            queueExecCmdStateLocked(*portState);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetMainMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PadTraceGuard padTrace("scePadSetMainMode", ctx);
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->analogMode = (getRegU32(ctx, 6) != 0u);
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadSetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            portState->reqState = static_cast<uint32_t>(getRegU32(ctx, 6) ? 1u : 0u);
            queueExecCmdStateLocked(*portState);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetVrefParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void scePadSetWarningLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void scePadStateIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = "UNKNOWN";
        if (state == 6)
        {
            text = "STABLE";
        }
        else if (state == 1)
        {
            text = "FINDPAD";
        }
        else if (state == 5)
        {
            text = "EXECCMD";
        }
        else if (state == 0)
        {
            text = "DISCONNECTED";
        }

        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        setReturnS32(ctx, 0);
    }

    PadDebugSnapshot getPadDebugSnapshot()
    {
        PadDebugSnapshot snapshot{};
        {
            std::lock_guard<std::mutex> lock(g_padOverrideMutex);
            snapshot.overrideEnabled = g_padOverrideEnabled;
            snapshot.overrideButtons = g_padOverrideState.buttons;
            snapshot.overrideRx = g_padOverrideState.rx;
            snapshot.overrideRy = g_padOverrideState.ry;
            snapshot.overrideLx = g_padOverrideState.lx;
            snapshot.overrideLy = g_padOverrideState.ly;
        }

        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            snapshot.readLogCount = g_padReadLogCount;
            for (size_t port = 0; port < kPadDebugPortCount; ++port)
            {
                for (size_t slot = 0; slot < kPadDebugSlotCount; ++slot)
                {
                    const PadPortState &src = g_padPorts[port];
                    PadDebugPortSnapshot &dst = snapshot.ports[port][slot];
                    dst.open = src.open;
                    dst.analogMode = src.analogMode;
                    dst.pressureEnabled = src.pressureEnabled;
                    dst.lastUsedOverride = src.lastUsedOverride;
                    dst.lastUsedBackend = src.lastUsedBackend;
                    dst.lastReadOk = src.lastReadOk;
                    dst.buttonMask = src.buttonMask;
                    dst.lastButtons = src.lastInput.buttons;
                    dst.dmaAddr = src.dmaAddr;
                    dst.reqState = src.reqState;
                    dst.readCount = src.readCount;
                    dst.lastReadDataAddr = src.lastReadDataAddr;
                    dst.rx = src.lastInput.rx;
                    dst.ry = src.lastInput.ry;
                    dst.lx = src.lastInput.lx;
                    dst.ly = src.lastInput.ly;
                    std::memcpy(dst.lastData, src.lastData, sizeof(dst.lastData));
                }
            }
        }
        return snapshot;
    }

    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = true;
        g_padOverrideState.buttons = buttons;
        g_padOverrideState.lx = lx;
        g_padOverrideState.ly = ly;
        g_padOverrideState.rx = rx;
        g_padOverrideState.ry = ry;
    }

    void clearPadOverrideState()
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = false;
        g_padOverrideState = PadInputState{};
    }
}
