# Auto-init git submodules when a fresh clone hasn't done it yet.
# Plain `git clone` skips submodules; mi2-redux's bootstrap.sh and other
# downstream wrappers hit this. Detect a missing per-submodule sentinel
# (CMakeLists.txt for the cmake-built ones; spng.h for libspng), fire
# `git submodule update --init --recursive` once, and fail with a
# clear message if we're outside a git checkout (e.g. tarball install).
# Sentinel files: must exist when the corresponding submodule is
# checked out. libspng is vendored (not a submodule), so it's not
# listed here. ssimulacra2 ships its source tree under src/ — pick a
# file inside the repo root rather than a non-existent top-level path.
set(_p2a_required_submodule_files
    "${CMAKE_CURRENT_SOURCE_DIR}/libwebp/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/ssimulacra2/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/constixel/constixel.hpp")
set(_p2a_need_init FALSE)
foreach(_p2a_f IN LISTS _p2a_required_submodule_files)
    if(NOT EXISTS "${_p2a_f}")
        set(_p2a_need_init TRUE)
        break()
    endif()
endforeach()
if(_p2a_need_init)
    if(EXISTS "${CMAKE_SOURCE_DIR}/.git" AND
       EXISTS "${CMAKE_SOURCE_DIR}/.gitmodules")
        find_package(Git REQUIRED)
        message(STATUS "Initializing git submodules (third_party/*)")
        # `-c safe.directory=*` sidesteps Git 2.35.2+'s "dubious
        # ownership" guard, which trips inside CI Docker containers
        # where the workspace dir is owned by a different uid than the
        # user running cmake. Per-process flag — does NOT mutate global
        # config.
        execute_process(
            COMMAND ${GIT_EXECUTABLE} -c "safe.directory=*"
                    submodule update --init --recursive
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE _p2a_subres)
        if(NOT _p2a_subres EQUAL 0)
            message(FATAL_ERROR
                "git submodule update --init --recursive failed (rc=${_p2a_subres}). "
                "Run it manually in ${CMAKE_SOURCE_DIR} and re-run cmake.")
        endif()
    else()
        message(FATAL_ERROR
            "third_party submodules are missing and the source tree is not a git "
            "checkout (no .git or .gitmodules). Re-clone with "
            "`git clone --recurse-submodules` or fetch the release tarball that "
            "bundles third_party/.")
    endif()
endif()

add_library(stb STATIC stb_impl.c)
target_include_directories(stb SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# libspng (fast PNG decoder) built against miniz (single-file zlib drop-in).
# Keeps the vendored tree self-contained and avoids a system zlib dep.
add_library(miniz STATIC miniz/miniz.c)
target_include_directories(miniz SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/miniz)

add_library(spng STATIC libspng/spng.c)
# SPNG_STATIC must be PUBLIC: on MinGW/MSVC, spng.h without this macro
# declares SPNG_API as __declspec(dllimport), producing __imp_spng_* symbol
# references that the static archive can't satisfy at link time.
target_compile_definitions(spng PRIVATE SPNG_USE_MINIZ=1)
target_compile_definitions(spng PUBLIC SPNG_STATIC)
target_include_directories(spng SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libspng)
target_link_libraries(spng PRIVATE miniz)

# WASM pthreads link mode propagates -shared-memory across the whole
# program, so every TU (including these vendored C libraries) must be
# compiled with -pthread / atomics+bulk-memory or wasm-ld rejects the
# link. -pthread on the C-only targets is harmless on native (just
# enables -D_REENTRANT / -lpthread which they don't use).
if(EMSCRIPTEN)
    target_compile_options(stb   PRIVATE -pthread)
    target_compile_options(miniz PRIVATE -pthread)
    target_compile_options(spng  PRIVATE -pthread)
endif()

# libwebp: decoder only, no tools, no threading (for WASM portability)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_USE_THREAD OFF CACHE BOOL "" FORCE)
add_subdirectory(libwebp EXCLUDE_FROM_ALL)

# Expose libwebp public headers (src/webp/*.h) as a separate interface target
add_library(webp_headers INTERFACE)
target_include_directories(webp_headers SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/libwebp/src)

# constixel: header-only, single-file (constixel.hpp). Exposed as an INTERFACE
# target so any consumer can `#include <constixel.hpp>` after linking.
add_library(constixel INTERFACE)
target_include_directories(constixel SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/constixel)

# nlohmann/json: header-only JSON parser (MIT, single-file). Used for
# palette JSON load (--palette *.json) so we get spec-compliant
# tokenizing — the previous hand-rolled substring scanner broke on
# key reordering, embedded keyword strings, etc.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/json)

# smol-atlas: 2D rectangular bin packer (Shelf Best Height Fit heuristic),
# vendored as a single-file pair in smol-atlas/. Used by --joint-input mode
# to pack the training set into a 2D atlas with black-filled empty regions
# (matches the PyTexturePacker behaviour mi2-redux's 2D-pack pipeline had).
add_library(smol_atlas STATIC smol-atlas/smol-atlas.cpp)
target_include_directories(smol_atlas SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/smol-atlas)
if(EMSCRIPTEN)
    target_compile_options(smol_atlas PRIVATE -pthread)
endif()
