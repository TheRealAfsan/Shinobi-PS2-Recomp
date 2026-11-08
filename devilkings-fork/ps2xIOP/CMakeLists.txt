cmake_minimum_required(VERSION 3.21)

project(ps2xIOP LANGUAGES CXX)

option(PS2X_IOP_BUILD_TESTS "Build ps2xIOP emulator smoke tests" OFF)

add_library(ps2_iop STATIC
    src/ps2_path.cpp
    src/iop_module_manager.cpp
    src/iop_subsystem.cpp
    src/emulator/iop_emulator.cpp
    src/emulator/core/iop_cpu.cpp
    src/emulator/core/iop_kernel.cpp
    src/emulator/core/iop_memory.cpp
    src/emulator/core/iop_spu2.cpp
    src/emulator/services/iop_module_loader.cpp
    src/emulator/services/iop_rpc.cpp
    src/emulator/imports/iop_cdvd.cpp
    src/emulator/imports/iop_heaplib.cpp
    src/emulator/imports/iop_imports.cpp
    src/emulator/imports/iop_intrman.cpp
    src/emulator/imports/iop_ioman.cpp
    src/emulator/imports/iop_loadcore.cpp
    src/emulator/imports/iop_stdio.cpp
    src/emulator/imports/iop_sysclib.cpp
    src/emulator/imports/iop_sysmem.cpp
    src/emulator/imports/iop_timrman.cpp
    src/emulator/imports/iop_vblank.cpp
    src/modules/dbcman.cpp
    src/modules/libsd.cpp
    src/modules/mcserv.cpp
)

target_compile_features(ps2_iop PUBLIC cxx_std_20)

target_include_directories(ps2_iop
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# LOCAL PATCH: PCSX2's SPU2 core (GPL-3.0+, PCSX2 v2.8.0, unmodified sources in shinobi_port/third_party/pcsx2) driven
# by our IOP through src/emulator/spu2pcsx2 (spu2_port.cpp replaces PCSX2's spu2.cpp and IOP glue; shim/ stands in for
# the PCSX2 headers it includes). Selected at run time with PS2X_SPU2=pcsx2 (default) / legacy (iop_spu2.cpp's own model).
set(PS2X_PCSX2_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/pcsx2/pcsx2" CACHE PATH "PCSX2 source tree (pcsx2/ folder)")
add_library(ps2x_spu2_pcsx2 STATIC
    ${PS2X_PCSX2_SRC_DIR}/SPU2/ADSR.cpp
    ${PS2X_PCSX2_SRC_DIR}/SPU2/Dma.cpp
    ${PS2X_PCSX2_SRC_DIR}/SPU2/Mixer.cpp
    ${PS2X_PCSX2_SRC_DIR}/SPU2/ReadInput.cpp
    ${PS2X_PCSX2_SRC_DIR}/SPU2/RegTable.cpp
    ${PS2X_PCSX2_SRC_DIR}/SPU2/Reverb.cpp
    ${PS2X_PCSX2_SRC_DIR}/SPU2/ReverbResample.cpp
    ${PS2X_PCSX2_SRC_DIR}/SPU2/spu2sys.cpp
    src/emulator/spu2pcsx2/spu2_port.cpp
)
target_compile_features(ps2x_spu2_pcsx2 PUBLIC cxx_std_20)
target_include_directories(ps2x_spu2_pcsx2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/spu2pcsx2/shim
    ${PS2X_PCSX2_SRC_DIR}
    ${PS2X_PCSX2_SRC_DIR}/SPU2
)
if(MSVC)
    # Third-party code: keep the build log readable. The prelude stands in for PCSX2's precompiled header.
    target_compile_options(ps2x_spu2_pcsx2 PRIVATE /W0 "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/spu2pcsx2/shim/spu2pcsx2_prelude.h")
else()
    target_compile_options(ps2x_spu2_pcsx2 PRIVATE -w -include "${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/spu2pcsx2/shim/spu2pcsx2_prelude.h")
endif()
target_link_libraries(ps2_iop PRIVATE ps2x_spu2_pcsx2)

add_library(ps2x::iop ALIAS ps2_iop)

if(PS2X_IOP_BUILD_TESTS)
    enable_testing()
    add_executable(ps2_iop_emulator_tests tests/iop_emulator_tests.cpp)
    target_link_libraries(ps2_iop_emulator_tests PRIVATE ps2_iop)
    add_test(NAME ps2_iop_emulator_tests COMMAND ps2_iop_emulator_tests)

    add_executable(ps2_iop_import_tests tests/iop_import_tests.cpp)
    target_link_libraries(ps2_iop_import_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_import_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_import_tests COMMAND ps2_iop_import_tests)

    add_executable(ps2_iop_compatibility_tests tests/iop_compatibility_tests.cpp)
    target_link_libraries(ps2_iop_compatibility_tests PRIVATE ps2_iop)
    add_test(NAME ps2_iop_compatibility_tests COMMAND ps2_iop_compatibility_tests)

    add_executable(ps2_iop_import_version_tests tests/iop_import_version_tests.cpp)
    target_link_libraries(ps2_iop_import_version_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_import_version_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_import_version_tests COMMAND ps2_iop_import_version_tests)

endif()

install(TARGETS ps2_iop
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)
