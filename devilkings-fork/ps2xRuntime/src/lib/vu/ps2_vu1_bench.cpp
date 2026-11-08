// LOCAL PATCH: headless VU1 replay benchmark and equivalence check.
//
// Record real runs while playing:   PS2X_VU1_THREADS=0 PS2X_VU1_RECORD=<dir>[:<every>[:<max>]] ps2EntryRunner.exe <elf>
// (ps2_vu1_core.cpp saves the complete starting state of every <every>-th program run as <dir>/run_<n>.bin), then
//   PS2X_VU1_BENCH=<dir> PS2X_GS_NO_DRAW=1 ps2EntryRunner.exe                (translated programs where they match)
//   PS2X_VU1_BENCH=<dir> PS2X_GS_NO_DRAW=1 PS2X_VU1_COMPILED=0 ps2EntryRunner.exe   (interpreter)
// No game and no window. The checksums of the two modes must be identical; ns/cycle shows the speedup per program.
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_compiled.h"
#include "runtime/ps2_sysstats.h"

#include <memory>
#include <limits>
#include <immintrin.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
    struct Run
    {
        uint32_t startPC = 0, top = 0, itop = 0, maxCycles = 0, kind = 0;
        VU1State state{};
        std::vector<uint8_t> code, data;
        const ps2x::vu1c::Program *program = nullptr;
        size_t codeId = 0;
    };

    uint64_t fnv(uint64_t hash, const void *data, size_t size)
    {
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    uint64_t checksum(const VU1State &s, const uint8_t *data, uint32_t dataSize)
    {
        uint64_t h = 1469598103934665603ull;
        h = fnv(h, s.vf, sizeof(s.vf));
        h = fnv(h, s.vi, sizeof(s.vi));
        h = fnv(h, s.acc, sizeof(s.acc));
        h = fnv(h, &s.q, sizeof(s.q));
        h = fnv(h, &s.p, sizeof(s.p));
        h = fnv(h, &s.i, sizeof(s.i));
        h = fnv(h, &s.mac, sizeof(s.mac));
        h = fnv(h, &s.clip, sizeof(s.clip));
        h = fnv(h, &s.status, sizeof(s.status));
        h = fnv(h, &s.pc, sizeof(s.pc));
        h = fnv(h, &s.cycles, sizeof(s.cycles));
        h = fnv(h, data, dataSize);
        return h;
    }

    bool loadRun(const std::string &path, Run &run)
    {
        std::FILE *file = std::fopen(path.c_str(), "rb");
        if (!file)
            return false;
        uint32_t header[10] = {};
        bool ok = std::fread(header, 9 * sizeof(uint32_t), 1, file) == 1 && header[0] == 0x52315556u &&
                  header[6] == sizeof(VU1State);
        if (ok && header[1] >= 2u)
            ok = std::fread(&header[9], sizeof(uint32_t), 1, file) == 1;
        if (ok)
        {
            run.startPC = header[2];
            run.top = header[3];
            run.itop = header[4];
            run.maxCycles = header[5];
            run.kind = header[1] >= 2u ? header[9] : 0u;
            ok = std::fread(&run.state, sizeof(VU1State), 1, file) == 1;
            run.code.resize(header[7]);
            run.data.resize(header[8]);
            ok = ok && std::fread(run.code.data(), 1, run.code.size(), file) == run.code.size() &&
                 std::fread(run.data.data(), 1, run.data.size(), file) == run.data.size();
        }
        std::fclose(file);
        return ok;
    }
}

