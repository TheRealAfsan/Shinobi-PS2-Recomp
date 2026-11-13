#include "runtime/ps2_sysstats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")
#include <tlhelp32.h>
#include <dxgi1_4.h>
#include <dbghelp.h>
#include <io.h>
#include <fcntl.h>
#include <thread>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

// LOCAL PATCH: DbgHelp is single-threaded. The profilers (one thread per profiled thread), the hitch sampler and the
// per-second wait report all symbolise addresses; done at the same time they corrupted DbgHelp's state (garbled
// profiler output, then a crash in _Mtx_unlock). Every DbgHelp call goes through this lock, and SymInitialize runs once.
namespace
{
    std::recursive_mutex g_dbghelpMutex;

    void initSymbolsOnce()
    {
        static std::once_flag s_once;
        std::call_once(s_once,
                       []()
                       {
                           std::lock_guard<std::recursive_mutex> lock(g_dbghelpMutex);
                           SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
                           char exePath[MAX_PATH] = {};
                           GetModuleFileNameA(nullptr, exePath, MAX_PATH);
                           std::string exeDir = exePath;
                           const size_t slash = exeDir.find_last_of("\\/");
                           if (slash != std::string::npos)
                               exeDir.resize(slash);
                           SymInitialize(GetCurrentProcess(), exeDir.c_str(), TRUE);
                       });
    }
}

namespace
{
    uint64_t fileTimeTo100ns(const FILETIME &ft)
    {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    // The busiest threads of this process since the last call ("name 97%", percent of one core), so the title bar
    // shows which thread limits the frame rate.
    std::string topThreads()
    {
        static std::unordered_map<DWORD, uint64_t> s_last;
        static std::chrono::steady_clock::time_point s_lastWall{};
        const auto now = std::chrono::steady_clock::now();
        const double wall100ns = s_lastWall.time_since_epoch().count() != 0
                                     ? std::chrono::duration<double>(now - s_lastWall).count() * 1.0e7
                                     : 0.0;
        s_lastWall = now;
        std::vector<std::pair<double, std::string>> busy;
        const DWORD pid = GetCurrentProcessId();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return std::string();
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID != pid)
                    continue;
                HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                if (!thread)
                    continue;
                FILETIME creation{}, exit{}, kernel{}, user{};
                if (GetThreadTimes(thread, &creation, &exit, &kernel, &user))
                {
                    const uint64_t total = fileTimeTo100ns(kernel) + fileTimeTo100ns(user);
                    uint64_t &previous = s_last[entry.th32ThreadID];
                    if (wall100ns > 0.0 && previous != 0)
                    {
                        const double percent = 100.0 * static_cast<double>(total - previous) / wall100ns;
                        if (percent >= 8.0)
                        {
                            std::string name = "thread" + std::to_string(entry.th32ThreadID);
                            PWSTR description = nullptr;
                            if (SUCCEEDED(GetThreadDescription(thread, &description)) && description)
                            {
                                std::string converted;
                                for (const wchar_t *c = description; *c; ++c)
                                    converted.push_back(*c < 128 ? static_cast<char>(*c) : '?');
                                if (!converted.empty())
                                    name = converted;
                                LocalFree(description);
                            }
                            busy.push_back({percent, name});
                        }
                    }
                    previous = total;
                }
                CloseHandle(thread);
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        std::sort(busy.rbegin(), busy.rend());
        std::string text;
        for (size_t i = 0; i < busy.size() && i < 3u; ++i)
        {
            char one[64];
            std::snprintf(one, sizeof(one), "%s%s %.0f%%", i ? ", " : "", busy[i].second.c_str(), busy[i].first);
            text += one;
        }
        return text;
    }

    // The dedicated-memory-biggest adapter (the discrete GPU), asked how much video memory this process uses.
    SIZE_T g_dedicatedVram = 0; // the chosen adapter's own video memory, set with s_adapter below

