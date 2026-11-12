// LOCAL PATCH: the launcher reads a PS5 DualSense / DualSense Edge itself (Windows HID, read only) and drives the page
// with "pad:up" / "pad:down" / ... messages. The page's Gamepad API loses the DualSense once the game has used its
// haptics over Bluetooth: that switches the controller to its full report (0x31), which only a direct reader decodes,
// and it stays that way until the controller reconnects. This reads both the simple (0x01) and the full report, over
// USB and Bluetooth, and sends nothing to the controller. Other controllers stay with the page's Gamepad API.

#include "launcher_dualsense.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace launcher::dualsense
{
    namespace
    {
        std::atomic<bool> g_running{false};
        std::thread g_thread;

        HANDLE openPad(bool &bluetooth)
        {
            GUID hidGuid;
            HidD_GetHidGuid(&hidGuid);
            HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (set == INVALID_HANDLE_VALUE)
                return INVALID_HANDLE_VALUE;
            HANDLE found = INVALID_HANDLE_VALUE;
            SP_DEVICE_INTERFACE_DATA iface{};
            iface.cbSize = sizeof(iface);
            for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); ++i)
            {
                DWORD need = 0;
                SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
                std::vector<uint8_t> buffer(need + sizeof(WCHAR));
                auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr))
                    continue;
                HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                HIDD_ATTRIBUTES a{};
                a.Size = sizeof(a);
                PHIDP_PREPARSED_DATA prepared = nullptr;
                HIDP_CAPS caps{};
                if (HidD_GetAttributes(h, &a) && a.VendorID == 0x054C && (a.ProductID == 0x0CE6 || a.ProductID == 0x0DF2) &&
                    HidD_GetPreparsedData(h, &prepared))
                {
                    HidP_GetCaps(prepared, &caps);
                    HidD_FreePreparsedData(prepared);
                    if (caps.UsagePage == 0x01 && caps.Usage == 0x05)
                    {
                        bluetooth = caps.OutputReportByteLength >= 78;
                        found = h;
                        continue;
                    }
                }
                CloseHandle(h);
            }
            SetupDiDestroyDeviceInfoList(set);
            return found;
        }

        // The menu buttons from a report: up, down, left, right, cross, circle, l1, r1, options. False if not a report
        // with buttons (the simple Bluetooth report has them elsewhere).
        bool decode(const uint8_t *r, DWORD size, bool bluetooth, bool out[9])
        {
            const uint8_t *p = nullptr;
            uint8_t lx = 128, ly = 128, b0 = 8, b1 = 0;
            if (r[0] == 0x31 && size >= 12)
                p = r + 2; // full report (Bluetooth)
            else if (r[0] == 0x01 && !bluetooth && size >= 11)
                p = r + 1; // USB
            if (p)
            {
                lx = p[0];
                ly = p[1];
                b0 = p[7];
                b1 = p[8];
            }
            else if (r[0] == 0x01 && bluetooth && size >= 10)
            {
                // Simple Bluetooth report: sticks at 1..4, buttons at 5..6 (as a DualShock 4's).
                lx = r[1];
                ly = r[2];
                b0 = r[5];
                b1 = r[6];
            }
            else
                return false;
            const int hat = b0 & 0x0F;
            const float dx = (lx - 127.5f) / 127.5f, dy = (ly - 127.5f) / 127.5f;
            out[0] = hat == 7 || hat == 0 || hat == 1 || dy < -0.6f;
            out[1] = (hat >= 3 && hat <= 5) || dy > 0.6f;
            out[2] = (hat >= 5 && hat <= 7) || dx < -0.6f;
            out[3] = (hat >= 1 && hat <= 3) || dx > 0.6f;
            out[4] = (b0 & 0x20) != 0; // cross
            out[5] = (b0 & 0x40) != 0; // circle
            out[6] = (b1 & 0x01) != 0; // L1
            out[7] = (b1 & 0x02) != 0; // R1
            out[8] = (b1 & 0x20) != 0; // options
            return true;
        }

        void worker(std::function<void(const std::wstring &)> send)
        {
            static const wchar_t *kNames[9] = {L"up", L"down", L"left", L"right", L"cross", L"circle", L"l1", L"r1", L"start"};
            const auto clock = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
            while (g_running.load())
            {
                bool bluetooth = false;
                HANDLE h = openPad(bluetooth);
                if (h == INVALID_HANDLE_VALUE)
                {
                    for (int i = 0; i < 20 && g_running.load(); ++i)
                        Sleep(100);
                    continue;
                }
                send(L"pad:host"); // the page leaves this controller to us from now on
                bool held[9] = {};
                double next[9] = {};
                bool first = true;
                std::vector<uint8_t> report(128);
                while (g_running.load())
                {
                    DWORD got = 0;
                    if (!ReadFile(h, report.data(), static_cast<DWORD>(report.size()), &got, nullptr) || got == 0)
                        break; // disconnected
                    bool now[9] = {};
                    if (!decode(report.data(), got, bluetooth, now))
                        continue;
                    const double t = clock();
                    for (int i = 0; i < 9; ++i)
                    {
                        if (first)
                        {
                            held[i] = now[i]; // already held at start: counts after a release
                            next[i] = 1e30;
                            continue;
                        }
                        if (!now[i])
                        {
                            held[i] = false;
                            continue;
                        }
                        if (!held[i])
                        {
                            held[i] = true;
                            next[i] = t + 0.38;
                            send(std::wstring(L"pad:") + kNames[i]);
                        }
                        else if (i < 4 && t >= next[i])
                        {
                            next[i] = t + 0.11;
                            send(std::wstring(L"pad:") + kNames[i]);
                        }
                    }
                    first = false;
                }
                CloseHandle(h);
                send(L"pad:lost");
            }
        }
    }

    void start(std::function<void(const std::wstring &)> send)
    {
        if (g_running.exchange(true))
            return;
        g_thread = std::thread(worker, std::move(send));
        g_thread.detach(); // ReadFile blocks: the thread ends with the process
    }
}