namespace
{
    // PS2X_VU1_SELFTEST=1: OPMULA's fast path (fmacVec / fmacLite) against the exact path, on random vectors full of
    // +-0, tiny, huge and ordinary values. Results (DEST lanes) and the queued MAC/status flags must be bit-identical.
    int opmulaSelfTest()
    {
        using ps2x::vu1c::Exec;
        uint64_t seed = 0x9E3779B97F4A7C15ull;
        auto next = [&]() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
        auto value = [&]() -> float
        {
            const uint64_t r = next() % 10u;
            const float sign = (next() & 1u) ? -1.0f : 1.0f;
            if (r < 4u) return sign * 0.0f;                                   // +-0, very common in cross products
            if (r == 4u) return sign * 1.0e-30f;                              // tiny
            if (r == 5u) return sign * 1.0e30f;                               // huge
            return sign * static_cast<float>(next() % 2000000u) / 1000.0f;    // ordinary
        };
        const unsigned int saved = _mm_getcsr();
        _mm_setcsr((saved & ~0x6000u) | 0x6000u | 0x0040u); // round to zero, DAZ: as during a compiled run
        auto fresh = []()
        {
            auto x = std::make_unique<Exec>();
            x->nextCommit = ps2x::vu1c::kNever;
            return x;
        };
        uint64_t mismatches = 0, cases = 0, fastTaken = 0;
        for (int i = 0; i < 200000; ++i)
        {
            alignas(16) float l[4], r[4];
            for (int c = 0; c < 4; ++c) { l[c] = value(); r[c] = value(); }
            const __m128 lv = _mm_loadu_ps(l), rv = _mm_loadu_ps(r);
            const uint8_t dest = 0xE; // OPMULA writes xyz
            auto a = fresh();
            const uint64_t exactBefore = ps2x::vu1c::g_exactCalls.load();
            alignas(16) float fastOut[4];
            _mm_storeu_ps(fastOut, a->fmacVec<ps2x::vu1c::kFmacOpmula, false>(0u, dest, lv, rv, _mm_setzero_ps()));
            if (ps2x::vu1c::g_exactCalls.load() == exactBefore)
                ++fastTaken;
            auto b = fresh();
            alignas(16) float exactOut[4];
            __m128 product = _mm_blend_ps(_mm_mul_ps(lv, rv), _mm_setzero_ps(), 0x8);
            _mm_storeu_ps(exactOut, product);
            b->fmacVecExact<ps2x::vu1c::kFmacOpmula, false>(0u, dest, lv, rv, _mm_setzero_ps(), exactOut);
            ++cases;
            bool bad = false;
            for (int c = 0; c < 3; ++c)
                bad = bad || ps2x::vu1c::floatBits(fastOut[c]) != ps2x::vu1c::floatBits(exactOut[c]);
            const auto &fa = a->flags[(a->flagHead + a->flagCount - 1u) & 15u];
            const auto &fb = b->flags[(b->flagHead + b->flagCount - 1u) & 15u];
            bad = bad || a->flagCount != b->flagCount || fa.mac != fb.mac || fa.status != fb.status ||
                  fa.extraSticky != fb.extraSticky;
            if (bad && ++mismatches <= 5)
                std::fprintf(stderr, "[selftest] MISMATCH l=(%g %g %g) r=(%g %g %g) fast=(%08x %08x %08x) mac=%x st=%x "
                                     "exact=(%08x %08x %08x) mac=%x st=%x\n",
                             l[0], l[1], l[2], r[0], r[1], r[2], ps2x::vu1c::floatBits(fastOut[0]), ps2x::vu1c::floatBits(fastOut[1]),
                             ps2x::vu1c::floatBits(fastOut[2]), fa.mac, fa.status, ps2x::vu1c::floatBits(exactOut[0]),
                             ps2x::vu1c::floatBits(exactOut[1]), ps2x::vu1c::floatBits(exactOut[2]), fb.mac, fb.status);
        }
        _mm_setcsr(saved);
        std::fprintf(stderr, "[selftest] OPMULA fast vs exact: %llu cases, fast path taken in %llu, %llu mismatches\n",
                     static_cast<unsigned long long>(cases), static_cast<unsigned long long>(fastTaken),
                     static_cast<unsigned long long>(mismatches));
        return mismatches == 0 ? 0 : 1;
    }