    double processVramBytes()
    {
        static IDXGIAdapter3 *s_adapter = nullptr;
        static bool s_tried = false;
        if (!s_tried)
        {
            s_tried = true;
            IDXGIFactory1 *factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) && factory)
            {
                SIZE_T bestMemory = 0;
                IDXGIAdapter1 *best = nullptr;
                IDXGIAdapter1 *adapter = nullptr;
                for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
                {
                    DXGI_ADAPTER_DESC1 desc{};
                    adapter->GetDesc1(&desc);
                    if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.DedicatedVideoMemory >= bestMemory)
                    {
                        if (best)
                            best->Release();
                        best = adapter;
                        bestMemory = desc.DedicatedVideoMemory;
                    }
                    else
                    {
                        adapter->Release();
                    }
                }
                if (best)
                {
                    g_dedicatedVram = bestMemory;
                    best->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&s_adapter));
                    best->Release();
                }
                factory->Release();
            }
        }
        if (!s_adapter)
            return -1.0;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(s_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return -1.0;
        return static_cast<double>(info.CurrentUsage);
    }
}

namespace
{
    // The expensive part of the stats line: a system-wide thread snapshot plus a query per thread (topThreads) and a DXGI
    // video-memory query. Run by the "Stats" thread only (see summary()).
    // LOCAL PATCH (performance overlay): this process's share of the GPU's 3D engines, from the "GPU Engine" counters
    // Task Manager uses (summed over this process's 3D engine instances). -1 when the counters are unavailable.
    double processGpuPercent()
    {
        static PDH_HQUERY s_query = nullptr;
        static PDH_HCOUNTER s_counter = nullptr;
        static bool s_failed = false;
        if (s_failed)
            return -1.0;
        if (!s_query)
        {
            if (PdhOpenQueryW(nullptr, 0, &s_query) != ERROR_SUCCESS ||
                PdhAddEnglishCounterW(s_query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &s_counter) != ERROR_SUCCESS)
            {
                s_failed = true;
                return -1.0;
            }
            PdhCollectQueryData(s_query);
            return -1.0; // a rate needs two samples
        }
        if (PdhCollectQueryData(s_query) != ERROR_SUCCESS)
            return -1.0;
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(s_counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA)
            return -1.0;
        std::vector<unsigned char> buffer(bytes);
        auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
        if (PdhGetFormattedCounterArrayW(s_counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS)
            return -1.0;
        wchar_t prefix[32];
        swprintf(prefix, 32, L"pid_%lu_", GetCurrentProcessId());
        double total = 0.0;
        for (DWORD i = 0; i < count; ++i)
        {
            const wchar_t *name = items[i].szName;
            if (name && wcsstr(name, prefix) == name && wcsstr(name, L"engtype_3D") && items[i].FmtValue.CStatus == ERROR_SUCCESS)
                total += items[i].FmtValue.doubleValue;
        }
        return std::min(total, 100.0);
    }

    std::string computeSummary()
    {
        static uint64_t s_lastProcess = 0;
        static std::chrono::steady_clock::time_point s_lastWall{};
        static double s_cpuPercent = 0.0;
        static unsigned s_cpus = 0;
        if (s_cpus == 0)
        {
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            s_cpus = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
        }
        FILETIME creation{}, exit{}, kernel{}, user{};
        const auto now = std::chrono::steady_clock::now();
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        {
            const uint64_t total = fileTimeTo100ns(kernel) + fileTimeTo100ns(user);
            if (s_lastWall.time_since_epoch().count() != 0)
            {
                const double wall100ns =
                    std::chrono::duration<double>(now - s_lastWall).count() * 1.0e7;
                if (wall100ns > 0.0)
                    s_cpuPercent = 100.0 * static_cast<double>(total - s_lastProcess) / (wall100ns * s_cpus);
            }
            s_lastProcess = total;
            s_lastWall = now;
        }
        PROCESS_MEMORY_COUNTERS_EX mem{};
        mem.cb = sizeof(mem);
        double ramGb = 0.0;
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&mem), sizeof(mem)))
            ramGb = static_cast<double>(mem.PrivateUsage) / (1024.0 * 1024.0 * 1024.0);
        const double vram = processVramBytes();
        char text[256];
        const std::string threads = topThreads();
        if (vram >= 0.0)
            std::snprintf(text, sizeof(text), "CPU %.0f%% | RAM %.2f GB | VRAM %.2f GB", s_cpuPercent, ramGb,
                          vram / (1024.0 * 1024.0 * 1024.0));
        else
            std::snprintf(text, sizeof(text), "CPU %.0f%% | RAM %.2f GB", s_cpuPercent, ramGb);
        std::string result = text;
        // " | GPU n%" right after the CPU figure (the overlay and the title read it).
        if (const double gpu = processGpuPercent(); gpu >= 0.0)
        {
            const size_t bar = result.find(" | RAM");
            char g[32];
            std::snprintf(g, sizeof(g), " | GPU %.0f%%", gpu);
            result.insert(bar == std::string::npos ? result.size() : bar, g);
        }
        if (!threads.empty())
            result += " | " + threads;
        return result;
    }
}

