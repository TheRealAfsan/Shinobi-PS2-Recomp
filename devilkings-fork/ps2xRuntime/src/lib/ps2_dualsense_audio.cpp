// LOCAL PATCH (DualSense HD haptics): over USB the DualSense is also a 4-channel sound card; channels 3 and 4 drive the
// left and right voice-coil actuators directly. This plays short tuned pulses there (WASAPI, shared mode), so a left
// footstep is felt in the left grip and a right one in the right grip. Over Bluetooth there is no such device and the
// rumble emulation in ps2_dualsense.cpp is used instead.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/ps2_settings.h"

#pragma comment(lib, "ole32.lib")

namespace
{
    struct Voice
    {
        double start;      // seconds (steady clock, same as now())
        double length;     // seconds
        float freq;        // Hz
        float left, right; // 0..1
        float freqEnd = 0.0f; // sweep: the pitch at the end (0 = a plain tap)
        double phase = 0.0;
    };

    std::mutex g_mutex;
    std::vector<Voice> g_voices;
    std::atomic<bool> g_active{false}; // the controller's haptic channels are open
    std::atomic<bool> g_btActive{false}; // Bluetooth: pulses streamed in report 0x32 (ps2_dualsense.cpp)
    std::atomic<bool> g_running{false};
    std::thread g_thread;

    double now()
    {
        static const auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    template <typename T> void release(T *&p)
    {
        if (p)
            p->Release();
        p = nullptr;
    }

    // The render endpoint of the DualSense ("Wireless Controller" / "DualSense").
    IMMDevice *findController(IMMDeviceEnumerator *enumerator)
    {
        IMMDeviceCollection *list = nullptr;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list)))
            return nullptr;
        UINT count = 0;
        list->GetCount(&count);
        IMMDevice *found = nullptr;
        for (UINT i = 0; i < count && !found; ++i)
        {
            IMMDevice *device = nullptr;
            if (FAILED(list->Item(i, &device)))
                continue;
            IPropertyStore *props = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)))
            {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
                {
                    std::wstring n = name.pwszVal;
                    std::transform(n.begin(), n.end(), n.begin(), ::towlower);
                    if (n.find(L"wireless controller") != std::wstring::npos || n.find(L"dualsense") != std::wstring::npos)
                        found = device;
                }
                PropVariantClear(&name);
                props->Release();
            }
            if (found != device)
                device->Release();
        }
        list->Release();
        return found;
    }

    void worker()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator *enumerator = nullptr;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                         reinterpret_cast<void **>(&enumerator));
        double lastTry = -10.0;
        while (g_running.load(std::memory_order_acquire))
        {
            if (!enumerator || now() - lastTry < 3.0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            lastTry = now();
            IMMDevice *device = findController(enumerator);
            if (!device)
                continue;
            IAudioClient *client = nullptr;
            IAudioRenderClient *render = nullptr;
            WAVEFORMATEX *mix = nullptr;
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            bool ok = SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                 reinterpret_cast<void **>(&client))) &&
                      SUCCEEDED(client->GetMixFormat(&mix));
            // Our own format: 4 channels of float at the device's rate (Windows converts if it has to).
            WAVEFORMATEXTENSIBLE fmt{};
            if (ok)
            {
                fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                fmt.Format.nChannels = 4;
                fmt.Format.nSamplesPerSec = mix->nSamplesPerSec;
                fmt.Format.wBitsPerSample = 32;
                fmt.Format.nBlockAlign = 16;
                fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * 16;
                fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                fmt.Samples.wValidBitsPerSample = 32;
                fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
                fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                ok = mix->nChannels >= 4 &&
                     SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                                  100000 /* 10 ms */, 0, &fmt.Format, nullptr)) &&
                     SUCCEEDED(client->SetEventHandle(event)) &&
                     SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&render))) &&
                     SUCCEEDED(client->Start());
                if (mix->nChannels < 4)
                    std::fprintf(stderr, "[haptics] the controller's sound device has %u channels (needs 4: plug it in by USB)\n",
                                 mix->nChannels);
            }
            if (ok)
            {
                const double rate = static_cast<double>(fmt.Format.nSamplesPerSec);
                UINT32 bufferFrames = 0;
                client->GetBufferSize(&bufferFrames);
                std::fprintf(stderr, "[haptics] HD haptics on (controller sound channels 3 and 4, %u Hz)\n",
                             fmt.Format.nSamplesPerSec);
                g_active.store(true, std::memory_order_release);
                while (g_running.load(std::memory_order_acquire))
                {
                    if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0)
                        continue;
                    UINT32 padding = 0;
                    if (FAILED(client->GetCurrentPadding(&padding)))
                        break;
                    const UINT32 frames = bufferFrames - padding;
                    if (!frames)
                        continue;
                    BYTE *out = nullptr;
                    if (FAILED(render->GetBuffer(frames, &out)))
                        break;
                    float *samples = reinterpret_cast<float *>(out);
                    std::fill(samples, samples + frames * 4u, 0.0f);
                    const bool enabled = ps2x::settings::getBool("input", "haptics");
                    const float strength =
                        static_cast<float>(std::clamp(ps2x::settings::getNumber("input", "haptic_strength"), 0, 100)) /
                        100.0f;
                    // The first sample written now is heard after what is already queued.
                    const double t0 = now() + padding / rate;
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(),
                                                      [t0](const Voice &v) { return t0 > v.start + v.length; }),
                                       g_voices.end());
                        if (enabled)
                            for (Voice &v : g_voices)
                                for (UINT32 i = 0; i < frames; ++i)
                                {
                                    const double t = t0 + i / rate - v.start;
                                    if (t < 0.0 || t > v.length)
                                        continue;
                                    const double k = t / v.length;
                                    double env, f = v.freq;
                                    if (v.freqEnd > 0.0f)
                                    {
                                        // A sweep: swells and rises in pitch, then falls away (the dash's rush).
                                        f = v.freq + (v.freqEnd - v.freq) * k;
                                        env = std::sin(3.14159265 * std::pow(k, 0.7));
                                    }
                                    else // a sharp attack (2 ms) and an exponential fall: a tap, not a buzz
                                        env = std::min(1.0, t / 0.002) * std::exp(-4.0 * k);
                                    v.phase += 6.283185307 * f / rate;
                                    const float s = static_cast<float>(std::sin(v.phase) * env) * strength;
                                    samples[i * 4u + 2u] += s * v.left;
                                    samples[i * 4u + 3u] += s * v.right;
                                }
                    }
                    for (UINT32 i = 0; i < frames * 4u; ++i)
                        samples[i] = std::clamp(samples[i], -1.0f, 1.0f);
                    render->ReleaseBuffer(frames, 0);
                }
                g_active.store(false, std::memory_order_release);
                client->Stop();
                std::fprintf(stderr, "[haptics] HD haptics off\n");
            }
            if (mix)
                CoTaskMemFree(mix);
            release(render);
            release(client);
            release(device);
            CloseHandle(event);
        }
        release(enumerator);
        CoUninitialize();
    }

    struct Stopper
    {
        ~Stopper()
        {
            if (g_running.exchange(false) && g_thread.joinable())
                g_thread.join();
        }
    } g_stopper;
}