    // Every FMAC kind's fast path (fmacVec with queued flags, fmacLite with sticky bits) against the exact path, on
    // random vectors with many exact cancellations (r = l or -l), signed zeros, denormals, tiny, huge and infinite
    // values. DEST lanes of the result and the flags must be bit-identical. Returns the number of mismatches.
    template <int kKind, bool kProductSum>
    uint64_t fmacKindSelfTest(const char *name)
    {
        using ps2x::vu1c::Exec;
        uint64_t seed = 0xD1B54A32D192ED03ull ^ static_cast<uint64_t>(kKind * 7919 + (kProductSum ? 1 : 0));
        auto next = [&]() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
        auto value = [&]() -> float
        {
            const uint64_t r = next() % 12u;
            const float sign = (next() & 1u) ? -1.0f : 1.0f;
            if (r < 3u) return sign * 0.0f;
            if (r == 3u) return ps2x::vu1c::bitsFloat(static_cast<uint32_t>((next() % 0x7FFFFFu) + 1u) | (sign < 0 ? 0x80000000u : 0u)); // denormal
            if (r == 4u) return sign * 1.0e-30f;
            if (r == 5u) return sign * 1.0e30f;
            if (r == 6u) return sign * 3.0e38f;
            if (r == 7u) return sign * std::numeric_limits<float>::infinity();
            return sign * static_cast<float>(next() % 2000000u) / 1000.0f;
        };
        auto fresh = []()
        {
            auto x = std::make_unique<Exec>();
            x->nextCommit = ps2x::vu1c::kNever;
            return x;
        };
        constexpr bool kUsesAcc = kKind == ps2x::vu1c::kFmacMadd || kKind == ps2x::vu1c::kFmacMsub || kKind == ps2x::vu1c::kFmacOpmsub;
        uint64_t mismatches = 0, cases = 0, fastTaken = 0;
        for (int i = 0; i < 200000; ++i)
        {
            alignas(16) float l[4], r[4], acc[4];
            for (int c = 0; c < 4; ++c)
            {
                l[c] = value();
                const uint64_t pick = next() % 4u;
                r[c] = pick == 0u ? l[c] : pick == 1u ? -l[c] : value(); // exact cancellations
                acc[c] = value();
            }
            // ACC only ever holds FMAC results: normalised (no denormals, no infinities).
            const __m128 accv = ps2x::vu1c::norm4(_mm_loadu_ps(acc));
            const __m128 lv = _mm_loadu_ps(l), rv = _mm_loadu_ps(r);
            uint8_t dest = static_cast<uint8_t>(next() % 15u + 1u);
            if constexpr (kKind == ps2x::vu1c::kFmacOpmsub || kKind == ps2x::vu1c::kFmacOpmula)
                dest = 0xE;
            const __m128 accNorm = kUsesAcc ? accv : _mm_setzero_ps();
            const uint32_t lanes = ps2x::vu1c::destLaneBits(dest);

            // fmacVec against fmacVecExact (queued MAC/status).
            auto a = fresh();
            const uint64_t exactBefore = ps2x::vu1c::g_exactCalls.load();
            alignas(16) float fastOut[4], exactOut[4];
            _mm_storeu_ps(fastOut, a->fmacVec<kKind, kProductSum>(0u, dest, lv, rv, accv));
            if (ps2x::vu1c::g_exactCalls.load() == exactBefore)
                ++fastTaken;
            auto b = fresh();
            b->fmacVecExact<kKind, kProductSum>(0u, dest, lv, rv, accNorm, exactOut);
            bool bad = false;
            for (int c = 0; c < 4; ++c)
                if ((lanes >> c) & 1u)
                    bad = bad || ps2x::vu1c::floatBits(fastOut[c]) != ps2x::vu1c::floatBits(exactOut[c]);
            const auto &fa = a->flags[(a->flagHead + a->flagCount - 1u) & 15u];
            const auto &fb = b->flags[(b->flagHead + b->flagCount - 1u) & 15u];
            bad = bad || a->flagCount != b->flagCount || fa.mac != fb.mac || fa.status != fb.status ||
                  fa.extraSticky != fb.extraSticky;

            // fmacLite against the exact path's sticky bits (both folded into status).
            auto la = fresh();
            __m128i signAcc = _mm_setzero_si128(), minAbs = _mm_set1_epi32(-1);
            alignas(16) float liteOut[4], liteExactOut[4];
            _mm_storeu_ps(liteOut, la->fmacLite<kKind, kProductSum>(dest, lv, rv, accv, signAcc, minAbs));
            la->foldLite(signAcc, minAbs);
            auto lb = fresh();
            lb->fmacVecExact<kKind, kProductSum, true>(0u, dest, lv, rv, accNorm, liteExactOut);
            lb->foldLite(_mm_setzero_si128(), _mm_set1_epi32(-1));
            for (int c = 0; c < 4; ++c)
                if ((lanes >> c) & 1u)
                    bad = bad || ps2x::vu1c::floatBits(liteOut[c]) != ps2x::vu1c::floatBits(liteExactOut[c]);
            bad = bad || la->status != lb->status;

            ++cases;
            if (bad && ++mismatches <= 3)
                std::fprintf(stderr, "[selftest] %s MISMATCH dest=%x l=(%g %g %g %g) r=(%g %g %g %g) fast mac=%x st=%x exact mac=%x st=%x "
                                     "lite st=%x exact-lite st=%x\n",
                             name, dest, l[0], l[1], l[2], l[3], r[0], r[1], r[2], r[3], fa.mac, fa.status, fb.mac, fb.status,
                             la->status, lb->status);
        }
        std::fprintf(stderr, "[selftest] %-14s fast vs exact: %llu cases, fast path taken in %llu (%.0f%%), %llu mismatches\n", name,
                     static_cast<unsigned long long>(cases), static_cast<unsigned long long>(fastTaken),
                     100.0 * static_cast<double>(fastTaken) / static_cast<double>(cases), static_cast<unsigned long long>(mismatches));
        return mismatches;
    }

