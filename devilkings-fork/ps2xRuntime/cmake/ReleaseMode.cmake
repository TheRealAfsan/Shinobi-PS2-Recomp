include(CheckIPOSupported)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

# LOCAL CHANGE: whole-program optimization (/GL + /LTCG, IPO) defers all code generation to a
# single-threaded link, which is impractically slow for ~57k generated functions. Off by default.
# LOCAL PATCH: PS2X_BASELINE_CPU=ON builds for any x64 CPU with SSE4.1 (2008+, the same minimum as PCSX2) instead of
# AVX2: the fallback game program the launcher starts on CPUs without AVX2 (bin/Shinobi-SSE4.exe).
option(PS2X_BASELINE_CPU "Build for SSE4.1 CPUs (no AVX2): the fallback game program" OFF)
# LOCAL PATCH (GitHub #28, AMD FX-8350 at ~40 fps on the SSE4 build): a middle tier for CPUs with AVX but no AVX2
# (AMD FX / Bulldozer-Piledriver, Intel Sandy / Ivy Bridge). The SSE4 build leaves MSVC on its x64 default (SSE2) for
# all but the VU1 programs; this one uses AVX (VEX encoding, 256-bit registers) everywhere. bin/Shinobi-AVX.exe.
option(PS2X_AVX_CPU "Build for AVX CPUs without AVX2: the middle game program" OFF)
if(PS2X_AVX_CPU)
    set(PS2X_ARCH_FLAG "/arch:AVX")
    set(PS2X_CLANG_ARCH_FLAG "/arch:AVX")
elseif(PS2X_BASELINE_CPU)
    set(PS2X_ARCH_FLAG "")
    set(PS2X_CLANG_ARCH_FLAG "-msse4.1")
else()
    set(PS2X_ARCH_FLAG "/arch:AVX2")
    set(PS2X_CLANG_ARCH_FLAG "/arch:AVX2")
endif()

option(PS2X_ENABLE_LTO "Whole-program optimization (/GL, /LTCG, IPO) for Release builds" OFF)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                $<$<BOOL:${PS2X_ENABLE_LTO}>:/GL> # whole program opt (off by default, see above)
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                ${PS2X_ARCH_FLAG} # AVX2, or nothing for the SSE4.1 fallback build (PS2X_BASELINE_CPU)
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
        )

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    $<$<BOOL:${PS2X_ENABLE_LTO}>:/LTCG> # link-time code generation (off by default)
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                >
            )
        endif()
    endif()

    if(IPO_SUPPORTED AND PS2X_ENABLE_LTO)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    elseif(IPO_SUPPORTED)
        # LTO disabled on purpose (PS2X_ENABLE_LTO=OFF)
    else()
        message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
    endif()
endfunction()