namespace ps2x::sysstats
{
    // LOCAL PATCH: the window thread asked for this line once a second (title bar and perf recorder, twice per second in
    // all) and computed it right there. Snapshotting every thread of the system, opening each of ours and asking the GPU
    // driver for video memory stalled frame presentation every second: one frame gap of 110-170 ms in EVERY second of play
    // (perf_20261107_013715.csv: slow50 = 1 in 105 of 105 seconds). Now a background "Stats" thread refreshes the line once
    // a second and callers get the latest copy at once.
    std::string summary()
    {
        static std::mutex s_mutex;
        static std::string s_text;
        static std::once_flag s_once;
        std::call_once(s_once,
                       []()
                       {
                           std::thread(
                               []()
                               {
                                   SetThreadDescription(GetCurrentThread(), L"Stats");
                                   for (;;)
                                   {
                                       std::string text = computeSummary();
                                       {
                                           std::lock_guard<std::mutex> lock(s_mutex);
                                           s_text = std::move(text);
                                       }
                                       Sleep(1000);
                                   }
                               })
                               .detach();
                       });
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_text;
    }
}
namespace
{
    // LOCAL PATCH: DbgHelp alone mislabels samples. The PDB carries only public symbols for much of the code (the
    // translated VU1 programs' static runPart functions have none), so SymFromAddr returns whatever public symbol lies
    // before the address (seen: 25% "_NLG_Return2" and an "exact path" function that never ran). The x64 unwind
    // table gives every non-leaf function's exact start; the linker map (/MAP, ps2EntryRunner.map next to the exe)
    // names every function, static ones included. A sample is named after the map entry that starts exactly at its
    // function's start; failing that, DbgHelp's name only when that symbol starts there too, else "fn@<rva>".
    struct MapSymbols
    {
        std::vector<std::pair<uint32_t, std::string>> byRva; // sorted
        bool loaded = false;
    };

