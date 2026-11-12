// LOCAL PATCH (haptics research, temporary): records the game events haptics will be tied to, with the time, into
// captures/haptics_trace.csv: the player's animation states (on change), the foot nodes, one-shot sound effects (the
// footsteps are animation-driven sounds), shuriken spawns, the spell bullet, the game's own vibration requests and what
// it sends to the pad's motors, and the L2 trigger's travel. Read by scripts/haptics_trace_summary.py.

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <string>

namespace
{
    std::mutex g_mutex;
    std::atomic<uint64_t> g_frame{0}; // game frames (vsyncs) since boot
    FILE *g_file = nullptr;
    bool g_opened = false;
    const char *g_lastState = nullptr;
    uint32_t g_lastFoot[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};

    double now()
    {
        static const auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    void write(const char *tag, uint32_t a, uint32_t b, uint32_t c)
    {
        if (!g_opened)
        {
            g_opened = true;
            std::error_code ec;
            std::filesystem::create_directories("captures", ec);
            // Developers only: PS2X_HAPTIC_TRACE=1 writes the event log (nothing is written otherwise).
            if (!std::getenv("PS2X_HAPTIC_TRACE"))
                return;
            g_file = std::fopen("captures/haptics_trace.csv", "w");
            if (g_file)
            {
                std::fputs("t,frame,tag,a,b,c\n", g_file);
                std::fprintf(stderr, "[haptics] recording game events to captures/haptics_trace.csv\n");
            }
        }
        if (!g_file)
            return;
        std::fprintf(g_file, "%.4f,%llu,%s,%u,%u,%u\n", now(),
                     static_cast<unsigned long long>(g_frame.load(std::memory_order_relaxed)), tag, a, b, c);
        std::fflush(g_file);
    }
}

// Any event: tag plus up to three values.
void ps2xHapticThrow();
void ps2xHapticPulse(float left, float right, float ms, float delayMs);
void ps2xHapticTrace(const char *tag, uint32_t a, uint32_t b, uint32_t c)
{
    if (std::strcmp(tag, "MAKE_BULLET") == 0)
        ps2xHapticThrow();
    else if (std::strcmp(tag, "SE") == 0 && a == 3u)
        ps2xHapticPulse(1.0f, 1.0f, 180.0f, 25.0f); // the sound bank of a sword hit landing: a heavy thud
    else if (std::strcmp(tag, "VIB_PRESET") == 0 && a == 5u)
        ; // the storm's release: felt from SHOT_JUMP01 (ps2xHapticState), a little ahead of this
    else if (std::strcmp(tag, "VIB_PRESET") == 0 && a != 10u)
        ps2xHapticPulse(a == 1u ? 0.8f : 0.5f, a == 1u ? 0.8f : 0.5f, a == 1u ? 90.0f : 70.0f, 25.0f); // a hit landed
    std::lock_guard<std::mutex> lock(g_mutex);
    write(tag, a, b, c);
}

// `count` words of guest memory at `address`, as one line (tag, address, then the words in hex).
void ps2xHapticTraceWords(const char *tag, const uint8_t *rdram, uint32_t address, uint32_t count)
{
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!address || physical + count * 4u > 0x2000000u)
        return;
    if (std::strcmp(tag, "SE_BUF") == 0)
    {
        uint32_t word0;
        std::memcpy(&word0, rdram + physical, 4u);
        void ps2xHapticSound(uint32_t word0);
        ps2xHapticSound(word0);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_opened || !g_file)
        write("OPEN", 0u, 0u, 0u);
    if (!g_file)
        return;
    std::fprintf(g_file, "%.4f,%llu,%s,%u", now(), static_cast<unsigned long long>(g_frame.load(std::memory_order_relaxed)),
                 tag, address);
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t word;
        std::memcpy(&word, rdram + physical + 4u * i, 4u);
        std::fprintf(g_file, ",%08x", word);
    }
    std::fputc(10, g_file);
    std::fflush(g_file);
}

