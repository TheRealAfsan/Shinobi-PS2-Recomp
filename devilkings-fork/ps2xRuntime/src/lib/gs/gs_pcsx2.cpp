// LOCAL: PCSX2 GS renderer backend, behind PS2X_RENDERER=pcsx2.
//
// PCSX2's hardware GS (Direct3D 11 or Vulkan) is built into a bridge DLL with a small, PCSX2-header-free C interface
// (ABI 5; tools/pcsx2-gs-bridge of github.com/danisandoval/fukami, GPL-3.0-or-later). This file loads that DLL at run
// time, feeds it the same post-arbitration GIF packets the CPU backend gets, tells it each field's privileged
// registers, and takes the finished picture back as RGBA. The GS front-end keeps parsing the stream (registers,
// SIGNAL/FINISH, uploads into its own VRAM) but draws nothing, exactly as with the paraLLEl-GS backend (gs_pgs.cpp),
// whose entry points this mirrors; gs_pgs.cpp dispatches here when this backend is wanted.
//
// Environment:
//   PS2X_RENDERER=pcsx2            select this backend
//   PS2X_PCSX2_BRIDGE=<dll>        the bridge DLL (default: pcsx2-gs-bridge\rrv-pcsx2-gs-bridge.dll next to the exe)
//   PS2X_PCSX2_RENDERER=d3d11|vulkan|software   (default d3d11)
//   PS2X_PCSX2_LOG=1               log every error and the first frames
#include "runtime/gs/gs_pgs.h"
#include "runtime/gs/rrv_pcsx2_gs_bridge.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <unordered_map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/ps2_sysstats.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace ps2x::pcsx2gs
{
    // LOCAL PATCH (freeze report): GIF bytes handed to PCSX2 since start, read by the perf recorder.
    static std::atomic<uint64_t> g_bytesSubmitted{0};

    // LOCAL PATCH (F7 drawing capture): a copy of graphics memory, then every drawing record for a while, in the
    // recordStream format, so PS2X_PCSX2_REPLAY can play it back offline. g_captureHold keeps new drawing out of the queue
    // while the memory copy is taken. All three are only touched with the queue mutex held (recordStream runs there).
    static std::atomic<bool> g_captureHold{false};
    static FILE *g_captureFile = nullptr;
    static uint64_t g_captureEndNs = 0u;
    uint64_t bytesSubmitted()
    {
        return g_bytesSubmitted.load(std::memory_order_relaxed);
    }

    namespace
    {
        struct Api
        {
            uint32_t (*version)() = nullptr;
            RrvPcsx2GsBridge *(*create)(const RrvPcsx2GsBridgeConfig *, RrvPcsx2GsBridgeCapabilities *, char *, uint32_t) = nullptr;
            void (*destroy)(RrvPcsx2GsBridge *) = nullptr;
            int (*submit)(RrvPcsx2GsBridge *, uint32_t, const uint8_t *, uint32_t, char *, uint32_t) = nullptr;
            int (*vsync)(RrvPcsx2GsBridge *, const uint64_t *, uint64_t, uint32_t, char *, uint32_t) = nullptr;
            int (*snapshot)(RrvPcsx2GsBridge *, RrvPcsx2GsBridgeFrame *, char *, uint32_t) = nullptr;
            int (*snapshotLocalMemory)(RrvPcsx2GsBridge *, uint8_t *, uint32_t, char *, uint32_t) = nullptr;
            int (*readLocalMemory)(RrvPcsx2GsBridge *, uint8_t *, uint32_t, uint64_t, uint64_t, uint64_t, char *, uint32_t) = nullptr;
            int (*resetGs)(RrvPcsx2GsBridge *, char *, uint32_t) = nullptr;
            int (*updateOptions)(RrvPcsx2GsBridge *, char *, uint32_t) = nullptr; // LOCAL PATCH, optional
        };

        struct Job
        {
            enum Kind : uint8_t
            {
                Submit,
                Vsync,
                Reset,
                UpdateOptions // LOCAL PATCH: settings changed while playing (applyLiveSettings)
            } kind = Submit;
            uint32_t pathId = 0;
            uint32_t bytes = 0;
            std::vector<uint8_t> data;
            uint64_t regs[RRV_PCSX2_GS_BRIDGE_PRIV_REG_COUNT] = {};
            uint64_t index = 0;
        };

        struct State
        {
            std::mutex mutex;
            bool started = false;
            bool failed = false;
            Api api;
            RrvPcsx2GsBridge *bridge = nullptr;
            GSRegisters *regs = nullptr;
            uint64_t fieldIndex = 0;
            std::vector<uint8_t> frame;
            uint32_t frameWidth = 0;
            uint32_t frameHeight = 0;
            bool frameFresh = false;
            std::vector<uint8_t> stage; // aligned copy of a misaligned packet
            uint64_t misaligned = 0;
            int fullFrameScale = 1;
            uint64_t ofyRewrites = 0; // odd-field XYOFFSET writes put on the even grid (progressive mode)  // PS2X_PCSX2_SCALE: 2..8 = the bridge's full-frame (progressive) mode at that scale
            uint32_t failStreak = 0; // consecutive vsyncs the bridge failed
            uint64_t submitNs = 0, vsyncNs = 0, snapNs = 0; // time spent inside the bridge, for the log
            uint64_t bytesByPath[4] = {};
            uint64_t bigPackets = 0;
            uint64_t submits = 0;
            uint64_t vsyncs = 0;
            uint64_t frames = 0;

            // LOCAL PATCH: GS worker thread (PS2X_PCSX2_GS_THREAD=0 = off). The bridge calls (submit, vsync, snapshot:
            // ~2 ms a frame at 2x) used to run on the VIF1 thread, which also runs VU1 and is the bottleneck. Packets are
            // now copied into a queue in order and a worker thread hands them to PCSX2. `mutex` above is held by whoever
            // calls into the bridge; `qMutex` guards the queue; `frameMutex` the finished picture.
            std::mutex qMutex;
            std::condition_variable workCv; // worker: a job arrived or stop
            std::condition_variable doneCv; // producers: a job finished / the bridge started / room in the queue
            std::deque<Job> jobs;
            std::vector<std::vector<uint8_t>> spare; // recycled packet buffers
            uint64_t pushed = 0, done = 0;           // jobs queued / finished, ever
            uint64_t vsyncsPushed = 0, vsyncsDone = 0;
            size_t queuedBytes = 0;
            size_t maxQueuedJobs = 0;
            uint64_t stalls = 0, stallNs = 0; // producers waiting for room
            bool launched = false;
            bool threaded = false;
            bool stop = false;
            bool workerExited = false;
            bool workerWaiting = false;
            std::thread worker;
            std::atomic<GSRegisters *> regsPtr{nullptr};

            std::mutex frameMutex;
            std::vector<uint8_t> back; // worker's copy of the newest snapshot before it is published
            // LOCAL PATCH: finished pictures wait in a short queue (PS2X_PCSX2_FRAME_QUEUE, default 3; 1 = the old single
            // slot) instead of one slot the next picture overwrote: when two finished within one pass of the window loop,
            // the first was never shown (the game made 60 frames a second, the window showed ~55). `readySpare` recycles
            // the 4-18 MB buffers. framesDropped counts pictures thrown away because the queue was full.
            struct ReadyFrame
            {
                std::vector<uint8_t> rgba;
                uint32_t width = 0, height = 0;
            };
            std::deque<ReadyFrame> ready;
            std::vector<std::vector<uint8_t>> readySpare;
            std::atomic<uint64_t> framesProduced{0}, framesDropped{0}, framesSkipped{0};
        };

        size_t frameQueueMax()
        {
            static const size_t s_max = []() -> size_t
            {
                const char *e = std::getenv("PS2X_PCSX2_FRAME_QUEUE");
                const long n = e ? std::atol(e) : 3L;
                return static_cast<size_t>(std::clamp(n, 1L, 8L));
            }();
            return s_max;
        }


        // Never destroyed: emulation threads may still send packets while the process exits.
        State &state()
        {
            static State *s = new State;
            return *s;
        }

        // -1 until the first packet starts the backend, then 1 (running) or 0 (failed).
        std::atomic<int> g_started{-1};

        bool logging()
        {
            static const bool s_on = std::getenv("PS2X_PCSX2_LOG") != nullptr;
            return s_on;
        }

        bool wantedNow()
        {
            const char *value = std::getenv("PS2X_RENDERER");
            return value && std::strcmp(value, "pcsx2") == 0;
        }

        // The console counts DW in video clocks and, in field mode, DH in lines of the whole frame; the runtime's sceGs
        // display stubs write the frame buffer's size. Same conversion as the paraLLEl-GS backend (gs_pgs.cpp).
        uint64_t displayForCrtc(uint64_t display, uint64_t smode2)
        {
            constexpr uint32_t kLineClocks = 2560u;
            const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu);
            const uint32_t width = static_cast<uint32_t>((display >> 32) & 0xFFFu) + 1u;
            if (magh == 0u && width <= kLineClocks / 2u)
            {
                const uint32_t clocksPerPixel = std::max(1u, std::min(16u, kLineClocks / width));
                const uint64_t dw = static_cast<uint64_t>(width * clocksPerPixel - 1u);
                display &= ~((0xFull << 23) | (0xFFFull << 32));
                display |= (static_cast<uint64_t>(clocksPerPixel - 1u) << 23) | (dw << 32);
            }
            const bool fieldMode = (smode2 & 3u) == 3u;
            const uint32_t height = static_cast<uint32_t>((display >> 44) & 0x7FFu) + 1u;
            if (fieldMode && height <= 256u)
            {
                display &= ~(0x7FFull << 44);
                display |= static_cast<uint64_t>(height * 2u - 1u) << 44;
            }
            return display;
        }

        // PS2X_PCSX2_STREAM=<file> records what is handed to the bridge, for replay (PS2X_PCSX2_REPLAY=<file>) without
        // the game. Records: [type u32][bytes u32][data]. Type 1..3 is a GIF packet on that path; type 10 is a vsync
        // (19 privileged registers then the field index, 160 bytes). PS2X_PCSX2_STREAM_MB caps the file (default 64).
        constexpr uint32_t kRecordVsync = 10u;

        uint64_t nowNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        }
        void recordStream(uint32_t type, const void *data, uint32_t bytes)
        {
            if (g_captureFile)
            {
                if (nowNs() >= g_captureEndNs)
                {
                    std::fclose(g_captureFile);
                    g_captureFile = nullptr;
                    std::fprintf(stderr, "[capture] drawing capture finished\n");
                }
                else
                {
                    const uint32_t header[2] = {type, bytes};
                    std::fwrite(header, sizeof(header), 1u, g_captureFile);
                    if (bytes)
                        std::fwrite(data, 1u, bytes, g_captureFile);
                }
            }
            static FILE *file = []() -> FILE *
            {
                // "1" (settings [effects] record_drawing) = captures/gs_stream.bin; "0" or empty = off; else a path.
                const char *e = std::getenv("PS2X_PCSX2_STREAM");
                if (!e || !e[0] || std::strcmp(e, "0") == 0)
                    return nullptr;
                const char *path = std::strcmp(e, "1") == 0 ? "captures/gs_stream.bin" : e;
                FILE *f = std::fopen(path, "wb");
                std::fprintf(stderr, "[pcsx2gs] recording the drawing stream to %s%s\n", path, f ? "" : " (could not open it)");
                return f;
            }();
            static uint64_t written = 0u;
            static const uint64_t cap = []
            {
                // The effects check records a whole session from boot (textures are loaded early): 40 GB by default then.
                const char *e = std::getenv("PS2X_PCSX2_STREAM_MB");
                const char *mode = std::getenv("PS2X_PCSX2_STREAM");
                const int fallback = (mode && std::strcmp(mode, "1") == 0) ? 40000 : 64;
                return static_cast<uint64_t>(e ? std::max(1, std::atoi(e)) : fallback) << 20;
            }();
            if (!file || written >= cap)
                return;
            const uint32_t header[2] = {type, bytes};
            std::fwrite(header, sizeof(header), 1u, file);
            std::fwrite(data, 1u, bytes, file);
            written += sizeof(header) + bytes;
            if (written >= cap)
            {
                std::fclose(file);
                std::fprintf(stderr, "[pcsx2gs] stream recording reached its size cap\n");
            }
        }

        // A call into the bridge DLL that survives a crash inside it (access violation and the like): the picture is then
        // handed back to the software renderer instead of taking the game down. Returns kCrashed after a crash.
        constexpr int kCrashed = -2;