    MapSymbols &mapSymbols()
    {
        static MapSymbols s_map;
        if (s_map.loaded)
            return s_map;
        s_map.loaded = true;
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string mapPath = exePath;
        const size_t dot = mapPath.find_last_of('.');
        if (dot != std::string::npos)
            mapPath.resize(dot);
        mapPath += ".map";
        FILE *file = std::fopen(mapPath.c_str(), "r");
        if (!file)
            return s_map;
        uint64_t preferredBase = 0x140000000ull;
        char line[4096];
        while (std::fgets(line, sizeof(line), file))
        {
            if (std::strstr(line, "Preferred load address is"))
            {
                const char *hex = std::strstr(line, "is ") + 3;
                preferredBase = std::strtoull(hex, nullptr, 16);
                continue;
            }
            // " 0001:000123ab       ?name@@...       00000001400123ab f   lib:object.obj"
            char section[32], name[2048], address[32];
            if (std::sscanf(line, " %31s %2047s %31s", section, name, address) != 3)
                continue;
            if (std::strlen(section) != 13u || section[4] != ':' || std::strlen(address) != 16u)
                continue;
            const uint64_t value = std::strtoull(address, nullptr, 16);
            if (value < preferredBase)
                continue;
            s_map.byRva.push_back({static_cast<uint32_t>(value - preferredBase), name});
        }
        std::fclose(file);
        std::sort(s_map.byRva.begin(), s_map.byRva.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        std::fprintf(stderr, "[prof] %zu symbols from %s\n", s_map.byRva.size(), mapPath.c_str());
        return s_map;
    }

    std::string undecorated(const std::string &name)
    {
        char out[512] = {};
        if (!name.empty() && name[0] == '?' && UnDecorateSymbolName(name.c_str(), out, sizeof(out), UNDNAME_NAME_ONLY))
            return out;
        return name;
    }

    // Caller holds g_dbghelpMutex.
    std::string sampleFunctionName(DWORD64 address)
    {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(address, &imageBase, nullptr);
        const MapSymbols &map = mapSymbols();
        const HMODULE exe = GetModuleHandleA(nullptr);
        HMODULE owner = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &owner);
        const bool inExe = owner == exe;
        if (entry && inExe && !map.byRva.empty())
        {
            const uint32_t begin = entry->BeginAddress;
            const auto it = std::upper_bound(map.byRva.begin(), map.byRva.end(), begin,
                                             [](uint32_t value, const auto &item) { return value < item.first; });
            if (it != map.byRva.begin() && std::prev(it)->first == begin)
                return undecorated(std::prev(it)->second);
        }
        const DWORD64 exeBase = reinterpret_cast<DWORD64>(exe);
        if (!entry && !map.byRva.empty() && address >= exeBase && address - exeBase < 0xFFFFFFFFull)
        {
            // A leaf function (no unwind entry): the map's nearest symbol below, marked approximate with "~".
            const uint32_t rva = static_cast<uint32_t>(address - exeBase);
            const auto it = std::upper_bound(map.byRva.begin(), map.byRva.end(), rva,
                                             [](uint32_t value, const auto &item) { return value < item.first; });
            if (it != map.byRva.begin() && rva - std::prev(it)->first < 0x10000u)
                return "~" + undecorated(std::prev(it)->second);
        }
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        std::memset(symbol, 0, sizeof(SYMBOL_INFO));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        const bool named = SymFromAddr(GetCurrentProcess(), address, &displacement, symbol) != FALSE;
        if (inExe && named && (!entry || symbol->Address == imageBase + entry->BeginAddress))
            return symbol->Name; // DbgHelp's symbol really is this function (or a leaf function: no better data)
        if (!inExe)
        {
            // Another module (system / C runtime DLL, the PCSX2 bridge): its exports only, so name the module too.
            char modulePath[MAX_PATH] = {};
            std::string module = "?";
            if (owner && GetModuleFileNameA(owner, modulePath, MAX_PATH))
            {
                module = modulePath;
                const size_t slash = module.find_last_of("\\/");
                if (slash != std::string::npos)
                    module = module.substr(slash + 1u);
            }
            return module + "!" + (named ? std::string(symbol->Name) : std::string("?"));
        }
        char unknown[64];
        if (entry)
            std::snprintf(unknown, sizeof(unknown), "fn@%08x", static_cast<unsigned>(entry->BeginAddress));
        else
            std::snprintf(unknown, sizeof(unknown), "0x%llx", static_cast<unsigned long long>(address));
        return unknown;
    }
}

namespace ps2x::sysstats
{
    void startProfilerForCurrentThread(const char *label)
    {
        startProfiler(label, ::GetCurrentThreadId());
    }

