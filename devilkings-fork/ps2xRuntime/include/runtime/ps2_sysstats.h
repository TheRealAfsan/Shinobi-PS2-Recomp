#pragma once
// LOCAL PATCH: process statistics for the window title (CPU, RAM, GPU VRAM). Kept in its own translation unit because
// the Windows headers it needs clash with raylib's names.
#include <string>

namespace ps2x::sysstats
{
    // e.g. "CPU 41% | RAM 1.9 GB | VRAM 0.8 GB" (empty where the platform has no implementation).
    std::string summary();
    // The biggest graphics card's dedicated video memory in bytes (0 when unknown).
    double dedicatedVramBytes();

    // PS2X_PROFILE=1: sample where thread `tid` is executing (~1 kHz) and print the hottest functions to stderr every 10 s.
    void startProfiler(const char *label, unsigned long tid);
    void startProfilerForCurrentThread(const char *label);

    // Windows: 1 ms timers and no power / timer-resolution throttling for the process. The default 15.6 ms clock made the
    // emulated vsync ticks arrive in bursts. PS2X_NO_TIMER_FIX=1 skips it.
    void initHostTiming();
    // Windows: the calling thread (game, VIF1+VU1) runs at "above normal" priority so background programs cannot delay a
    // frame's work. PS2X_THREAD_PRIORITY=0 skips it.
    void raiseCurrentThreadPriority(const char *label);
    // Windows: when stderr is a file, log lines go through a pipe and a writer thread, so a slow disk write cannot stall a
    // game thread for 100-300 ms. PS2X_LOG_ASYNC=0 skips it.
    void startAsyncStderr();
    // Windows: on a crash, prints the crashing thread and its call stack to stderr and writes captures/crash_<time>.dmp.
    void installCrashReporter();
    // PS2X_HITCH_LOG=1: call once per frame shown. When no frame has been shown for 60 ms in a stage, prints the call stack
    // of the game, VIF1/VU1 and GS threads ("[hitch]" lines), to show what a slow frame is waiting for.
    void noteFrameShown();
    // The function name at a code address ("0x..." when unknown), for logging call sites.
    std::string symbolName(unsigned long long address);
}