// The EE scheduler's vsync: every event carries the game frame it happened on.
void ps2xHapticTraceVsync(uint64_t tick)
{
    g_frame.store(tick, std::memory_order_relaxed);
}

// A player animation state function (called every frame while in that state, with its arguments): logged whenever the
// state or its arguments change, so walking and running are recorded frame by frame.
void ps2xHapticState(const char *state, uint32_t phase);
void ps2xHapticSound(uint32_t word0);
void ps2xHapticThrow();

void ps2xHapticTraceState(const char *state, uint32_t a, uint32_t b)
{
    ps2xHapticState(state, a);
    static uint32_t s_a = 0xFFFFFFFFu, s_b = 0xFFFFFFFFu;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_lastState == state && s_a == a && s_b == b)
        return;
    g_lastState = state;
    s_a = a;
    s_b = b;
    write(state, a, b, 0u);
}

// CPlayerAnimation::SetFootNodeBoth(left, right): logged when either changes.
void ps2xHapticTraceFoot(uint32_t self, uint32_t left, uint32_t right)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_lastFoot[0] == left && g_lastFoot[1] == right)
        return;
    g_lastFoot[0] = left;
    g_lastFoot[1] = right;
    write("FOOT_NODES", self, left, right);
}

// The pad's analog L2 (0..255) and the digital bit, per controller, logged in steps of 16.
void ps2xHapticTraceL2(int pad, int value, bool down)
{
    static int s_last[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    static bool s_down[8] = {};
    const int slot = pad & 7;
    const int bucket = value / 16;
    if (bucket == s_last[slot] && down == s_down[slot])
        return;
    s_last[slot] = bucket;
    s_down[slot] = down;
    std::lock_guard<std::mutex> lock(g_mutex);
    write("L2", static_cast<uint32_t>(value), down ? 1u : 0u, static_cast<uint32_t>(pad));
}

// The game frame the trace is on (for the video recorder below).
uint64_t ps2xHapticTraceFrame()
{
    return g_frame.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------- the frame recorder
// Every presented frame, shrunk to 320x180, goes to captures/haptics_video.raw with the game frame it showed, so the
// footsteps in the picture can be lined up with the trace exactly. Written on its own thread; at most 12 minutes.
// Each record: uint64 game frame, double seconds, then 320*180*3 bytes RGB (rows bottom-up, as read from the GPU).
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>

namespace
{
    constexpr int kVideoW = 320, kVideoH = 180;
    struct VideoFrame
    {
        uint64_t frame;
        double t;
        std::vector<uint8_t> rgba;
    };
    std::mutex g_videoMutex;
    std::condition_variable g_videoCv;
    std::deque<VideoFrame> g_videoQueue;
    std::thread g_videoThread;
    bool g_videoStarted = false;
    uint64_t g_videoCount = 0;

    void videoWorker()
    {
        FILE *f = std::fopen("captures/haptics_video.raw", "wb");
        if (!f)
            return;
        std::fprintf(stderr, "[haptics] recording the picture to captures/haptics_video.raw\n");
        std::vector<uint8_t> rgb(static_cast<size_t>(kVideoW) * kVideoH * 3u);
        for (;;)
        {
            VideoFrame vf;
            {
                std::unique_lock<std::mutex> lock(g_videoMutex);
                g_videoCv.wait(lock, [] { return !g_videoQueue.empty(); });
                vf = std::move(g_videoQueue.front());
                g_videoQueue.pop_front();
            }
            for (size_t i = 0, n = static_cast<size_t>(kVideoW) * kVideoH; i < n; ++i)
            {
                rgb[i * 3u + 0u] = vf.rgba[i * 4u + 0u];
                rgb[i * 3u + 1u] = vf.rgba[i * 4u + 1u];
                rgb[i * 3u + 2u] = vf.rgba[i * 4u + 2u];
            }
            std::fwrite(&vf.frame, sizeof(vf.frame), 1, f);
            std::fwrite(&vf.t, sizeof(vf.t), 1, f);
            std::fwrite(rgb.data(), 1, rgb.size(), f);
            std::fflush(f);
        }
    }
}

int ps2xHapticVideoWidth() { return kVideoW; }
int ps2xHapticVideoHeight() { return kVideoH; }

// Called by the presenter with the shrunk frame (RGBA, kVideoW x kVideoH).
void ps2xHapticVideoFrame(const uint8_t *rgba)
{
    if (g_videoCount >= 60ull * 60ull * 12ull)
        return;
    ++g_videoCount;
    std::lock_guard<std::mutex> lock(g_videoMutex);
    if (!g_videoStarted)
    {
        g_videoStarted = true;
        g_videoThread = std::thread(videoWorker);
        g_videoThread.detach();
    }
    if (g_videoQueue.size() > 240u) // the disk is behind: drop the frame rather than slow the game
        return;
    VideoFrame vf;
    vf.frame = g_frame.load(std::memory_order_relaxed);
    vf.t = now();
    vf.rgba.assign(rgba, rgba + static_cast<size_t>(kVideoW) * kVideoH * 4u);
    g_videoQueue.push_back(std::move(vf));
    g_videoCv.notify_one();
}

// ---------------------------------------------------------------- game events -> DualSense haptics
// Timed to the game: each event arrives on the game frame it happens, and the picture of that frame reaches the screen
// about a frame and a half later, so the pulse is delayed by that much (kDisplayDelayMs) to land with what is seen.
void ps2xHapticPulse(float left, float right, float ms, float delayMs); // ps2_dualsense.cpp

namespace
{
    constexpr float kDisplayDelayMs = 25.0f;
    std::atomic<int> g_locomotion{0}; // 0 none, 1 walking, 2 running
    std::atomic<int> g_nextFoot{1};   // 0 left, 1 right (Hotsuma steps off with his right foot)
    std::atomic<int> g_nextHand{1};   // 1 right, 0 left

    int locomotionOf(const char *state)
    {
        const std::string s = state;
        if (s == "WALK" || s == "WALK2" || s == "LOCKED_WALK")
            return 1;
        if (s == "DASH" || s == "LOCKED_DASH" || s.find("_DASH") != std::string::npos)
            return 3; // dashing: light, through the whole controller
        if (s == "RUN" || s.find("_RUN") != std::string::npos)
            return 2;
        return 0;
    }
}

// Every call of a player animation state function (before the trace's de-duplication). `phase` 0 = the state starts.
void ps2xHapticState(const char *state, uint32_t phase)
{
    const int loco = locomotionOf(state);
    if (loco != g_locomotion.load(std::memory_order_relaxed))
    {
        g_locomotion.store(loco, std::memory_order_relaxed);
    }
    if (phase != 0u)
        return;
    const std::string s = state;
    if (s == "SPELL00" || s == "SPELL01" || s == "SPELL02")
    {
        // Ninja magic: a swell across the whole controller, then a long fade.
        ps2xHapticPulse(0.6f, 0.6f, 120.0f, kDisplayDelayMs);
        ps2xHapticPulse(1.0f, 1.0f, 700.0f, kDisplayDelayMs + 80.0f);
    }
    else if (s == "DASH" || s == "LOCKED_DASH" || s.find("_DASH") != std::string::npos)
    {
        // The dash: a rush that rises in pitch and strength, then drops away; a plain thud without HD haptics.
        extern bool ps2xHapticAudioActive();
        extern void ps2xHapticAudioSweep(float, float, float, float, float, float);
        (void)ps2xHapticAudioActive;
        (void)ps2xHapticAudioSweep;
        // The dash: quick taps that swap sides and come faster as they fade, so the feeling travels across the
        // controller instead of shaking it.
        const float gaps[] = {0.0f, 55.0f, 45.0f, 36.0f, 29.0f, 24.0f, 20.0f};
        float at = kDisplayDelayMs;
        for (int i = 0; i < 7; ++i)
        {
            at += gaps[i];
            const float a = 0.75f - 0.07f * static_cast<float>(i);
            ps2xHapticPulse(i % 2 ? 0.0f : a, i % 2 ? a : 0.0f, 30.0f, at);
        }
    }
    else if (s.rfind("SLASH", 0) == 0 || s.find("_SLASH") != std::string::npos || s == "STAB_START")
        ps2xHapticPulse(0.75f, 0.75f, 110.0f, kDisplayDelayMs); // the sword swing
    else if (s == "SHOT_JUMP01")
    {
        // The shuriken storm leaves Hotsuma's hands (the spin in the air before it stays quiet; released ~26 frames after the throw starts, felt from 22): a heavy burst, then
        // an electric crackle of quick ticks jumping between the sides at random for about 0.65 s.
        ps2xHapticPulse(1.0f, 1.0f, 200.0f, kDisplayDelayMs + 360.0f);
        uint32_t seed = 0x9E3779B9u;
        float at = kDisplayDelayMs + 360.0f + 30.0f;
        for (int i = 0; i < 22; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const bool left = (seed >> 16) & 1u;
            const float amp = 0.8f + 0.2f * static_cast<float>((seed >> 8) & 0xFFu) / 255.0f;
            ps2xHapticPulse(left ? amp : 0.0f, left ? 0.0f : amp, 22.0f, at);
            at += 22.0f + static_cast<float>((seed >> 4) & 15u);
        }
    }
    else if (s == "DAMAGE" || s == "KNOCK_BACK" || s == "KNOCK_FLY")
        ps2xHapticPulse(0.8f, 0.8f, 160.0f, kDisplayDelayMs);
    else if (s == "LANDING")
        ps2xHapticPulse(0.45f, 0.45f, 70.0f, kDisplayDelayMs);
}

// A sound effect request (word 0 of the request: byte 2 = the sound's number). The footsteps are sounds the walk and run
// animations play on the frame each foot lands.
void ps2xHapticSound(uint32_t word0)
{
    const uint32_t id = (word0 >> 16) & 0xFFu;
    const int loco = g_locomotion.load(std::memory_order_relaxed);
    if (!loco || id == 0x1Cu || id == 0x2Cu || id == 0x2Du) // shuriken and magic sounds are not steps
        return;
    // Steps alternate; after a pause of over a second the next walk starts on the right foot again.
    static std::atomic<double> s_lastStep{0.0};
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (t - s_lastStep.load(std::memory_order_relaxed) > 1.0)
        g_nextFoot.store(1, std::memory_order_relaxed);
    s_lastStep.store(t, std::memory_order_relaxed);
    const int foot = g_nextFoot.fetch_xor(1, std::memory_order_relaxed);
    if (loco == 3)
    {
        ps2xHapticPulse(0.35f, 0.35f, 50.0f, kDisplayDelayMs);
        return;
    }
    // Walking: a light tap. Running: heavier.
    const float strength = loco == 2 ? 0.8f : 0.22f;
    ps2xHapticPulse(foot == 0 ? strength : 0.0f, foot == 0 ? 0.0f : strength, loco == 2 ? 60.0f : 45.0f, kDisplayDelayMs);
}

// A shuriken leaves the hand (CPlayerAnimation::MakeBullet): a push on that hand's side, alternating right, left.
void ps2xHapticThrow()
{
    const int hand = g_nextHand.fetch_xor(1, std::memory_order_relaxed);
    ps2xHapticPulse(hand == 0 ? 0.85f : 0.0f, hand == 0 ? 0.0f : 0.85f, 80.0f, kDisplayDelayMs);
}
