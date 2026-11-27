#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "core/iop_cpu_execute.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <utility>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
// (only these two: windows.h would bring in macros that clash with names here)
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void *thread, int priority);
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread();
#endif

namespace ps2x::iop::detail
{
    // Host nanoseconds spent running the IOP (timed runs only: when the runtime has set deferral).
    static std::atomic<uint64_t> g_iopRunNs{0};
    // LOCAL PATCH (GitHub #28): with the IOP on its own thread, the host time the game's thread still loses to it (waiting
    // for the worker, running its host calls, small inline runs); that is what iopRunNanoseconds() reports then.
    static std::atomic<uint64_t> g_iopGameNs{0};
    static std::atomic<bool> g_iopThreaded{false};
    static thread_local bool t_iopWorker = false;
    static uint64_t iopNowNs() noexcept
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            for (uint32_t &base : nativeBase)
                base = 0u;
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            nextDmaInterruptCycle = UINT64_MAX;
            pendingGuestCallbacks.clear();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            memory.setCycleCounter(&totalCycles);
            totalInstructions = 0;
            eeCycleCarry = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            // LOCAL PATCH: interrupts raised by PCSX2's SPU2 core (DMA 4/7 done, SPU2 IRQ) are due now.
            if (memory.spu2().hasPendingIrqs())
            {
                for (const int irq : memory.spu2().takePendingIrqs())
                    pendingDmaInterrupts[irq] = totalCycles;
                updateNextDmaInterruptCycle();
            }
            if (!memory.hasDmaStart())
                return;
            if (const auto dma = memory.takeDmaStart())
            {
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
                updateNextDmaInterruptCycle();
            }
        }

        void updateNextDmaInterruptCycle()
        {
            nextDmaInterruptCycle = UINT64_MAX;
            for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                nextDmaInterruptCycle = std::min(nextDmaInterruptCycle, completionCycle);
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thmsgbx"))
            {
                return kernel.dispatchMailboxImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        // Native versions of hot routines in IOP libraries (HLE). A routine is replaced only when
        // a module's code matches it word for word (jump targets aside: the loader relocates
        // them). Its first word then becomes an opcode-31 marker, which the R3000 does not
        // decode; step() runs the native version, counts the cycles the original takes so the
        // IOP's clock stays where it was, and returns to the caller.
        struct NativeRoutine
        {
            const char *module;         // file name prefix, e.g. "CRI_ADXI"
            uint32_t offset;            // from the module's base
            std::vector<uint32_t> code; // the routine as linked
            uint32_t cycles;            // what the original takes
            bool (*run)(Impl &, CpuState &); // false: not handled (the original word is put back and interpreted)
            uint32_t resume = 0u;       // 0: a whole routine (back to ra); else a block, continued at base + resume
        };
        static constexpr uint32_t kNativeMarker = 0x7C000000u;
        static constexpr size_t kMaxNativeRoutines = 8u;
        uint32_t nativeBase[kMaxNativeRoutines] = {}; // where each routine was installed (0: not)
        uint32_t nativeExtraCycles = 0u;              // set by a routine whose cost depends on its data

        // CRI ADX sound driver (CRI_ADXI.IRX): the mixer's last step turns 256 accumulated
        // 32-bit samples at a0 into 16-bit ones in place, saturating to +-32767.
        static bool adxMixClampToS16(Impl &impl, CpuState &cpu)
        {
            uint32_t source = cpu.gpr[4];
            uint32_t destination = source;
            for (uint32_t i = 0; i < 256u; ++i, source += 4u, destination += 2u)
            {
                const int32_t value = static_cast<int32_t>(impl.memory.read32(source));
                const int32_t clamped = value > 32767 ? 32767 : value < -32767 ? -32767 : value;
                impl.memory.write16(destination, static_cast<uint16_t>(clamped));
            }
            cpu.gpr[2] = 0u;
            return true;
        }

        // LOCAL PATCH (performance): the same clamp compiled a second time, unoptimised, at CRI_ADXI+0x5A64 (it is what
        // the IOP spent ~60% of its time on in a stage). Same in-place clamp; it returns with v0 = 256 (its loop counter).
        static bool adxMixClampToS16Unopt(Impl &impl, CpuState &cpu)
        {
            const uint32_t start = cpu.gpr[4];
            adxMixClampToS16(impl, cpu);
            cpu.gpr[2] = 0x100u;
            cpu.gpr[3] = start + 0x400u; // v1, a0: where the loop's pointers end
            cpu.gpr[4] = start + 0x200u;
            return true;
        }

        // LOCAL PATCH (performance, GitHub issue #28: on an FX-8350 the sound processor took 378 ms of every second, 5-10
        // ms of each frame on the game's thread; its time was spread evenly over these few routines). Sega's sound driver
        // (SKDRV.IRX), once a tick, asks the sequencer (MODSESQ.IRX export 11) about each of 32 slots of every track and
        // keeps the answers as bit masks: ~70 interpreted instructions per slot. Done natively, with the same results and
        // the same IOP cycle count.
        //
        // MODSESQ+0x2710 (a0 = sequencer, a1 = track): the track's slot block, or 0 when there is none.
        static uint32_t seqTrackSlots(Impl &impl, uint32_t sequencer, uint32_t track, uint32_t &cycles)
        {
            if (sequencer == 0u)
            {
                cycles += 4u;
                return 0u;
            }
            const uint32_t table = impl.memory.read32(sequencer + 4u);
            const uint32_t index = track * 2u + 1u;
            if (!(index < impl.memory.read32(table)))
            {
                cycles += 11u;
                return 0u;
            }
            cycles += 18u;
            return impl.memory.read32(index * 8u + impl.memory.read32(table + 4u) + 4u) + 0x50u;
        }

        // MODSESQ exports 11 / 12 (a0 = sequencer, a1 = track, a2 = slot): bit 1 / bit 2 of the slot's flags if the
        // slot is in use, else -1.
        static uint32_t seqSlotFlag(Impl &impl, uint32_t sequencer, uint32_t track, uint32_t slot, uint32_t shift,
                                    uint32_t &cycles)
        {
            cycles += 7u + 2u; // prologue + call, test after it
            const uint32_t slots = seqTrackSlots(impl, sequencer, track, cycles);
            if (slots == 0u)
            {
                cycles += 5u;
                return 0xFFFFFFFFu;
            }
            cycles += 7u;
            if ((impl.memory.read32(slots + 0x14u) & (1u << (slot & 31u))) == 0u)
            {
                cycles += 5u;
                return 0xFFFFFFFFu;
            }
            cycles += 14u;
            return (impl.memory.read32(slots + slot * 152u + 0x88u) >> shift) & 1u;
        }

        static bool seqSlotFlagRoutine(Impl &impl, CpuState &cpu, uint32_t shift)
        {
            uint32_t cycles = 0u;
            cpu.gpr[2] = seqSlotFlag(impl, cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], shift, cycles);
            impl.nativeExtraCycles = cycles;
            return true;
        }
        static bool seqSlotFlag1(Impl &impl, CpuState &cpu) { return seqSlotFlagRoutine(impl, cpu, 1u); }
        static bool seqSlotFlag2(Impl &impl, CpuState &cpu) { return seqSlotFlagRoutine(impl, cpu, 2u); }

        // SKDRV+0x37f0..0x38a4 (s0 = driver): for each track (count at +0x7e4) and slot 0..31, the slot's bit in the
        // track's mask (table at +0x7e0) set when export 11 says 1, cleared otherwise. Only while that import really is
        // the MODSESQ code matched above (else the block is put back and interpreted).
        static constexpr size_t kSeqSlotFlag1Id = 2u;
        static bool skdrvSlotMasks(Impl &impl, CpuState &cpu)
        {
            const uint32_t target = impl.imports.resolve("modsesq", 11u);
            if (target == 0u || impl.nativeBase[kSeqSlotFlag1Id] == 0u ||
                IopMemory::physicalAddress(target) != IopMemory::physicalAddress(impl.nativeBase[kSeqSlotFlag1Id] + 0x2614u))
                return false;
            const uint32_t driver = cpu.gpr[16];
            const uint32_t sequencer = driver + 0x748u;
            uint32_t cycles = 3u + 5u;
            uint32_t track = 0u;
            while (static_cast<int32_t>(track) < static_cast<int32_t>(impl.memory.read32(driver + 0x7E4u)))
            {
                cycles += 6u + 6u; // the track loop's own instructions: into the slot loop, and after it
                for (uint32_t slot = 0u; slot < 32u; ++slot)
                {
                    const uint32_t bit = 1u << slot;
                    cycles += 7u + 1u + 3u + 4u; // call, import stub, test, loop
                    const uint32_t flag = seqSlotFlag(impl, sequencer, track, slot, 1u, cycles);
                    const uint32_t entry = impl.memory.read32(driver + 0x7E0u) + track * 4u;
                    const uint32_t mask = impl.memory.read32(entry);
                    impl.memory.write32(entry, flag == 1u ? (mask | bit) : (mask & ~bit));
                    cycles += flag == 1u ? 10u : 9u;
                }
                cpu.gpr[18] = 32u;          // s2
                cpu.gpr[19] = 1u << 31;     // s3
                cpu.gpr[5] = track * 2u + 1u;   // a1..a3 as the last call left them (its helper's index, its locals)
                cpu.gpr[6] = cpu.gpr[29] - 0x10u;
                cpu.gpr[7] = cpu.gpr[29] - 0x0Cu;
                cpu.gpr[31] = cpu.pc + (0x3824u - 0x37F0u); // ra: after the call (cpu.pc is still this block's start)
                ++track;
            }
            cpu.gpr[17] = track; // s1
            cpu.gpr[2] = 0u;     // v0: the last loop test
            impl.nativeExtraCycles = cycles;
            return true;
        }

        static const std::vector<NativeRoutine> &nativeRoutines()
        {
            static const std::vector<NativeRoutine> s_routines = {
                {"CRI_ADXI", 0x36F4u,
                 {0x00801821u, 0x00003021u, 0x24077FFFu, 0x24088001u, 0x8C850000u, 0x00000000u, 0x00E5102Au,
                  0x14400008u, 0x28A28001u, 0x14400008u, 0x00000000u, 0x94820000u, 0x24840004u, 0xA4620000u,
                  0x08000DD2u, 0x24630002u, 0x08000DD0u, 0xA4670000u, 0xA4680000u, 0x24630002u, 0x24840004u,
                  0x24C60001u, 0x2CC20100u, 0x1440FFECu, 0x00000000u, 0x03E00008u, 0x00000000u},
                 256u * 15u + 6u, &Impl::adxMixClampToS16},
                {"CRI_ADXI", 0x5A64u,
                 {0x27BDFFF8u, 0xAFBE0000u, 0x03A0F021u, 0xAFC40008u, 0x8FC30008u, 0x8FC40008u, 0x00001021u, 0x2C450100u,
                  0x14A00003u, 0x00000000u, 0x080016CEu, 0x00000000u, 0x8C650000u, 0x24067FFFu, 0x00C5282Au, 0x10A00005u,
                  0x00000000u, 0x080016BFu, 0x00000000u, 0x080016BEu, 0x00000000u, 0x8C650000u, 0x00000000u, 0x28A68001u,
                  0x10C00005u, 0x00000000u, 0x080016C5u, 0x00000000u, 0x080016BEu, 0x00000000u, 0x94650000u, 0x00000000u,
                  0xA4850000u, 0x24630004u, 0x24840002u, 0x080016CBu, 0x00000000u, 0x00000000u, 0x24057FFFu, 0xA4850000u,
                  0x24840002u, 0x24630004u, 0x080016CBu, 0x00000000u, 0x24058001u, 0xA4850000u, 0x24840002u, 0x24630004u,
                  0x080016CBu, 0x00000000u, 0x24420001u, 0x080016A0u, 0x00000000u, 0x03C0E821u, 0x8FBE0000u, 0x27BD0008u,
                  0x03E00008u, 0x00000000u},
                 256u * 23u + 14u, &Impl::adxMixClampToS16Unopt},
                // (kSeqSlotFlag1Id = 2) MODSESQ export 11, with its helper at +0x2710 checked too (see below)
                {"MODSESQ", 0x2614u,
                 {0x27BDFFE0u, 0xAFB00018u, 0x00C08021u, 0x27A60010u, 0xAFBF001Cu, 0x0C0009C4u, 0x27A70014u, 0x10400012u,
                  0x24020001u, 0x8FA40014u, 0x00000000u, 0x8C830014u, 0x02021004u, 0x00621824u, 0x1060000Bu, 0x00101080u,
                  0x00501021u, 0x00021080u, 0x00501023u, 0x000210C0u, 0x00821021u, 0x8C420088u, 0x00000000u, 0x00021042u,
                  0x080009A0u, 0x30420001u, 0x2402FFFFu, 0x8FBF001Cu, 0x8FB00018u, 0x03E00008u, 0x27BD0020u},
                 0u, &Impl::seqSlotFlag1},
                // MODSESQ export 12 (bit 2)
                {"MODSESQ", 0x2690u,
                 {0x27BDFFE0u, 0xAFB00018u, 0x00C08021u, 0x27A60010u, 0xAFBF001Cu, 0x0C0009C4u, 0x27A70014u, 0x10400012u,
                  0x24020001u, 0x8FA40014u, 0x00000000u, 0x8C830014u, 0x02021004u, 0x00621824u, 0x1060000Bu, 0x00101080u,
                  0x00501021u, 0x00021080u, 0x00501023u, 0x000210C0u, 0x00821021u, 0x8C420088u, 0x00000000u, 0x00021082u,
                  0x080009BFu, 0x30420001u, 0x2402FFFFu, 0x8FBF001Cu, 0x8FB00018u, 0x03E00008u, 0x27BD0020u},
                 0u, &Impl::seqSlotFlag2},
                // SKDRV's per-tick slot masks: a block inside its update routine, continued at +0x38a4
                {"SKDRV", 0x37F0u,
                 {0x00008821u, 0x10000026u, 0x00000000u, 0x00009021u, 0x1000001Fu, 0x00000000u, 0x24020001u, 0x02429804u,
                  0x26040748u, 0x02202821u, 0x02403021u, 0x0C000094u, 0x00000000u, 0x24030001u, 0x1443000Bu, 0x00000000u,
                  0x00111880u, 0x8E0207E0u, 0x00000000u, 0x00431821u, 0x8C620000u, 0x00000000u, 0x00531025u, 0xAC620000u,
                  0x1000000Au, 0x00000000u, 0x00111880u, 0x8E0207E0u, 0x00000000u, 0x00432021u, 0x02601827u, 0x8C820000u,
                  0x00000000u, 0x00431024u, 0xAC820000u, 0x26520001u, 0x2A420020u, 0x1440FFE0u, 0x00000000u, 0x26310001u,
                  0x8E0207E4u, 0x00000000u, 0x0222102Au, 0x1440FFD7u, 0x00000000u},
                 0u, &Impl::skdrvSlotMasks, 0x38A4u},
            };
            return s_routines;
        }

        // MODSESQ+0x2710, the helper both exports call (the natives above do its work too)
        static const std::vector<uint32_t> &seqTrackSlotsCode()
        {
            static const std::vector<uint32_t> s_code = {
                0x10800011u, 0x00051840u, 0x8C840004u, 0x00000000u, 0x8C820000u, 0x24650001u, 0x00A2102Bu, 0x1040000Au,
                0x000518C0u, 0x8C820004u, 0x00000000u, 0x00621821u, 0x8C630004u, 0x24020001u, 0xACC30000u, 0x24630050u,
                0x03E00008u, 0xACE30000u, 0x03E00008u, 0x00001021u};
            return s_code;
        }

        bool codeMatches(uint32_t address, const std::vector<uint32_t> &code) const
        {
            for (size_t i = 0; i < code.size(); ++i)
            {
                const uint32_t expected = code[i];
                const uint32_t actual = memory.read32(address + static_cast<uint32_t>(i) * 4u);
                const bool jump = (expected >> 26) == 2u || (expected >> 26) == 3u;
                if (jump ? (actual >> 26) != (expected >> 26) : actual != expected)
                    return false;
            }
            return true;
        }

        // PS2X_IOP_NATIVE=0 keeps every routine interpreted.
        void installNativeRoutines(const Module &module)
        {
            static const bool s_enabled = []()
            {
                const char *value = std::getenv("PS2X_IOP_NATIVE");
                return !(value && value[0] == '0');
            }();
            if (!s_enabled)
                return;
            const std::vector<NativeRoutine> &routines = nativeRoutines();
            for (size_t id = 0; id < routines.size(); ++id)
            {
                const NativeRoutine &routine = routines[id];
                if (module.name.rfind(routine.module, 0) != 0 ||
                    routine.offset + routine.code.size() * 4u > module.size)
                    continue;
                const uint32_t address = module.base + routine.offset;
                if (id >= kMaxNativeRoutines || !codeMatches(address, routine.code))
                    continue;
                // the sequencer's exports read their slots through the helper at +0x2710: it must match too
                if (routine.module[0] == 'M' && (0x2710u + seqTrackSlotsCode().size() * 4u > module.size ||
                                                 !codeMatches(module.base + 0x2710u, seqTrackSlotsCode())))
                    continue;
                memory.write32(address, kNativeMarker | static_cast<uint32_t>(id));
                nativeBase[id] = module.base;
                std::ostringstream out;
                out << "[IOP] native " << routine.module << "+0x" << std::hex << routine.offset;
                log(LogLevel::Info, out.str());
            }
        }

        PS2X_IOP_INLINE bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            const uint32_t word = memory.read32(cpu.pc);
            if ((word & 0xFC000000u) == kNativeMarker)
            {
                const uint32_t id = word & 0xFFFFu;
                if (id < nativeRoutines().size())
                {
                    const NativeRoutine &routine = nativeRoutines()[id];
                    nativeExtraCycles = 0u;
                    if (!routine.run(*this, cpu))
                    {
                        // not this time (e.g. its import now goes elsewhere): the original code from now on
                        memory.write32(cpu.pc, routine.code[0]);
                        return true;
                    }
                    const uint32_t cycles = routine.cycles + nativeExtraCycles;
                    totalInstructions += cycles;
                    totalCycles += cycles;
                    cpu.pc = routine.resume ? cpu.pc - routine.offset + routine.resume : cpu.gpr[31];
                    cpu.branchPending = false;
                    return !cpu.stopped;
                }
            }

            // Import stubs start with jr ra (decode checks it too); skip the call otherwise.
            if (const auto import = word == 0x03E00008u ? imports.decode(cpu.pc) : std::optional<IopImportCall>{})
            {
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstructionInline(cpu);
            if (memory.hasDmaStart() || memory.spu2().hasPendingIrqs())
                schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        // Diagnostics: PS2X_IOP_PC_STATS samples the IOP program counter every 1024
        // instructions and, about every five seconds, prints the busiest addresses with
        // the module holding them and the instruction rate.
        // Also switched on for a few seconds by the hot monitor (see timedRun) when the IOP costs far more than usual.
        bool hotSampling = false;
        bool hotSamplingRestart = false;
        bool pcStatsEnabled() const
        {
            static const bool s_enabled = std::getenv("PS2X_IOP_PC_STATS") != nullptr;
            return s_enabled || hotSampling;
        }

        void samplePc(uint32_t pc)
        {
            static std::unordered_map<uint32_t, uint64_t> s_samples;
            static auto s_last = std::chrono::steady_clock::now();
            static uint64_t s_lastInstructions = 0;
            if (hotSamplingRestart)
            {
                hotSamplingRestart = false;
                s_samples.clear();
                s_last = std::chrono::steady_clock::now();
                s_lastInstructions = totalInstructions;
            }
            ++s_samples[pc];
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - s_last).count();
            if (seconds < 5.0)
                return;
            std::vector<std::pair<uint64_t, uint32_t>> ranked;
            uint64_t total = 0;
            for (const auto &[address, count] : s_samples)
            {
                ranked.push_back({count, address});
                total += count;
            }
            std::sort(ranked.rbegin(), ranked.rend());
            std::fprintf(stderr, "[iop-pc] %.1f M instructions/s, %llu samples\n",
                         static_cast<double>(totalInstructions - s_lastInstructions) / seconds / 1.0e6,
                         static_cast<unsigned long long>(total));
            // LOCAL PATCH (GitHub #28): first by 256-byte stretch of each module (roughly one routine or loop): code whose
            // time is spread over many addresses never reaches the per-address list below.
            {
                std::map<std::pair<std::string, uint32_t>, uint64_t> regions;
                for (const auto &[count, address] : ranked)
                {
                    const uint32_t phys = address & 0x1FFFFFFFu;
                    std::pair<std::string, uint32_t> key{"?", phys & ~0xFFu};
                    for (const auto &[id, module] : modules)
                    {
                        if (phys >= module.base && phys < module.base + module.size)
                        {
                            key = {module.name, (phys - module.base) & ~0xFFu};
                            break;
                        }
                    }
                    regions[key] += count;
                }
                std::vector<std::pair<uint64_t, std::pair<std::string, uint32_t>>> byRegion;
                for (const auto &[key, count] : regions)
                    byRegion.push_back({count, key});
                std::sort(byRegion.begin(), byRegion.end(),
                          [](const auto &a, const auto &b) { return a.first > b.first; });
                for (size_t i = 0; i < byRegion.size() && i < 12u; ++i)
                    std::fprintf(stderr, "[iop-region] %5.1f%%  %s+%x..%x\n",
                                 100.0 * static_cast<double>(byRegion[i].first) / static_cast<double>(total),
                                 byRegion[i].second.first.c_str(), byRegion[i].second.second,
                                 byRegion[i].second.second + 0xFFu);
            }
            for (size_t i = 0; i < ranked.size() && i < 16u; ++i)
            {
                const uint32_t address = ranked[i].second;
                const char *name = "?";
                uint32_t offset = address;
                for (const auto &[id, module] : modules)
                {
                    const uint32_t phys = address & 0x1FFFFFFFu;
                    if (phys >= module.base && phys < module.base + module.size)
                    {
                        name = module.name.c_str();
                        offset = phys - module.base;
                        break;
                    }
                }
                std::fprintf(stderr, "[iop-pc]   %5.1f%%  %08x  %s+%x\n",
                             100.0 * static_cast<double>(ranked[i].first) / static_cast<double>(total), address, name,
                             offset);
            }
            s_samples.clear();
            s_last = now;
            s_lastInstructions = totalInstructions;
            hotSampling = false; // one report per trigger; the monitor rearms after its cool-down
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            const bool samplePcs = pcStatsEnabled();
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (samplePcs && (totalInstructions & 1023u) == 0u)
                    samplePc(cpu.pc);
                if (!step(cpu))
                    break;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                {
                    // Not due yet: only the critical-section bookkeeping that
                    // servicePendingDmaInterrupts() would do before returning.
                    if (totalCycles >= nextDmaInterruptCycle)
                        servicePendingDmaInterrupts();
                    else
                        (void)inCriticalSection();
                }
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            runCpu(cpu, budget);
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // A thread that suspends interrupts (CpuSuspendIntr) is neither
        // preempted nor interrupted on hardware until it resumes them; IOP
        // modules rely on that to update shared state atomically. A module that
        // leaves interrupts off for too long is ignored so the rest of the IOP
        // keeps running.
        static constexpr uint64_t kMaxCriticalSectionCycles = 1'000'000u;
        uint64_t criticalSectionStart = 0u;
        bool criticalSectionOpen = false;
        bool inCriticalSection()
        {
            if (memory.interruptControl() != 0u)
            {
                criticalSectionOpen = false;
                return false;
            }
            if (!criticalSectionOpen)
            {
                criticalSectionOpen = true;
                criticalSectionStart = totalCycles;
            }
            return totalCycles - criticalSectionStart < kMaxCriticalSectionCycles;
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty() || inCriticalSection())
                return;
            if (totalCycles < nextDmaInterruptCycle)
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            updateNextDmaInterruptCycle();
            try
            {
                for (const int irq : completed)
                    (void)intrman.dispatchInterrupt(irq, *this);
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty() || inCriticalSection())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    // LOCAL PATCH: PCSX2's SPU2 core asks to be woken at a cycle (its IOP counter 6): a DMA completion or
                    // its regular tick. Its interrupts then go out like the other DMA interrupts.
                    memory.spu2().serviceEvents(totalCycles);
                    if (memory.spu2().hasPendingIrqs())
                        schedulePendingDma();
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    if (!inCriticalSection())
                        timrman.serviceDue(totalCycles, *this);
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        nextWake = std::min(nextWake, memory.spu2().nextEventCycle(totalCycles));
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    const uint64_t spuEvent = memory.spu2().nextEventCycle(totalCycles);
                    const uint64_t sliceEnd = std::min(target, std::max(spuEvent, totalCycles + 1u));
                    runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, sliceEnd - totalCycles)));
                    // The slice may not end inside a critical section.
                    while (inCriticalSection() && !next->cpu.stopped && !next->cpu.yielded &&
                           next->state == IopThreadState::Running)
                        runCpu(next->cpu, kDefaultSlice);
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                }
                memory.spu2().advanceTo(totalCycles);
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;
            installNativeRoutines(module);

            // IRX entry: start(int argc, char *argv[]). Like loadcore, argv[0] is
            // the module path and the NUL-separated load arguments follow.
            std::vector<std::string> argvStrings{module.path};
            if (arguments && argumentSize)
            {
                const auto *bytes = static_cast<const char *>(arguments);
                uint32_t begin = 0u;
                for (uint32_t i = 0; i < argumentSize; ++i)
                {
                    if (bytes[i] == 0)
                    {
                        argvStrings.emplace_back(bytes + begin, i - begin);
                        begin = i + 1u;
                    }
                }
                if (begin < argumentSize)
                    argvStrings.emplace_back(bytes + begin, argumentSize - begin);
            }
            uint32_t stringBytes = 0u;
            for (const std::string &text : argvStrings)
                stringBytes += static_cast<uint32_t>(text.size()) + 1u;
            const uint32_t pointerBytes = static_cast<uint32_t>((argvStrings.size() + 1u) * 4u);
            const uint32_t argvBlock = allocate(pointerBytes + stringBytes, 16u);
            uint32_t argv = 0u;
            if (argvBlock)
            {
                argv = argvBlock;
                uint32_t cursor = argvBlock + pointerBytes;
                for (size_t i = 0; i < argvStrings.size(); ++i)
                {
                    const std::string &text = argvStrings[i];
                    const uint32_t pointer = cursor;
                    writeRam(argv + static_cast<uint32_t>(i) * 4u, &pointer, 4u);
                    writeRam(cursor, text.data(), text.size());
                    write8(cursor + static_cast<uint32_t>(text.size()), 0u);
                    cursor += static_cast<uint32_t>(text.size()) + 1u;
                }
                const uint32_t terminator = 0u;
                writeRam(argv + static_cast<uint32_t>(argvStrings.size()) * 4u, &terminator, 4u);
            }
            const uint32_t argc = argvBlock ? static_cast<uint32_t>(argvStrings.size()) : 0u;
            const uint32_t startResult = callFunction(module.entry, argc, argv, 0u, 0u, module.gp);
            if (argvBlock)
                freeAllocation(argvBlock);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        // Earliest completion cycle in pendingDmaInterrupts (UINT64_MAX when empty).
        uint64_t nextDmaInterruptCycle = UINT64_MAX;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;

        // LOCAL PATCH (frame pacing): the runtime may ask for bigger batches and a time budget (setDeferral). The IOP then
        // runs in batches of deferMinBatch cycles; once it has used deferBudgetNs of host time since the game last idled,
        // further cycles wait (up to deferMaxCycles) for the next idle point, an EE<->IOP interaction (every entry point
        // flushes first, exactly as before) or the cap. Emulated time and the order of events are unchanged: only where
        // in the host frame the IOP does its work. Off (0) by default, which is the old behaviour.
        uint64_t deferMinBatch = 0;
        uint64_t deferBudgetNs = 0;
        uint64_t deferMaxCycles = 0;
        uint64_t workNs = 0; // host time used by the IOP since the last idle point

        // Hot monitor: host time per wall second; three seconds in a row above the limit switch PC sampling on once.
        std::chrono::steady_clock::time_point hotWindowStart{};
        std::chrono::steady_clock::time_point hotCooldownUntil{};
        uint64_t hotWindowNs = 0;
        uint32_t hotSeconds = 0;

        void timedRun(uint64_t iopCycles) noexcept
        {
            const auto start = std::chrono::steady_clock::now();
            runCycles(iopCycles);
            const auto end = std::chrono::steady_clock::now();
            const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            workNs += ns;
            g_iopRunNs.fetch_add(ns, std::memory_order_relaxed);
            if (!t_iopWorker)
                g_iopGameNs.fetch_add(ns, std::memory_order_relaxed);
            hotWindowNs += ns;
            if (hotWindowStart == std::chrono::steady_clock::time_point{})
                hotWindowStart = end;
            const double window = std::chrono::duration<double>(end - hotWindowStart).count();
            if (window >= 1.0)
            {
                const double msPerSecond = static_cast<double>(hotWindowNs) / 1.0e6 / window;
                hotSeconds = msPerSecond > 330.0 ? hotSeconds + 1u : 0u;
                if (hotSeconds >= 3u && !hotSampling && end >= hotCooldownUntil)
                {
                    std::fprintf(stderr, "[iop-hot] the sound processor is using %.0f ms of every second (normal is about 200): "
                                         "sampling where for 5 seconds\n", msPerSecond);
                    hotSampling = true;
                    hotSamplingRestart = true;
                    hotCooldownUntil = end + std::chrono::seconds(120);
                    hotSeconds = 0u;
                }
                hotWindowStart = end;
                hotWindowNs = 0u;
            }
        }

        // The game's CPU has nothing to run: do the waiting IOP work now, in time that would be slept away.
        void idleFlush() noexcept
        {
            const uint64_t iopCycles = eeCycleCarry / 8u;
            if (iopCycles)
            {
                eeCycleCarry %= 8u;
                if (deferMinBatch)
                    timedRun(iopCycles);
                else
                    runCycles(iopCycles);
            }
            workNs = 0u;
        }

        // Run EE cycles that runEeCycles() batched, so anything that observes or
        // talks to the IOP sees it at the EE's current time.
        void flushPendingEeCycles() noexcept
        {
            const uint64_t iopCycles = eeCycleCarry / 8u;
            eeCycleCarry %= 8u;
            if (iopCycles)
            {
                if (deferMinBatch)
                    timedRun(iopCycles);
                else
                    runCycles(iopCycles);
            }
        }
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    // LOCAL PATCH (performance, GitHub #28: on an AMD FX-8350 the IOP took 5-10 ms of every frame on the game's thread,
    // which put fights at 30 fps). The IOP's batched cycles run on a thread of their own while the game goes on.
    //  - The game's thread hands the cycles over at its checkpoints (runEeCycles) instead of running them.
    //  - Every other way in (RPC, SIF transfers, IOP memory, module loads, save states, the game's idle point) first
    //    waits for the worker to finish (join), then works exactly as before: the IOP is at the EE's time there.
    //  - The worker is the Impl's host. Host calls that touch the game's side (SIF commands to the EE, memory card,
    //    sound commands, the EE heap, RPC handles, path lookups) are run by the game's thread, at its next checkpoint or
    //    while it waits in join; the worker waits for the result. Calls back into the IOP from inside such a call do not
    //    wait (the worker is stopped in the middle of its run, as the inline IOP was before). Guest memory copies, host
    //    file reads and logging run on the worker directly.
    // PS2X_IOP_THREAD=0 turns it off, =1 on; unset: on with 6 or more logical CPUs.
    class IopEmulator::Worker final : public IopHost
    {
    public:
        explicit Worker(IopHost &host) : m_host(host)
        {
            const char *value = std::getenv("PS2X_IOP_THREAD");
            if (value && value[0] != '\0')
                enabled = value[0] != '0';
            else
                enabled = std::thread::hardware_concurrency() >= 6u;
        }
        ~Worker() override { stop(); }

        bool enabled = false;

        void start(Impl *impl)
        {
            if (!enabled || m_thread.joinable())
                return;
            m_impl = impl;
            m_thread = std::thread([this]() { loop(); });
            m_id = m_thread.get_id();
        }

        void stop()
        {
            if (!m_thread.joinable())
                return;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_wake.notify_all();
            m_proxyCv.notify_all();
            m_thread.join();
        }

        bool onWorker() const { return m_thread.joinable() && std::this_thread::get_id() == m_id; }

        // Game's side: hand cycles over. Too many waiting (the worker is slower than the game): wait for it.
        void submit(uint64_t iopCycles)
        {
            constexpr uint64_t kMaxQueued = 1228800u; // two frames of IOP time
            std::unique_lock<std::mutex> lock(m_mutex);
            m_work += iopCycles;
            if (!m_busy)
            {
                m_busy = true;
                m_wake.notify_one();
            }
            if (m_work > kMaxQueued)
            {
                lock.unlock();
                join();
            }
        }

        // Game's side: a host call the worker is waiting on, if any.
        void servicePending() const
        {
            if (!m_callPosted.load(std::memory_order_acquire) || onWorker())
                return;
            const uint64_t start = iopNowNs();
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_call)
                runCall(lock);
            lock.unlock();
            g_iopGameNs.fetch_add(iopNowNs() - start, std::memory_order_relaxed);
        }

        // Game's side: wait until the worker has run everything handed to it (running its host calls meanwhile).
        void join() const
        {
            if (!m_thread.joinable() || onWorker() || m_inCall > 0)
                return;
            const uint64_t start = iopNowNs();
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_busy && !m_work)
                return;
            while (m_busy || m_work)
            {
                if (m_call)
                {
                    runCall(lock);
                    continue;
                }
                m_eeCv.wait(lock, [this]() { return (!m_busy && !m_work) || m_call != nullptr; });
            }
            lock.unlock();
            g_iopGameNs.fetch_add(iopNowNs() - start, std::memory_order_relaxed);
        }

        // ---- IopHost: memory copies, files and logs straight through; the rest on the game's thread
        bool readGuest(uint32_t address, void *destination, size_t size) const override { return m_host.readGuest(address, destination, size); }
        bool writeGuest(uint32_t address, const void *source, size_t size) override { return m_host.writeGuest(address, source, size); }
        bool zeroGuest(uint32_t address, size_t size) override { return m_host.zeroGuest(address, size); }
        bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override { return m_host.normalizeGuestAddress(address, normalized); }
        bool readIopMemory(uint32_t address, void *destination, size_t size) const override { return m_host.readIopMemory(address, destination, size); }
        bool writeIopMemory(uint32_t address, const void *source, size_t size) override { return m_host.writeIopMemory(address, source, size); }
        bool zeroIopMemory(uint32_t address, size_t size) override { return m_host.zeroIopMemory(address, size); }
        bool normalizeIopAddress(uint32_t address, uint32_t &normalized) const override { return m_host.normalizeIopAddress(address, normalized); }
        uint32_t allocateIopHandle(IopHandleKind kind) override { return proxied([&]() { return m_host.allocateIopHandle(kind); }); }
        uint32_t allocateGuest(uint32_t size, uint32_t alignment) override { return proxied([&]() { return m_host.allocateGuest(size, alignment); }); }
        void freeGuest(uint32_t address) override { proxied([&]() { m_host.freeGuest(address); }); }
        void audioCommand(uint32_t sid, uint32_t function, GuestBuffer send, GuestBuffer receive) override
        {
            proxied([&]() { m_host.audioCommand(sid, function, send, receive); });
        }
        std::string hostPath(HostPathKind kind) const override { return proxied([&]() { return m_host.hostPath(kind); }); }
        std::string translateGuestPath(std::string_view path) const override { return proxied([&]() { return m_host.translateGuestPath(path); }); }
        uint64_t openHostFile(std::string_view path) override { return m_host.openHostFile(path); }
        bool hostFileSize(uint64_t handle, uint64_t &size) const override { return m_host.hostFileSize(handle, size); }
        bool readHostFile(uint64_t handle, uint64_t offset, void *destination, size_t size, size_t &bytesRead) override
        {
            return m_host.readHostFile(handle, offset, destination, size, bytesRead);
        }
        void closeHostFile(uint64_t handle) override { m_host.closeHostFile(handle); }
        int32_t memoryCard(const MemoryCardRequest &request) override { return proxied([&]() { return m_host.memoryCard(request); }); }
        bool hasGuestFunction(uint32_t address) const override { return m_host.hasGuestFunction(address); }
        bool invokeGuestFunction(uint64_t callToken, uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t *resultAddress) override
        {
            return proxied([&]() { return m_host.invokeGuestFunction(callToken, address, a0, a1, a2, a3, resultAddress); });
        }
        bool sendSifCommand(uint32_t commandId, const void *packet, size_t packetSize) override
        {
            return proxied([&]() { return m_host.sendSifCommand(commandId, packet, packetSize); });
        }
        void log(LogLevel level, std::string_view message) override { m_host.log(level, message); }

    private:
        void loop();

        // The game's thread runs the posted call (lock held on entry and on return).
        void runCall(std::unique_lock<std::mutex> &lock) const
        {
            const std::function<void()> *call = m_call;
            m_call = nullptr;
            m_callPosted.store(false, std::memory_order_release);
            lock.unlock();
            ++m_inCall;
            (*call)();
            --m_inCall;
            lock.lock();
            m_callDone = true;
            m_proxyCv.notify_all();
        }

        template <class F>
        auto proxied(F &&f) const -> decltype(f())
        {
            using R = decltype(f());
            if (!onWorker())
                return f();
            if constexpr (std::is_void_v<R>)
            {
                const std::function<void()> call = [&]() { f(); };
                post(call);
            }
            else
            {
                R result{};
                const std::function<void()> call = [&]() { result = f(); };
                post(call);
                return result;
            }
        }

        void post(const std::function<void()> &call) const
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_stop)
            {
                lock.unlock();
                call(); // shutting down: nobody on the game's side any more
                return;
            }
            m_call = &call;
            m_callDone = false;
            m_callPosted.store(true, std::memory_order_release);
            m_eeCv.notify_all();
            m_proxyCv.wait(lock, [this]() { return m_callDone || m_stop; });
            if (!m_callDone)
            {
                m_call = nullptr;
                m_callPosted.store(false, std::memory_order_release);
                lock.unlock();
                call();
            }
        }

        IopHost &m_host;
        Impl *m_impl = nullptr;
        std::thread m_thread;
        std::thread::id m_id{};
        mutable std::mutex m_mutex;
        mutable std::condition_variable m_wake;    // worker: work or stop
        mutable std::condition_variable m_eeCv;    // game's side: worker idle, or a call posted
        mutable std::condition_variable m_proxyCv; // worker: its posted call done
        uint64_t m_work = 0;                       // IOP cycles handed over and not yet taken
        bool m_busy = false;                       // the worker has work (taken or not)
        bool m_stop = false;
        mutable const std::function<void()> *m_call = nullptr;
        mutable bool m_callDone = false;
        mutable std::atomic<bool> m_callPosted{false};
        mutable int m_inCall = 0;                  // game's thread inside a posted call (only it touches this)
    };

    void IopEmulator::Worker::loop()
    {
        t_iopWorker = true;
#if defined(_WIN32)
        // above normal, as the game's and the VIF1 threads are: the game waits for this one at its sync points
        SetThreadPriority(GetCurrentThread(), 1 /* THREAD_PRIORITY_ABOVE_NORMAL */);
#endif
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;)
        {
            m_wake.wait(lock, [this]() { return m_stop || m_work != 0u; });
            if (m_stop)
                break;
            const uint64_t cycles = m_work;
            m_work = 0u;
            lock.unlock();
            m_impl->timedRun(cycles);
            lock.lock();
            if (m_work == 0u)
            {
                m_busy = false;
                m_eeCv.notify_all();
            }
        }
        m_busy = false;
        m_eeCv.notify_all();
    }

    IopEmulator::IopEmulator(IopHost &host)
        : m_worker(std::make_unique<Worker>(host)),
          m_impl(std::make_unique<Impl>(*m_worker))
    {
        m_worker->start(m_impl.get());
        g_iopThreaded.store(m_worker->enabled, std::memory_order_relaxed);
        if (m_worker->enabled)
            std::fprintf(stderr, "[iop] sound processor on its own thread (PS2X_IOP_THREAD=0 to keep it on the game's)\n");
    }

    IopEmulator::~IopEmulator()
    {
        m_worker->join();
        m_worker->stop(); // before the Impl it runs goes
    }

    bool IopEmulator::threaded() const noexcept
    {
        return m_worker->enabled;
    }

    void IopEmulator::reset()
    {
        m_worker->join();
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        m_worker->join();
        m_impl->flushPendingEeCycles();
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        m_worker->join();
        m_impl->flushPendingEeCycles();
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        m_worker->join();
        m_impl->flushPendingEeCycles();
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        if (m_worker->enabled && !m_worker->onWorker())
        {
            m_worker->servicePending();
            constexpr uint64_t kThreadBatchCycles = 8192u; // ~0.2 ms of IOP time per hand-over
            const uint64_t total = m_impl->eeCycleCarry + eeCycles;
            if (total / 8u < kThreadBatchCycles)
            {
                m_impl->eeCycleCarry = total;
                return;
            }
            m_impl->eeCycleCarry = total % 8u;
            m_worker->submit(total / 8u);
            return;
        }
        // The EE scheduler reports progress every few dozen cycles (each guest
        // loop checkpoint). Running the IOP scheduler for a handful of cycles per
        // call costs far more than the emulated work, so batch small advances.
        constexpr uint64_t kMinIopBatchCycles = 128u;
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        const uint64_t minBatch = m_impl->deferMinBatch ? m_impl->deferMinBatch : kMinIopBatchCycles;
        if (total / 8u < minBatch)
        {
            m_impl->eeCycleCarry = total;
            return;
        }
        if (m_impl->deferMinBatch)
        {
            // Over this stretch's time budget: leave the work for the idle point, unless too much is waiting already.
            if (m_impl->workNs >= m_impl->deferBudgetNs && total / 8u < m_impl->deferMaxCycles)
            {
                m_impl->eeCycleCarry = total;
                return;
            }
            m_impl->eeCycleCarry = total % 8u;
            m_impl->timedRun(total / 8u);
            return;
        }
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    void IopEmulator::setDeferral(uint64_t minBatchCycles, uint64_t budgetNanoseconds, uint64_t maxDeferredCycles) noexcept
    {
        m_impl->deferMinBatch = minBatchCycles;
        m_impl->deferBudgetNs = budgetNanoseconds;
        m_impl->deferMaxCycles = std::max<uint64_t>(maxDeferredCycles, minBatchCycles);
    }

    void IopEmulator::idleFlush() noexcept
    {
        if (m_worker->enabled && !m_worker->onWorker())
        {
            // the game has nothing to run: the IOP catches up to the EE's time before it sleeps, as inline
            const uint64_t iopCycles = m_impl->eeCycleCarry / 8u;
            if (iopCycles)
            {
                m_impl->eeCycleCarry %= 8u;
                m_worker->submit(iopCycles);
            }
            m_worker->join();
            m_impl->workNs = 0u;
            return;
        }
        m_impl->idleFlush();
    }

    uint64_t iopRunNanoseconds() noexcept
    {
        return g_iopThreaded.load(std::memory_order_relaxed) ? g_iopGameNs.load(std::memory_order_relaxed)
                                                             : g_iopRunNs.load(std::memory_order_relaxed);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        m_worker->join();
        m_impl->flushPendingEeCycles();
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        m_worker->join();
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_worker->join();
        m_impl->flushPendingEeCycles();
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        m_worker->join();
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        m_worker->join();
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        m_worker->join();
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        m_worker->join();
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        m_worker->join();
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    size_t IopEmulator::readAudio(int16_t *destination, size_t frames)
    {
        return m_impl->memory.spu2().readOutput(destination, frames);
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        // Include batched cycles; they run before the next IOP interaction.
        return m_impl->totalCycles + m_impl->eeCycleCarry / 8u;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}

// ---- LOCAL PATCH (save states, GitHub #16) ------------------------------------------------------------------------
// The IOP state that changes while the game plays: memory (RAM, scratch, hardware registers, allocations, interrupt and
// DMA state), the kernel's threads and objects, timers, interrupt handlers, the RPC bridge, the CD driver, pending DMA
// interrupts / guest callbacks, the clocks and the SPU2 (PCSX2's core). Loaded modules, imports and devices are set up
// at boot and are the same in every session, so they are not written.
#include "../../../ps2xRuntime/include/runtime/ps2_savestate.h"
#include "spu2pcsx2/spu2_pcsx2.h"

namespace ps2x::iop::detail
{
    struct IopSaveStateAccess
    {
        using Writer = ps2x::savestate::Writer;
        using Reader = ps2x::savestate::Reader;

        template <typename K, typename V, typename M>
        static void writeMap(Writer &out, const M &map)
        {
            out.u32(static_cast<uint32_t>(map.size()));
            for (const auto &[k, v] : map)
            {
                out.pod(static_cast<K>(k));
                out.pod(static_cast<const V &>(v));
            }
        }
        template <typename K, typename V, typename M>
        static void readMap(Reader &in, M &map)
        {
            map.clear();
            const uint32_t n = in.u32();
            for (uint32_t i = 0; i < n && !in.failed(); ++i)
            {
                const K k = in.pod<K>();
                const V v = in.pod<V>();
                map.emplace(k, v);
            }
        }

        static void save(const IopMemory &m, Writer &out)
        {
            out.section("iop-memory");
            out.bytes(m.m_ram);
            out.bytes(m.m_owned);
            out.bytes(m.m_scratch);
            writeMap<uint32_t, uint32_t>(out, m.m_hardware);
            out.podVector(m.m_allocations);
            out.u32(m.m_heapCursor);
            out.u32(m.m_interruptStatus);
            out.u32(m.m_interruptMask);
            out.u32(m.m_interruptControl);
            out.pod(m.m_dmaStart.has_value());
            out.pod(m.m_dmaStart.value_or(IopMemory::DmaStart{}));
            out.pod(m.m_spuAutoDma);
            const Spu2 &s = m.m_spu2;
            out.pod(s.m_pcsx2);
            out.pod(s.m_regs);
            out.u64(s.m_nextSampleCycle);
            out.pod(s.m_clockStarted);
            out.podVector(s.m_pendingIrqs);
            out.u64(s.m_pcsx2Samples);
            std::vector<uint8_t> core;
            if (s.m_pcsx2)
                spu2pcsx2::saveState(core);
            out.bytes(core);
        }

        static bool load(IopMemory &m, Reader &in, std::string &error)
        {
            if (!in.section("iop-memory"))
                return false;
            std::vector<uint8_t> ram = in.bytes(), owned = in.bytes(), scratch = in.bytes();
            if (ram.size() != m.m_ram.size() || owned.size() != m.m_owned.size() || scratch.size() != m.m_scratch.size())
            {
                error = "IOP memory size differs";
                return false;
            }
            // Copied into the live buffers: the SPU2 core, DMA hooks and the EE side hold pointers into them (and into
            // the hardware register map's entries, which are updated in place for the same reason).
            std::memcpy(m.m_ram.data(), ram.data(), ram.size());
            std::memcpy(m.m_owned.data(), owned.data(), owned.size());
            std::memcpy(m.m_scratch.data(), scratch.data(), scratch.size());
            {
                const uint32_t n = in.u32();
                for (uint32_t i = 0; i < n && !in.failed(); ++i)
                {
                    const uint32_t key = in.u32();
                    m.m_hardware[key] = in.u32();
                }
            }
            in.podVector(m.m_allocations);
            m.m_heapCursor = in.u32();
            m.m_interruptStatus = in.u32();
            m.m_interruptMask = in.u32();
            m.m_interruptControl = in.u32();
            const bool hasDma = in.pod<bool>();
            const IopMemory::DmaStart dma = in.pod<IopMemory::DmaStart>();
            if (hasDma)
                m.m_dmaStart = dma;
            else
                m.m_dmaStart.reset();
            in.pod(m.m_spuAutoDma);
            Spu2 &s = m.m_spu2;
            const bool pcsx2 = in.pod<bool>();
            in.pod(s.m_regs);
            s.m_nextSampleCycle = in.u64();
            in.pod(s.m_clockStarted);
            in.podVector(s.m_pendingIrqs);
            s.m_pcsx2Samples = in.u64();
            const std::vector<uint8_t> core = in.bytes();
            if (in.failed())
                return false;
            if (pcsx2 != s.m_pcsx2)
            {
                error = "the state was saved with the other sound chip setting";
                return false;
            }
            if (pcsx2 && !spu2pcsx2::loadState(core.data(), core.size()))
            {
                error = "sound chip state does not match this build";
                return false;
            }
            {
                // The host's output buffer starts empty again (it held sound from after the save).
                std::lock_guard<std::mutex> lock(s.m_outputMutex);
                s.m_outputRead = 0;
                s.m_outputCount = 0;
                s.m_playing = false;
                s.m_resamplePos = 0.0;
            }
            return true;
        }

        static void save(const IopKernel &k, Writer &out)
        {
            out.section("iop-kernel");
            writeMap<int, IopThread>(out, k.m_threads);
            out.u32(static_cast<uint32_t>(k.m_semaphores.size()));
            for (const auto &[id, s] : k.m_semaphores)
                out.pod(s);
            out.u32(static_cast<uint32_t>(k.m_eventFlags.size()));
            for (const auto &[id, f] : k.m_eventFlags)
                out.pod(f);
            out.u32(static_cast<uint32_t>(k.m_mailboxes.size()));
            for (const auto &[id, b] : k.m_mailboxes)
            {
                out.pod(b.id);
                out.u32(b.attr);
                out.u32(b.option);
                out.podDeque(b.messages);
            }
            out.u32(k.m_nextThreadId);
            out.u32(k.m_nextSemaphoreId);
            out.u32(k.m_nextEventFlagId);
            out.u32(k.m_nextMailboxId);
            out.pod(k.m_currentThread ? k.m_currentThread->id : 0);
        }

        static bool load(IopKernel &k, Reader &in)
        {
            if (!in.section("iop-kernel"))
                return false;
            readMap<int, IopThread>(in, k.m_threads);
            k.m_semaphores.clear();
            uint32_t n = in.u32();
            for (uint32_t i = 0; i < n && !in.failed(); ++i)
            {
                const auto s = in.pod<IopKernel::Semaphore>();
                k.m_semaphores[s.id] = s;
            }
            k.m_eventFlags.clear();
            n = in.u32();
            for (uint32_t i = 0; i < n && !in.failed(); ++i)
            {
                const auto f = in.pod<IopKernel::EventFlag>();
                k.m_eventFlags[f.id] = f;
            }
            k.m_mailboxes.clear();
            n = in.u32();
            for (uint32_t i = 0; i < n && !in.failed(); ++i)
            {
                IopKernel::Mailbox b;
                in.pod(b.id);
                b.attr = in.u32();
                b.option = in.u32();
                in.podDeque(b.messages);
                k.m_mailboxes[b.id] = std::move(b);
            }
            k.m_nextThreadId = in.u32();
            k.m_nextSemaphoreId = in.u32();
            k.m_nextEventFlagId = in.u32();
            k.m_nextMailboxId = in.u32();
            const int current = in.pod<int>();
            auto it = k.m_threads.find(current);
            k.m_currentThread = (current != 0 && it != k.m_threads.end()) ? &it->second : nullptr;
            return !in.failed();
        }

        static void save(const IopTimrman &t, Writer &out)
        {
            out.section("iop-timers");
            out.pod(t.m_timers);
            out.u32(t.m_holdMode);
        }
        static bool load(IopTimrman &t, Reader &in)
        {
            if (!in.section("iop-timers"))
                return false;
            in.pod(t.m_timers);
            t.m_holdMode = in.u32();
            t.m_servicing = false;
            return !in.failed();
        }

        static void save(const IopIntrman &t, Writer &out)
        {
            out.section("iop-intrman");
            writeMap<int, IopIntrman::Handler>(out, t.m_handlers);
            writeMap<int, bool>(out, t.m_enabled);
        }
        static bool load(IopIntrman &t, Reader &in)
        {
            if (!in.section("iop-intrman"))
                return false;
            readMap<int, IopIntrman::Handler>(in, t.m_handlers);
            readMap<int, bool>(in, t.m_enabled);
            return !in.failed();
        }

        static void save(const IopRpcBridge &r, Writer &out)
        {
            out.section("iop-rpc");
            writeMap<uint32_t, IopRpcBridge::RpcServer>(out, r.m_servers);
            out.u32(r.m_nextDmaId);
            out.pod(r.m_sifInitialized);
        }
        static bool load(IopRpcBridge &r, Reader &in)
        {
            if (!in.section("iop-rpc"))
                return false;
            readMap<uint32_t, IopRpcBridge::RpcServer>(in, r.m_servers);
            r.m_nextDmaId = in.u32();
            in.pod(r.m_sifInitialized);
            return !in.failed();
        }
    };

    bool IopEmulator::saveStateClean(std::string &why) const
    {
        m_worker->join();
        const Impl &d = *m_impl;
        if (d.activeCpu || d.callDepth != 0u || d.servicingDmaInterrupts || d.servicingGuestCallbacks)
        {
            why = "the sound processor is busy";
            return false;
        }
        return true;
    }

    void IopEmulator::saveState(std::vector<uint8_t> &bytes) const
    {
        m_worker->join();
        const Impl &d = *m_impl;
        ps2x::savestate::Writer out;
        out.section("iop");
        IopSaveStateAccess::save(d.memory, out);
        IopSaveStateAccess::save(d.kernel, out);
        IopSaveStateAccess::save(d.timrman, out);
        IopSaveStateAccess::save(d.intrman, out);
        IopSaveStateAccess::save(d.rpc, out);
        out.section("iop-cdvd");
        out.pod(d.cdvd.saveState());
        out.section("iop-core");
        IopSaveStateAccess::writeMap<int, uint64_t>(out, d.pendingDmaInterrupts);
        out.u64(d.nextDmaInterruptCycle);
        out.u32(static_cast<uint32_t>(d.pendingGuestCallbacks.size()));
        for (const auto &[cycle, cb] : d.pendingGuestCallbacks)
        {
            out.u64(cycle);
            out.pod(cb);
        }
        out.u32(d.nextModuleId);
        out.u32(d.moduleCursor);
        out.u64(d.totalCycles);
        out.u64(d.totalInstructions);
        out.u64(d.eeCycleCarry);
        out.pod(d.secrMcCommandHandler);
        out.pod(d.secrMcDevIdHandler);
        out.pod(d.checkKelfPathCallback);
        bytes = out.data();
    }

    bool IopEmulator::loadState(const uint8_t *data, size_t size, std::string &error)
    {
        m_worker->join();
        Impl &d = *m_impl;
        ps2x::savestate::Reader in(data, size);
        if (!in.section("iop"))
        {
            error = in.error();
            return false;
        }
        bool ok = IopSaveStateAccess::load(d.memory, in, error) && IopSaveStateAccess::load(d.kernel, in) &&
                  IopSaveStateAccess::load(d.timrman, in) && IopSaveStateAccess::load(d.intrman, in) &&
                  IopSaveStateAccess::load(d.rpc, in) && in.section("iop-cdvd");
        if (ok)
        {
            d.cdvd.loadState(in.pod<IopCdvd::SavedState>());
            ok = in.section("iop-core");
        }
        if (ok)
        {
            IopSaveStateAccess::readMap<int, uint64_t>(in, d.pendingDmaInterrupts);
            d.nextDmaInterruptCycle = in.u64();
            d.pendingGuestCallbacks.clear();
            const uint32_t n = in.u32();
            for (uint32_t i = 0; i < n && !in.failed(); ++i)
            {
                const uint64_t cycle = in.u64();
                d.pendingGuestCallbacks.emplace(cycle, in.pod<Impl::ScheduledGuestCallback>());
            }
            d.nextModuleId = in.u32();
            d.moduleCursor = in.u32();
            d.totalCycles = in.u64();
            d.totalInstructions = in.u64();
            d.eeCycleCarry = in.u64();
            in.pod(d.secrMcCommandHandler);
            in.pod(d.secrMcDevIdHandler);
            in.pod(d.checkKelfPathCallback);
            d.workNs = 0u;
            ok = !in.failed();
        }
        if (!ok && error.empty())
            error = in.error().empty() ? "IOP state could not be read" : in.error();
        return ok;
    }
}
