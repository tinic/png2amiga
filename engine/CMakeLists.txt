cmake_minimum_required(VERSION 3.28)
project(pa-engine LANGUAGES C CXX ASM)

# ---------------------------------------------------------------------------
# C++26 is used both on host (for concept-only smoke tests) and on the m68k
# cross-compiler (bare-metal, -nostdlib). Host and cross both reject
# exceptions / RTTI — the engine has none.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PA_BUILD_EXAMPLES "Build engine example programs"       ON)
option(PA_BUILD_HOST_TESTS "Build host-side API smoke tests"   ON)
set(PA_CHIPSET "ocs" CACHE STRING "Target chipset: ocs | aga")
set_property(CACHE PA_CHIPSET PROPERTY STRINGS "ocs" "aga")

# ---------------------------------------------------------------------------
# The engine itself — header-only public surface + a thin platform impl
# (platform_amiga.cpp works for both host and cross at the moment, using
# __mc68000__ detection internally).
# ---------------------------------------------------------------------------
add_library(pa-engine STATIC
    src/platform_amiga.cpp
)
target_include_directories(pa-engine PUBLIC include)
if(PA_CHIPSET STREQUAL "aga")
    target_compile_definitions(pa-engine PUBLIC PA_CHIPSET_AGA=1)
endif()

# ---------------------------------------------------------------------------
# When cross-compiling for Amiga we need the template's support code so the
# final .exe has memcpy/memset/startup. Detect the cross-build by the
# existence of the toolchain root.
# ---------------------------------------------------------------------------
if(DEFINED PA_TOOLCHAIN_ROOT AND DEFINED PA_TEMPLATE_SUPPORT_SRCS)
    set(_pa_cross 1)
    message(STATUS "pa-engine: cross-compiling for m68k-amiga-elf")
else()
    set(_pa_cross 0)
    message(STATUS "pa-engine: host build (API smoke tests)")
endif()

# ---------------------------------------------------------------------------
# Helper: given a target name + its .cpp sources + optional assets, produce
# a Hunk-format AmigaOS executable. Only active on cross builds.
# ---------------------------------------------------------------------------
function(pa_add_amiga_exe TGT)
    cmake_parse_arguments(X "ADF" "" "SOURCES;INCLUDES" ${ARGN})
    if(NOT _pa_cross)
        message(STATUS "  [skip amiga exe] ${TGT} — no m68k toolchain")
        return()
    endif()
    set(_elf_name "${TGT}_elf")
    add_executable(${_elf_name} ${X_SOURCES} ${PA_TEMPLATE_SUPPORT_SRCS})
    target_link_libraries(${_elf_name} PRIVATE pa-engine)
    foreach(inc ${X_INCLUDES})
        target_include_directories(${_elf_name} PRIVATE ${inc})
    endforeach()
    target_include_directories(${_elf_name} PRIVATE ${PA_TEMPLATE_SUPPORT_DIR}/..)
    set_target_properties(${_elf_name} PROPERTIES
        OUTPUT_NAME "${TGT}"
        SUFFIX ".elf")
    # POST_BUILD: ELF → HUNK .exe via elf2hunk (always). Optionally also
    # build a bootable floppy .adf via exe2adf.
    add_custom_command(TARGET ${_elf_name} POST_BUILD
        COMMAND ${PA_ELF2HUNK} "$<TARGET_FILE:${_elf_name}>" "${TGT}.exe"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "elf2hunk -> ${TGT}.exe")
    if(X_ADF)
        add_custom_command(TARGET ${_elf_name} POST_BUILD
            COMMAND ${PA_EXE2ADF} -i "${TGT}.exe" -a "${TGT}.adf" -l "${TGT}"
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "exe2adf -> ${TGT}.adf")
    endif()
endfunction()

if(PA_BUILD_EXAMPLES)
    add_subdirectory(examples/hello_tiles)
    add_subdirectory(examples/hello_scroll)
endif()

if(PA_BUILD_HOST_TESTS AND NOT _pa_cross)
    enable_testing()
    add_subdirectory(tests)
endif()