    void startProfiler(const char *label, unsigned long tid)
    {
        static const bool s_enabled = std::getenv("PS2X_PROFILE") != nullptr;
        if (!s_enabled)
            return;
        const std::string name = label;
        std::thread(
            [name, tid]()
            {
                timeBeginPeriod(1);
                initSymbolsOnce();
                HANDLE target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                           static_cast<DWORD>(tid));
                if (!target)
                    return;
                std::unordered_map<uint64_t, uint32_t> hist;
                uint32_t samples = 0;
                auto lastPrint = std::chrono::steady_clock::now();
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    if (SuspendThread(target) == static_cast<DWORD>(-1))
                        break;
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_CONTROL;
                    const bool ok = GetThreadContext(target, &ctx) != 0;
                    ResumeThread(target);
                    if (ok)
                    {
                        ++hist[ctx.Rip];
                        ++samples;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastPrint < std::chrono::seconds(10))
                        continue;
                    lastPrint = now;
                    std::lock_guard<std::recursive_mutex> dbghelpLock(g_dbghelpMutex); // whole report: symbols + printing
                    // Aggregate by function.
                    std::unordered_map<std::string, uint32_t> byFunction;
                    for (const auto &[address, count] : hist)
                        byFunction[sampleFunctionName(address)] += count;
                    // LOCAL DEBUG: PS2X_PROFILE_RAW=<file> appends "<thread> <rva hex> <count>" for every sampled address in
                    // the exe, so scripts/vu1_hotlines.py can map samples to source lines through a /FAsc listing.
                    if (const char *rawPath = std::getenv("PS2X_PROFILE_RAW"))
                        if (FILE *raw = std::fopen(rawPath, "a"))
                        {
                            const DWORD64 exeBase = reinterpret_cast<DWORD64>(GetModuleHandleA(nullptr));
                            std::string tag = name;
                            std::replace(tag.begin(), tag.end(), ' ', '_');
                            for (const auto &[address, count] : hist)
                                if (address >= exeBase && address - exeBase < 0x40000000ull)
                                    std::fprintf(raw, "%s %08llx %u\n", tag.c_str(), static_cast<unsigned long long>(address - exeBase), count);
                            std::fclose(raw);
                        }
                    std::vector<std::pair<uint32_t, std::string>> ranked;
                    for (const auto &[function, count] : byFunction)
                        ranked.push_back({count, function});
                    std::sort(ranked.rbegin(), ranked.rend());
                    std::fprintf(stderr, "[prof %s] %u samples, top functions (exe loaded at %p):\n", name.c_str(), samples, static_cast<void *>(GetModuleHandleA(nullptr)));
                    for (size_t i = 0; i < ranked.size() && i < 22u; ++i)
                        std::fprintf(stderr, "[prof %s]  %5.1f%%  %s\n", name.c_str(), 100.0 * ranked[i].first / samples,
                                     ranked[i].second.substr(0, 110).c_str());
                    if (std::getenv("PS2X_PROFILE_LINES"))
                    {
                        // The hottest source lines (each line of a translated VU1 program is one instruction pair).
                        std::unordered_map<std::string, uint32_t> byLine;
                        for (const auto &[address, count] : hist)
                        {
                            IMAGEHLP_LINE64 line{};
                            line.SizeOfStruct = sizeof(line);
                            DWORD lineDisplacement = 0;
                            std::string key;
                            if (SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &line) && line.FileName)
                            {
                                std::string file = line.FileName;
                                const size_t slash = file.find_last_of("\\/");
                                if (slash != std::string::npos)
                                    file = file.substr(slash + 1u);
                                key = file + ":" + std::to_string(line.LineNumber);
                            }
                            else
                                key = "(no line info)";
                            byLine[key] += count;
                        }
                        std::vector<std::pair<uint32_t, std::string>> rankedLines;
                        for (const auto &[key, count] : byLine)
                            rankedLines.push_back({count, key});
                        std::sort(rankedLines.rbegin(), rankedLines.rend());
                        for (size_t i = 0; i < rankedLines.size() && i < 30u; ++i)
                            std::fprintf(stderr, "[prof %s line] %5.2f%%  %s\n", name.c_str(),
                                         100.0 * rankedLines[i].first / samples, rankedLines[i].second.c_str());
                    }
                    hist.clear();
                    samples = 0;
                }
            })
            .detach();
    }
}

