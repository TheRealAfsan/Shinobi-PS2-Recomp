// LOCAL PATCH (DualSense haptics and adaptive triggers): talks to a PS5 DualSense / DualSense Edge directly through
// Windows HID (no Sony SDK), over USB or Bluetooth.
//
// Output: the two voice-coil actuators (left and right sides of the controller) and the L2 adaptive trigger, sent in
// output report 0x02 (USB, 48 bytes) or 0x31 (Bluetooth, 78 bytes with a CRC32), the layout the Linux hid-playstation
// driver uses. Input: once the game sends a Bluetooth output report the pad switches to its full report (0x31), which
// the usual Windows controller APIs do not read, so the pad's buttons, sticks and triggers are read here too, over both
// connections, and handed to ps2_pad.cpp (ps2xDualSenseRead).
//
// Haptic events come from the game (ps2_haptics_trace.cpp calls ps2xHapticEvent): footsteps, shuriken throws, ninja
// magic, being hit. Each is a short pulse on one side or both, mixed and sent ~250 times a second.
//
// Settings: [input] haptics (on/off), haptic_strength (0..100), trigger_resistance (on/off). All live.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/ps2_settings.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

// What ps2_pad.cpp reads.
struct Ps2xDualSenseInput
{
    uint8_t lx, ly, rx, ry; // sticks, 0..255, 128 centre
    uint8_t l2, r2;         // trigger travel, 0..255
    uint8_t hat;            // d-pad: 0 up, then clockwise to 7; 8 = released
    bool square, cross, circle, triangle, l1, r1, l2Button, r2Button, create, options, l3, r3, ps;
};

bool ps2xHapticAudioActive(); // ps2_dualsense_audio.cpp
void ps2xHapticBtActive(bool on);
void ps2xHapticRenderStereo(float *lr, unsigned frames, double rate);

namespace
{
    constexpr USHORT kSony = 0x054C;
    constexpr USHORT kDualSense = 0x0CE6, kDualSenseEdge = 0x0DF2;

    struct Pulse
    {
        double start, length; // seconds
        float left, right;    // 0..1
    };