// True while pulses go to the controller's actuators as sound (ps2_dualsense.cpp then sends no rumble).
bool ps2xHapticAudioActive()
{
    static std::once_flag once;
    std::call_once(once, [] {
        g_running.store(true, std::memory_order_release);
        g_thread = std::thread(worker);
    });
    return g_active.load(std::memory_order_acquire) || g_btActive.load(std::memory_order_acquire);
}

// A pulse on the actuators: shorter pulses are higher (a footstep's tick), longer ones lower (a thud, the magic).
void ps2xHapticAudioPulse(float left, float right, float ms, float delayMs)
{
    const float freq = ms < 70.0f ? 180.0f : ms < 150.0f ? 130.0f : 70.0f;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_voices.size() < 64u)
        g_voices.push_back(Voice{now() + delayMs / 1000.0, ms / 1000.0, freq, left, right});
}

// A sweep from `freq` to `freqEnd` Hz over `ms` (see the renderer above).
void ps2xHapticAudioSweep(float left, float right, float freq, float freqEnd, float ms, float delayMs)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_voices.size() < 64u)
    {
        Voice v{now() + delayMs / 1000.0, ms / 1000.0, freq, left, right};
        v.freqEnd = freqEnd;
        g_voices.push_back(v);
    }
}

// ---------------------------------------------------------------- Bluetooth: the same pulses as 3 kHz samples
// Over Bluetooth there is no sound device; ps2_dualsense.cpp streams these samples in output report 0x32 instead.

void ps2xHapticBtActive(bool on)
{
    g_btActive.store(on, std::memory_order_release);
}

bool ps2xHapticBtIsActive()
{
    return g_btActive.load(std::memory_order_acquire);
}

// `frames` stereo frames at `rate` Hz from now: lr[2i] = left actuator, lr[2i+1] = right, -1..1.
void ps2xHapticRenderStereo(float *lr, unsigned frames, double rate)
{
    std::fill(lr, lr + frames * 2u, 0.0f);
    if (!ps2x::settings::getBool("input", "haptics"))
        return;
    const float strength =
        static_cast<float>(std::clamp(ps2x::settings::getNumber("input", "haptic_strength"), 0, 100)) / 100.0f;
    const double t0 = now();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(),
                                  [t0](const Voice &v) { return t0 > v.start + v.length; }),
                   g_voices.end());
    for (Voice &v : g_voices)
        for (unsigned i = 0; i < frames; ++i)
        {
            const double t = t0 + i / rate - v.start;
            if (t < 0.0 || t > v.length)
                continue;
            const double k = t / v.length;
            double env, f = v.freq;
            if (v.freqEnd > 0.0f)
            {
                f = v.freq + (v.freqEnd - v.freq) * k;
                env = std::sin(3.14159265 * std::pow(k, 0.7));
            }
            else
                env = std::min(1.0, t / 0.002) * std::exp(-4.0 * k);
            v.phase += 6.283185307 * f / rate;
            const float s = static_cast<float>(std::sin(v.phase) * env) * strength;
            lr[i * 2u] += s * v.left;
            lr[i * 2u + 1u] += s * v.right;
        }
}