extern std::atomic<bool> g_ps2StageDetected; // ps2_vu1_core.cpp

namespace
{
    std::atomic<int64_t> s_lastFrameNs{0};
    std::atomic<uint64_t> s_frameCounter{0};
    std::once_flag s_hitchOnce;

    int64_t nowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void ensureSymbols()
    {
        initSymbolsOnce();
    }

    // Unwinds a suspended thread with the PE unwind tables only (no heap use, so it cannot deadlock on a lock the
    // suspended thread holds). Returns the number of return addresses written to pcs.
    int unwindSuspended(CONTEXT ctx, DWORD64 *pcs, int maxPcs)
    {
        int count = 0;
        __try
        {
            while (count < maxPcs && ctx.Rip != 0)
            {
                pcs[count++] = ctx.Rip;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (!entry)
                {
                    ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp); // leaf function: the return address is on top
                    ctx.Rsp += 8;
                }
                else
                {
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, entry, &ctx, &handlerData, &establisher, nullptr);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return count;
    }

    std::string functionName(DWORD64 address)
    {
        std::lock_guard<std::recursive_mutex> dbghelpLock(g_dbghelpMutex);
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        std::memset(symbol, 0, sizeof(SYMBOL_INFO));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
        {
            std::string name = symbol->Name;
            return name.size() > 48u ? name.substr(0, 48u) : name;
        }
        char unknown[24];
        std::snprintf(unknown, sizeof(unknown), "0x%llx", static_cast<unsigned long long>(address));
        return unknown;
    }

    void captureHitch(int gapMs)
    {
        ensureSymbols();
        struct Captured
        {
            std::string name;
            DWORD64 pcs[12];
            int count = 0;
        };
        std::vector<Captured> captured;
        int bands = 0;
        const DWORD pid = GetCurrentProcessId();
        const DWORD self = GetCurrentThreadId();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
                    continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                           entry.th32ThreadID);
                if (!thread)
                    continue;
                PWSTR description = nullptr;
                std::string name;
                if (SUCCEEDED(GetThreadDescription(thread, &description)) && description)
                {
                    for (const wchar_t *c = description; *c; ++c)
                        name.push_back(*c < 128 ? static_cast<char>(*c) : '?');
                    LocalFree(description);
                }
                const bool wanted = name == "GameThread" || name == "VIF1+VU1" || name == "GS worker" ||
                                    name == "GS present" || (name == "GS band" && bands++ < 2);
                if (!wanted)
                {
                    CloseHandle(thread);
                    continue;
                }
                Captured c;
                c.name = name;
                if (SuspendThread(thread) != static_cast<DWORD>(-1))
                {
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(thread, &ctx))
                        c.count = unwindSuspended(ctx, c.pcs, 12);
                    ResumeThread(thread);
                }
                CloseHandle(thread);
                captured.push_back(std::move(c));
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        std::fprintf(stderr, "[hitch] no new frame for %d ms\n", gapMs);
        for (const Captured &c : captured)
        {
            std::string line;
            for (int i = 0; i < c.count && i < 9; ++i)
            {
                if (i)
                    line += " < ";
                line += functionName(c.pcs[i]);
            }
            std::fprintf(stderr, "[hitch]   %-10s %s\n", c.name.c_str(), line.c_str());
        }
    }

    void hitchLoop()
    {
        // PS2X_HITCH_AT=<seconds>: capture the thread stacks once at that time, stage or not (finds a frozen game).
        if (const char *at = std::getenv("PS2X_HITCH_AT"))
        {
            const int seconds = std::atoi(at);
            for (int i = 0; i < 3; ++i)
            {
                std::this_thread::sleep_for(std::chrono::seconds(i == 0 ? seconds : 5));
                captureHitch(-1);
            }
        }
        uint64_t lastHandled = 0;
        int reported = 0;
        int64_t lastReport = 0;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const int64_t last = s_lastFrameNs.load(std::memory_order_relaxed);
            if (last == 0 || !g_ps2StageDetected.load(std::memory_order_relaxed))
                continue;
            const int64_t now = nowNs();
            const int gapMs = static_cast<int>((now - last) / 1000000);
            const uint64_t frame = s_frameCounter.load(std::memory_order_relaxed);
            if (gapMs >= 60 && frame != lastHandled && reported < 80 && now - lastReport > 500000000)
            {
                lastHandled = frame; // once per slow frame
                lastReport = now;
                ++reported;
                captureHitch(gapMs);
            }
        }
    }
}