    struct Device
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        bool bluetooth = false;
        USHORT inputLength = 0, outputLength = 0;
        std::wstring path;
    };

    std::mutex g_mutex;
    std::vector<Pulse> g_pulses;
    Ps2xDualSenseInput g_input{};
    std::atomic<bool> g_inputValid{false};
    std::atomic<double> g_inputTime{0.0};
    std::atomic<bool> g_running{false};
    std::thread g_thread;
    uint8_t g_btSequence = 0;

    double now()
    {
        static const auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0xFFFFFFFFu)
    {
        for (size_t i = 0; i < size; ++i)
        {
            crc ^= data[i];
            for (int k = 0; k < 8; ++k)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return crc;
    }

    bool openPad(Device &out)
    {
        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);
        HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE)
            return false;
        bool found = false;
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); ++i)
        {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
            std::vector<uint8_t> buffer(need + sizeof(WCHAR));
            auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr))
                continue;
            HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE)
                continue;
            HIDD_ATTRIBUTES attributes{};
            attributes.Size = sizeof(attributes);
            PHIDP_PREPARSED_DATA prepared = nullptr;
            HIDP_CAPS caps{};
            const bool sony = HidD_GetAttributes(h, &attributes) && attributes.VendorID == kSony &&
                              (attributes.ProductID == kDualSense || attributes.ProductID == kDualSenseEdge);
            if (sony && HidD_GetPreparsedData(h, &prepared))
            {
                HidP_GetCaps(prepared, &caps);
                HidD_FreePreparsedData(prepared);
                // The game controller collection (usage page 1, usage 5) is the one with the reports.
                if (caps.UsagePage == 0x01 && caps.Usage == 0x05 && caps.OutputReportByteLength >= 48)
                {
                    out.handle = h;
                    out.path = detail->DevicePath;
                    out.inputLength = caps.InputReportByteLength;
                    out.outputLength = caps.OutputReportByteLength;
                    out.bluetooth = caps.OutputReportByteLength >= 78;
                    std::fprintf(stderr, "[dualsense] %s connected over %s (in %u, out %u bytes)\n",
                                 attributes.ProductID == kDualSenseEdge ? "DualSense Edge" : "DualSense",
                                 out.bluetooth ? "Bluetooth" : "USB", caps.InputReportByteLength,
                                 caps.OutputReportByteLength);
                    found = true;
                    continue;
                }
            }
            CloseHandle(h);
        }
        SetupDiDestroyDeviceInfoList(set);
        return found;
    }

    bool writeReport(Device &d, const uint8_t *report, DWORD size)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD written = 0;
        BOOL ok = WriteFile(d.handle, report, size, &written, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, 100) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(d.handle, &ov, &written, FALSE);
            else
            {
                CancelIo(d.handle);
                ok = FALSE;
            }
        }
        CloseHandle(ov.hEvent);
        return ok != FALSE;
    }

    // The 47-byte block both connections share (hid-playstation's dualsense_output_report_common), with the adaptive
    // trigger effects at bytes 10 (R2) and 21 (L2).
    void fillCommon(uint8_t *c, uint8_t left, uint8_t right, const uint8_t l2Effect[11])
    {
        // While the actuators play sound (HD haptics, ps2_dualsense_audio.cpp) the rumble emulation stays off.
        c[0] = ::ps2xHapticAudioActive() ? (0x04 | 0x08) : (0x01 | 0x02 | 0x04 | 0x08); // [vibration, haptics,] triggers
        c[1] = 0x00;
        c[2] = right; // right actuator
        c[3] = left;  // left actuator
        c[10] = 0x05; // R2: off
        std::memcpy(c + 21, l2Effect, 11);
        c[38] = 0x04; // improved vibration emulation (newer firmware)
    }

    bool sendOutput(Device &d, uint8_t left, uint8_t right, const uint8_t l2Effect[11])
    {
        if (d.bluetooth)
        {
            uint8_t report[78] = {};
            report[0] = 0x31;
            report[1] = static_cast<uint8_t>(g_btSequence << 4);
            g_btSequence = static_cast<uint8_t>((g_btSequence + 1u) & 15u);
            report[2] = 0x10;
            fillCommon(report + 3, left, right, l2Effect);
            const uint8_t seed = 0xA2;
            const uint32_t crc = ~crc32(report, 74, crc32(&seed, 1));
            std::memcpy(report + 74, &crc, 4);
            return writeReport(d, report, std::max<DWORD>(78, d.outputLength));
        }
        uint8_t report[64] = {};
        report[0] = 0x02;
        fillCommon(report + 1, left, right, l2Effect);
        return writeReport(d, report, std::min<DWORD>(64, std::max<DWORD>(48, d.outputLength)));
    }

    // Bluetooth HD haptics (the layout reverse-engineered by SAxense, as dualsense-bt-haptics sends it): report 0x32,
    // 142 bytes: a 0x11 packet (frame counter), then a 0x12 packet of 32 stereo frames of signed 8-bit samples at
    // 3 kHz (left actuator, right actuator), and a CRC32 over bytes 0..137 (seeded with 0xA2) at 138..141.
    // One report every 32 / 3000 s = 10.67 ms.
    constexpr double kBtRate = 3000.0;
    constexpr unsigned kBtFrames = 32u;
    uint8_t g_btPacketCounter = 0;

    bool sendHaptics(Device &d)
    {
        float lr[kBtFrames * 2u];
        ::ps2xHapticRenderStereo(lr, kBtFrames, kBtRate);
        uint8_t report[142] = {};
        report[0] = 0x32;
        report[1] = static_cast<uint8_t>(g_btSequence << 4);
        g_btSequence = static_cast<uint8_t>((g_btSequence + 1u) & 15u);
        report[2] = 0x11 | 0x80; // packet 0x11, sized
        report[3] = 7;
        report[4] = 0xFE;
        report[9] = 0xFF;
        report[10] = g_btPacketCounter++;
        report[11] = 0x12 | 0x80; // packet 0x12, sized
        report[12] = static_cast<uint8_t>(kBtFrames * 2u);
        for (unsigned i = 0; i < kBtFrames * 2u; ++i)
            report[13 + i] = static_cast<uint8_t>(static_cast<int8_t>(std::clamp(lr[i] * 127.0f, -128.0f, 127.0f)));
        const uint8_t seed = 0xA2;
        const uint32_t crc = ~crc32(report, 138, crc32(&seed, 1));
        std::memcpy(report + 138, &crc, 4);
        // Windows wants the buffer as long as the device's longest output report; the controller gets 0x32's 142.
        if (d.outputLength > 142)
        {
            std::vector<uint8_t> padded(d.outputLength, 0);
            std::memcpy(padded.data(), report, 142);
            return writeReport(d, padded.data(), d.outputLength);
        }
        return writeReport(d, report, 142);
    }

    void parseInput(const uint8_t *r, DWORD size, bool bluetooth)
    {
        const uint8_t *p = nullptr;
        if (!bluetooth && size >= 11 && r[0] == 0x01)
            p = r + 1;
        else if (size >= 12 && r[0] == 0x31)
            p = r + 2;
        if (!p)
            return; // simple Bluetooth report (0x01, before the first output report): left to the usual APIs
        Ps2xDualSenseInput in{};
        in.lx = p[0];
        in.ly = p[1];
        in.rx = p[2];
        in.ry = p[3];
        in.l2 = p[4];
        in.r2 = p[5];
        const uint8_t b0 = p[7], b1 = p[8], b2 = p[9];
        in.hat = b0 & 0x0F;
        in.square = b0 & 0x10;
        in.cross = b0 & 0x20;
        in.circle = b0 & 0x40;
        in.triangle = b0 & 0x80;
        in.l1 = b1 & 0x01;
        in.r1 = b1 & 0x02;
        in.l2Button = b1 & 0x04;
        in.r2Button = b1 & 0x08;
        in.create = b1 & 0x10;
        in.options = b1 & 0x20;
        in.l3 = b1 & 0x40;
        in.r3 = b1 & 0x80;
        in.ps = b2 & 0x01;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_input = in;
        }
        g_inputTime.store(now(), std::memory_order_relaxed);
        g_inputValid.store(true, std::memory_order_release);
    }

    // L2: "weapon" effect (mode 0x25): resistance from zone `start` that gives way at zone `end` (zones 0..9 of the
    // trigger's travel), like a trigger's break. Off: mode 0x05.
    void l2Effect(uint8_t out[11], bool on)
    {
        std::memset(out, 0, 11);
        if (!on)
        {
            out[0] = 0x05;
            return;
        }
        // Ninja magic fires at about 80% travel (seen in game: L2 counts as pressed at 202 of 255). The wall starts
        // at 40% and breaks at 70%, so the snap comes just before the magic.
        constexpr int start = 4, end = 7, strength = 8;
        const uint16_t zones = static_cast<uint16_t>((1u << start) | (1u << end));
        out[0] = 0x25;
        out[1] = static_cast<uint8_t>(zones & 0xFF);
        out[2] = static_cast<uint8_t>(zones >> 8);
        out[3] = static_cast<uint8_t>(strength - 1);
    }

    void worker()
    {
        Device d;
        double lastTry = -10.0;
        std::vector<uint8_t> readBuffer;
        OVERLAPPED readOv{};
        readOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool reading = false;
        uint8_t lastLeft = 255, lastRight = 255;
        bool lastTrigger = false, sentOnce = false;
        double lastSend = 0.0;
        while (g_running.load(std::memory_order_acquire))
        {
            if (d.handle == INVALID_HANDLE_VALUE)
            {
                g_inputValid.store(false, std::memory_order_release);
                if (now() - lastTry < 2.0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                lastTry = now();
                if (!openPad(d))
                    continue;
                readBuffer.assign(std::max<USHORT>(d.inputLength, 78), 0);
                if (d.bluetooth)
                {
                    ::ps2xHapticBtActive(true);
                    std::fprintf(stderr, "[haptics] HD haptics over Bluetooth on (report 0x32, 3 kHz)\n");
                }
                reading = false;
                sentOnce = false;
            }

            // Input: keep one read in flight; take every report that has arrived.
            for (int n = 0; n < 8; ++n)
            {
                if (!reading)
                {
                    ResetEvent(readOv.hEvent);
                    DWORD got = 0;
                    if (ReadFile(d.handle, readBuffer.data(), static_cast<DWORD>(readBuffer.size()), &got, &readOv))
                    {
                        parseInput(readBuffer.data(), got, d.bluetooth);
                        continue;
                    }
                    if (GetLastError() != ERROR_IO_PENDING)
                    {
                        std::fprintf(stderr, "[dualsense] disconnected\n");
                        CloseHandle(d.handle);
                        d.handle = INVALID_HANDLE_VALUE;
                        break;
                    }
                    reading = true;
                }
                if (WaitForSingleObject(readOv.hEvent, 0) != WAIT_OBJECT_0)
                    break;
                DWORD got = 0;
                reading = false;
                if (GetOverlappedResult(d.handle, &readOv, &got, FALSE))
                    parseInput(readBuffer.data(), got, d.bluetooth);
            }
            if (d.handle == INVALID_HANDLE_VALUE)
                continue;

            // Output: the pulses mixed now.
            const bool enabled = ps2x::settings::getBool("input", "haptics");
            const float strength =
                static_cast<float>(std::clamp(ps2x::settings::getNumber("input", "haptic_strength"), 0, 100)) / 100.0f;
            const bool trigger = ps2x::settings::getBool("input", "trigger_resistance");
            float left = 0.0f, right = 0.0f;
            {
                const double t = now();
                std::lock_guard<std::mutex> lock(g_mutex);
                g_pulses.erase(std::remove_if(g_pulses.begin(), g_pulses.end(),
                                              [t](const Pulse &p) { return t > p.start + p.length; }),
                               g_pulses.end());
                for (const Pulse &p : g_pulses)
                {
                    if (t < p.start)
                        continue;
                    // A quick attack then a fall: feels like a tap rather than a buzz.
                    const float x = static_cast<float>((t - p.start) / p.length);
                    const float envelope = x < 0.15f ? x / 0.15f : 1.0f - (x - 0.15f) / 0.85f;
                    left = std::max(left, p.left * envelope);
                    right = std::max(right, p.right * envelope);
                }
            }
            const uint8_t l = enabled ? static_cast<uint8_t>(std::clamp(left * strength * 255.0f, 0.0f, 255.0f)) : 0;
            const uint8_t r = enabled ? static_cast<uint8_t>(std::clamp(right * strength * 255.0f, 0.0f, 255.0f)) : 0;
            const double t = now();
            if (!sentOnce || l != lastLeft || r != lastRight || trigger != lastTrigger || t - lastSend > 0.5)
            {
                uint8_t effect[11];
                l2Effect(effect, trigger);
                if (!sendOutput(d, l, r, effect))
                {
                    std::fprintf(stderr, "[dualsense] could not send to the controller\n");
                    CloseHandle(d.handle);
                    d.handle = INVALID_HANDLE_VALUE;
                    continue;
                }
                lastLeft = l;
                lastRight = r;
                lastTrigger = trigger;
                lastSend = t;
                sentOnce = true;
            }
            if (d.bluetooth)
            {
                // Haptic samples on a steady 10.67 ms clock (catching up after a stall, never bunching more than 4).
                static double s_next = 0.0;
                const double tn = now();
                if (s_next == 0.0 || tn - s_next > 4.0 * kBtFrames / kBtRate)
                    s_next = tn;
                while (tn >= s_next)
                {
                    if (!sendHaptics(d))
                        break;
                    s_next += kBtFrames / kBtRate;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        if (d.handle != INVALID_HANDLE_VALUE)
        {
            // Leave the controller as it was: actuators still, triggers free.
            uint8_t effect[11];
            l2Effect(effect, false);
            sendOutput(d, 0, 0, effect);
            CancelIo(d.handle);
            CloseHandle(d.handle);
        }
        CloseHandle(readOv.hEvent);
    }

    struct Starter
    {
        ~Starter()
        {
            if (g_running.exchange(false) && g_thread.joinable())
                g_thread.join();
        }
    } g_starter;

    void ensureStarted()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            g_running.store(true, std::memory_order_release);
            g_thread = std::thread(worker);
        });
    }
}