#ifdef _WIN32
        template <class F, class... A>
        int guardedCall(F f, A... a)
        {
            __try
            {
                return static_cast<int>(f(a...));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return kCrashed;
            }
        }
#else
        template <class F, class... A>
        int guardedCall(F f, A... a)
        {
            return static_cast<int>(f(a...));
        }
#endif

        // Same for a call that returns nothing (destroy). Returns false when the call crashed.
#ifdef _WIN32
        template <class F, class... A>
        bool guardedCallVoid(F f, A... a)
        {
            __try
            {
                f(a...);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
#else
        template <class F, class... A>
        bool guardedCallVoid(F f, A... a)
        {
            f(a...);
            return true;
        }
#endif

        // Gives up on the PCSX2 renderer for the rest of the run (caller holds the state mutex).
        void abandon(State &s, const char *why)
        {
            std::fprintf(stderr, "[pcsx2gs] %s: switching to the software renderer for the rest of this run\n", why);
            s.started = false;
            s.failed = true;
            g_started.store(0);
        }

        bool fail(const char *why, const char *detail = "")
        {
            std::fprintf(stderr, "[pcsx2gs] %s%s%s: the PCSX2 renderer is unavailable, using the software renderer\n", why,
                         detail[0] ? ": " : "", detail);
            g_started.store(0);
            return false;
        }

#if defined(_WIN32)
        bool loadApi(Api &api, std::string &dllPath)
        {
            const char *override_ = std::getenv("PS2X_PCSX2_BRIDGE");
            if (override_ && *override_)
                dllPath = override_;
            else
            {
                char exe[MAX_PATH] = {};
                GetModuleFileNameA(nullptr, exe, MAX_PATH);
                std::string dir = exe;
                const size_t slash = dir.find_last_of("\\/");
                dllPath = (slash == std::string::npos ? std::string() : dir.substr(0, slash + 1u)) +
                          "pcsx2-gs-bridge\\rrv-pcsx2-gs-bridge.dll";
            }
            // The bridge's own dependency DLLs sit beside it: resolve them from its directory.
            HMODULE module = LoadLibraryExA(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!module)
                return false;
            const auto get = [&](const char *name) { return reinterpret_cast<void *>(GetProcAddress(module, name)); };
            api.version = reinterpret_cast<decltype(api.version)>(get("rrv_pcsx2_gs_bridge_version"));
            api.create = reinterpret_cast<decltype(api.create)>(get("rrv_pcsx2_gs_bridge_create"));
            api.destroy = reinterpret_cast<decltype(api.destroy)>(get("rrv_pcsx2_gs_bridge_destroy"));
            api.submit = reinterpret_cast<decltype(api.submit)>(get("rrv_pcsx2_gs_bridge_submit"));
            api.vsync = reinterpret_cast<decltype(api.vsync)>(get("rrv_pcsx2_gs_bridge_vsync"));
            api.snapshot = reinterpret_cast<decltype(api.snapshot)>(get("rrv_pcsx2_gs_bridge_snapshot"));
            api.resetGs = reinterpret_cast<decltype(api.resetGs)>(get("rrv_pcsx2_gs_bridge_reset_gs")); // optional
            api.updateOptions = reinterpret_cast<decltype(api.updateOptions)>(get("rrv_pcsx2_gs_bridge_update_options")); // optional
            api.readLocalMemory = reinterpret_cast<decltype(api.readLocalMemory)>(get("rrv_pcsx2_gs_bridge_read_local_memory"));
            api.snapshotLocalMemory =
                reinterpret_cast<decltype(api.snapshotLocalMemory)>(get("rrv_pcsx2_gs_bridge_snapshot_local_memory"));
            return api.version && api.create && api.destroy && api.submit && api.vsync && api.snapshot;
        }
#else
        bool loadApi(Api &, std::string &) { return false; }
#endif

        bool startLocked(State &s)
        {
            if (s.started)
                return true;
            if (s.failed)
                return false;
            s.failed = true;
#ifdef _WIN32
            {
                // The bridge ships Fukami's Ridge Racer V hardware profile (texture-inside-RT, CPU sprite render, GPU
                // palette conversion). PCSX2's own game database lists Shinobi (SLUS-20459) with no fixes at all, so
                // run PCSX2's defaults unless the user asks for the other profile (PS2X_PCSX2_PROFILE=rr5) or sets one
                // of the bridge's variables themselves.
                const char *profile = std::getenv("PS2X_PCSX2_PROFILE");
                if (!(profile && std::strcmp(profile, "rr5") == 0))
                {
                    // LOCAL PATCH (effects): two more of the bridge's full-frame (upscaled) settings are Ridge Racer V fixes
                    // that can eat Shinobi's screen effects: a depth-of-field blur rewrite keyed to one RR5 scratch block
                    // (0x1a40) and "large-ST" mode 2, which DROPS triangles with large texture coordinates (full-screen
                    // effect quads are such triangles). Off for Shinobi, as in PCSX2. And PCSX2's own fix for blur/glow/fog
                    // passes at a raised resolution, Native Scaling (they are drawn at the PS2's size, then scaled),
                    // on (1 = Normal). Settings [effects] (launcher Mod Menu) override each one.
                    const char *defaults[][2] = {{"RRV_PCSX2_GS_METAL_TEXTURE_INSIDE_RT", "0"},
                                                 {"RRV_PCSX2_GS_CPU_SPRITE_BW", "0"},
                                                 {"RRV_PCSX2_GS_PALTEX", "0"},
                                                 {"RRV_PCSX2_GS_FULL_DOF_NATIVE", "0"},
                                                 {"RRV_PCSX2_GS_FULL_LARGE_ST", "0"},
                                                 {"RRV_PCSX2_GS_FULL_NATIVE_SCALING", "1"}};
                    for (const auto &kv : defaults)
                        if (!std::getenv(kv[0]))
                            _putenv_s(kv[0], kv[1]);
                    std::fprintf(stderr, "[effects] RR5 DoF fix %s, large-ST mode %s, native scaling %s, half-pixel offset %s, "
                                         "round sprite %s, sprite filter %s, mipmapping %s\n",
                                 std::getenv("RRV_PCSX2_GS_FULL_DOF_NATIVE"), std::getenv("RRV_PCSX2_GS_FULL_LARGE_ST"),
                                 std::getenv("RRV_PCSX2_GS_FULL_NATIVE_SCALING"),
                                 std::getenv("RRV_PCSX2_GS_FULL_HPO") ? std::getenv("RRV_PCSX2_GS_FULL_HPO") : "bridge default",
                                 std::getenv("RRV_PCSX2_GS_FULL_ROUND_SPRITE") ? std::getenv("RRV_PCSX2_GS_FULL_ROUND_SPRITE") : "bridge default",
                                 std::getenv("RRV_PCSX2_GS_SPRITE_FILTER") ? std::getenv("RRV_PCSX2_GS_SPRITE_FILTER") : "bridge default",
                                 std::getenv("RRV_PCSX2_GS_FULL_HWMIPMAP") ? std::getenv("RRV_PCSX2_GS_FULL_HWMIPMAP") : "bridge default");
                }

                // Shinobi draws 640x224 fields, each with a half-line vertical offset that alternates with the field. How the
                // fields are put together decides whether the whole picture shimmers: shown one after the other (off, bob) it
                // shakes by a line 60 times a second, woven it combs on movement. Field mode therefore blends by default;
                // PS2X_PCSX2_INTERLACE=auto|off|weave|bob|blend|adaptive (optionally _tff / _bff) or a number 0..9 picks another.
                // PS2X_PCSX2_SCALE=2..8 selects the bridge's full-frame mode instead: one progressive frame per field, drawn
                // at that scale (2 = 1280x448), with no deinterlacing at all.
                // Default 2 (the user's verdict on the progressive 2x mode: "really beautiful"); PS2X_PCSX2_SCALE=1 = field mode.
                const char *scaleText = std::getenv("PS2X_PCSX2_SCALE");
                const int scale = scaleText ? std::atoi(scaleText) : 2;
                if (scale >= 2 && scale <= 12) // LOCAL PATCH: 12 = 8K (7680x5376)
                {
                    s.fullFrameScale = scale;
                    if (!std::getenv("RRV_PCSX2_GS_FULL_SCALE"))
                        _putenv_s("RRV_PCSX2_GS_FULL_SCALE", std::to_string(scale).c_str());
                }
                else if (!std::getenv("RRV_PCSX2_GS_INTERLACE"))
                {
                    const char *name = std::getenv("PS2X_PCSX2_INTERLACE");
                    std::string mode = name && name[0] ? name : "blend";
                    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    const bool bff = mode.size() > 4 && mode.compare(mode.size() - 4, 4, "_bff") == 0;
                    if (mode.size() > 4 && (bff || mode.compare(mode.size() - 4, 4, "_tff") == 0))
                        mode.resize(mode.size() - 4);
                    int value = -1;
                    if (mode.size() == 1 && mode[0] >= '0' && mode[0] <= '9')
                        value = mode[0] - '0';
                    else if (mode == "auto")
                        value = 0;
                    else if (mode == "off")
                        value = 1;
                    else if (mode == "weave")
                        value = bff ? 3 : 2;
                    else if (mode == "bob")
                        value = bff ? 5 : 4;
                    else if (mode == "blend")
                        value = bff ? 7 : 6;
                    else if (mode == "adaptive")
                        value = bff ? 9 : 8;
                    if (value >= 0)
                    {
                        const char digit[2] = {static_cast<char>('0' + value), '\0'};
                        _putenv_s("RRV_PCSX2_GS_INTERLACE", digit);
                    }
                }
            }
#endif
            std::string dllPath;
            if (!loadApi(s.api, dllPath))
                return fail("cannot load the bridge", dllPath.c_str());
            if (s.api.version() != RRV_PCSX2_GS_BRIDGE_ABI_VERSION)
                return fail("bridge ABI version mismatch");

            RrvPcsx2GsBridgeConfig config{};
            config.struct_size = sizeof(config);
            config.snapshot_width = 0; // PCSX2's native presentation size
            config.snapshot_height = 0;
            config.sw_threads = 0;
            config.gs_render_mode = s.fullFrameScale >= 2 ? RRV_PCSX2_GS_RENDER_MODE_FULL_FRAME // see PS2X_PCSX2_SCALE
                                                          : RRV_PCSX2_GS_RENDER_MODE_FIELD;      // the game sends fields
            config.presentation_mode = RRV_PCSX2_GS_PRESENTATION_LEGACY_CPU_SNAPSHOT;
            const char *kind = std::getenv("PS2X_PCSX2_RENDERER");
            config.renderer_kind = RRV_PCSX2_GS_RENDERER_D3D11;
            if (kind && std::strcmp(kind, "vulkan") == 0)
                config.renderer_kind = RRV_PCSX2_GS_RENDERER_VULKAN;
            else if (kind && std::strcmp(kind, "software") == 0)
                config.renderer_kind = RRV_PCSX2_GS_RENDERER_SOFTWARE;

            RrvPcsx2GsBridgeCapabilities caps{};
            caps.struct_size = sizeof(caps);
            char error[512] = {};
            s.bridge = s.api.create(&config, &caps, error, sizeof(error));
            if (!s.bridge && (caps.supported_render_modes & RRV_PCSX2_GS_RENDER_MODE_CAP_FIELD) == 0u)
            {
                // The renderer only takes whole frames: ask again in that mode (which refuses a deinterlace setting).
                config.gs_render_mode = RRV_PCSX2_GS_RENDER_MODE_FULL_FRAME;
#ifdef _WIN32
                _putenv_s("RRV_PCSX2_GS_INTERLACE", "");
#endif
                error[0] = '\0';
                s.bridge = s.api.create(&config, &caps, error, sizeof(error));
            }
            if (!s.bridge)
                return fail("bridge create failed", error);
            s.failed = false;
            s.started = true;
            g_started.store(1);
            std::fprintf(stderr, "[pcsx2gs] PCSX2 GS renderer: %s (mode %s)\n", caps.renderer_name ? caps.renderer_name : "?",
                         config.gs_render_mode == RRV_PCSX2_GS_RENDER_MODE_FIELD ? "field" : "full frame");
            return true;
        }
    }

    bool wanted()
    {
        static const bool s_wanted = wantedNow();
        return s_wanted;
    }

    bool active()
    {
        const int started = g_started.load(std::memory_order_relaxed);
        return started < 0 ? wanted() : started == 1;
    }

    bool exclusive() { return active(); }

    void setRegisters(GSRegisters *regs)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.regs = regs;
        s.regsPtr.store(regs, std::memory_order_release);
    }

    namespace
    {
        bool gsThreadWanted()
        {
            static const bool s_on = []
            {
                const char *e = std::getenv("PS2X_PCSX2_GS_THREAD");
                return !(e && e[0] == '0');
            }();
            return s_on;
        }

        // What may wait in the queue: bytes (big texture uploads) and whole frames (each queued vsync is a frame the
        // picture lags behind the game).
        constexpr size_t kMaxQueuedBytes = static_cast<size_t>(64u) << 20;
        constexpr uint64_t kMaxQueuedVsyncs = 2u;
        constexpr size_t kMaxSpareBufferBytes = static_cast<size_t>(1u) << 20;

        // ---- jobs, run with the bridge mutex held (on the worker thread, or inline when it is off) ----

        void runSubmit(State &s, Job &job)
        {
            const uint8_t *data = job.data.data();
            const uint32_t sizeBytes = job.bytes;
            // PCSX2's GIF parser reads packed registers with aligned 16-byte loads: a misaligned packet faults inside it.
            // (The job buffers come from operator new, 16-byte aligned on x64; this is the safety net.)
            if ((reinterpret_cast<uintptr_t>(data) & 15u) != 0u)
            {
                ++s.misaligned;
                if (s.stage.size() < static_cast<size_t>(sizeBytes) + 16u)
                    s.stage.resize(static_cast<size_t>(sizeBytes) + 16u);
                uint8_t *aligned = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(s.stage.data()) + 15u) & ~uintptr_t(15u));
                std::memcpy(aligned, data, sizeBytes);
                data = aligned;
            }
            char error[256] = {};
            const uint64_t submitStart = nowNs();
            const int submitted = guardedCall(s.api.submit, s.bridge, job.pathId, data, sizeBytes, static_cast<char *>(error),
                                              static_cast<uint32_t>(sizeof(error)));
            s.submitNs += nowNs() - submitStart;
            if (submitted == kCrashed)
            {
                abandon(s, "the PCSX2 bridge crashed while taking a GIF packet");
                return;
            }
            if (submitted == 0 && logging())
                std::fprintf(stderr, "[pcsx2gs] submit failed: %s\n", error);
            ++s.submits;
        }

        void runVsync(State &s, Job &job)
        {
            char error[256] = {};
            const uint64_t index = job.index;
            const uint64_t vsyncStart = nowNs();
            const int vsynced = guardedCall(s.api.vsync, s.bridge, static_cast<const uint64_t *>(job.regs), index,
                                            static_cast<uint32_t>(index & 1u), static_cast<char *>(error),
                                            static_cast<uint32_t>(sizeof(error)));
            s.vsyncNs += nowNs() - vsyncStart;
            if (vsynced == kCrashed)
            {
                abandon(s, "the PCSX2 bridge crashed at a vsync");
                return;
            }
            if (vsynced == 0)
            {
                if (logging())
                    std::fprintf(stderr, "[pcsx2gs] vsync failed: %s\n", error);
                if (++s.failStreak >= 120u)
                    abandon(s, "the PCSX2 bridge failed 120 vsyncs in a row");
                return;
            }
            ++s.vsyncs;
            RrvPcsx2GsBridgeFrame frame{};
            frame.struct_size = sizeof(frame);
            error[0] = '\0';
            const uint64_t snapStart = nowNs();
            const int snapped = guardedCall(s.api.snapshot, s.bridge, &frame, static_cast<char *>(error),
                                            static_cast<uint32_t>(sizeof(error)));
            s.snapNs += nowNs() - snapStart;
            if (snapped == kCrashed)
            {
                abandon(s, "the PCSX2 bridge crashed taking a snapshot");
                return;
            }
            if (snapped == 0 || !frame.rgba || frame.width == 0u || frame.height == 0u)
            {
                if (logging() && error[0])
                    std::fprintf(stderr, "[pcsx2gs] snapshot failed: %s\n", error);
                return;
            }
            s.failStreak = 0u;
            // Diagnostic: PS2X_PCSX2_DUMP=<frame> writes PCSX2's 4 MiB graphics memory to pcsx2_vram.bin at that frame.
            static const long dumpAt = [] { const char *e = std::getenv("PS2X_PCSX2_DUMP"); return e ? std::atol(e) : -1L; }();
            if (dumpAt >= 0 && static_cast<long>(s.frames) == dumpAt && s.api.snapshotLocalMemory)
            {
                std::vector<uint8_t> mem(RRV_PCSX2_GS_BRIDGE_LOCAL_MEMORY_BYTES);
                char memError[128] = {};
                if (guardedCall(s.api.snapshotLocalMemory, s.bridge, mem.data(), static_cast<uint32_t>(mem.size()),
                                static_cast<char *>(memError), static_cast<uint32_t>(sizeof(memError))) == 1)
                    if (FILE *f = std::fopen("pcsx2_vram.bin", "wb"))
                    {
                        std::fwrite(mem.data(), 1u, mem.size(), f);
                        std::fclose(f);
                        std::fprintf(stderr, "[pcsx2gs] dumped graphics memory at frame %llu\n", static_cast<unsigned long long>(s.frames));
                    }
            }
            if (logging() && (s.frames % 300u) == 0u && s.api.snapshotLocalMemory)
            {
                // Is PCSX2 receiving and drawing anything? Count the non-zero 32-bit words of its graphics memory.
                std::vector<uint8_t> mem(RRV_PCSX2_GS_BRIDGE_LOCAL_MEMORY_BYTES);
                char memError[128] = {};
                if (guardedCall(s.api.snapshotLocalMemory, s.bridge, mem.data(), static_cast<uint32_t>(mem.size()),
                                static_cast<char *>(memError), static_cast<uint32_t>(sizeof(memError))) == 1)
                {
                    const uint32_t *words = reinterpret_cast<const uint32_t *>(mem.data());
                    size_t nonzero = 0u;
                    const size_t count = mem.size() / 4u;
                    for (size_t i = 0; i < count; ++i)
                        nonzero += words[i] != 0u ? 1u : 0u;
                    std::fprintf(stderr, "[pcsx2gs] submits=%llu (misaligned %llu) vsyncs=%llu; GS local memory: %.1f%% non-zero\n",
                                 static_cast<unsigned long long>(s.submits), static_cast<unsigned long long>(s.misaligned),
                                 static_cast<unsigned long long>(s.vsyncs), 100.0 * nonzero / count);
                    std::fprintf(stderr, "[pcsx2gs] odd-field XYOFFSET writes rewritten to the even grid: %llu\n",
                                 static_cast<unsigned long long>(s.ofyRewrites));
                    std::fprintf(stderr, "[pcsx2gs] bytes: path1 %llu path2 %llu path3 %llu; packets >=4KB: %llu\n",
                                 static_cast<unsigned long long>(s.bytesByPath[1]), static_cast<unsigned long long>(s.bytesByPath[2]),
                                 static_cast<unsigned long long>(s.bytesByPath[3]), static_cast<unsigned long long>(s.bigPackets));
                    // Cumulative time inside the bridge: submit (all packets), vsync (the field's draw and merge), snapshot
                    // (the picture copied back to the CPU). Per-frame values are the totals over the vsyncs counted.
                    const double frames = static_cast<double>(std::max<uint64_t>(s.vsyncs, 1u));
                    std::fprintf(stderr, "[pcsx2gs] bridge time per frame: submit %.3f ms, vsync %.3f ms, snapshot %.3f ms (total %.3f ms)%s\n",
                                 s.submitNs / 1e6 / frames, s.vsyncNs / 1e6 / frames, s.snapNs / 1e6 / frames,
                                 (s.submitNs + s.vsyncNs + s.snapNs) / 1e6 / frames,
                                 s.threaded ? " on the GS thread" : " on the calling thread");
                    if (s.threaded)
                        std::fprintf(stderr, "[pcsx2gs] GS thread queue: longest %zu jobs; producer waited %llu times (%.1f ms in all)\n",
                                     s.maxQueuedJobs, static_cast<unsigned long long>(s.stalls), s.stallNs / 1e6);
                }
            }
            const size_t bytes = static_cast<size_t>(frame.stride_bytes) * frame.height;
            // Average colour and alpha of the picture as PCSX2 returned it (before any fix-up), for the log.
            uint64_t sumR = 0u, sumG = 0u, sumB = 0u, sumA = 0u, samples = 0u;
            if (s.frames < 3u || (logging() && ((s.frames + 1u) % 300u) == 0u))
                for (size_t i = 0; i + 3u < bytes; i += 4u * 61u)
                {
                    sumR += frame.rgba[i];
                    sumG += frame.rgba[i + 1u];
                    sumB += frame.rgba[i + 2u];
                    sumA += frame.rgba[i + 3u];
                    ++samples;
                }
            // LOCAL PATCH (perf): copy and make opaque in one pass (the window's texture is opaque; PCSX2's snapshot alpha
            // is not guaranteed to be). It was a copy plus a second pass over every pixel: ~18 MB twice a frame at 4x on
            // the GS thread. Recycled buffers are already the right size, so resize does not clear anything.
            s.back.resize(bytes);
            {
                const size_t pixels = bytes / 4u;
                const uint8_t *src = frame.rgba;
                uint8_t *dst = s.back.data();
                for (size_t i = 0; i < pixels; ++i)
                {
                    uint32_t p;
                    std::memcpy(&p, src + i * 4u, 4u);
                    p |= 0xFF000000u; // RGBA8 little-endian: alpha is the top byte
                    std::memcpy(dst + i * 4u, &p, 4u);
                }
                for (size_t i = pixels * 4u; i < bytes; ++i)
                    dst[i] = src[i];
            }
            {
                std::lock_guard<std::mutex> frameLock(s.frameMutex);
                State::ReadyFrame ready;
                ready.rgba.swap(s.back);
                ready.width = frame.width;
                ready.height = frame.height;
                s.ready.push_back(std::move(ready));
                s.framesProduced.fetch_add(1u, std::memory_order_relaxed);
                while (s.ready.size() > frameQueueMax())
                {
                    s.readySpare.push_back(std::move(s.ready.front().rgba));
                    s.ready.pop_front();
                    s.framesDropped.fetch_add(1u, std::memory_order_relaxed);
                }
                if (!s.readySpare.empty()) // the next snapshot reuses a buffer instead of allocating one
                {
                    s.back.swap(s.readySpare.back());
                    s.readySpare.pop_back();
                }
            }
            if (++s.frames <= 3u || (logging() && (s.frames % 300u) == 0u))
                std::fprintf(stderr, "[pcsx2gs] frame %llu: %ux%u stride %u  avg rgba %llu,%llu,%llu,%llu\n",
                             static_cast<unsigned long long>(s.frames), frame.width, frame.height, frame.stride_bytes,
                             static_cast<unsigned long long>(sumR / std::max<uint64_t>(samples, 1u)),
                             static_cast<unsigned long long>(sumG / std::max<uint64_t>(samples, 1u)),
                             static_cast<unsigned long long>(sumB / std::max<uint64_t>(samples, 1u)),
                             static_cast<unsigned long long>(sumA / std::max<uint64_t>(samples, 1u)));
        }

        void runReset(State &s)
        {
            if (!s.api.resetGs)
                return;
            char error[256] = {};
            const int reset = guardedCall(s.api.resetGs, s.bridge, static_cast<char *>(error), static_cast<uint32_t>(sizeof(error)));
            if (reset == kCrashed)
            {
                abandon(s, "the PCSX2 bridge crashed resetting the GS");
                return;
            }
            if (reset == 0 && logging())
                std::fprintf(stderr, "[pcsx2gs] reset_gs failed: %s\n", error);
            else if (logging())
                std::fprintf(stderr, "[pcsx2gs] GS reset (CSR.RESET)\n");
        }

        // LOCAL PATCH: the renderer options from the RRV_PCSX2_GS_* variables, applied to the running renderer.
        void runUpdateOptions(State &s)
        {
            if (!s.api.updateOptions)
            {
                std::fprintf(stderr, "[pcsx2gs] this bridge cannot change options while running\n");
                return;
            }
            char error[256] = {};
            const int updated = guardedCall(s.api.updateOptions, s.bridge, static_cast<char *>(error),
                                            static_cast<uint32_t>(sizeof(error)));
            if (updated == kCrashed)
            {
                abandon(s, "the PCSX2 bridge crashed changing its options");
                return;
            }
            if (updated == 0)
                std::fprintf(stderr, "[pcsx2gs] options update failed: %s\n", error);
        }

        // Caller holds s.mutex.
        void runJob(State &s, Job &job)
        {
            if (!s.started || !s.bridge)
                return; // abandoned: the software renderer has the picture now
            switch (job.kind)
            {
            case Job::Submit:
                runSubmit(s, job);
                break;
            case Job::Vsync:
                runVsync(s, job);
                break;
            case Job::Reset:
                runReset(s);
                break;
            case Job::UpdateOptions:
                runUpdateOptions(s);
                break;
            }
        }

        // Caller holds qMutex.
        void recycleLocked(State &s, Job &job)
        {
            if (job.data.capacity() != 0u && job.data.capacity() <= kMaxSpareBufferBytes && s.spare.size() < 256u)
            {
                job.data.clear();
                s.spare.push_back(std::move(job.data));
            }
        }

        void workerMain()
        {
#ifdef _WIN32
            SetThreadDescription(GetCurrentThread(), L"PCSX2 GS");
#endif
            ps2x::sysstats::startProfilerForCurrentThread("PCSX2 GS");
            State &s = state();
            {
                // The bridge (and its D3D11 device) is created on this thread, which then makes every call into it.
                std::lock_guard<std::mutex> lock(s.mutex);
                startLocked(s);
            }
            std::unique_lock<std::mutex> q(s.qMutex);
            s.doneCv.notify_all(); // ensureStarted() waits for the outcome
            for (;;)
            {
                if (s.jobs.empty())
                {
                    if (s.stop || g_started.load(std::memory_order_relaxed) != 1)
                        break;
                    s.workerWaiting = true;
                    s.workCv.wait(q, [&] { return s.stop || !s.jobs.empty(); });
                    s.workerWaiting = false;
                    continue;
                }
                Job job = std::move(s.jobs.front());
                s.jobs.pop_front();
                const bool run = !s.stop;
                q.unlock();
                if (run)
                {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    runJob(s, job);
                }
                q.lock();
                s.queuedBytes -= job.bytes;
                if (job.kind == Job::Vsync)
                    ++s.vsyncsDone;
                ++s.done;
                recycleLocked(s, job);
                s.doneCv.notify_all();
            }
            s.workerExited = true;
            s.doneCv.notify_all();
        }

        // Starts the bridge (on the GS thread when there is one). False when the PCSX2 renderer is unavailable.
        bool ensureStarted(State &s)
        {
            const int started = g_started.load(std::memory_order_acquire);
            if (started >= 0)
                return started == 1;
            std::unique_lock<std::mutex> q(s.qMutex);
            if (!s.launched)
            {
                s.launched = true;
                s.threaded = gsThreadWanted();
                if (s.threaded)
                    s.worker = std::thread(workerMain);
                else
                {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    startLocked(s);
                }
            }
            s.doneCv.wait(q, [] { return g_started.load(std::memory_order_acquire) >= 0; });
            return g_started.load(std::memory_order_acquire) == 1;
        }

        // Caller holds qMutex through q. Waits for room, then queues the job in order (or runs it at once when the GS
        // thread is off).
        void pushLocked(State &s, std::unique_lock<std::mutex> &q, Job &&job)
        {
            if (!s.threaded)
            {
                {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    runJob(s, job);
                }
                if (job.kind == Job::Vsync)
                {
                    ++s.vsyncsPushed;
                    ++s.vsyncsDone;
                }
                ++s.pushed;
                ++s.done;
                recycleLocked(s, job);
                return;
            }
            const auto full = [&]
            {
                if (s.workerExited)
                    return false;
                if (!s.jobs.empty() && s.queuedBytes + job.bytes > kMaxQueuedBytes)
                    return true;
                return job.kind == Job::Vsync && s.vsyncsPushed - s.vsyncsDone >= kMaxQueuedVsyncs;
            };
            if (full())
            {
                ++s.stalls;
                const uint64_t waitStart = nowNs();
                s.doneCv.wait(q, [&] { return !full(); });
                s.stallNs += nowNs() - waitStart;
            }
            if (s.workerExited)
                return;
            s.queuedBytes += job.bytes;
            if (job.kind == Job::Vsync)
                ++s.vsyncsPushed;
            ++s.pushed;
            s.jobs.push_back(std::move(job));
            s.maxQueuedJobs = std::max(s.maxQueuedJobs, s.jobs.size());
            if (s.workerWaiting)
                s.workCv.notify_one();
        }

        // Waits until every job queued so far has gone to the bridge (a read must see all the writes before it).
        void drain(State &s)
        {
            std::unique_lock<std::mutex> q(s.qMutex);
            const uint64_t target = s.pushed;
            s.doneCv.wait(q, [&] { return s.done >= target || s.workerExited; });
        }
    }


    // LOCAL PATCH (scarf colour mod): 1 KB image uploads (a 256-colour palette: 16x16 PSMCT32) are remembered by their
    // contents with where they went (BITBLTBUF/TRXPOS/TRXREG), so a palette the game uploaded once can be uploaded
    // again with other colours (ps2_scarf_colour.cpp, resendPalette). Parsed per path as the packets arrive; an IMAGE
    // transfer may continue over several packets.
    namespace
    {
        struct PaletteUpload
        {
            uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
        };
        struct PathParse
        {
            uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
            uint32_t imageLeft = 0;  // bytes of IMAGE data still to come
            bool collecting = false; // that data is a 1 KB palette being collected
            std::vector<uint8_t> buffer;
        };
        std::mutex g_paletteMutex;
        PathParse g_pathParse[4];
        std::unordered_map<uint64_t, PaletteUpload> g_paletteUploads;

        uint64_t paletteHash(const uint8_t *bytes)
        {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t i = 0; i < 1024u; ++i)
                h = (h ^ bytes[i]) * 1099511628211ull;
            return h;
        }

        void notePaletteUploads(uint32_t pathId, const uint8_t *data, uint32_t sizeBytes)
        {
            std::lock_guard<std::mutex> lock(g_paletteMutex);
            PathParse &p = g_pathParse[pathId & 3u];
            uint32_t pos = 0;
            while (pos < sizeBytes)
            {
                if (p.imageLeft)
                {
                    const uint32_t take = std::min(p.imageLeft, sizeBytes - pos);
                    if (p.collecting)
                        p.buffer.insert(p.buffer.end(), data + pos, data + pos + take);
                    p.imageLeft -= take;
                    pos += take;
                    if (!p.imageLeft && p.collecting)
                    {
                        p.collecting = false;
                        if (g_paletteUploads.size() > 8192u)
                            g_paletteUploads.clear();
                        g_paletteUploads[paletteHash(p.buffer.data())] = PaletteUpload{p.bitbltbuf, p.trxpos, p.trxreg};
                    }
                    continue;
                }
                if (pos + 16u > sizeBytes)
                    return;
                uint64_t tag, regs;
                std::memcpy(&tag, data + pos, 8u);
                std::memcpy(&regs, data + pos + 8u, 8u);
                pos += 16u;
                const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
                uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                if (flg == 0u)
                {
                    const uint64_t count = static_cast<uint64_t>(nloop) * nreg;
                    bool hasAd = false;
                    for (uint32_t r = 0; r < nreg; ++r)
                        hasAd = hasAd || ((regs >> (4u * r)) & 0xFu) == 0xEu;
                    if (!hasAd) // geometry (vertices, colours): skipped whole
                    {
                        if (pos + count * 16u > sizeBytes)
                            return;
                        pos += static_cast<uint32_t>(count * 16u);
                        continue;
                    }
                    for (uint64_t i = 0; i < count; ++i)
                    {
                        if (pos + 16u > sizeBytes)
                            return; // the tag goes on in the next packet: not followed
                        if (((regs >> (4u * (i % nreg))) & 0xFu) == 0xEu)
                        {
                            uint64_t value, address;
                            std::memcpy(&value, data + pos, 8u);
                            std::memcpy(&address, data + pos + 8u, 8u);
                            if ((address & 0xFFu) == 0x50u)
                                p.bitbltbuf = value;
                            else if ((address & 0xFFu) == 0x51u)
                                p.trxpos = value;
                            else if ((address & 0xFFu) == 0x52u)
                                p.trxreg = value;
                        }
                        pos += 16u;
                    }
                }
                else if (flg == 1u)
                    pos += static_cast<uint32_t>(((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u);
                else if (flg == 2u)
                {
                    p.imageLeft = nloop * 16u;
                    // A 16x16 32-bit upload (TRXREG: width bits 0..11, height bits 32..43; DPSM 0 = PSMCT32).
                    p.collecting = p.imageLeft == 1024u && ((p.bitbltbuf >> 56) & 0x3Fu) == 0u &&
                                   (p.trxreg & 0xFFFu) == 16u && ((p.trxreg >> 32) & 0xFFFu) == 16u;
                    p.buffer.clear();
                }
                else
                    return;
            }
        }
    }

    void gifTransfer(uint32_t path, const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u || !active())
            return;
        State &s = state();
        if (!ensureStarted(s))
            return;
        // ps2x::pgs::Path: VU1 = 0, VIF1 = 1, GIF = 2, host writes = 3. The bridge numbers PATH1..PATH3 as 1..3, and the
        // runtime's own writes count as PATH3 (the packet is post-arbitration; the id is only a diagnostic).
        const uint32_t pathId = path == 0u ? 1u : path == 1u ? 2u : 3u;
        {
            // The VIF1 interpreter prefixes an IMAGE continuation with a tag of its own (marked in the REGS half) for the
            // CPU GS. PCSX2 keeps the transfer state itself and already has the game's tag, so drop ours.
            uint64_t regsHalf = 0u;
            std::memcpy(&regsHalf, data + 8u, sizeof(regsHalf));
            if (regsHalf == 0x53594E5449474154ull)
            {
                data += 16u;
                sizeBytes -= 16u;
                if (sizeBytes < 16u)
                    return;
            }
        }
        sizeBytes &= ~15u;
        std::unique_lock<std::mutex> q(s.qMutex);
        while (g_captureHold.load(std::memory_order_acquire))
        {
            q.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            q.lock();
        }
        s.bytesByPath[pathId] += sizeBytes;
        g_bytesSubmitted.fetch_add(sizeBytes, std::memory_order_relaxed);
        recordStream(pathId, data, sizeBytes);
        notePaletteUploads(pathId, data, sizeBytes);
        if (sizeBytes >= 4096u)
            ++s.bigPackets;
        // The packet is copied: the source is guest memory (or a caller's buffer) that changes once this returns.
        Job job;
        job.kind = Job::Submit;
        job.pathId = pathId;
        job.bytes = sizeBytes;
        if (!s.spare.empty())
        {
            job.data = std::move(s.spare.back());
            s.spare.pop_back();
        }
        job.data.assign(data, data + sizeBytes);
        // LOCAL PATCH (progressive mode): Shinobi draws its two fields with XYOFFSET OFY 0x7880 / 0x7888, half a line
        // apart. The bridge's full-frame mode only puts the odd field back on the even field's grid for Ridge Racer V's
        // values (0x7908 -> 0x7900), so here every field bobbed by a line at 2x: the whole picture shook. A+D writes of
        // XYOFFSET_1/2 with the odd OFY are rewritten to the even one. PS2X_PCSX2_FIELD_OFY=<odd>:<even> (hex) sets the
        // pair, 0 turns it off. Only an A+D qword (register 0x18/0x19 in a clean descriptor) with exactly that OFY matches.
        if (s.fullFrameScale >= 2)
        {
            static const std::pair<uint32_t, uint32_t> s_ofy = []() -> std::pair<uint32_t, uint32_t>
            {
                const char *v = std::getenv("PS2X_PCSX2_FIELD_OFY");
                if (v && v[0] == '0' && v[1] == '\0')
                    return {0u, 0u};
                unsigned odd = 0x7888u, even = 0x7880u;
                if (v)
                    std::sscanf(v, "%x:%x", &odd, &even);
                return {odd, even};
            }();
            if (s_ofy.first != 0u)
            {
                uint8_t *packet = job.data.data();
                for (uint32_t offset = 0; offset + 16u <= sizeBytes; offset += 16u)
                {
                    uint64_t lo = 0u, hi = 0u;
                    std::memcpy(&lo, packet + offset, 8u);
                    std::memcpy(&hi, packet + offset + 8u, 8u);
                    const uint64_t reg = hi & 0xFFu;
                    if ((reg != 0x18u && reg != 0x19u) || (hi >> 8u) != 0u || ((lo >> 32u) & 0xFFFFu) != s_ofy.first)
                        continue;
                    lo = (lo & ~(0xFFFFull << 32u)) | (static_cast<uint64_t>(s_ofy.second) << 32u);
                    std::memcpy(packet + offset, &lo, 8u);
                    ++s.ofyRewrites;
                }
            }
        }
        pushLocked(s, q, std::move(job));
    }

    void vsync()
    {
        if (!active())
            return;
        State &s = state();
        if (!ensureStarted(s))
            return;
        const GSRegisters *r = s.regsPtr.load(std::memory_order_acquire);
        if (!r)
            return;
        // NTSC when the game never programmed the CRTC itself.
        constexpr uint64_t kNtscSmode1 = 0x0000000740814504ull;
        // RRV GSRegisters POD order: PMODE, SMODE1, SMODE2, SRFSH, SYNCH1, SYNCH2, SYNCV, DISPFB1, DISPLAY1, DISPFB2,
        // DISPLAY2, EXTBUF, EXTDATA, EXTWRITE, BGCOLOR, CSR, IMR, BUSDIR, SIGLBLID. Read now: they belong to this field.
        Job job;
        job.kind = Job::Vsync;
        const uint64_t regs[RRV_PCSX2_GS_BRIDGE_PRIV_REG_COUNT] = {
            r->pmode,
            r->smode1 ? r->smode1 : kNtscSmode1,
            r->smode2,
            r->srfsh,
            r->synch1,
            r->synch2,
            r->syncv,
            r->dispfb1,
            displayForCrtc(r->display1, r->smode2),
            r->dispfb2,
            displayForCrtc(r->display2, r->smode2),
            r->extbuf,
            r->extdata,
            r->extwrite,
            r->bgcolor,
            r->csr.load(std::memory_order_relaxed),
            r->imr,
            r->busdir,
            r->siglblid,
        };
        std::memcpy(job.regs, regs, sizeof(regs));
        std::unique_lock<std::mutex> q(s.qMutex);
        job.index = s.fieldIndex++;
        {
            uint64_t record[RRV_PCSX2_GS_BRIDGE_PRIV_REG_COUNT + 1];
            std::memcpy(record, regs, sizeof(regs));
            record[RRV_PCSX2_GS_BRIDGE_PRIV_REG_COUNT] = job.index;
            recordStream(kRecordVsync, record, sizeof(record));
        }
        pushLocked(s, q, std::move(job));
    }

    bool takeFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height, bool &field)
    {
        State &s = state();
        {
            // The window thread asks for the picture right after the frame's vsync was queued: give the GS thread a
            // moment to finish it, so it is not shown a refresh late. PS2X_PCSX2_FRAME_WAIT_US (default 4000, 0 = never).
            static const long s_waitUs = []
            {
                const char *e = std::getenv("PS2X_PCSX2_FRAME_WAIT_US");
                return e ? std::max(0L, std::atol(e)) : 4000L;
            }();
            std::unique_lock<std::mutex> q(s.qMutex);
            if (s.threaded && s_waitUs > 0)
            {
                const uint64_t target = s.vsyncsPushed;
                if (s.vsyncsDone < target)
                    s.doneCv.wait_for(q, std::chrono::microseconds(s_waitUs),
                                      [&] { return s.vsyncsDone >= target || s.workerExited; });
            }
        }
        // The oldest waiting picture, so each one is shown in order. With a frame-rate cap (fps_target 30 / 50) the caller
        // asks less often than pictures arrive: then the newest, and the older ones are skipped (not shown late).
        static const bool s_newest = []()
        {
            const char *e = std::getenv("PS2X_FPS_TARGET");
            return e && (std::strcmp(e, "30") == 0 || std::strcmp(e, "50") == 0);
        }();
        std::lock_guard<std::mutex> lock(s.frameMutex);
        if (s.ready.empty())
            return false;
        while (s_newest && s.ready.size() > 1u)
        {
            s.readySpare.push_back(std::move(s.ready.front().rgba));
            s.ready.pop_front();
            s.framesSkipped.fetch_add(1u, std::memory_order_relaxed);
        }
        State::ReadyFrame &next = s.ready.front();
        rgba.swap(next.rgba); // the caller's old buffer comes back to be refilled
        width = next.width;
        height = next.height;
        if (next.rgba.capacity() != 0u && s.readySpare.size() < 4u)
            s.readySpare.push_back(std::move(next.rgba));
        s.ready.pop_front();
        field = false; // PCSX2 returns the composed frame (its own deinterlacing), nothing for the caller to weave
        return true;
    }

    void frameStats(uint64_t &produced, uint64_t &dropped, uint64_t &waiting)
    {
        State &s = state();
        produced = s.framesProduced.load(std::memory_order_relaxed);
        dropped = s.framesDropped.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(s.frameMutex);
        waiting = s.ready.size();
    }

    void resetGs()
    {
        if (g_started.load(std::memory_order_acquire) != 1)
            return;
        State &s = state();
        Job job;
        job.kind = Job::Reset;
        std::unique_lock<std::mutex> q(s.qMutex);
        recordStream(11u, nullptr, 0u);
        pushLocked(s, q, std::move(job));
    }

    // LOCAL PATCH (settings without a restart): the renderer settings as they are now (render scale, anisotropic
    // filtering, texture filter, edge anti-aliasing, blending accuracy) go to the bridge's variables, and the GS thread
    // hands them to the running PCSX2 renderer. The render scale only changes within the progressive mode (2x..12x):
    // to or from 1x is a different render mode, which still needs a restart (ps2_settings.cpp restartNeeded).
    void applyLiveSettings()
    {
        if (g_started.load(std::memory_order_acquire) != 1)
            return;
        State &s = state();
        const std::string scale = ps2x::settings::get("video", "render_scale");
        if (s.fullFrameScale >= 2 && scale != "1")
            _putenv_s("RRV_PCSX2_GS_FULL_SCALE", scale.c_str());
        const std::string aniso = ps2x::settings::get("video", "anisotropic");
        _putenv_s("RRV_PCSX2_GS_ANISO", aniso == "off" ? "0" : aniso.c_str());
        _putenv_s("RRV_PCSX2_GS_TEXTURE_FILTER", ps2x::settings::get("video", "texture_filter") == "bilinear" ? "bilinear" : "ps2");
        _putenv_s("RRV_PCSX2_GS_FULL_HWAA1", ps2x::settings::getBool("video", "edge_aa") ? "1" : "0");
        const std::string blending = ps2x::settings::get("video", "blending");
        _putenv_s("RRV_PCSX2_GS_BLENDING", blending == "minimum" ? "0" : blending == "medium" ? "2" : blending == "high" ? "3" : "1");
        _putenv_s("RRV_PCSX2_GS_DITHERING", ps2x::settings::get("video", "dithering") == "off" ? "0" : "2");
        Job job;
        job.kind = Job::UpdateOptions;
        std::unique_lock<std::mutex> q(s.qMutex);
        pushLocked(s, q, std::move(job));
        std::fprintf(stderr, "[pcsx2gs] renderer settings changed while playing (scale %s, anisotropic %s, blending %s)\n",
                     scale.c_str(), aniso.c_str(), blending.c_str());
    }

    // LOCAL PATCH (scarf colour mod): uploads `palette` (1 KB) where the game last uploaded a palette equal to `match1`
    // or `match2`. False if no such upload was seen (yet).
    bool resendPalette(const uint8_t *match1, const uint8_t *match2, const uint8_t *palette)
    {
        if (!active())
            return false;
        PaletteUpload where;
        {
            std::lock_guard<std::mutex> lock(g_paletteMutex);
            auto it = g_paletteUploads.find(paletteHash(match1));
            if (it == g_paletteUploads.end() && match2)
                it = g_paletteUploads.find(paletteHash(match2));
            if (it == g_paletteUploads.end())
                return false;
            where = it->second;
        }
        alignas(16) uint8_t packet[16u * 6u + 1024u];
        uint64_t q[12] = {};
        q[0] = 4u | (1ull << 60); // NLOOP 4, PACKED, NREG 1
        q[1] = 0xEu;              // A+D
        q[2] = where.bitbltbuf;
        q[3] = 0x50u;
        q[4] = where.trxpos;
        q[5] = 0x51u;
        q[6] = where.trxreg;
        q[7] = 0x52u;
        q[8] = 0u; // TRXDIR: host -> local
        q[9] = 0x53u;
        q[10] = 64u | (1ull << 15) | (2ull << 58); // IMAGE, 64 qwords, EOP
        q[11] = 0u;
        std::memcpy(packet, q, sizeof(q));
        std::memcpy(packet + sizeof(q), palette, 1024u);
        gifTransfer(2u, packet, static_cast<uint32_t>(sizeof(packet)));
        return true;
    }

    bool readLocalToHost(uint8_t *dst, uint32_t byteCount, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg)
    {
        if (!dst || byteCount == 0u || !active())
            return false;
        State &s = state();
        if (!ensureStarted(s))
            return false;
        drain(s);
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started || !s.api.readLocalMemory)
            return false;
        char error[256] = {};
        const int result = guardedCall(s.api.readLocalMemory, s.bridge, dst, byteCount, bitbltbuf, trxpos, trxreg,
                                       static_cast<char *>(error), static_cast<uint32_t>(sizeof(error)));
        if (result == kCrashed)
        {
            abandon(s, "the PCSX2 bridge crashed during a GS->EE read");
            return false;
        }
        const bool ok = result != 0;
        if (!ok && logging())
            std::fprintf(stderr, "[pcsx2gs] GS->EE read of %u bytes failed: %s\n", byteCount, error);
        return ok;
    }

    // F7: copies graphics memory through PCSX2 (so it is what the GPU really has) as 16 upload packets of 64x1024 32-bit
    // pixels, then records the drawing for `seconds`. Replay with PS2X_PCSX2_REPLAY=<file>.
    bool startCapture(double seconds)
    {
        if (!active())
            return false;
        State &s = state();
        if (!ensureStarted(s))
            return false;
        {
            std::lock_guard<std::mutex> q(s.qMutex);
            if (g_captureFile)
                return false; // one at a time
            g_captureHold.store(true, std::memory_order_release);
        }
        std::error_code ec;
        std::filesystem::create_directories("captures", ec);
        const std::time_t t = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&t));
        const std::string path = std::string("captures/gs_capture_") + stamp + ".bin";
        FILE *file = std::fopen(path.c_str(), "wb");
        bool ok = file != nullptr;
        constexpr uint32_t kRows = 1024u, kWidth = 64u, kChunks = 16u; // 16 x 256 KB = all 4 MB
        constexpr uint32_t kBytes = kRows * kWidth * 4u;
        std::vector<uint8_t> pixels(kBytes);
        std::vector<uint8_t> packet(16u * 6u + kBytes);
        for (uint32_t chunk = 0; ok && chunk < kChunks; ++chunk)
        {
            const uint64_t bp = static_cast<uint64_t>(chunk) * (kRows / 32u) * 32u; // 32 pages of 32 blocks per chunk
            const uint64_t trxreg = static_cast<uint64_t>(kWidth) | (static_cast<uint64_t>(kRows) << 32);
            const uint64_t readBuf = bp | (1ull << 16); // SBP, SBW = 1 (64 pixels), SPSM = PSMCT32
            ok = readLocalToHost(pixels.data(), kBytes, readBuf, 0u, trxreg);
            if (!ok)
                break;
            // GIF packet: A+D tag (4 registers), BITBLTBUF/TRXPOS/TRXREG/TRXDIR=0, then an IMAGE tag and the pixels.
            uint64_t q[12] = {};
            q[0] = 4u | (1ull << 60); // NLOOP 4, PACKED, NREG 1
            q[1] = 0xEu;                                  // A+D
            q[2] = bp << 32 | (1ull << 48);               // DBP, DBW = 1, DPSM = PSMCT32
            q[3] = 0x50u;
            q[4] = 0u;
            q[5] = 0x51u;
            q[6] = trxreg;
            q[7] = 0x52u;
            q[8] = 0u;
            q[9] = 0x53u;
            q[10] = static_cast<uint64_t>(kBytes / 16u) | (1ull << 15) | (2ull << 58); // IMAGE, EOP
            q[11] = 0u;
            std::memcpy(packet.data(), q, sizeof(q));
            std::memcpy(packet.data() + sizeof(q), pixels.data(), kBytes);
            const uint32_t header[2] = {2u, static_cast<uint32_t>(sizeof(q) + kBytes)};
            ok = std::fwrite(header, sizeof(header), 1u, file) == 1u &&
                 std::fwrite(packet.data(), 1u, sizeof(q) + kBytes, file) == sizeof(q) + kBytes;
        }
        {
            std::lock_guard<std::mutex> q(s.qMutex);
            if (ok)
            {
                g_captureFile = file;
                g_captureEndNs = nowNs() + static_cast<uint64_t>(seconds * 1.0e9);
            }
            else if (file)
                std::fclose(file);
            g_captureHold.store(false, std::memory_order_release);
        }
        std::fprintf(stderr, "[capture] %s %s (graphics memory, then %.0f seconds of drawing)\n",
                     ok ? "recording" : "could not record", path.c_str(), seconds);
        return ok;
    }

    bool readVram(uint8_t *dst, uint32_t sizeBytes)
    {
        if (!dst || !active())
            return false;
        State &s = state();
        if (!ensureStarted(s))
            return false;
        drain(s);
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.started || !s.api.snapshotLocalMemory)
            return false;
        // The bridge copies the whole 4 MiB image; copy the part asked for.
        std::vector<uint8_t> all(RRV_PCSX2_GS_BRIDGE_LOCAL_MEMORY_BYTES);
        char error[256] = {};
        const int copied = guardedCall(s.api.snapshotLocalMemory, s.bridge, all.data(), static_cast<uint32_t>(all.size()),
                                       static_cast<char *>(error), static_cast<uint32_t>(sizeof(error)));
        if (copied == kCrashed)
        {
            abandon(s, "the PCSX2 bridge crashed copying graphics memory");
            return false;
        }
        if (copied == 0)
            return false;
        std::memcpy(dst, all.data(), std::min<size_t>(sizeBytes, all.size()));
        return true;
    }


    // LOCAL PATCH (scarf colour mod): finds a 256-colour palette (16x16 PSMCT32, uploaded at the start of a block, as a
    // game uploads a CLUT) in graphics memory by its colours, and writes `palette` over it. Used when the game's own
    // upload of the scarf palette was not seen (it is uploaded once, when a stage loads). `candidates` are palettes it
    // may hold now (the game's original, the colours last written); the first one found is replaced. One copy of the
    // 4 MB memory, only when the player changes the colour.
    bool rewritePaletteInVram(const std::vector<const uint8_t *> &candidates, const uint8_t *palette)
    {
        if (!active() || candidates.empty())
            return false;
        std::vector<uint8_t> vram(RRV_PCSX2_GS_BRIDGE_LOCAL_MEMORY_BYTES);
        if (!readVram(vram.data(), static_cast<uint32_t>(vram.size())))
            return false;
        // PSMCT32 layout (the GS's own swizzle): a block is 8x8 pixels (64 words), blocks within a page and words within
        // a block are in these orders.
        static const uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21},
                                               {2, 3, 6, 7, 18, 19, 22, 23},
                                               {8, 9, 12, 13, 24, 25, 28, 29},
                                               {10, 11, 14, 15, 26, 27, 30, 31}};
        static const uint8_t kColumn32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13},         {2, 3, 6, 7, 10, 11, 14, 15},
                                                {16, 17, 20, 21, 24, 25, 28, 29},   {18, 19, 22, 23, 26, 27, 30, 31},
                                                {32, 33, 36, 37, 40, 41, 44, 45},   {34, 35, 38, 39, 42, 43, 46, 47},
                                                {48, 49, 52, 53, 56, 57, 60, 61},   {50, 51, 54, 55, 58, 59, 62, 63}};
        uint32_t offset[256]; // word offset of each of the 16x16 pixels from the base block
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x)
                offset[y * 16 + x] = kBlock32[y / 8][x / 8] * 64u + kColumn32[y % 8][x % 8];
        const uint32_t *words = reinterpret_cast<const uint32_t *>(vram.data());
        const uint32_t blocks = static_cast<uint32_t>(vram.size() / 256u);
        for (const uint8_t *want : candidates)
        {
            if (!want)
                continue;
            const uint32_t *pal = reinterpret_cast<const uint32_t *>(want);
            // The GS adds the block order above to the upload's base block (any block), so every base is tried.
            for (uint32_t bp = 0; bp + 4u <= blocks; ++bp)
            {
                const uint32_t base = bp * 64u;
                bool same = true;
                for (uint32_t i = 0; i < 256u && same; ++i)
                    same = words[base + offset[i]] == pal[i];
                if (!same)
                    continue;
                alignas(16) uint8_t packet[16u * 6u + 1024u];
                uint64_t q[12] = {};
                q[0] = 4u | (1ull << 60); // NLOOP 4, PACKED, NREG 1
                q[1] = 0xEu;              // A+D
                q[2] = (static_cast<uint64_t>(bp) << 32) | (1ull << 48); // DBP, DBW 1 (64 pixels), DPSM PSMCT32
                q[3] = 0x50u;
                q[4] = 0u; // TRXPOS 0,0
                q[5] = 0x51u;
                q[6] = 16u | (16ull << 32); // TRXREG 16x16
                q[7] = 0x52u;
                q[8] = 0u; // TRXDIR host -> local
                q[9] = 0x53u;
                q[10] = 64u | (1ull << 15) | (2ull << 58); // IMAGE, 64 qwords, EOP
                q[11] = 0u;
                std::memcpy(packet, q, sizeof(q));
                std::memcpy(packet + sizeof(q), palette, 1024u);
                gifTransfer(2u, packet, static_cast<uint32_t>(sizeof(packet)));
                std::fprintf(stderr, "[scarf] palette found in graphics memory at block 0x%x and replaced\n", bp);
                return true;
            }
        }
        std::fprintf(stderr, "[scarf] palette not found in graphics memory\n");
        return false;
    }

    // PS2X_PCSX2_REPLAY=<recording>: feeds a recorded stream (see recordStream) to the bridge with no game running and
    // writes what it draws, so two renderers (PS2X_PCSX2_RENDERER=software|d3d11|vulkan) can be compared on exactly
    // the same input. PS2X_PCSX2_REPLAY_OUT=<prefix> (default "replay") and PS2X_PCSX2_REPLAY_EVERY=<n> (default 60)
    // choose which frames are saved as <prefix>_<frame>_<w>x<h>.rgba; every frame's hash goes to stdout.
    // Returns the process exit code, or -1 when no replay was asked for.
    int replayMain()
    {
        const char *path = std::getenv("PS2X_PCSX2_REPLAY");
        if (!path || !path[0])
            return -1;
        const char *outEnv = std::getenv("PS2X_PCSX2_REPLAY_OUT");
        const std::string prefix = outEnv && outEnv[0] ? outEnv : "replay";
        const char *everyEnv = std::getenv("PS2X_PCSX2_REPLAY_EVERY");
        const uint64_t every = everyEnv ? std::max(1, std::atoi(everyEnv)) : 60;
        FILE *in = std::fopen(path, "rb");
        if (!in)
        {
            std::fprintf(stderr, "[replay] cannot open %s\n", path);
            return 2;
        }
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!startLocked(s))
            return 3;
        std::vector<uint8_t> packet;
        uint64_t frames = 0u, packets = 0u;
        uint32_t header[2];
        while (std::fread(header, sizeof(header), 1u, in) == 1u)
        {
            packet.resize(header[1] + 16u);
            uint8_t *data = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(packet.data()) + 15u) & ~uintptr_t(15u));
            if (header[1] != 0u && std::fread(data, 1u, header[1], in) != header[1])
                break;
            char error[256] = {};
            if (header[0] >= 1u && header[0] <= 3u)
            {
                // Same odd-field OFY rewrite as gifTransfer (progressive mode), so a replay shows what the game would.
                static const bool s_replayOfy = []() { const char *v = std::getenv("PS2X_PCSX2_FIELD_OFY"); return !(v && v[0] == '0'); }();
                if (s.fullFrameScale >= 2 && s_replayOfy)
                    for (uint32_t offset = 0; offset + 16u <= header[1]; offset += 16u)
                    {
                        uint64_t lo = 0u, hi = 0u;
                        std::memcpy(&lo, data + offset, 8u);
                        std::memcpy(&hi, data + offset + 8u, 8u);
                        const uint64_t reg = hi & 0xFFu;
                        if ((reg == 0x18u || reg == 0x19u) && (hi >> 8u) == 0u && ((lo >> 32u) & 0xFFFFu) == 0x7888u)
                        {
                            lo = (lo & ~(0xFFFFull << 32u)) | (0x7880ull << 32u);
                            std::memcpy(data + offset, &lo, 8u);
                        }
                    }
                if (guardedCall(s.api.submit, s.bridge, header[0], static_cast<const uint8_t *>(data), header[1],
                                static_cast<char *>(error), static_cast<uint32_t>(sizeof(error))) != 1)
                    std::fprintf(stderr, "[replay] submit failed at packet %llu: %s\n", static_cast<unsigned long long>(packets), error);
                ++packets;
            }
            else if (header[0] == 11u && s.api.resetGs)
            {
                guardedCall(s.api.resetGs, s.bridge, static_cast<char *>(error), static_cast<uint32_t>(sizeof(error)));
            }
            else if (header[0] == kRecordVsync && header[1] == sizeof(uint64_t) * (RRV_PCSX2_GS_BRIDGE_PRIV_REG_COUNT + 1u))
            {
                uint64_t regs[RRV_PCSX2_GS_BRIDGE_PRIV_REG_COUNT];
                uint64_t index = 0u;
                std::memcpy(regs, data, sizeof(regs));
                std::memcpy(&index, data + sizeof(regs), sizeof(index));
                if (guardedCall(s.api.vsync, s.bridge, static_cast<const uint64_t *>(regs), index,
                                static_cast<uint32_t>(index & 1u), static_cast<char *>(error),
                                static_cast<uint32_t>(sizeof(error))) != 1)
                {
                    std::fprintf(stderr, "[replay] vsync failed at frame %llu: %s\n", static_cast<unsigned long long>(frames), error);
                    continue;
                }
                RrvPcsx2GsBridgeFrame frame{};
                frame.struct_size = sizeof(frame);
                if (guardedCall(s.api.snapshot, s.bridge, &frame, static_cast<char *>(error),
                                static_cast<uint32_t>(sizeof(error))) == 1 && frame.rgba)
                {
                    uint64_t hash = 1469598103934665603ull;
                    const size_t bytes = static_cast<size_t>(frame.stride_bytes) * frame.height;
                    uint64_t lit = 0u;
                    // Temporal stability: mean absolute difference per channel against the previous frame (a still picture that
                    // shakes or flickers scores above zero), over every 4th pixel.
                    static std::vector<uint8_t> previous;
                    uint64_t diffSum = 0u, diffCount = 0u;
                    const bool comparable = previous.size() == bytes;
                    for (size_t i = 0; i + 3u < bytes; i += 4u)
                    {
                        uint32_t px;
                        std::memcpy(&px, frame.rgba + i, 4u);
                        px &= 0x00FFFFFFu;
                        hash = (hash ^ px) * 1099511628211ull;
                        lit += px != 0u ? 1u : 0u;
                        if (comparable && ((i >> 2) & 3u) == 0u)
                        {
                            for (size_t c = 0; c < 3u; ++c)
                                diffSum += static_cast<uint64_t>(std::abs(static_cast<int>(frame.rgba[i + c]) - static_cast<int>(previous[i + c])));
                            diffCount += 3u;
                        }
                    }
                    previous.assign(frame.rgba, frame.rgba + bytes);
                    std::printf("frame %llu %ux%u lit=%llu hash=%016llx tdiff=%.4f\n", static_cast<unsigned long long>(frames),
                                frame.width, frame.height, static_cast<unsigned long long>(lit), static_cast<unsigned long long>(hash),
                                diffCount ? static_cast<double>(diffSum) / static_cast<double>(diffCount) : 0.0);
                    if (frames % every == 0u)
                    {
                        char name[512];
                        std::snprintf(name, sizeof(name), "%s_%05llu_%ux%u.rgba", prefix.c_str(),
                                      static_cast<unsigned long long>(frames), frame.width, frame.height);
                        if (FILE *out = std::fopen(name, "wb"))
                        {
                            std::fwrite(frame.rgba, 1u, bytes, out);
                            std::fclose(out);
                        }
                    }
                }
                ++frames;
            }
        }
        std::fclose(in);
        std::printf("replayed %llu packets, %llu frames\n", static_cast<unsigned long long>(packets), static_cast<unsigned long long>(frames));
        std::fflush(stdout);
        return 0;
    }

    void shutdown()
    {
        State &s = state();
        {
            // Stop the GS thread first (queued work is dropped); it may be inside a bridge call, give it a moment.
            bool exited = true;
            {
                std::unique_lock<std::mutex> q(s.qMutex);
                if (s.threaded && !s.workerExited)
                {
                    s.stop = true;
                    s.workCv.notify_all();
                    s.doneCv.wait_for(q, std::chrono::seconds(3), [&] { return s.workerExited; });
                    exited = s.workerExited;
                }
            }
            if (s.worker.joinable())
            {
                if (exited)
                    s.worker.join();
                else
                    s.worker.detach();
            }
#ifdef _WIN32
            if (!exited)
            {
                std::fprintf(stderr, "[pcsx2gs] the GS thread did not stop (stuck in the bridge); ending the process\n");
                std::fflush(stderr);
                Sleep(150);
                TerminateProcess(GetCurrentProcess(), 0);
            }
#endif
        }
        std::lock_guard<std::mutex> lock(s.mutex);
        const bool hadBridge = s.bridge != nullptr;
        bool closedCleanly = true;
        if (s.bridge && s.api.destroy)
            closedCleanly = guardedCallVoid(s.api.destroy, s.bridge);
        s.bridge = nullptr;
        s.started = false;
        g_started.store(0);
#ifdef _WIN32
        if (hadBridge && !closedCleanly)
        {
            // PCSX2's GS shutdown crashed (seen in its texture cache clean-up). Its renderer is now half destroyed, and the
            // DLL would run the same clean-up again when it is unloaded, so end the process here instead of returning.
            std::fprintf(stderr, "[pcsx2gs] the PCSX2 bridge crashed while closing; ending the process\n");
            std::fflush(stderr);
            Sleep(150); // the log writer thread
            TerminateProcess(GetCurrentProcess(), 0);
        }
#endif
    }
}
