#ifndef PS2_LOG_H
#define PS2_LOG_H

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef PS2_RUNTIME_LOGS
#define PS2_RUNTIME_LOGS 0
#endif

#ifndef AGRESSIVE_LOGS
#define AGRESSIVE_LOGS 0
#endif

#define RUNTIME_ERROR(x)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        std::ostringstream _ps2_runtime_error_stream;                                                                  \
        _ps2_runtime_error_stream << x;                                                                                \
        const std::string _ps2_runtime_error_text =                                                                    \
            _ps2_runtime_error_stream.str();                                                                           \
                                                                                                                       \
        std::cerr << _ps2_runtime_error_text;                                                                           \
        ps2_log::append_runtime_log_text(_ps2_runtime_error_text);                                                      \
    } while (0)

namespace ps2_log
{
struct RuntimeLogEntry
{
    uint64_t seq = 0;
    std::string text;
};

inline constexpr size_t kMaxRuntimeLogEntries = 4096;

inline std::mutex &runtime_log_mutex()
{
    static std::mutex m;
    return m;
}

inline std::deque<RuntimeLogEntry> &runtime_log_entries()
{
    static std::deque<RuntimeLogEntry> entries;
    return entries;
}

inline uint64_t &runtime_log_next_seq()
{
    static uint64_t seq = 1;
    return seq;
}

inline bool &runtime_log_paused()
{
    static bool paused = false;
    return paused;
}

inline void set_runtime_log_paused(bool paused)
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    runtime_log_paused() = paused;
}

inline bool is_runtime_log_paused()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    return runtime_log_paused();
}

inline void append_runtime_log_text(const std::string &text)
{
    if (text.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    if (runtime_log_paused())
    {
        return;
    }

    auto &entries = runtime_log_entries();
    RuntimeLogEntry entry{};
    entry.seq = runtime_log_next_seq()++;
    entry.text = text;
    entries.push_back(std::move(entry));

    while (entries.size() > kMaxRuntimeLogEntries)
    {
        entries.pop_front();
    }
}

inline std::vector<RuntimeLogEntry> snapshot_runtime_log_entries()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    const auto &entries = runtime_log_entries();
    return std::vector<RuntimeLogEntry>(entries.begin(), entries.end());
}

inline void clear_runtime_log_entries()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    runtime_log_entries().clear();
}
}

#if PS2_RUNTIME_LOGS || AGRESSIVE_LOGS
#define RUNTIME_LOG(x)                                                                                                  \
    do                                                                                                                  \
    {                                                                                                                   \
        std::ostringstream _ps2_runtime_log_stream;                                                                     \
        _ps2_runtime_log_stream << x;                                                                                   \
        const std::string _ps2_runtime_log_text = _ps2_runtime_log_stream.str();                                        \
        if (_ps2_runtime_log_text.empty())                                                                            \
        {                                                                                                               \
            std::cout.flush();                                                                                          \
        }                                                                                                               \
        else                                                                                                            \
        {                                                                                                               \
            std::cout << _ps2_runtime_log_text;                                                                         \
            ps2_log::append_runtime_log_text(_ps2_runtime_log_text);                                                    \
        }                                                                                                               \
    } while (0)
#else
#define RUNTIME_LOG(x) do {} while(0)
#endif

#if AGRESSIVE_LOGS

namespace ps2_log
{
inline std::string log_path()
{
    static std::string path;
    if (path.empty())
    {
        path = (std::filesystem::current_path() / "ps2_log.txt").string();
    }
    return path;
}
inline std::ostream &log_stream()
{
    static std::ofstream f(log_path(), std::ios::out);
    return f.is_open() ? f : std::cerr;
}
inline int &depth()
{
    static thread_local int d = 0;
    return d;
}
// LOCAL DEBUG: with PS2X_FUNC_HIST set, count calls per generated function (name pointers are
// string literals, so unique per function) and print each thread's 25 busiest every 5 seconds
// as "[fhist]" lines on stderr, instead of writing every call to ps2_log.txt.
inline bool func_hist_enabled()
{
    static const bool enabled = std::getenv("PS2X_FUNC_HIST") != nullptr;
    return enabled;
}
struct FuncHist
{
    std::unordered_map<const char *, uint64_t> counts;
    uint64_t sinceCheck = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
inline FuncHist &func_hist()
{
    static thread_local FuncHist h;
    return h;
}
inline void func_hist_count(const char *name)
{
    FuncHist &h = func_hist();
    ++h.counts[name];
    if (++h.sinceCheck < 4096)
        return;
    h.sinceCheck = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - h.last < std::chrono::seconds(5))
        return;
    h.last = now;
    uint64_t total = 0;
    std::vector<std::pair<const char *, uint64_t>> top(h.counts.begin(), h.counts.end());
    for (const auto &kv : top)
        total += kv.second;
    if (total >= 20000)
    {
        // PS2X_FUNC_HIST_SKIP="Pad,IzMem,entry_" hides functions whose names contain any of these.
        static const std::vector<std::string> skip = []()
        {
            std::vector<std::string> out;
            if (const char *env = std::getenv("PS2X_FUNC_HIST_SKIP"))
            {
                std::string cur;
                for (const char *p = env;; ++p)
                {
                    if (*p == ',' || *p == '\0')
                    {
                        if (!cur.empty())
                            out.push_back(cur);
                        cur.clear();
                        if (*p == '\0')
                            break;
                    }
                    else
                        cur += *p;
                }
            }
            return out;
        }();
        std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::fprintf(stderr, "[fhist] thread=%zu calls=%llu\n", std::hash<std::thread::id>{}(std::this_thread::get_id()),
                     static_cast<unsigned long long>(total));
        size_t shown = 0;
        for (size_t i = 0; i < top.size() && shown < 45; ++i)
        {
            bool hidden = false;
            for (const std::string &s : skip)
                if (std::strstr(top[i].first, s.c_str()) != nullptr)
                    hidden = true;
            if (hidden)
                continue;
            ++shown;
            std::fprintf(stderr, "[fhist]   %8llu  %s\n", static_cast<unsigned long long>(top[i].second), top[i].first);
        }
        std::fflush(stderr);
    }
    h.counts.clear();
}
inline void log_entry(const char *name)
{
    if (func_hist_enabled())
    {
        func_hist_count(name);
        return;
    }
    for (int i = 0; i < depth(); ++i)
        log_stream() << '\t';
    log_stream() << ">> " << name << " enter\n";
    log_stream().flush();
    depth()++;
}
inline void log_exit(const char *name)
{
    if (func_hist_enabled())
        return;
    depth()--;
    for (int i = 0; i < depth(); ++i)
        log_stream() << '\t';
    log_stream() << "<< " << name << " exit\n";
    log_stream().flush();
}
inline void print_saved_location()
{
    std::cout << "[PS2 LOG] Logs saved at " << log_path() << std::endl;
}
}

#define PS_LOG_ENTRY(name) \
    ps2_log::log_entry(name); \
    struct _ps2_log_guard_ { const char *_n; _ps2_log_guard_(const char *n) : _n(n) {} \
        ~_ps2_log_guard_() { ps2_log::log_exit(_n); } } _ps2_log_guard_(name)
#define PS2_IF_AGRESSIVE_LOGS(code) \
    do                              \
    {                               \
        code;                       \
    } while (0)

#else

namespace ps2_log
{
inline std::string log_path()
{
    return (std::filesystem::current_path() / "ps2_log.txt").string();
}
inline void print_saved_location() {}
}
#define PS_LOG_ENTRY(name) ((void)0)
#define PS2_IF_AGRESSIVE_LOGS(code) ((void)0)

#endif

#endif