// The pad's state, when a DualSense is connected and reporting (false otherwise: the usual APIs are used).
bool ps2xDualSenseRead(Ps2xDualSenseInput &out)
{
    ensureStarted();
    if (!g_inputValid.load(std::memory_order_acquire) || now() - g_inputTime.load(std::memory_order_relaxed) > 0.5)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    out = g_input;
    return true;
}

// A haptic pulse: `left` and `right` strengths 0..1, `ms` long, starting `delayMs` from now.
void ps2xHapticPulse(float left, float right, float ms, float delayMs)
{
    ensureStarted();
    extern bool ps2xHapticAudioActive();
    extern void ps2xHapticAudioPulse(float left, float right, float ms, float delayMs);
    if (ps2xHapticAudioActive())
    {
        ps2xHapticAudioPulse(left, right, ms, delayMs);
        return;
    }
    // The left actuator is the heavy, slow one: a short weak tap never gets it moving, so left-side taps are made
    // stronger and longer to feel like the right side's.
    if (left > right)
    {
        left = std::min(1.0f, left * 1.9f + 0.15f);
        ms *= 1.8f;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pulses.size() < 64u)
        g_pulses.push_back(Pulse{now() + delayMs / 1000.0, ms / 1000.0, left, right});
}

// Called when the game exits normally: actuators still, trigger free.
void ps2xDualSenseShutdown()
{
    if (g_running.exchange(false) && g_thread.joinable())
        g_thread.join();
}
