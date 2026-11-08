include(CheckIPOSupported)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

# LOCAL CHANGE: whole-program optimization (/GL + /LTCG, IPO) defers all code generation to a
# single-threaded link, which is impractically slow for ~57k generated functions. Off by default.
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
                /arch:AVX2 # Advanced Vector Extensions 2
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