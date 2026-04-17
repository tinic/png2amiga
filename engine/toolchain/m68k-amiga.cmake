# CMake toolchain for cross-compiling the engine to AmigaOS.
#
# Uses the vscode-amiga-debug toolchain (m68k-amiga-elf-gcc 14, elf2hunk, ...).
# Matches build-amiga.sh's flags. Tested against the Bebbo/Bartman
# vscode-amiga-debug submodule at ../third_party/vscode-amiga-debug/.
#
# Drive a build with:
#   cmake -S engine -B engine/build-amiga \
#         -DCMAKE_TOOLCHAIN_FILE=$(pwd)/engine/toolchain/m68k-amiga.cmake
#   cmake --build engine/build-amiga
#
# Set PA_TOOLCHAIN_ROOT in the environment or as a cache variable to override
# the default toolchain location.

set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  m68k)

if(NOT DEFINED PA_TOOLCHAIN_ROOT)
    if(APPLE)
        set(_host_subdir "darwin")
    elseif(UNIX)
        set(_host_subdir "linux")
    elseif(WIN32)
        set(_host_subdir "win32")
    else()
        message(FATAL_ERROR "Unsupported host platform for vscode-amiga-debug toolchain")
    endif()
    # engine/ sits beside third_party/; CMAKE_CURRENT_LIST_DIR is engine/toolchain
    set(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../..")
    set(PA_TOOLCHAIN_ROOT "${_repo_root}/third_party/vscode-amiga-debug/bin/${_host_subdir}"
        CACHE PATH "vscode-amiga-debug toolchain root")
endif()

set(PA_TOOLCHAIN_BIN "${PA_TOOLCHAIN_ROOT}/opt/bin")
set(PA_TOOLCHAIN_TEMPLATE "${PA_TOOLCHAIN_ROOT}/../../template"
    CACHE PATH "vscode-amiga-debug template (supplies support code + SDK)")

# The vscode-amiga-debug bundle ships only `gcc` (no separate g++/c++). gcc
# dispatches on source-file extension, so we point both C and C++ at it.
set(CMAKE_C_COMPILER   "${PA_TOOLCHAIN_BIN}/m68k-amiga-elf-gcc")
set(CMAKE_CXX_COMPILER "${PA_TOOLCHAIN_BIN}/m68k-amiga-elf-gcc")
# Route .s files through gcc as the driver so our -Wa,--register-prefix-optional
# flag + preprocessor includes "just work" the same way they do in the
# template's Makefile / build-amiga.sh.
set(CMAKE_ASM_COMPILER "${PA_TOOLCHAIN_BIN}/m68k-amiga-elf-gcc")
set(CMAKE_ASM_COMPILE_OBJECT
    "<CMAKE_ASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -c <SOURCE> -o <OBJECT>")

# Binutils: without explicit AR/RANLIB, CMake falls back to the host tools
# (macOS /usr/bin/ranlib), which can't read m68k ELF and silently produces
# empty archive tables of contents.
set(_pa_binutils "${PA_TOOLCHAIN_ROOT}/opt/m68k-amiga-elf/bin")
set(CMAKE_AR     "${_pa_binutils}/ar"     CACHE FILEPATH "m68k ar")
set(CMAKE_RANLIB "${_pa_binutils}/ranlib" CACHE FILEPATH "m68k ranlib")
set(CMAKE_NM     "${_pa_binutils}/nm"     CACHE FILEPATH "m68k nm")
set(CMAKE_OBJCOPY "${_pa_binutils}/objcopy" CACHE FILEPATH "m68k objcopy")
set(CMAKE_OBJDUMP "${_pa_binutils}/objdump" CACHE FILEPATH "m68k objdump")
set(CMAKE_STRIP   "${_pa_binutils}/strip"   CACHE FILEPATH "m68k strip")

# Skip CMake's try-compile (we produce .elf, not a normal executable).
set(CMAKE_C_COMPILER_WORKS   1)
set(CMAKE_CXX_COMPILER_WORKS 1)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Shared compile flags — mirrors the template Makefile's production build
# (build-amiga.sh uses a lighter subset). LTO + whole-program is what
# lets 308 blits/frame actually hit 50 Hz: cross-unit inlining turns the
# engine's thin blitter helpers into a register-write loop in the caller.
set(PA_COMMON_FLAGS
    "-m68000"
    "-Ofast"
    "-nostdlib"
    "-fomit-frame-pointer"
    "-fno-exceptions"
    "-fno-tree-loop-distribution"
    "-flto"
    "-fwhole-program"
    "-ffunction-sections"
    "-fdata-sections"
    "-Wa,--register-prefix-optional")

set(PA_CXX_FLAGS
    ${PA_COMMON_FLAGS}
    "-fno-rtti"
    "-fno-use-cxa-atexit")

# String-join into the INIT flags CMake expects.
string(REPLACE ";" " " _pa_common  "${PA_COMMON_FLAGS}")
string(REPLACE ";" " " _pa_cxx     "${PA_CXX_FLAGS}")
set(CMAKE_C_FLAGS_INIT   "${_pa_common}")
set(CMAKE_CXX_FLAGS_INIT "${_pa_cxx}")
# ASM passes through gcc (see CMAKE_ASM_COMPILE_OBJECT above). We pass the
# same -m68000 + -Wa,--register-prefix-optional that build-amiga.sh uses.
set(CMAKE_ASM_FLAGS_INIT "-m68000 -Wa,--register-prefix-optional")

set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--emit-relocs,--gc-sections,-Ttext=0 -flto -fwhole-program")

# The bundle is freestanding: no libstdc++ headers. `freestanding_stl/` under
# engine/ supplies the minimal subset (<array>, <span>, <cstdint>, ...) that
# the engine + generated asset .cpp files use. Inject it via -isystem so it
# behaves like a real standard library (less-strict diagnostics, lower
# include priority than user code).
set(_pa_fstl "${CMAKE_CURRENT_LIST_DIR}/../freestanding_stl")
string(APPEND CMAKE_C_FLAGS_INIT   " -isystem ${_pa_fstl}")
string(APPEND CMAKE_CXX_FLAGS_INIT " -isystem ${_pa_fstl}")

set(PA_ELF2HUNK "${PA_TOOLCHAIN_ROOT}/elf2hunk" CACHE FILEPATH "elf2hunk tool")
set(PA_EXE2ADF  "${PA_TOOLCHAIN_ROOT}/exe2adf"  CACHE FILEPATH "exe2adf tool")

set(PA_TEMPLATE_SUPPORT_DIR "${PA_TOOLCHAIN_TEMPLATE}/support")
set(PA_TEMPLATE_SUPPORT_SRCS
    "${PA_TEMPLATE_SUPPORT_DIR}/gcc8_a_support.s"
    "${PA_TEMPLATE_SUPPORT_DIR}/gcc8_c_support.c"
    CACHE STRING "Support sources (startup, memcpy/memset, debugger helpers)")