    int fmacSelfTest()
    {
        int failed = opmulaSelfTest();
        const unsigned int saved = _mm_getcsr();
        _mm_setcsr((saved & ~0x6000u) | 0x6000u | 0x0040u); // round to zero, DAZ: as during a compiled run
        using namespace ps2x::vu1c;
        uint64_t total = 0;
        total += fmacKindSelfTest<kFmacAdd, false>("ADD");
        total += fmacKindSelfTest<kFmacSub, false>("SUB");
        total += fmacKindSelfTest<kFmacMul, false>("MUL");
        total += fmacKindSelfTest<kFmacMadd, true>("MADD");
        total += fmacKindSelfTest<kFmacMsub, true>("MSUB");
        total += fmacKindSelfTest<kFmacOpmsub, true>("OPMSUB");
        total += fmacKindSelfTest<kFmacOpmula, false>("OPMULA");
        _mm_setcsr(saved);
        std::fprintf(stderr, "[selftest] all FMAC kinds: %llu mismatches\n", static_cast<unsigned long long>(total));
        return (failed != 0 || total != 0) ? 1 : 0;
    }
}

namespace ps2x
{
    int runVu1Bench(const char *dir)
    {
        if (std::getenv("PS2X_VU1_SELFTEST"))
            return fmacSelfTest();
        std::vector<Run> runs;
        for (int index = 0;; ++index)
        {
            Run run;
            if (!loadRun(std::string(dir) + "/run_" + std::to_string(index) + ".bin", run))
                break;
            runs.push_back(std::move(run));
        }
        if (runs.empty())
        {
            std::fprintf(stderr, "[bench] no run_*.bin files in %s (record some with PS2X_VU1_RECORD)\n", dir);
            return 1;
        }

        PS2Memory mem;
        if (!mem.initialize())
        {
            std::fprintf(stderr, "[bench] memory init failed\n");
            return 1;
        }
        GS gs;
        gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
        VU1Interpreter vu1;

        // Which programs the runs belong to (matched against each run's own micro memory).
        std::vector<std::vector<uint8_t>> images;
        size_t translated = 0;
        std::map<std::string, size_t> perProgram;
        for (Run &run : runs)
        {
            size_t id = images.size();
            for (size_t k = 0; k < images.size(); ++k)
                if (images[k] == run.code)
                {
                    id = k;
                    break;
                }
            if (id == images.size())
                images.push_back(run.code);
            run.codeId = id;
            std::memcpy(mem.getVU1Code(), run.code.data(), std::min<size_t>(run.code.size(), PS2_VU1_CODE_SIZE));
            run.program = ps2x::vu1c::findProgram(mem.getVU1Code(), PS2_VU1_CODE_SIZE);
            if (run.program)
                ++translated;
            ++perProgram[run.program ? run.program->name : "(interpreter only)"];
        }
        std::fprintf(stderr, "[bench] %zu runs, %zu distinct micro images, %zu runs match a translated program\n",
                     runs.size(), images.size(), translated);
        for (const auto &[name, count] : perProgram)
            std::fprintf(stderr, "[bench]   %-28s %zu runs\n", name.c_str(), count);

        size_t loadedImage = ~size_t(0);
        const auto prepare = [&](const Run &run)
        {
            if (run.codeId != loadedImage)
            {
                std::memcpy(mem.getVU1Code(), run.code.data(), std::min<size_t>(run.code.size(), PS2_VU1_CODE_SIZE));
                mem.write32(PS2_VU1_CODE_BASE, *reinterpret_cast<const uint32_t *>(run.code.data())); // bumps the generation
                loadedImage = run.codeId;
            }
            std::memcpy(mem.getVU1Data(), run.data.data(), std::min<size_t>(run.data.size(), PS2_VU1_DATA_SIZE));
            vu1.reset();
            vu1.state() = run.state;
            vu1.state().cycles = 0;
        };
        const auto execute = [&](const Run &run) -> uint64_t
        {
            if (run.kind == 1u)
                vu1.resume(mem.getVU1Code(), PS2_VU1_CODE_SIZE, mem.getVU1Data(), PS2_VU1_DATA_SIZE, gs, &mem, run.top,
                           run.itop, run.maxCycles ? run.maxCycles : 65536u);
            else
                vu1.execute(mem.getVU1Code(), PS2_VU1_CODE_SIZE, mem.getVU1Data(), PS2_VU1_DATA_SIZE, gs, &mem,
                            run.startPC, run.top, run.itop, run.maxCycles ? run.maxCycles : 65536u);
            return vu1.state().cycles;
        };
        const auto replay = [&](const Run &run) -> uint64_t
        {
            prepare(run);
            return execute(run);
        };

        // Pass 1: results and checksum (identical in every mode if the translation is exact).
        uint64_t total = 1469598103934665603ull;
        uint64_t cycles1 = 0;
        for (const Run &run : runs)
        {
            const uint64_t c = replay(run);
            cycles1 += c;
            total = fnv(total, &c, sizeof(c));
            const uint64_t sum = checksum(vu1.state(), mem.getVU1Data(), PS2_VU1_DATA_SIZE);
            total = fnv(total, &sum, sizeof(sum));
        }
        std::fprintf(stderr, "[bench] pass 1: %llu cycles in %zu runs (avg %.0f), CHECKSUM %016llx\n",
                     static_cast<unsigned long long>(cycles1), runs.size(), static_cast<double>(cycles1) / runs.size(),
                     static_cast<unsigned long long>(total));

        // Timing passes, per program.
        struct Timing
        {
            double ns = 0;
            uint64_t cycles = 0;
            size_t runs = 0;
        };
        std::map<std::string, Timing> timing;
        // PS2X_VU1_BENCH_PASSES=<n> (default 8); with PS2X_PROFILE set this thread is sampled too (a long run, e.g. 400
        // passes, gives the profiler's per-function and per-line report of the translated code).
        const char *passesText = std::getenv("PS2X_VU1_BENCH_PASSES");
        const int passes = passesText ? std::max(1, std::atoi(passesText)) : 8;
        ps2x::sysstats::startProfilerForCurrentThread("vu1-bench");
        double totalNs = 0;
        uint64_t totalCycles = 0;
        for (int pass = 0; pass < passes; ++pass)
        {
            for (const Run &run : runs)
            {
                prepare(run);
                const auto start = std::chrono::steady_clock::now();
                const uint64_t c = execute(run);
                const double ns =
                    std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
                Timing &t = timing[run.program ? run.program->name : "(interpreter only)"];
                t.ns += ns;
                t.cycles += c;
                ++t.runs;
                totalNs += ns;
                totalCycles += c;
            }
        }
        for (const auto &[name, t] : timing)
            std::fprintf(stderr, "[bench]   %-28s runs=%zu avg %.0f cycles  %.2f ns/cycle  (%.0f ns/run)\n", name.c_str(),
                         t.runs, static_cast<double>(t.cycles) / t.runs, t.ns / t.cycles, t.ns / t.runs);
        std::fprintf(stderr, "[bench] ALL: %.2f ns/cycle  = %.1f million cycles/s on one core (60 fps needs ~380)\n",
                     totalNs / totalCycles, 1000.0 / (totalNs / totalCycles));
        ps2x::vu1c::printExactStats();
        std::fflush(stderr);
        return 0;
    }
}
