cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS2X_ENABLE_RUNNER_UNITY_BUILD "Build ps2EntryRunner with CMake unity build" ON)
set(PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE 32 CACHE STRING "Unity build batch size for ps2EntryRunner")
option(PS2X_ENABLE_RUNNER_PCH "Precompile the heavy runtime headers for ps2EntryRunner" ON)
option(PS2X_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)

option(PS2X_ENABLE_RUNTIME_LOGS "Enable PS2 runtime logs" ON)
option(PS2X_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" ON)
option(PS2X_ENABLE_IOP_RPC_TRACE "Log unhandled IOP/SIF RPC trace suggestions" ON)
option(PS2X_STRICT_RETURN_DIAGNOSTICS "Route generated JR $ra returns through runtime branch diagnostics" OFF)
option(PS2X_SHOW_WINDOWS_CONSOLE "Show a console window for ps2EntryRunner on Windows release builds" ON)
option(PS2X_ENABLE_DEBUG_UI "Build the desktop runtime debug UI" ON)

if(PS2X_ENABLE_SCCACHE)
    find_program(PS2X_SCCACHE_PROGRAM sccache)

    if(PS2X_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        endif()

        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${PS2X_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(PS2X_IS_VITA OFF)

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
    (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_VITA ON)
endif()

set(PS2X_IS_ANDROID OFF)

if(ANDROID)
    set(PS2X_IS_ANDROID ON)
endif()

option(PS2X_VITA_CREATE_PACKAGE "Create Vita eboot/vpk targets" ON)
set(PS2X_VITA_APP_NAME "PS2 Retro X" CACHE STRING "Display name for the Vita bubble")
set(PS2X_VITA_TITLEID "RANJ00001" CACHE STRING "9-character Vita title id")
set(PS2X_VITA_VERSION "01.00" CACHE STRING "Vita app version")
set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

if(PS2X_IS_VITA)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not defined. Configure inside the VitaSDK environment or pass the Vita toolchain file.")
    endif()

    set(PS2X_VITASDK "$ENV{VITASDK}")

    if(NOT EXISTS "${PS2X_VITASDK}/share/vita.cmake")
        message(FATAL_ERROR "Could not find vita.cmake under ${PS2X_VITASDK}/share.")
    endif()

    include("${PS2X_VITASDK}/share/vita.cmake" REQUIRED)
else()
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    if(PS2X_IS_ANDROID)
        set(PLATFORM "Android" CACHE STRING "" FORCE)
    endif()

    FetchContent_Declare(
        raylib
        GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
        GIT_TAG "5.5" # we will migrate to 4.2.0 later, trust me it will be better
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(raylib)

    if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_ANDROID)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG "v1.92.7-docking"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(imgui)

        if(NOT imgui_POPULATED)
            FetchContent_Populate(imgui)
        endif()

        set(PS2X_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")

        add_library(imgui STATIC
            "${PS2X_IMGUI_SOURCE_DIR}/imgui.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
        )
        target_include_directories(imgui PUBLIC "${PS2X_IMGUI_SOURCE_DIR}")

        FetchContent_Declare(
            rlImGui
            GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
            GIT_TAG "Raylib_5_5"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(rlImGui)

        if(NOT rlimgui_POPULATED)
            FetchContent_Populate(rlImGui)
        endif()

        set(PS2X_RLIMGUI_SOURCE_DIR "${rlimgui_SOURCE_DIR}")

        add_library(rlImGui STATIC
            "${PS2X_RLIMGUI_SOURCE_DIR}/rlImGui.cpp"
        )
        target_include_directories(rlImGui PUBLIC "${PS2X_RLIMGUI_SOURCE_DIR}")
        target_link_libraries(rlImGui PUBLIC raylib imgui)
    endif()
endif()

add_library(ps2_host_backend INTERFACE)

set(PS2X_DEFAULT_BOOT_ELF_DEFINE "")

if(PS2X_DEFAULT_BOOT_ELF)
    set(PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
endif()

if(PS2X_IS_VITA)
    set(PS2X_VITA_PREFIX "${PS2X_VITASDK}/arm-vita-eabi")
    set(PS2X_VITA_EXTRA_INCLUDE_DIRS "" CACHE STRING "Extra include directories for Vita host dependencies")
    set(PS2X_VITA_LIBRARY_DIR "${PS2X_VITA_PREFIX}/lib")
    set(PS2X_VITA_REQUIRED_LIBRARY_NAMES
        raylib
        SDL2
        OpenSLES
        taihen_stub
        SceAppMgr_stub
        SceCtrl_stub
        SceGxm_stub
        SceCommonDialog_stub
        SceLibKernel_stub
        SceAudio_stub
        SceTouch_stub
        SceHid_stub
        SceMotion_stub
        SceSysmodule_stub
        SceIofilemgr_stub
        SceNetCtl_stub
        SceNet_stub
        SceDisplay_stub
        SceAppUtil_stub
        SceAudioIn_stub
        ScePower_stub
        SceProcessmgr_stub
        SceIme_stub
        libIMGEGL_stub_weak
        libgpu_es4_ext_stub_weak
        libGLESv2_stub_weak
        libGL_stub_weak
        libGLESv1_CM_stub_weak
    )

    set(PS2X_VITA_INCLUDE_DIRS
        "${PS2X_VITA_PREFIX}/include"
    )

    foreach(PS2X_VITA_OPTIONAL_INCLUDE_DIR IN ITEMS
        "${PS2X_VITA_PREFIX}/include/raylib"
        "${PS2X_VITA_PREFIX}/include/SDL2")
        if(EXISTS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
            list(APPEND PS2X_VITA_INCLUDE_DIRS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
        endif()
    endforeach()

    if(PS2X_VITA_EXTRA_INCLUDE_DIRS)
        list(APPEND PS2X_VITA_INCLUDE_DIRS ${PS2X_VITA_EXTRA_INCLUDE_DIRS})
    endif()

    if(NOT EXISTS "${PS2X_VITA_PREFIX}/include/SDL2/SDL.h")
        message(FATAL_ERROR
            "Missing SDL2 headers under ${PS2X_VITA_PREFIX}/include/SDL2. "
            "Quenom/raylib-5.5-vita requires SDL2 with PVR support installed into VitaSDK.")
    endif()

    if(NOT EXISTS "${PS2X_VITA_LIBRARY_DIR}")
        message(FATAL_ERROR "Missing Vita library directory: ${PS2X_VITA_LIBRARY_DIR}")
    endif()

    function(ps2x_find_vita_library out_var library_name)
        string(MAKE_C_IDENTIFIER "${library_name}" library_id)
        set(cache_var "PS2X_VITA_LIB_${library_id}")

        find_library(${cache_var}
            NAMES "${library_name}" "${library_name}.a" "lib${library_name}.a"
            PATHS "${PS2X_VITA_LIBRARY_DIR}"
            NO_DEFAULT_PATH
        )

        if(NOT ${cache_var})
            message(FATAL_ERROR
                "Could not find Vita library '${library_name}' under ${PS2X_VITA_LIBRARY_DIR}. "
                "Install Quenom/raylib-5.5-vita and its SDL2/PVR dependencies into VitaSDK first.")
        endif()

        set(${out_var} "${${cache_var}}" PARENT_SCOPE)
    endfunction()

    set(PS2X_VITA_RESOLVED_LIBRARIES "")

    foreach(PS2X_VITA_LIBRARY_NAME IN LISTS PS2X_VITA_REQUIRED_LIBRARY_NAMES)
        ps2x_find_vita_library(PS2X_VITA_LIBRARY_PATH "${PS2X_VITA_LIBRARY_NAME}")
        list(APPEND PS2X_VITA_RESOLVED_LIBRARIES "${PS2X_VITA_LIBRARY_PATH}")
    endforeach()

    target_compile_definitions(ps2_host_backend INTERFACE PLATFORM_VITA)
    target_include_directories(ps2_host_backend INTERFACE ${PS2X_VITA_INCLUDE_DIRS})
    target_link_libraries(ps2_host_backend INTERFACE
        ${PS2X_VITA_RESOLVED_LIBRARIES}
        m
        c
        pthread
    )
else()
    target_link_libraries(ps2_host_backend INTERFACE raylib)

    if(PS2X_IS_ANDROID)
        target_link_libraries(ps2_host_backend INTERFACE log android)
    endif()
endif()

if(PS2X_IS_ANDROID)
    set(PS2X_ENABLE_FFMPEG_DEFAULT OFF)
else()
    set(PS2X_ENABLE_FFMPEG_DEFAULT ON)
endif()

option(PS2X_ENABLE_FFMPEG "Enable FFmpeg-backed MPEG video decoding" ${PS2X_ENABLE_FFMPEG_DEFAULT})

if(NOT PS2X_ENABLE_FFMPEG)
    message(STATUS "FFmpeg disabled; MPEG video decode falls back to stub frames")
    add_library(ffmpeg INTERFACE)
elseif(WIN32)
    include(ExternalProject)

    set(FFMPEG_PREBUILT_TAG "n7.1-241205")
    set(FFMPEG_PREBUILT_URL
        "https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/${FFMPEG_PREBUILT_TAG}/ffmpeg-${FFMPEG_PREBUILT_TAG}-lgpl-amd64-shared.zip")

    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    set(FFMPEG_SOURCE_DIR "${FFMPEG_PREFIX_DIR}/src/ffmpeg_external")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_SOURCE_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_SOURCE_DIR}/bin")
    set(FFMPEG_BIN_DIR "${FFMPEG_SOURCE_DIR}/bin")

    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_PREBUILT_URL}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )

    # Link against the import libraries shipped with the Windows DLLs.
    add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avcodec ffmpeg_external)

    add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avformat.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avformat ffmpeg_external)

    add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avutil ffmpeg_external)

    add_library(ffmpeg_swresample STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swresample.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swresample ffmpeg_external)

    add_library(ffmpeg_swscale STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swscale.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swscale ffmpeg_external)

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swresample
        ffmpeg_swscale
        bcrypt
        secur32
        ws2_32
        user32
        advapi32
        ole32
        shell32
    )

    set(PS2X_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(PS2X_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavformat
        libavutil
        libswresample
        libswscale
    )

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

function(ps2x_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT PS2X_FFMPEG_BIN_DIR OR NOT PS2X_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${PS2X_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${PS2X_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

add_library(ps2_runtime STATIC
    src/lib/game_overrides.cpp
    src/lib/gs/ps2_gif_arbiter.cpp
    src/lib/ps2_audio.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/gs/ps2_gs_memory.cpp
    src/lib/gs/gs_frontend.cpp
    src/lib/gs/gs_hud_layout.cpp
    src/lib/gs/gs_cpu_backend.cpp
    src/lib/ps2_iop_host.cpp
    src/lib/ps2_memory.cpp
    src/lib/ps2_pad.cpp
    src/lib/ps2_settings.cpp
    src/lib/ps2_rom_device.cpp
    src/lib/ps2_vfs.cpp
    src/lib/ps2_runtime.cpp
    src/lib/ps2_profiler.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_upper.cpp
    src/lib/vu/ps2_vu1_lower.cpp
    src/lib/vu/ps2_vu1_compiled.cpp
    src/lib/vu/ps2_vu1_parallel.cpp
    src/lib/vu/ps2_vu1_bench.cpp
    src/lib/ps2_sysstats.cpp
    src/lib/ps2_intro_video.cpp
    src/lib/ps2_ingame_menu.cpp
    src/lib/ps2_adx_check.cpp
    src/lib/ps2_scarf_colour.cpp
    src/lib/ps2_haptics_trace.cpp
    src/lib/ps2_dualsense.cpp
    src/lib/ps2_dualsense_audio.cpp
    src/lib/ps2_close_camera.cpp
    src/lib/ps2_estimate_trace.cpp
    src/launcher/launcher_settings_map.cpp
    src/lib/games_database.cpp
)

if(PS2X_ENABLE_RUNTIME_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

if(PS2X_ENABLE_AGRESSIVE_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        AGRESSIVE_LOGS=1
        PS2_FUNCTION_LOG_TRACKER=1
    )
endif()

if(PS2X_ENABLE_IOP_RPC_TRACE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=1
    )
else()
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=0
    )
endif()

if(PS2X_STRICT_RETURN_DIAGNOSTICS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_STRICT_RETURN_DIAGNOSTICS=1
    )
endif()

target_compile_definitions(ps2_runtime PRIVATE
    PS2X_HAS_FFMPEG=$<BOOL:${PS2X_ENABLE_FFMPEG}>
)

file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

target_sources(ps2_runtime PRIVATE
    ${KERNEL_SRC_FILES}
)

file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/*.cpp"
)

set(RUNNER_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

# Code recompiled separately for modules the game loads at runtime goes in
# src/runner/modules. It comes last so that adding a module only changes the
# final unity batches, not the ones holding the main executable's code.
file(GLOB_RECURSE RUNNER_MODULE_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/modules/*.cpp"
)
list(APPEND RUNNER_SRC_FILES ${RUNNER_MODULE_SRC_FILES})

# VU1 micro programs translated by tools/vu1rec.py go in src/runner/vu1, after
# the modules for the same reason: new programs keep turning up as more of a
# game is played.
file(GLOB RUNNER_VU1_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/vu1/*.cpp"
)
# LOCAL PATCH: compile the translated VU1 programs with clang-cl when it is installed (LLVM for Windows). Measured with
# the VU1 replay benchmark (PS2X_VU1_BENCH, 622 recorded Shinobi runs): 6.6 vs 8.9 ns per VU cycle, about 26% less time on
# the VIF1+VU1 thread, with the same checksum (bit-identical results). The objects link into the MSVC build as they are.
# -DPS2X_VU1_CLANG=OFF compiles them with MSVC like everything else.
option(PS2X_VU1_CLANG "Compile the translated VU1 programs with clang-cl when it is found" ON)
set(_ps2x_vu1_clang "")
if(MSVC AND PS2X_VU1_CLANG AND RUNNER_VU1_SRC_FILES)
    find_program(PS2X_CLANG_CL clang-cl HINTS "$ENV{ProgramFiles}/LLVM/bin" "C:/Program Files/LLVM/bin")
    if(PS2X_CLANG_CL)
        set(_ps2x_vu1_clang "${PS2X_CLANG_CL}")
    endif()
endif()
if(_ps2x_vu1_clang)
    message(STATUS "VU1 programs: compiled with ${_ps2x_vu1_clang}")
    set(_ps2x_vu1_objs "")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vu1clang")
    foreach(_src IN LISTS RUNNER_VU1_SRC_FILES)
        get_filename_component(_name "${_src}" NAME_WE)
        set(_obj "${CMAKE_CURRENT_BINARY_DIR}/vu1clang/${_name}.obj")
        add_custom_command(
            OUTPUT "${_obj}"
            COMMAND "${_ps2x_vu1_clang}" /nologo /c /O2 /Ob2 /Oi /Gy /Gw /GF /DNDEBUG /DWIN32 /D_WINDOWS /EHsc
                    -std:c++20 /MD /arch:AVX2 /GS- /Z7 -DPLATFORM_DESKTOP -DPS2X_HAVE_PGS=1
                    "-I${CMAKE_CURRENT_SOURCE_DIR}/include" "-I${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel"
                    "-I${CMAKE_CURRENT_SOURCE_DIR}/../ps2xIOP/include"
                    "${_src}" "/Fo${_obj}"
            DEPENDS "${_src}" "${CMAKE_CURRENT_SOURCE_DIR}/include/runtime/ps2_vu1_compiled.h"
            COMMENT "clang-cl ${_name}"
            VERBATIM)
        list(APPEND _ps2x_vu1_objs "${_obj}")
    endforeach()
    set_source_files_properties(${_ps2x_vu1_objs} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    list(APPEND RUNNER_SRC_FILES ${_ps2x_vu1_objs})
    set(RUNNER_VU1_SRC_FILES "") # the MSVC options further down are for MSVC-compiled programs only
else()
    list(APPEND RUNNER_SRC_FILES ${RUNNER_VU1_SRC_FILES})
endif()
# LOCAL PATCH: Shinobi's translated programs are huge (3-5 MB each); batching them into one unity translation unit made
# a single compiler process run for an hour and use many GB. As separate sources they compile in parallel.
if(RUNNER_VU1_SRC_FILES)
    set_source_files_properties(${RUNNER_VU1_SRC_FILES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
endif()

if(PS2X_IS_ANDROID)
    add_library(ps2EntryRunner SHARED
        ${RUNNER_SRC_FILES}
    )

    target_link_options(ps2EntryRunner PRIVATE "-Wl,--undefined=ANativeActivity_onCreate")
else()
    add_executable(ps2EntryRunner
        ${RUNNER_SRC_FILES}
    )
endif()

if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
    set_target_properties(ps2EntryRunner PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

if(PS2X_ENABLE_RUNNER_PCH)
    set(PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h"
    )

    foreach(PS2X_RUNNER_GENERATED_HEADER IN ITEMS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_functions.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_stubs.h")
        if(EXISTS "${PS2X_RUNNER_GENERATED_HEADER}")
            list(APPEND PS2X_RUNNER_PCH_HEADERS "${PS2X_RUNNER_GENERATED_HEADER}")
        endif()
    endforeach()

    list(APPEND PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_syscalls.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_stubs.h"
    )

    target_precompile_headers(ps2EntryRunner PRIVATE ${PS2X_RUNNER_PCH_HEADERS})
endif()

if(PS2X_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_DEFAULT_BOOT_ELF="${PS2X_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /FS)
    # LOCAL PATCH: the translated VU1 programs are SSE intrinsics; VEX encoding (3-operand, no register copies) shortens them.
    # Results must stay bit-identical (check the PS2X_VU1_BENCH checksum). -DPS2X_VU1_AVX2=OFF for a baseline-x64 build.
    # Measured with the VU1 replay benchmark: bit-identical but about 5% slower (8.7 vs 8.2 ns/cycle), so OFF.
    option(PS2X_VU1_AVX2 "Compile the translated VU1 programs with /arch:AVX2" OFF)
    # LOCAL PATCH: line-level debug info (/Z7: in the object, no shared PDB server between the parallel compiles) for
    # the translated VU1 programs, so the sampling profiler (PS2X_PROFILE_LINES) can name the hot instruction pairs.
    # It does not change the generated code.
    option(PS2X_VU1_LINE_INFO "Line-level debug info for the translated VU1 programs" ON)
    set(_ps2x_vu1_opts "")
    if(PS2X_VU1_AVX2)
        list(APPEND _ps2x_vu1_opts "/arch:AVX2")
    endif()
    if(PS2X_VU1_LINE_INFO)
        list(APPEND _ps2x_vu1_opts "/Z7")
    endif()
    if(_ps2x_vu1_opts AND RUNNER_VU1_SRC_FILES)
        set_source_files_properties(${RUNNER_VU1_SRC_FILES} PROPERTIES COMPILE_OPTIONS "${_ps2x_vu1_opts}")
    endif()
    # Record only the PDB's file name in the executable, not the build folder's path.
    target_link_options(ps2EntryRunner PRIVATE "/PDBALTPATH:%_PDB%")
    # LOCAL PATCH: a linker map (every function, static ones too) for the sampling profiler (ps2_sysstats.cpp): the PDB
    # has no names for the translated VU1 programs' static run parts, so DbgHelp alone mislabels their samples.
    target_link_options(ps2EntryRunner PRIVATE "/MAP")
endif()

# PS2X_RUNNER_ICON=<file.ico>: the game's icon for the Windows executable. GLFW gives its windows
# the resource named GLFW_ICON, so the window and the taskbar show it too.
set(PS2X_RUNNER_ICON "" CACHE FILEPATH "Icon (.ico) embedded in the Windows runner executable")
# LOCAL PATCH: Shinobi's own icon by default (launcher_assets/icon.ico).
if(WIN32 AND NOT PS2X_RUNNER_ICON AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../launcher_assets/icon.ico")
    set(PS2X_RUNNER_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../../launcher_assets/icon.ico")
endif()
if(WIN32 AND PS2X_RUNNER_ICON)
    file(TO_CMAKE_PATH "${PS2X_RUNNER_ICON}" PS2X_RUNNER_ICON_PATH)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ps2x_runner_icon.rc" "GLFW_ICON ICON \"${PS2X_RUNNER_ICON_PATH}\"\n")
    target_sources(ps2EntryRunner PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ps2x_runner_icon.rc")
endif()

target_include_directories(ps2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
)

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg ps2_iop)
if(WIN32)
    # LOCAL PATCH: studio intro video (ps2_intro_video.cpp): Windows Media Foundation.
    target_link_libraries(ps2_runtime PUBLIC mfplat mfreadwrite mfuuid ole32)
    # LOCAL PATCH: in-game menu (ps2_ingame_menu.cpp): the launcher page in Microsoft Edge WebView2.
    set(PS2X_WEBVIEW2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/webview2/build/native" CACHE PATH "WebView2 SDK build/native folder")
    set_source_files_properties(src/lib/ps2_ingame_menu.cpp PROPERTIES INCLUDE_DIRECTORIES "${PS2X_WEBVIEW2_DIR}/include")
    target_link_libraries(ps2_runtime PUBLIC "${PS2X_WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib" version shell32 user32)
endif()

# Vulkan renderer: paraLLEl-GS (the GS emulated in Vulkan compute, LGPL-3.0+), built when
# PS2X_PGS_DIR points at a checkout of https://github.com/Arntzen-Software/parallel-gs (or a fork)
# with its Granite submodule initialised recursively (on Windows: git config core.longpaths true).
set(PS2X_PGS_DIR "" CACHE PATH "paraLLEl-GS checkout for the Vulkan renderer (empty: not built)")
if(PS2X_PGS_DIR AND NOT PS2X_IS_VITA AND NOT APPLE)
    if(EXISTS "${PS2X_PGS_DIR}/CMakeLists.txt" AND EXISTS "${PS2X_PGS_DIR}/Granite/third_party/volk/volk.c")
        set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
        # paraLLEl-GS picks the static MSVC runtime unless one is set; the runtime links the DLL one.
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        add_subdirectory("${PS2X_PGS_DIR}" "${CMAKE_BINARY_DIR}/parallel-gs" EXCLUDE_FROM_ALL)
        target_sources(ps2_runtime PRIVATE src/lib/gs/gs_pgs.cpp src/lib/gs/gs_pcsx2.cpp)
        target_link_libraries(ps2_runtime PUBLIC parallel-gs)
        target_compile_definitions(ps2_runtime PUBLIC PS2X_HAVE_PGS=1)
        message(STATUS "[pgs] Vulkan renderer (paraLLEl-GS) from ${PS2X_PGS_DIR}")
    else()
        message(WARNING "[pgs] PS2X_PGS_DIR=${PS2X_PGS_DIR} is not a complete paraLLEl-GS checkout "
                        "(run git submodule update --init --recursive in it); the Vulkan renderer is not built")
    endif()
endif()

target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)

if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE
        src/lib/ps2_debug_panel.cpp
        src/lib/ps2_settings_menu.cpp
    )
    target_link_libraries(ps2EntryRunner PRIVATE rlImGui imgui)
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

ps2x_stage_ffmpeg_runtime_dlls(ps2EntryRunner)

if(PS2X_IS_VITA)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1") 
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

    if(PS2X_VITA_CREATE_PACKAGE)
        set(PS2X_VITA_VPK_FILES "")

        foreach(PS2X_VITA_MODULE IN ITEMS
            libgpu_es4_ext.suprx
            libIMGEGL.suprx
            libGLESv2.suprx
            libpvrPSP2_WSEGL.suprx
            libGL.suprx)
            set(PS2X_VITA_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vita/module/${PS2X_VITA_MODULE}")

            if(EXISTS "${PS2X_VITA_MODULE_PATH}")
                list(APPEND PS2X_VITA_VPK_FILES FILE "${PS2X_VITA_MODULE_PATH}" "module/${PS2X_VITA_MODULE}")
            else()
                message(WARNING
                    "Missing ${PS2X_VITA_MODULE_PATH}; the vpk will rely on ur0:data/external/ for it")
            endif()
        endforeach()

        vita_create_self(eboot.bin ps2EntryRunner UNSAFE)
        vita_create_vpk(ps2EntryRunner.vpk ${PS2X_VITA_TITLEID} eboot.bin
            VERSION ${PS2X_VITA_VERSION}
            NAME ${PS2X_VITA_APP_NAME}
            ${PS2X_VITA_VPK_FILES}
        )
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_runtime)
    EnableFastReleaseMode(ps2EntryRunner)

    if(WIN32 AND NOT PS2X_SHOW_WINDOWS_CONSOLE)
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
            )
        elseif(MINGW)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
            )
        endif()
    endif()
endif()

# Work around WinAPI vs raylib symbol clash for CloseWindow on x64
if(MSVC)
    target_link_options(ps2EntryRunner PRIVATE "/FORCE:MULTIPLE")
endif()

# Vita: Work around for GCC 10's LTO plugin
if(PS2X_IS_VITA)
    target_link_options(ps2EntryRunner PRIVATE "-Wl,--allow-multiple-definition")
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_vita_runtime.cpp)
    set_source_files_properties(src/lib/ps2_vita_runtime.cpp PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION TRUE
        COMPILE_OPTIONS "-fno-lto"
    )
endif()

if(PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_android_runtime.cpp)
    # main.cpp keeps the screen on through the activity of raylib's android_app.
    target_include_directories(ps2EntryRunner PRIVATE "${ANDROID_NDK}/sources/android/native_app_glue")
endif()

install(TARGETS ps2_runtime ps2EntryRunner
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(WIN32 AND PS2X_ENABLE_FFMPEG)
    install(DIRECTORY "${FFMPEG_BIN_DIR}/"
        DESTINATION bin
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

install(DIRECTORY include/
    DESTINATION include
)

# LOCAL PATCH: Shinobi (PS2 Recomp) Launcher - a settings window that writes game/settings.ini and starts the game.
if(WIN32 AND NOT PS2X_IS_VITA)
    # The window shows launcher_assets/launcher.html (the Claude Design page) in Microsoft Edge WebView2.
    set(PS2X_WEBVIEW2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/webview2/build/native" CACHE PATH "WebView2 SDK build/native folder")
    add_executable(ShinobiLauncher WIN32
        src/launcher/launcher_webview.cpp
        src/launcher/launcher_iso.cpp
        src/launcher/launcher_manual.cpp
        src/launcher/launcher_settings_map.cpp
        src/launcher/launcher_ini.cpp
        src/launcher/launcher_dualsense.cpp
        src/launcher/launcher_icon.rc
    )
    target_compile_features(ShinobiLauncher PRIVATE cxx_std_20)
    target_include_directories(ShinobiLauncher PRIVATE "${PS2X_WEBVIEW2_DIR}/include")
    target_link_libraries(ShinobiLauncher PRIVATE "${PS2X_WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib"
        version shlwapi ole32 user32 dwmapi shell32 advapi32
        windowsapp) # LOCAL PATCH: Windows.Data.Pdf for the manual (launcher_manual.cpp)
    if(MSVC)
        target_compile_options(ShinobiLauncher PRIVATE /utf-8)
    endif()
    # The older raylib launcher stays buildable for comparison.
    add_executable(ShinobiLauncherClassic WIN32 EXCLUDE_FROM_ALL
        src/launcher/launcher_main.cpp
        src/launcher/launcher_ini.cpp
        src/launcher/launcher_win.cpp
    )
    target_compile_features(ShinobiLauncherClassic PRIVATE cxx_std_20)
    target_link_libraries(ShinobiLauncherClassic PRIVATE raylib)
    if(MSVC)
        # WIN32 (no console window) but with a plain main(); source is UTF-8 (the hand-seal kanji).
        target_link_options(ShinobiLauncherClassic PRIVATE /ENTRY:mainCRTStartup)
        target_compile_options(ShinobiLauncherClassic PRIVATE /utf-8)
    endif()
endif()