namespace ps2x::sysstats
{
    void initHostTiming()
    {
        if (std::getenv("PS2X_HITCH_AT"))
            std::call_once(s_hitchOnce, []() { std::thread(hitchLoop).detach(); });
        static bool s_done = false;
        if (s_done || std::getenv("PS2X_NO_TIMER_FIX"))
            return;
        s_done = true;
        timeBeginPeriod(1);
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        state.StateMask = 0; // controlled and switched off: Windows must not throttle this process
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
    }

    void raiseCurrentThreadPriority(const char *label)
    {
        static const bool s_enabled = []() { const char *v = std::getenv("PS2X_THREAD_PRIORITY"); return !(v && v[0] == '0'); }();
        if (!s_enabled)
            return;
        const BOOL ok = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        std::fprintf(stderr, "[thread] %s priority above normal: %s\n", label, ok ? "ok" : "failed");
    }

    void startAsyncStderr()
    {
        static bool s_done = false;
        if (s_done)
            return;
        s_done = true;
        const char *off = std::getenv("PS2X_LOG_ASYNC");
        if (off && off[0] == '0')
            return;
        HANDLE original = GetStdHandle(STD_ERROR_HANDLE);
        if (!original || original == INVALID_HANDLE_VALUE || GetFileType(original) != FILE_TYPE_DISK)
            return; // a console or a pipe already behaves
        // _dup2 below closes the CRT's own stderr handle, so the writer thread works on a private copy of the file handle.
        HANDLE sink = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &sink, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return;
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, nullptr, 4u << 20))
        {
            CloseHandle(sink);
            return;
        }
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(writeEnd), _O_BINARY);
        if (fd < 0)
        {
            CloseHandle(readEnd);
            CloseHandle(writeEnd);
            return;
        }
        std::fflush(stderr);
        if (_dup2(fd, 2) != 0)
        {
            _close(fd);
            CloseHandle(readEnd);
            return;
        }
        _close(fd);
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(2)));
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::thread(
            [readEnd, sink]()
            {
                char buffer[65536];
                DWORD got = 0;
                while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got != 0)
                {
                    DWORD written = 0;
                    WriteFile(sink, buffer, got, &written, nullptr);
                }
            })
            .detach();
        std::atexit([]() { Sleep(150); }); // let the writer thread catch up before the process ends
    }

    std::string symbolName(unsigned long long address)
    {
        ensureSymbols();
        return functionName(static_cast<DWORD64>(address));
    }

    void noteFrameShown()
    {
        static const bool s_enabled = std::getenv("PS2X_HITCH_LOG") != nullptr;
        if (!s_enabled)
            return;
        std::call_once(s_hitchOnce, []() { std::thread(hitchLoop).detach(); });
        s_lastFrameNs.store(nowNs(), std::memory_order_relaxed);
        s_frameCounter.fetch_add(1u, std::memory_order_relaxed);
    }

    // LOCAL PATCH: crash reporter. An unhandled exception (one no __try guard caught) prints, before the process ends:
    // the exception, the name of the crashing thread, and its call stack as module+offset with function names where
    // the PDB has them, and writes a minidump to captures/crash_<time>.dmp. Everything goes to stderr (last_run.log).
    namespace
    {
        LONG WINAPI reportCrash(EXCEPTION_POINTERS *info)
        {
            static std::atomic<bool> s_reported{false};
            if (s_reported.exchange(true))
                return EXCEPTION_CONTINUE_SEARCH;
            const EXCEPTION_RECORD *record = info->ExceptionRecord;
            wchar_t *wideName = nullptr;
            char threadName[128] = "?";
            if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &wideName)) && wideName)
            {
                WideCharToMultiByte(CP_UTF8, 0, wideName, -1, threadName, sizeof(threadName), nullptr, nullptr);
                LocalFree(wideName);
            }
            std::fprintf(stderr, "[crash] exception 0x%08lx at %p on thread %lu (%s)",
                         static_cast<unsigned long>(record->ExceptionCode), record->ExceptionAddress,
                         static_cast<unsigned long>(GetCurrentThreadId()), threadName);
            if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
                std::fprintf(stderr, ", %s address %p", record->ExceptionInformation[0] == 0 ? "reading" : "writing",
                             reinterpret_cast<void *>(record->ExceptionInformation[1]));
            std::fprintf(stderr, "\n");
            DWORD64 pcs[48];
            const int count = unwindSuspended(*info->ContextRecord, pcs, 48);
            // Symbolise only if DbgHelp is free (the crash may have happened inside it).
            const bool symbols = g_dbghelpMutex.try_lock();
            if (symbols)
                initSymbolsOnce();
            for (int i = 0; i < count; ++i)
            {
                HMODULE module = nullptr;
                char modulePath[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(pcs[i]), &module))
                    GetModuleFileNameA(module, modulePath, MAX_PATH);
                const char *moduleName = std::strrchr(modulePath, '\\');
                moduleName = moduleName ? moduleName + 1 : modulePath;
                std::fprintf(stderr, "[crash]   #%02d %s+0x%llx  %s\n", i, moduleName,
                             static_cast<unsigned long long>(pcs[i] - reinterpret_cast<DWORD64>(module)),
                             symbols ? functionName(pcs[i]).c_str() : "");
            }
            if (symbols)
                g_dbghelpMutex.unlock();
            CreateDirectoryA("captures", nullptr);
            char dumpPath[MAX_PATH];
            SYSTEMTIME now;
            GetLocalTime(&now);
            std::snprintf(dumpPath, sizeof(dumpPath), "captures/crash_%04u%02u%02u_%02u%02u%02u.dmp", now.wYear, now.wMonth,
                          now.wDay, now.wHour, now.wMinute, now.wSecond);
            HANDLE file = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
                const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                                       static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                                                       &exception, nullptr, nullptr);
                CloseHandle(file);
                std::fprintf(stderr, "[crash] minidump %s %s\n", dumpPath, written ? "written" : "FAILED");
            }
            std::fflush(stderr);
            Sleep(300); // the log writer thread
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void installCrashReporter()
    {
        SetUnhandledExceptionFilter(reportCrash);
    }
}
#else
namespace ps2x::sysstats
{
    std::string summary() { return std::string(); }
    void startProfiler(const char *, unsigned long) {}
    void startProfilerForCurrentThread(const char *) {}
    void initHostTiming() {}
    void raiseCurrentThreadPriority(const char *) {}
    void startAsyncStderr() {}
    void installCrashReporter() {}
    void noteFrameShown() {}
    std::string symbolName(unsigned long long) { return std::string(); }
}
#endif

// LOCAL PATCH (4K/8K guard): the biggest graphics card's dedicated video memory in bytes, 0 when unknown.
double ps2x::sysstats::dedicatedVramBytes()
{
#if defined(_WIN32)
    processVramBytes(); // finds the adapter once
    return static_cast<double>(g_dedicatedVram);
#else
    return 0.0;
#endif
}
