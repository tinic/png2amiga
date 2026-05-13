cmake_minimum_required(VERSION 3.28)
project(png2amiga VERSION 1.87.1 LANGUAGES C CXX)

if(EMSCRIPTEN OR MSVC)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Capture the total commit count as a monotonically increasing build number.
# Re-runs of cmake pick up new commits; CI runs cmake fresh each time so the
# released binary always carries the right number. Falls back to 0 if git is
# unavailable (source tarball, no .git dir, etc).
set(PNG2AMIGA_BUILD_NUMBER 0)
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE PNG2AMIGA_BUILD_NUMBER
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT PNG2AMIGA_BUILD_NUMBER)
        set(PNG2AMIGA_BUILD_NUMBER 0)
    endif()
endif()
set(PNG2AMIGA_VERSION_FULL "${PROJECT_VERSION}.${PNG2AMIGA_BUILD_NUMBER}")
message(STATUS "png2amiga version: ${PNG2AMIGA_VERSION_FULL}")

# Generate src/version.hpp from version.hpp.in. Single source of truth for the
# version string — bump PROJECT_VERSION in the project() call above; the build
# number is auto-derived. The generated header lands in build/generated/ and is
# added to both the CLI and WASM include paths below.
configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    @ONLY
)

add_subdirectory(third_party)

# Core library (shared between CLI and WASM)
set(PNG2AMIGA_CORE_SOURCES
    src/png_io.cpp
    src/scale.cpp
    src/preprocess.cpp
    src/quantize.cpp
    src/quantize_metal_wrap.cpp
    src/palette_search.cpp
    src/dither.cpp
    src/bitplane.cpp
    src/degas.cpp
    src/ham.cpp
    src/iff.cpp
    src/cheader.cpp
    src/palette_io.cpp
    src/copper.cpp
    src/strips.cpp
    src/dither_tuning.cpp
    src/palette_locks.cpp
    src/cga_text.cpp
    src/cheader_dos_c.cpp
    src/cheader_genesis.cpp
    src/snes_io.cpp
    src/genesis.cpp
    src/c64.cpp
    src/c64_prg.cpp
    src/pipeline.cpp
    src/ssimulacra2.cpp
    src/api.cpp
    src/ocs_cand_table.cpp
)

if(EMSCRIPTEN)
    # --- WASM library build ---
    # Include quantize_metal.cpp without PNG2AMIGA_HAVE_METAL — it
    # compiles to lightweight stubs (false / std::unexpected) that
    # quantize_metal_wrap.cpp links against. Same TU on native builds
    # without Metal (Linux / Windows). On macOS-with-Metal the file
    # is compiled separately with apple-clang via a custom command
    # (see below) so the symbols come from there instead.
    add_executable(png2amiga_wasm
        ${PNG2AMIGA_CORE_SOURCES}
        src/quantize_metal.cpp
        src/wasm_bindings.cpp
    )
    target_include_directories(png2amiga_wasm PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(png2amiga_wasm PRIVATE stb spng webp_headers webpdecode webpdspdecode webputilsdecode constixel nlohmann_json)
    # -pthread enables std::thread / std::jthread / std::mutex / std::atomic
    # in WASM via Emscripten's pthreads-on-web-workers shim. Requires the
    # browser to be cross-origin isolated (COOP/COEP headers, see
    # service/nginx.conf and vite.config.js). Pool size = the number of
    # cores reported by navigator.hardwareConcurrency, allocated lazily.
    target_compile_options(png2amiga_wasm PRIVATE
        -O3 -msimd128 -flto
        -pthread
        # cga_font.hpp's `kCgaCanonicalBitmaps` consteval table calls
        # `make_glyph_canonical_bitmap` 32 times, each running a 256-step
        # inner loop. Emscripten's clang caps constexpr evaluation at
        # 1,048,576 steps by default — bump it so the table can be
        # generated at compile time. GCC's limit is much higher; this
        # flag is harmless on builds that don't need it.
        -fconstexpr-steps=10000000)
    target_link_options(png2amiga_wasm PRIVATE
        --bind
        -O3
        -flto
        -msimd128
        -pthread
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORT_ES6=1
        -sMODULARIZE=1
        -sEXPORT_NAME=createPng2Amiga
        # node included so the dither-thumbnail generator script
        # (web/scripts/gen-dither-thumbs.mjs) can load this module
        # under Node.js — Emscripten's pthread shim then routes to
        # Node's worker_threads instead of browser Web Workers.
        -sENVIRONMENT=web,worker,node
        -sNO_FILESYSTEM=1
        -sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency
    )
    set_target_properties(png2amiga_wasm PROPERTIES OUTPUT_NAME "png2amiga")
else()
    # --- Native CLI build ---
    include(cmake/CompilerWarnings.cmake)

    add_executable(png2amiga
        src/main.cpp
        ${PNG2AMIGA_CORE_SOURCES}
    )

    # api_pipeline_smoke: minimal CLI wrapper around api::encode_state,
    # used by tools/check_api_equiv.py to A/B compare the api-only path
    # against png2amiga's main.cpp inline pipelines. Built only on native
    # (not WASM); shares the core sources so it tracks api.cpp changes
    # automatically.
    add_executable(api_pipeline_smoke
        tools/api_pipeline_smoke.cpp
        ${PNG2AMIGA_CORE_SOURCES}
    )
    target_include_directories(api_pipeline_smoke PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(api_pipeline_smoke PRIVATE stb spng webp_headers webpdecode webpdspdecode webputilsdecode constixel nlohmann_json)
    target_compile_options(api_pipeline_smoke PRIVATE ${PNG2AMIGA_WARNING_FLAGS})

    # palette_search: experimental population-based palette search for
    # small palettes (lores OCS d ≤ 4). --best does k-means + 161
    # jittered restarts which rarely escapes basins at d=2/3 (sparse
    # 4-colour / 8-colour palette space over the 4096-colour OCS gamut).
    # This evolutionary search seeds from k-means + heavy perturbations
    # and runs 256-pop × 40-gen with crossover + mutation, ranked by
    # SSIMULACRA2. Standalone harness so we can sweep before deciding
    # whether to integrate as a CLI flag.
    add_executable(palette_search
        tools/palette_search.cpp
        ${PNG2AMIGA_CORE_SOURCES}
    )
    target_include_directories(palette_search PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(palette_search PRIVATE stb spng webp_headers webpdecode webpdspdecode webputilsdecode constixel nlohmann_json)
    target_compile_options(palette_search PRIVATE ${PNG2AMIGA_WARNING_FLAGS})

    # bench_ssimulacra2: standalone microbench for the SSIMULACRA2
    # metric. Used to drive AMD uProf / Apple Instruments profiling so
    # we know which kernel inside ssimulacra2.cpp dominates wall time.
    add_executable(bench_ssimulacra2
        tools/bench_ssimulacra2.cpp
        src/ssimulacra2.cpp
    )
    target_include_directories(bench_ssimulacra2 PRIVATE
        src
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_options(bench_ssimulacra2 PRIVATE ${PNG2AMIGA_WARNING_FLAGS})
    # Profilers (AMD uProf on Windows, perf/Instruments on Linux/macOS)
    # need debug info to attribute samples to function names. MSVC
    # Release default omits PDB; force /Zi (compile) + /DEBUG /OPT:REF
    # /OPT:ICF (link) so the bench builds with PDB without losing the
    # /O2 optimisations. Same idea on gcc/clang via -g (already in
    # RelWithDebInfo, but Release alone strips it).
    if(MSVC)
        target_compile_options(bench_ssimulacra2 PRIVATE /Zi)
        target_link_options(bench_ssimulacra2    PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    else()
        target_compile_options(bench_ssimulacra2 PRIVATE -g)
    endif()

    # bench_pow24: scalar vs polynomial vs AVX2 8-wide pow24 timing —
    # used to decide whether replacing std::pow inside the dither hot
    # loop is worth shipping. Header-only on color_space.hpp; no extra
    # link deps.
    add_executable(bench_pow24 tools/bench_pow24.cpp)
    target_include_directories(bench_pow24 PRIVATE
        src ${CMAKE_BINARY_DIR}/generated)
    target_compile_options(bench_pow24 PRIVATE ${PNG2AMIGA_WARNING_FLAGS})
    if(MSVC)
        target_compile_options(bench_pow24 PRIVATE /Zi /arch:AVX2)
        target_compile_definitions(bench_pow24 PRIVATE __AVX2__ __FMA__)
        target_link_options(bench_pow24 PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    else()
        target_compile_options(bench_pow24 PRIVATE -g)
        if(PNG2AMIGA_HAS_MAVX2)
            target_compile_options(bench_pow24 PRIVATE -mavx2 -mfma)
        endif()
    endif()
    # Include quantize_metal.cpp here too when there's no Metal —
    # it provides the C-stub symbols (false / std::unexpected) that
    # quantize_metal_wrap.cpp links against. The Metal-enabled
    # branch below replaces this TU with an apple-clang .o file.
    target_include_directories(png2amiga PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(png2amiga PRIVATE stb spng webp_headers webpdecode webpdspdecode webputilsdecode constixel smol_atlas nlohmann_json)
    target_compile_options(png2amiga PRIVATE ${PNG2AMIGA_WARNING_FLAGS})

    # SIMD baseline for the hand-vectorized kernels in quantize.cpp /
    # ssimulacra2.cpp. Three backends are supported in source:
    #   - AVX2 + FMA   → x86_64 Windows / Linux / Intel Mac
    #   - ARM NEON     → Apple Silicon / ARM64 Linux (implicit on AArch64)
    #   - WASM SIMD    → Emscripten (set via -msimd128 on the WASM target)
    # No scalar fallback. AVX2 is universal on modern x86_64 (Intel
    # Haswell 2013+, AMD Zen 2017+); NEON is mandatory on AArch64.
    if(MSVC)
        target_compile_options(png2amiga PRIVATE /arch:AVX2)
        target_compile_options(api_pipeline_smoke PRIVATE /arch:AVX2)
        target_compile_options(bench_ssimulacra2 PRIVATE /arch:AVX2)
        target_compile_options(palette_search PRIVATE /arch:AVX2)
        # /fp:contract — enable FMA contraction (a*b + c → vfmadd) only.
        # Stays under /fp:precise so reassociation, FTZ/DAZ, and
        # signed-zeros/NaN handling all keep IEEE semantics — none of
        # the Walczyk cbrt's Newton iterations are affected. Earlier
        # /fp:fast attempt regressed badly (FTZ/DAZ killed the cbrt's
        # subnormal-transit beam-search paths); contract-only avoids that.
        target_compile_options(png2amiga PRIVATE /fp:contract)
        target_compile_options(api_pipeline_smoke PRIVATE /fp:contract)
        target_compile_options(bench_ssimulacra2 PRIVATE /fp:contract)
        target_compile_options(palette_search PRIVATE /fp:contract)
        # MSVC's /arch:AVX2 does NOT predefine __AVX2__ (gcc/clang do).
        # ssimulacra2.cpp's SIMD-backend gate keys on __AVX2__, so define
        # it explicitly here for the MSVC path. Same for __FMA__ (the
        # fmadd intrinsics live in the same immintrin.h block).
        target_compile_definitions(png2amiga PRIVATE __AVX2__ __FMA__)
        target_compile_definitions(api_pipeline_smoke PRIVATE __AVX2__ __FMA__)
        target_compile_definitions(bench_ssimulacra2 PRIVATE __AVX2__ __FMA__)
        target_compile_definitions(palette_search PRIVATE __AVX2__ __FMA__)
        # cga_font.hpp's `kCgaCanonicalBitmaps` consteval table calls
        # `make_glyph_canonical_bitmap` 32 times, each scanning 256
        # glyphs with an O(n²) dedup walk — overshoots MSVC's default
        # /constexpr:steps 1048576. Bump to match the clang/Emscripten
        # path (10M steps). Set per source so every target that picks
        # up cga_text.cpp (png2amiga, palette_search, …) inherits it.
        set_source_files_properties(src/cga_text.cpp PROPERTIES
            COMPILE_OPTIONS /constexpr:steps10000000)
    else()
        # Probe for AVX2 — succeeds on x86_64, fails on ARM64 (Apple
        # Silicon, etc.) where NEON is the default ISA and no flag is
        # needed. CMake re-runs the probe whenever the compiler changes,
        # so universal-binary builds switching arch will Do The Right Thing.
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-mavx2" PNG2AMIGA_HAS_MAVX2)
        if(PNG2AMIGA_HAS_MAVX2)
            target_compile_options(png2amiga PRIVATE -mavx2 -mfma)
            target_compile_options(api_pipeline_smoke PRIVATE -mavx2 -mfma)
            target_compile_options(bench_ssimulacra2 PRIVATE -mavx2 -mfma)
            target_compile_options(palette_search PRIVATE -mavx2 -mfma)
        endif()
        # -ffp-contract=fast — GCC/Clang equivalent of MSVC's /fp:contract.
        # Allows FMA contraction across separate statements (e.g.
        # `float x = a*b; y = x + c;` can still fuse to vfmadd) but keeps
        # the rest of IEEE semantics intact. See the MSVC branch above
        # for why we don't go further (full -ffast-math regresses the
        # Walczyk cbrt's subnormal transit beam-search paths).
        target_compile_options(png2amiga PRIVATE -ffp-contract=fast)
        target_compile_options(api_pipeline_smoke PRIVATE -ffp-contract=fast)
        target_compile_options(bench_ssimulacra2 PRIVATE -ffp-contract=fast)
        target_compile_options(palette_search PRIVATE -ffp-contract=fast)
    endif()

    if(MSVC)
        # CMake 4.0.2 doesn't yet map CMAKE_CXX_STANDARD=26 to /std:c++26 for
        # MSVC, but /std:c++latest enables the same C++26 features we need
        # (constexpr <cmath>: std::pow / std::cbrt called from constexpr LUT
        # initializers in color_space.hpp / palette.hpp). CMAKE_CXX_STANDARD
        # stays at 23 so CMake's feature-test step succeeds; this flag is
        # appended after, so /std:c++latest wins.
        target_compile_options(png2amiga PRIVATE /std:c++latest)
        # MSVC's "secure CRT" deprecation noise (getenv, etc.) is W4-fatal
        # under /WX. We're not switching to *_s variants — they're MSVC-only.
        target_compile_definitions(png2amiga PRIVATE _CRT_SECURE_NO_WARNINGS)
        # Emit PDBs even in Release so AMDuProf / VTune can resolve symbols.
        # /Zi adds debug info to the .obj; /DEBUG tells the linker to write
        # the .pdb. /OPT:REF and /OPT:ICF restore the dead-strip and COMDAT
        # folding that /DEBUG would otherwise turn off, so we keep release
        # codegen. Source line tables are coarse at /O2 — that's expected;
        # function-level symbols are what AMDuProf needs.
        target_compile_options(png2amiga PRIVATE /Zi)
        target_link_options(png2amiga PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    endif()

    if(MINGW)
        # std::print's terminal helpers (__open_terminal, __write_to_terminal)
        # live in libstdc++exp.a on MinGW rather than the main libstdc++.a.
        # Must come AFTER object files in the link line, so use
        # target_link_libraries (not CMAKE_EXE_LINKER_FLAGS, which is positioned
        # before objects and won't pull any symbols from a static archive).
        target_link_libraries(png2amiga PRIVATE -lstdc++exp)
    endif()

    # ------------------------------------------------------------------
    # Optional Metal acceleration (gpu_restart quantizer).
    # macOS-only, requires the full Xcode metal/metallib toolchain
    # (CommandLineTools is not enough). Auto-detected; if missing, the
    # gpu_restart quantizer falls back to median_cut at runtime.
    # ------------------------------------------------------------------
    set(PNG2AMIGA_HAVE_METAL OFF)
    if(APPLE)
        execute_process(COMMAND xcrun -f metal
            RESULT_VARIABLE _metal_rc
            OUTPUT_VARIABLE PNG2AMIGA_METAL_EXE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        execute_process(COMMAND xcrun -f metallib
            RESULT_VARIABLE _metallib_rc
            OUTPUT_VARIABLE PNG2AMIGA_METALLIB_EXE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_metal_rc EQUAL 0 AND _metallib_rc EQUAL 0
                AND PNG2AMIGA_METAL_EXE AND PNG2AMIGA_METALLIB_EXE)
            set(PNG2AMIGA_HAVE_METAL ON)
            message(STATUS "Metal toolchain found — gpu_restart quantizer enabled")
        else()
            message(STATUS "Metal toolchain not found — gpu_restart disabled "
                "(install full Xcode to enable; CommandLineTools alone is not enough)")
        endif()
    endif()

    if(NOT PNG2AMIGA_HAVE_METAL)
        # No Metal toolchain available (Linux / Windows / macOS without
        # full Xcode): compile quantize_metal.cpp here as the gcc/clang
        # TU. With PNG2AMIGA_HAVE_METAL undefined the file collapses to
        # bool/int stubs that quantize_metal_wrap.cpp links against.
        target_sources(png2amiga PRIVATE src/quantize_metal.cpp)
        target_sources(api_pipeline_smoke PRIVATE src/quantize_metal.cpp)
        target_sources(palette_search PRIVATE src/quantize_metal.cpp)
    endif()

    if(PNG2AMIGA_HAVE_METAL)
        # Compile the .metal source to a .metallib at build time, then
        # bake the bytes into a header so the binary is self-contained.
        set(_metal_src ${CMAKE_SOURCE_DIR}/src/quantize_metal_kernels.metal)
        set(_metal_air ${CMAKE_BINARY_DIR}/generated/quantize_metal_kernels.air)
        set(_metal_lib ${CMAKE_BINARY_DIR}/generated/quantize_metal_kernels.metallib)
        set(_metal_hdr ${CMAKE_BINARY_DIR}/generated/quantize_metal_metallib.h)

        add_custom_command(
            OUTPUT  ${_metal_air}
            COMMAND ${PNG2AMIGA_METAL_EXE} -O3 -ffast-math -c ${_metal_src} -o ${_metal_air}
            DEPENDS ${_metal_src}
            COMMENT "MSL → AIR (quantize_metal_kernels)"
            VERBATIM)
        add_custom_command(
            OUTPUT  ${_metal_lib}
            COMMAND ${PNG2AMIGA_METALLIB_EXE} ${_metal_air} -o ${_metal_lib}
            DEPENDS ${_metal_air}
            COMMENT "AIR → metallib"
            VERBATIM)
        add_custom_command(
            OUTPUT  ${_metal_hdr}
            COMMAND ${CMAKE_COMMAND}
                -DINPUT=${_metal_lib}
                -DOUTPUT=${_metal_hdr}
                -DSYMBOL=quantize_metal_metallib_data
                -P ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
            DEPENDS ${_metal_lib} ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
            COMMENT "Embed metallib into header"
            VERBATIM)
        add_custom_target(quantize_metal_kernels DEPENDS ${_metal_hdr})
        add_dependencies(png2amiga quantize_metal_kernels)

        target_compile_definitions(png2amiga PRIVATE PNG2AMIGA_HAVE_METAL=1)
        target_link_libraries(png2amiga PRIVATE
            "-framework Foundation" "-framework Metal" "-framework QuartzCore")

        # quantize_metal.cpp pulls in metal-cpp, which uses Apple-clang-only
        # extensions (ObjC blocks in NSNotification / NSProcessInfo). gcc-15
        # cannot parse those headers. Compile this single TU with apple-clang
        # via a custom command and link the resulting object file into
        # png2amiga. The boundary in quantize_metal.hpp is strict C — only
        # PODs cross — so libc++ (apple-clang) and libstdc++ (gcc-15) STL
        # ABIs never meet across object files. quantize_metal_wrap.cpp on
        # the gcc side does the libstdc++ marshalling.
        execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
            OUTPUT_VARIABLE PNG2AMIGA_MACOS_SDK
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        execute_process(COMMAND xcrun -f clang++
            OUTPUT_VARIABLE PNG2AMIGA_APPLE_CLANGXX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        set(_metal_cpp_obj ${CMAKE_BINARY_DIR}/quantize_metal.cpp.o)
        add_custom_command(
            OUTPUT  ${_metal_cpp_obj}
            COMMAND ${PNG2AMIGA_APPLE_CLANGXX}
                -std=c++23 -O2 -fno-objc-arc -fPIC
                -DPNG2AMIGA_HAVE_METAL=1
                -isysroot ${PNG2AMIGA_MACOS_SDK}
                -I${CMAKE_SOURCE_DIR}/src
                -I${CMAKE_SOURCE_DIR}/third_party
                -I${CMAKE_SOURCE_DIR}/third_party/metal-cpp
                -I${CMAKE_BINARY_DIR}/generated
                -c ${CMAKE_SOURCE_DIR}/src/quantize_metal.cpp
                -o ${_metal_cpp_obj}
            DEPENDS ${CMAKE_SOURCE_DIR}/src/quantize_metal.cpp
                    ${CMAKE_SOURCE_DIR}/src/quantize_metal.hpp
                    ${_metal_hdr}
            COMMENT "Compiling quantize_metal.cpp with apple-clang"
            VERBATIM)
        add_custom_target(quantize_metal_obj DEPENDS ${_metal_cpp_obj})
        add_dependencies(png2amiga quantize_metal_obj)
        target_link_libraries(png2amiga PRIVATE ${_metal_cpp_obj})
        # The apple-clang object uses libc++ symbols (std::__1::*).
        # Link libc++ alongside the gcc libstdc++ — they don't overlap
        # since the boundary in quantize_metal.hpp is strict C, so no
        # std type from one stdlib reaches the other in this binary.
        target_link_libraries(png2amiga PRIVATE -lc++ -lc++abi)

        # api_pipeline_smoke needs the same Metal hook-up.
        target_compile_definitions(api_pipeline_smoke PRIVATE PNG2AMIGA_HAVE_METAL=1)
        target_link_libraries(api_pipeline_smoke PRIVATE
            "-framework Foundation" "-framework Metal" "-framework QuartzCore")
        add_dependencies(api_pipeline_smoke quantize_metal_obj)
        target_link_libraries(api_pipeline_smoke PRIVATE ${_metal_cpp_obj})
        target_link_libraries(api_pipeline_smoke PRIVATE -lc++ -lc++abi)

        # palette_search uses the same core sources (quantize::quantize
        # → ocs_bruteforce → maybe gpu_restart on Metal-enabled). Same
        # link recipe.
        target_compile_definitions(palette_search PRIVATE PNG2AMIGA_HAVE_METAL=1)
        target_link_libraries(palette_search PRIVATE
            "-framework Foundation" "-framework Metal" "-framework QuartzCore")
        add_dependencies(palette_search quantize_metal_obj)
        target_link_libraries(palette_search PRIVATE ${_metal_cpp_obj})
        target_link_libraries(palette_search PRIVATE -lc++ -lc++abi)
    endif()

    enable_testing()

    # Single shared test input. Lives under examples/ (tracked) so a
    # fresh checkout has everything ctest needs — no more local test.png
    # in the repo root that has to be hand-restored after a checkout.
    # Matches recoil_roundtrip.py's default so smoke + RECOIL tests
    # exercise the same source image.
    set(PNG2AMIGA_TEST_INPUT "${CMAKE_SOURCE_DIR}/examples/lovers.jpg")

    # Many ctest cases shell out to python3 -c '...' for cross-platform
    # post-conditions (file size, palette count, image dims). Resolve
    # the real interpreter via find_package so Windows can't fall through
    # to the Microsoft Store python3.exe shim that prints "Python was not
    # found" and exits non-zero.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(PNG2AMIGA_PYTHON "${Python3_EXECUTABLE}")

    # Basic build test: verify the binary runs and shows help
    add_test(
        NAME "help"
        COMMAND png2amiga --help
    )

    # RECOIL round-trip suite. Encodes one input through every IFF-
    # producing combo (including --sliced-best HAM6/HAM8), decodes each IFF
    # via Piotr Fusik's recoil2png, and PSNR-compares against png2amiga's
    # own preview. Skips cleanly if recoil2png isn't installed. Run with:
    #     cmake --build build --target recoil_check
    # or as part of ctest (the recoil ctest case runs the same script
    # and reports SKIP when recoil2png is absent, exit code 77).
    # Artifacts land in build/recoil_check/<combo>/ for inspection.
    add_custom_target(recoil_check
        COMMAND ${PNG2AMIGA_PYTHON}
                ${CMAKE_SOURCE_DIR}/tools/recoil_roundtrip.py
                --build ${CMAKE_BINARY_DIR}/recoil_check
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS png2amiga
        COMMENT "RECOIL round-trip suite — encode → IFF → recoil2png → PSNR"
        VERBATIM
        USES_TERMINAL
    )

    # Same script also wired as an opt-in ctest case (registered ONLY
    # when PNG2AMIGA_ENABLE_RECOIL_TEST is ON — off by default because
    # the round-trip is ~140s, which dominates ctest wall time and
    # delays the inner-loop edit/build/test cycle. Run manually via
    #   cmake --build build --target recoil_check
    # or
    #   cmake -DPNG2AMIGA_ENABLE_RECOIL_TEST=ON -B build .
    # to opt back in.
    option(PNG2AMIGA_ENABLE_RECOIL_TEST
        "Register the RECOIL round-trip suite as a ctest case (~140s)"
        OFF)
    if(PNG2AMIGA_ENABLE_RECOIL_TEST)
        add_test(
            NAME "recoil-roundtrip"
            COMMAND ${PNG2AMIGA_PYTHON}
                    ${CMAKE_SOURCE_DIR}/tools/recoil_roundtrip.py
                    --build ${CMAKE_BINARY_DIR}/recoil_check
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
        set_tests_properties("recoil-roundtrip" PROPERTIES
            SKIP_RETURN_CODE 77
            LABELS "recoil"
            TIMEOUT 600
        )
    endif()

    # --- Raw bitplane output ---
    add_test(
        NAME "raw-output"
        COMMAND png2amiga ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_output.raw
    )
    add_test(
        NAME "raw-output-exists"
        COMMAND ${CMAKE_COMMAND} -E sha256sum ${CMAKE_BINARY_DIR}/test_output.raw
    )
    set_tests_properties("raw-output-exists" PROPERTIES DEPENDS "raw-output")

    # --- kingcon-compat raw outputs: .bpl alias, --mask-layout, --rotate,
    # --flip-x/y, --trim. lovers.jpg is 320×256, 5 bitplanes by default,
    # so raw size = 320/8 × 256 × 5 = 51200 bytes. The mask-layout tests
    # use logo.png because lovers has no transparency.

    # Synthetic 64×32 (width-aligned, transparent border): the explicit
    # dims keep every size-based assertion below predictable regardless
    # of what's in examples/.
    add_test(
        NAME "kingcon-synth-input"
        COMMAND ${PNG2AMIGA_PYTHON} -c
            "from PIL import Image
img=Image.new('RGBA',(64,32),(255,255,255,0))
for y in range(8,24):
    for x in range(16,48):
        img.putpixel((x,y),(255,0,0,255))
img.save('${CMAKE_BINARY_DIR}/kc.png')"
    )

    # 64×32, 4 planes: bpr=8 (4 words × 2), total=32*8*4=1024 bytes.
    # Sibling .pal = 16 colors × 2 = 32 bytes.
    add_test(
        NAME "bpl-extension"
        COMMAND png2amiga --no-scale --depth 4
                ${CMAKE_BINARY_DIR}/kc.png
                ${CMAKE_BINARY_DIR}/kc.bpl
    )
    set_tests_properties("bpl-extension" PROPERTIES DEPENDS "kingcon-synth-input")
    add_test(
        NAME "bpl-extension-size"
        COMMAND ${PNG2AMIGA_PYTHON} -c
            "import os,sys; s=os.path.getsize('${CMAKE_BINARY_DIR}/kc.bpl'); sys.exit(0 if s==1024 else (print(f'expected 1024, got {s}') or 1))"
    )
    set_tests_properties("bpl-extension-size" PROPERTIES DEPENDS "bpl-extension")

    # .bpl writes a sibling .pal automatically (16 colors × 2 bytes).
    add_test(
        NAME "bpl-sibling-pal"
        COMMAND ${PNG2AMIGA_PYTHON} -c
            "import os,sys; s=os.path.getsize('${CMAKE_BINARY_DIR}/kc.pal'); sys.exit(0 if s==32 else (print(f'expected 32-byte sibling .pal, got {s}') or 1))"
    )
    set_tests_properties("bpl-sibling-pal" PROPERTIES DEPENDS "bpl-extension")

    # --rotate 90: 64×32 -> 32×64. bpr=4 (2 words × 2). 64×4×4 planes = 1024.
    add_test(
        NAME "rotate-90"
        COMMAND png2amiga --no-scale --depth 4 --rotate 90
                ${CMAKE_BINARY_DIR}/kc.png
                ${CMAKE_BINARY_DIR}/kc_r90.bpl
    )
    set_tests_properties("rotate-90" PROPERTIES DEPENDS "kingcon-synth-input")
    add_test(
        NAME "rotate-90-size"
        COMMAND ${PNG2AMIGA_PYTHON} -c
            "import os,sys; s=os.path.getsize('${CMAKE_BINARY_DIR}/kc_r90.bpl'); sys.exit(0 if s==1024 else (print(f'expected 1024, got {s}') or 1))"
    )
    set_tests_properties("rotate-90-size" PROPERTIES DEPENDS "rotate-90")

    # --rotate 180: same dims as input.
    add_test(
        NAME "rotate-180"
        COMMAND png2amiga --no-scale --depth 4 --rotate 180
                ${CMAKE_BINARY_DIR}/kc.png
                ${CMAKE_BINARY_DIR}/kc_r180.bpl
    )
    set_tests_properties("rotate-180" PROPERTIES DEPENDS "kingcon-synth-input")

    # --rotate 1 accepts the quarters form (= 90°).
    add_test(
        NAME "rotate-quarters-form"
        COMMAND png2amiga --no-scale --depth 4 --rotate 1
                ${CMAKE_BINARY_DIR}/kc.png
                ${CMAKE_BINARY_DIR}/kc_r1.bpl
    )
    set_tests_properties("rotate-quarters-form" PROPERTIES DEPENDS "kingcon-synth-input")

    # Invalid rotate values are rejected.
    add_test(
        NAME "rotate-rejects-bad-arg"
        COMMAND png2amiga --rotate 45
                ${CMAKE_BINARY_DIR}/kc.png
                ${CMAKE_BINARY_DIR}/kc_bad.bpl
    )
    set_tests_properties("rotate-rejects-bad-arg" PROPERTIES
        DEPENDS "kingcon-synth-input"
        WILL_FAIL TRUE)

    add_test(
        NAME "flip-x"
        COMMAND png2amiga --no-scale --depth 4 --flip-x
                ${CMAKE_BINARY_DIR}/kc.png
                ${CMAKE_BINARY_DIR}/kc_fx.bpl
    )
    set_tests_properties("flip-x" PROPERTIES DEPENDS "kingcon-synth-input")
    add_test(
        NAME "flip-y"
        COMMAND png2amiga --no-scale --depth 4 --flip-y
                ${CMAKE_BINARY_DIR}/kc.png
                ${CMAKE_BINARY_DIR}/kc_fy.bpl
    )
    set_tests_properties("flip-y" PROPERTIES DEPENDS "kingcon-synth-input")

    # --mask-layout against the synthetic 64×32 (RGBA alpha = 0 border).
    # 4 planes, bpr=8. Plain raw = 32*8*4 = 1024 bytes (bpl-extension-size
    # checks this). With mask appended = 32*8*(4+1) = 1280. With mask
    # replicated = 32*8*4*2 = 2048 (kingcon -Interleaved -Mask).
    add_test(
        NAME "mask-layout-appended"
        COMMAND png2amiga --no-scale --depth 4 --mask-layout appended
                ${CMAKE_BINARY_DIR}/kc.png
                ${CMAKE_BINARY_DIR}/kc_mask_app.bpl
    )
    set_tests_properties("mask-layout-appended" PROPERTIES DEPENDS "kingcon-synth-input")
    add_test(
        NAME "mask-layout-appended-size"
        COMMAND ${PNG2AMIGA_PYTHON} -c
            "import os,sys; s=os.path.getsize('${CMAKE_BINARY_DIR}/kc_mask_app.bpl'); sys.exit(0 if s==1280 else (print(f'expected 1280, got {s}') or 1))"
    )
    set_tests_properties("mask-layout-appended-size" PROPERTIES DEPENDS "mask-layout-appended")

    add_test(
        NAME "mask-layout-replicated"
        COMMAND png2amiga --no-scale --depth 4 --mask-layout replicated
                ${CMAKE_BINARY_DIR}/kc.png
                ${CMAKE_BINARY_DIR}/kc_mask_rep.bpl
    )
    set_tests_properties("mask-layout-replicated" PROPERTIES DEPENDS "kingcon-synth-input")
    add_test(
        NAME "mask-layout-replicated-size"
        COMMAND ${PNG2AMIGA_PYTHON} -c
            "import os,sys; s=os.path.getsize('${CMAKE_BINARY_DIR}/kc_mask_rep.bpl'); sys.exit(0 if s==2048 else (print(f'expected 2048, got {s}') or 1))"
    )
    set_tests_properties("mask-layout-replicated-size" PROPERTIES DEPENDS "mask-layout-replicated")

    # --mask-layout rejects unknown values.
    add_test(
        NAME "mask-layout-rejects-bad-arg"
        COMMAND png2amiga --mask-layout bogus
                ${CMAKE_BINARY_DIR}/kc.png
                ${CMAKE_BINARY_DIR}/mask-bad.bpl
    )
    set_tests_properties("mask-layout-rejects-bad-arg" PROPERTIES
        DEPENDS "kingcon-synth-input"
        WILL_FAIL TRUE)

    # --trim shrinks the bbox to the non-transparent content. Synthesize a
    # 64×64 PNG with a known 20×10 red region on white at (10,20) and
    # convert with --transparent-color FFFFFF --trim, then check size.
    add_test(
        NAME "trim-synth-input"
        COMMAND ${PNG2AMIGA_PYTHON} -c
            "from PIL import Image
img=Image.new('RGB',(64,64),(255,255,255))
for y in range(20,30):
    for x in range(10,30):
        img.putpixel((x,y),(255,0,0))
img.save('${CMAKE_BINARY_DIR}/trim_in.png')"
    )
    add_test(
        NAME "trim-encode"
        COMMAND png2amiga --no-scale --depth 4
                --transparent-color FFFFFF --trim
                ${CMAKE_BINARY_DIR}/trim_in.png
                ${CMAKE_BINARY_DIR}/trim_out.bpl
    )
    set_tests_properties("trim-encode" PROPERTIES DEPENDS "trim-synth-input")
    add_test(
        NAME "trim-encode-size"
        COMMAND ${PNG2AMIGA_PYTHON} -c
            "import os,sys
# Trim shrinks 64×64 -> 20×10 (the red block). 20 px -> 2 words = 4 B/row,
# 10 rows × 4 planes × 4 B = 160 bytes (no embedded mask).
expect=10*4*4
got=os.path.getsize('${CMAKE_BINARY_DIR}/trim_out.bpl')
sys.exit(0 if got==expect else (print(f'expected {expect}, got {got}') or 1))"
    )
    set_tests_properties("trim-encode-size" PROPERTIES DEPENDS "trim-encode")

    # --trim with no transparency is a friendly no-op (returns 0, output
    # matches the un-trimmed bytes exactly).
    add_test(
        NAME "trim-no-transparency-noop"
        COMMAND png2amiga --no-scale --depth 5 --trim
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/trim_noop.bpl
    )
    add_test(
        NAME "trim-no-transparency-noop-matches-baseline"
        COMMAND ${CMAKE_COMMAND} -E compare_files
                ${CMAKE_BINARY_DIR}/trim_noop.bpl
                ${CMAKE_BINARY_DIR}/trim_baseline.bpl
    )
    add_test(
        NAME "trim-no-transparency-baseline"
        COMMAND png2amiga --no-scale --depth 5
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/trim_baseline.bpl
    )
    set_tests_properties("trim-no-transparency-noop-matches-baseline"
        PROPERTIES DEPENDS "trim-no-transparency-noop;trim-no-transparency-baseline")

    # --- Palette output (.pal) ---
    add_test(
        NAME "pal-output"
        COMMAND png2amiga ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_output.pal
    )
    add_test(
        NAME "pal-output-exists"
        COMMAND ${CMAKE_COMMAND} -E sha256sum ${CMAKE_BINARY_DIR}/test_output.pal
    )
    set_tests_properties("pal-output-exists" PROPERTIES DEPENDS "pal-output")

    # --- Palette file loading: generate .pal then load it back ---
    # Step 1: Create a hex palette test file
    file(WRITE ${CMAKE_BINARY_DIR}/test_palette.txt
        "000000\nFF0000\n00FF00\n0000FF\nFFFF00\nFF00FF\n00FFFF\nFFFFFF\n"
        "880000\n008800\n000088\n888888\n444444\nCCCCCC\nFF8800\n0088FF\n"
    )
    add_test(
        NAME "palette-load-hex"
        COMMAND png2amiga --palette ${CMAKE_BINARY_DIR}/test_palette.txt
                --depth 4
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_from_hex_palette.iff
    )

    # GIMP palette format test
    file(WRITE ${CMAKE_BINARY_DIR}/test_palette.gpl
        "GIMP Palette\nName: Test\n#comment\n0   0   0\n255 0   0\n0   255 0\n0   0   255\n"
    )
    add_test(
        NAME "palette-load-gpl"
        COMMAND png2amiga --palette ${CMAKE_BINARY_DIR}/test_palette.gpl
                --depth 2
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_from_gpl_palette.png
    )

    # --- HAM6 greedy (fast) ---
    add_test(
        NAME "ham6-fast"
        COMMAND png2amiga --mode ham6
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_ham6_fast.iff
    )

    # --- HAM6 DP optimal ---
    add_test(
        NAME "ham6-optimal"
        COMMAND png2amiga --mode ham6 --ham-beam 32
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_ham6_optimal.iff
    )

    # --- HAM6 DP optimal with preview PNG ---
    add_test(
        NAME "ham6-optimal-png"
        COMMAND png2amiga --mode ham6 --ham-beam 16
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_ham6_optimal.png
    )

    # --- HAM8 DP optimal ---
    add_test(
        NAME "ham8-optimal"
        COMMAND png2amiga --mode ham8 --ham-beam 16
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_ham8_optimal.iff
    )

    # --- HAM6 DP: verify optimal has lower error than greedy ---
    # (This is a structural test: the DP output file should exist)
    add_test(
        NAME "ham6-optimal-exists"
        COMMAND ${CMAKE_COMMAND} -E sha256sum ${CMAKE_BINARY_DIR}/test_ham6_optimal.iff
    )
    set_tests_properties("ham6-optimal-exists" PROPERTIES DEPENDS "ham6-optimal")



    # --- HAM6 dithered (greedy, Floyd-Steinberg) ---
    add_test(
        NAME "ham6-dithered-fast"
        COMMAND png2amiga --mode ham6 --dither floyd-steinberg
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_ham6_dithered_fast.png
    )

    # --- HAM6 dithered (DP optimal, Floyd-Steinberg) ---
    add_test(
        NAME "ham6-dithered-optimal"
        COMMAND png2amiga --mode ham6 --ham-beam 16
                --dither floyd-steinberg
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_ham6_dithered_optimal.png
    )

    # --- HAM8 dithered (greedy, Atkinson) ---
    add_test(
        NAME "ham8-dithered-atkinson"
        COMMAND png2amiga --mode ham8 --dither atkinson
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_ham8_dithered_atkinson.iff
    )

    # IFF CMAP loading: generate an IFF file, then load its palette
    add_test(
        NAME "palette-iff-gen"
        COMMAND png2amiga --depth 4
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_iff_for_pal.iff
    )
    add_test(
        NAME "palette-load-iff"
        COMMAND png2amiga --palette ${CMAKE_BINARY_DIR}/test_iff_for_pal.iff
                --depth 4
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_from_iff_palette.png
    )
    set_tests_properties("palette-load-iff" PROPERTIES DEPENDS "palette-iff-gen")

    # ----- --lock-index / --pin-index-at -----

    # Single lock at index 0 in lores: replaces the implicit black-at-0
    add_test(
        NAME "lock-index-lores-single"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 0 ff00ff
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_lock_lores_single.iff
    )

    # Multiple locks in lores
    add_test(
        NAME "lock-index-lores-multi"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 0 000000
                --lock-index 5 ff0000
                --lock-index 31 ffffff
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_lock_lores_multi.iff
    )

    # Round-trip: load the locked IFF as a palette, depth 5 mode
    add_test(
        NAME "lock-index-roundtrip"
        COMMAND png2amiga --mode lores --depth 5
                --palette ${CMAKE_BINARY_DIR}/test_lock_lores_multi.iff
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_lock_roundtrip.iff
    )
    set_tests_properties("lock-index-roundtrip" PROPERTIES DEPENDS "lock-index-lores-multi")

    # Pin in lores: deterministic ordered dither so the source pixel resolves
    # the same on every run.
    add_test(
        NAME "pin-index-lores"
        COMMAND png2amiga --mode lores --depth 5 --dither bayer4x4
                --pin-index-at 5 100 50
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_pin_lores.iff
    )

    # Lock + pin combination
    add_test(
        NAME "lock-and-pin-lores"
        COMMAND png2amiga --mode lores --depth 5 --dither bayer4x4
                --lock-index 0 ff00ff
                --lock-index 31 ffff00
                --pin-index-at 5 100 50
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_lock_and_pin.iff
    )

    # Hex prefix variants (#rrggbb, 0xrrggbb, 3-digit shorthand)
    add_test(
        NAME "lock-index-hex-prefix"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 0 "#ff00ff"
                --lock-index 1 0xff0000
                --lock-index 2 f0c
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_lock_hex_prefix.iff
    )

    # Lock in EHB mode (operates on the 32 base colors)
    add_test(
        NAME "lock-index-ehb"
        COMMAND png2amiga --mode ehb
                --lock-index 0 000000
                --lock-index 1 ff0000
                --lock-index 31 ffffff
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_lock_ehb.iff
    )

    # Pin in EHB: deterministic dither so the resolved index is stable
    add_test(
        NAME "pin-index-ehb"
        COMMAND png2amiga --mode ehb --dither bayer4x4
                --pin-index-at 5 100 50
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_pin_ehb.iff
    )

    # Lock applied on top of a loaded user palette
    add_test(
        NAME "lock-index-with-user-palette"
        COMMAND png2amiga --mode lores --depth 4
                --palette ${CMAKE_BINARY_DIR}/test_palette.txt
                --lock-index 0 ffffff
                --lock-index 15 000000
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_lock_user_pal.iff
    )
    set_tests_properties("lock-index-with-user-palette"
        PROPERTIES DEPENDS "palette-load-hex")

    # ----- Rejection cases (WILL_FAIL) -----

    # HAM rejects locks
    add_test(
        NAME "lock-index-ham-rejected"
        COMMAND png2amiga --mode ham6
                --lock-index 0 ff0000
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_reject_ham_lock.iff
    )
    set_tests_properties("lock-index-ham-rejected" PROPERTIES WILL_FAIL TRUE)

    # HAM rejects pins
    add_test(
        NAME "pin-index-ham-rejected"
        COMMAND png2amiga --mode ham6
                --pin-index-at 5 100 50
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_reject_ham_pin.iff
    )
    set_tests_properties("pin-index-ham-rejected" PROPERTIES WILL_FAIL TRUE)

    # HAM6 + STRIPS: encoder produces both bitplane data and a per-line
    # copper move table; the .h emitter must wire strips_line_moves so
    # the STRIPS copper list lands in the header (without it the .h
    # silently degrades to plain HAM6 against the base 16-colour
    # palette and the STRIPS gain disappears).
    add_test(
        NAME "smoke-ham6-strips-h"
        COMMAND png2amiga --mode ham6 --strips --symbol h6strips
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/ham6-strips.h
    )
    add_test(
        NAME "smoke-ham6-strips-h-content"
        COMMAND ${PNG2AMIGA_PYTHON} -c
            "content = open('${CMAKE_BINARY_DIR}/ham6-strips.h').read(); \
            assert 'strips_ham6_ocs' in content, 'strips_ham6_ocs label missing'; \
            assert 'h6strips_strips_copper_list' in content, \
                'strips copper list missing'"
    )
    set_tests_properties("smoke-ham6-strips-h-content"
                         PROPERTIES DEPENDS "smoke-ham6-strips-h")

    # --tile: pre-replicate input 3x3, run pipeline, crop centre. Verify
    # that the encoded .h has the requested centre dimensions (W x H,
    # NOT 3W x 3H), and that the same flag is rejected on a HAM mode.
    add_test(
        NAME "smoke-tile-lores-h"
        COMMAND png2amiga --mode lores --depth 5 --tile
                --width 64 --height 64 --symbol tile_smoke
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/tile-smoke.h
    )
    add_test(
        NAME "smoke-tile-lores-h-dims"
        COMMAND ${PNG2AMIGA_PYTHON} -c
            "content = open('${CMAKE_BINARY_DIR}/tile-smoke.h').read(); \
            import re; \
            w = int(re.search(r'TILE_SMOKE_WIDTH\\s+(\\d+)', content).group(1)); \
            h = int(re.search(r'TILE_SMOKE_HEIGHT\\s+(\\d+)', content).group(1)); \
            assert w == 64 and h == 64, f'expected 64x64 centre crop, got {w}x{h}'"
    )
    set_tests_properties("smoke-tile-lores-h-dims"
                         PROPERTIES DEPENDS "smoke-tile-lores-h")
    add_test(
        NAME "tile-rejects-ham"
        COMMAND png2amiga --mode ham6 --tile --width 64 --height 64
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/tile-reject-ham.png
    )
    set_tests_properties("tile-rejects-ham" PROPERTIES WILL_FAIL TRUE)
    add_test(
        NAME "tile-rejects-sliced"
        COMMAND png2amiga --mode lores --depth 5 --sliced --tile
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/tile-reject-sliced.png
    )
    set_tests_properties("tile-rejects-sliced" PROPERTIES WILL_FAIL TRUE)
    add_test(
        NAME "tile-rejects-vga"
        COMMAND png2amiga --mode vga-13h --tile
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/tile-reject-vga.png
    )
    set_tests_properties("tile-rejects-vga" PROPERTIES WILL_FAIL TRUE)

    # --fade-to: bitmap encodes once, palette interpolates through one or
    # more target images. v1 supports lores/hires only; --preview animates.
    add_test(
        NAME "smoke-fade-to-lores"
        COMMAND png2amiga --mode lores --depth 5
                --fade-to ${CMAKE_SOURCE_DIR}/examples/tile/diamond.png
                --fade-frames 4
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/fade-smoke.png
    )
    add_test(
        NAME "smoke-fade-to-multi"
        COMMAND png2amiga --mode lores --depth 5
                --fade-to ${CMAKE_SOURCE_DIR}/examples/tile/diamond.png,${CMAKE_SOURCE_DIR}/examples/tile/plasma.png,${CMAKE_SOURCE_DIR}/examples/tile/ramp_horizontal.png
                --fade-frames 4 --fade-loop
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/fade-multi.png
    )
    add_test(
        NAME "fade-to-rejects-ham"
        COMMAND png2amiga --mode ham6
                --fade-to ${CMAKE_SOURCE_DIR}/examples/tile/diamond.png
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/fade-reject-ham.png
    )
    set_tests_properties("fade-to-rejects-ham" PROPERTIES WILL_FAIL TRUE)
    add_test(
        NAME "fade-to-rejects-sliced"
        COMMAND png2amiga --mode lores --depth 5 --sliced
                --fade-to ${CMAKE_SOURCE_DIR}/examples/tile/diamond.png
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/fade-reject-sliced.png
    )
    set_tests_properties("fade-to-rejects-sliced" PROPERTIES WILL_FAIL TRUE)
    add_test(
        NAME "fade-to-rejects-strips"
        COMMAND png2amiga --mode ehb --strips
                --fade-to ${CMAKE_SOURCE_DIR}/examples/tile/diamond.png
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/fade-reject-strips.png
    )
    set_tests_properties("fade-to-rejects-strips" PROPERTIES WILL_FAIL TRUE)
    add_test(
        NAME "fade-to-rejects-ehb-sliced"
        COMMAND png2amiga --mode ehb --sliced
                --fade-to ${CMAKE_SOURCE_DIR}/examples/tile/diamond.png
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/fade-reject-ehb-sliced.png
    )
    set_tests_properties("fade-to-rejects-ehb-sliced" PROPERTIES WILL_FAIL TRUE)
    add_test(
        NAME "smoke-fade-to-ehb"
        COMMAND png2amiga --mode ehb
                --fade-to ${CMAKE_SOURCE_DIR}/examples/tile/diamond.png,${CMAKE_SOURCE_DIR}/examples/tile/plasma.png
                --fade-frames 4
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/fade-ehb.png
    )

    # --mask: transparency mask export in 4 formats (.png / .iff / .raw / .h).
    # Uses logo.png since lovers.jpg has no alpha channel (save_mask early-
    # returns with "no transparency in source image" for opaque inputs).
    set(MASK_TEST_INPUT "${CMAKE_SOURCE_DIR}/examples/logo.png")
    foreach(ext IN ITEMS png iff raw h)
        add_test(
            NAME "mask-format-${ext}"
            COMMAND png2amiga --mode lores --depth 5
                    --mask ${CMAKE_BINARY_DIR}/mask-out.${ext}
                    ${MASK_TEST_INPUT}
                    ${CMAKE_BINARY_DIR}/mask-main.iff
        )
        add_test(
            NAME "mask-format-${ext}-exists"
            COMMAND ${CMAKE_COMMAND} -E
                    compare_files ${CMAKE_BINARY_DIR}/mask-out.${ext}
                                  ${CMAKE_BINARY_DIR}/mask-out.${ext}
        )
        set_tests_properties("mask-format-${ext}-exists"
                             PROPERTIES DEPENDS "mask-format-${ext}")
    endforeach()
    # Validate the .h variant's content: must have header guard, WIDTH/
    # HEIGHT/DEPTH/BPR/CAMG defines, and a plane0 array. Symbol derives
    # from the output stem so we check for the lowercased stem.
    add_test(
        NAME "mask-format-h-content"
        COMMAND python3 -c
            "import sys,re; \
            content = open('${CMAKE_BINARY_DIR}/mask-out.h').read(); \
            ok = all(re.search(p, content) for p in [ \
                r'#ifndef\\s+MASK_OUT_H', \
                r'#define\\s+MASK_OUT_WIDTH\\s+320', \
                r'#define\\s+MASK_OUT_HEIGHT\\s+\\d+', \
                r'#define\\s+MASK_OUT_DEPTH\\s+1', \
                r'#define\\s+MASK_OUT_BPR\\s+40', \
                r'const\\s+UWORD\\s+mask_out_plane0\\[\\]', \
            ]); \
            sys.exit(0 if ok else (print('mask .h missing expected defines/symbol') or 1))"
    )
    set_tests_properties("mask-format-h-content"
                         PROPERTIES DEPENDS "mask-format-h")

    # Copper supports locks (fixed palette slots for blitter objects)
    add_test(
        NAME "lock-index-copper-accepted"
        COMMAND png2amiga --mode lores --depth 5 --copper
                --lock-index 0 ff0000
                ${PNG2AMIGA_TEST_INPUT}
                -o ${CMAKE_BINARY_DIR}/test_copper_lock.png
    )

    # Out-of-range lock index (depth 5 → max_colors 32, index 32 invalid)
    add_test(
        NAME "lock-index-out-of-range"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 32 ff0000
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_reject_lock_oob.iff
    )
    set_tests_properties("lock-index-out-of-range" PROPERTIES WILL_FAIL TRUE)

    # Duplicate lock index
    add_test(
        NAME "lock-index-duplicate"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 5 ff0000
                --lock-index 5 00ff00
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_reject_lock_dup.iff
    )
    set_tests_properties("lock-index-duplicate" PROPERTIES WILL_FAIL TRUE)

    # Pin out of image bounds (test.png scales to 320x119 by default)
    add_test(
        NAME "pin-index-out-of-bounds"
        COMMAND png2amiga --mode lores --depth 5
                --pin-index-at 5 9999 9999
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_reject_pin_oob.iff
    )
    set_tests_properties("pin-index-out-of-bounds" PROPERTIES WILL_FAIL TRUE)

    # Pin targets a locked slot — semantically composes (lock pins
    # the slot's COLOUR; pin pushes a specific PIXEL into that slot;
    # apply_pins rewrites the pixel's index without swapping the
    # palette so the lock invariant holds). Should succeed.
    add_test(
        NAME "pin-and-lock-same-slot"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 5 ff0000
                --pin-index-at 5 100 50
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_pin_and_lock_same_slot.iff
    )

    # Pin targets a reserved slot — genuine conflict (reserve bans
    # image pixels from routing to the slot, pin demands one).
    add_test(
        NAME "pin-index-targets-reserved"
        COMMAND png2amiga --mode lores --depth 5
                --reserve-range 5 ff0000
                --pin-index-at 5 100 50
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_reject_pin_reserved.iff
    )
    set_tests_properties("pin-index-targets-reserved" PROPERTIES WILL_FAIL TRUE)

    # Bad hex format
    add_test(
        NAME "lock-index-bad-hex"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 0 zzzzzz
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_reject_lock_hex.iff
    )
    set_tests_properties("lock-index-bad-hex" PROPERTIES WILL_FAIL TRUE)

    # --- Smoke tests: new DOS / Atari / text modes encode without error ---
    # One test per mode, PNG output so correctness is visually inspectable and
    # the existence check guards against regressions.
    foreach(mode_pair
        "stf-low;stf_low"
        "stf-med;stf_med"
        "stf-hi;stf_hi"
        "ste-low;ste_low"
        "ste-med;ste_med"
        "ste-hi;ste_hi"
        "cga-320;cga_320"
        "cga-640;cga_640"
        "cga-composite;cga_composite"
        "cga-text80x100;cga_text80x100"
        "ega-320;ega_320"
        "ega-640;ega_640"
        "ega-hi;ega_hi"
        "vga-13h;vga_13h"
        "vga-10h;vga_10h"
        "vga-12h;vga_12h"
    )
        list(GET mode_pair 0 mode_flag)
        list(GET mode_pair 1 mode_slug)
        add_test(
            NAME "smoke-${mode_slug}"
            COMMAND png2amiga --mode ${mode_flag}
                    ${PNG2AMIGA_TEST_INPUT}
                    ${CMAKE_BINARY_DIR}/smoke_${mode_slug}.png
        )
        add_test(
            NAME "smoke-${mode_slug}-exists"
            COMMAND ${CMAKE_COMMAND} -E sha256sum
                    ${CMAKE_BINARY_DIR}/smoke_${mode_slug}.png
        )
        set_tests_properties("smoke-${mode_slug}-exists"
                             PROPERTIES DEPENDS "smoke-${mode_slug}")
    endforeach()

    # Degas output: .pi1 (low), .pi2 (med), .pi3 (hi).
    add_test(
        NAME "smoke-degas-pi1"
        COMMAND png2amiga --mode ste-low
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/smoke.pi1
    )
    add_test(
        NAME "smoke-degas-pi2"
        COMMAND png2amiga --mode ste-med
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/smoke.pi2
    )
    add_test(
        NAME "smoke-degas-pi3"
        COMMAND png2amiga --mode ste-hi
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/smoke.pi3
    )
    foreach(ext pi1 pi2 pi3)
        add_test(
            NAME "smoke-degas-${ext}-exists"
            COMMAND ${CMAKE_COMMAND} -E sha256sum
                    ${CMAKE_BINARY_DIR}/smoke.${ext}
        )
        set_tests_properties("smoke-degas-${ext}-exists"
                             PROPERTIES DEPENDS "smoke-degas-${ext}")
    endforeach()

    # --reserve-range smoke tests. Each mode encodes once with a
    # mode-appropriate reserve range, then a python helper checks the
    # produced .h has the magenta default colour exactly N times in
    # the palette (where N = reserved-slot count).
    set(reserve_specs
        "lores|24-31|--mode lores --depth 5"
        "hires|12-15|--mode hires --depth 4"
        "vga-13h|248-255|--mode vga-13h"
        "ehb|24-31|--mode ehb"
        "cap-lores|28-31|--mode lores --copper --depth 5"
        "cap-ehb|28-31|--mode ehb --copper"
        "scap-ehb|28-31|--mode ehb --strips"
        "stf-low|12-15|--mode stf-low"
        "ste-low|12-15|--mode ste-low"
        "stf-med|2-3|--mode stf-med"
        "ste-med|2-3|--mode ste-med"
        "ega-hi|12-15|--mode ega-hi"
    )
    foreach(spec ${reserve_specs})
        string(REPLACE "|" ";" parts ${spec})
        list(GET parts 0 mname)
        list(GET parts 1 range)
        list(GET parts 2 mode_args)
        separate_arguments(mode_args UNIX_COMMAND "${mode_args}")
        # Compute expected reserve count from the range (single hyphen).
        string(REPLACE "-" ";" rparts ${range})
        list(GET rparts 0 r_lo)
        list(GET rparts 1 r_hi)
        math(EXPR r_count "${r_hi} - ${r_lo} + 1")
        add_test(
            NAME "reserve-${mname}"
            COMMAND png2amiga ${mode_args}
                    --reserve-range ${range} ff00cc
                    --symbol reserve_test
                    ${PNG2AMIGA_TEST_INPUT}
                    ${CMAKE_BINARY_DIR}/reserve-${mname}.h
        )
        # Validate by checking that all r_count slots in the reserve range
        # hold the SAME value (mode-agnostic — different modes quantize the
        # default colour differently, but reserved slots must all overlay
        # to the identical post-quantization value).
        add_test(
            NAME "reserve-${mname}-count"
            COMMAND ${PNG2AMIGA_PYTHON} -c
                "import re,sys; \
                content = open('${CMAKE_BINARY_DIR}/reserve-${mname}.h').read(); \
                m = re.search(r'_palette\\[\\] = \\{([^}]+)\\}', content); \
                vals = re.findall(r'0x[0-9A-Fa-f]+', m.group(1)); \
                slice_ = vals[${r_lo}:${r_hi}+1]; \
                ok = len(slice_) == ${r_count} and len(set(slice_)) == 1; \
                sys.exit(0 if ok else (print(f'expected ${r_count} identical reserves at slots ${r_lo}..${r_hi}, got '+str(slice_)) or 1))"
        )
        set_tests_properties("reserve-${mname}-count"
                             PROPERTIES DEPENDS "reserve-${mname}")
    endforeach()

    # Fixed-palette EGA modes (ega-320, ega-640) keep their IRGB ordering,
    # so the "all reserved slots identical" check above doesn't apply.
    # Verify exclusion instead: render to PNG and confirm the image never
    # uses the four high IRGB colours (slots 12-15: FF5555, FF55FF,
    # FFFF55, FFFFFF).
    set(reserve_ega_excl
        "ega-320|--mode ega-320"
        "ega-640|--mode ega-640"
    )
    foreach(spec ${reserve_ega_excl})
        string(REPLACE "|" ";" parts ${spec})
        list(GET parts 0 mname)
        list(GET parts 1 mode_args)
        separate_arguments(mode_args UNIX_COMMAND "${mode_args}")
        add_test(
            NAME "reserve-${mname}"
            COMMAND png2amiga ${mode_args}
                    --reserve-range 12-15 ff00cc
                    --symbol reserve_test
                    ${PNG2AMIGA_TEST_INPUT}
                    ${CMAKE_BINARY_DIR}/reserve-${mname}.png
        )
        add_test(
            NAME "reserve-${mname}-excl"
            COMMAND ${PNG2AMIGA_PYTHON} -c
                "from PIL import Image; \
                img = Image.open('${CMAKE_BINARY_DIR}/reserve-${mname}.png').convert('RGB'); \
                colors = set(img.getdata()); \
                forbidden = {(0xFF,0x55,0x55), (0xFF,0x55,0xFF), (0xFF,0xFF,0x55), (0xFF,0xFF,0xFF)}; \
                used = colors & forbidden; \
                __import__('sys').exit(0 if not used else (print(f'reserved IRGB slots 12-15 leaked into image: {sorted(used)}') or 1))"
        )
        set_tests_properties("reserve-${mname}-excl"
                             PROPERTIES DEPENDS "reserve-${mname}")
    endforeach()

    # Sega Genesis: .bin (tiles + tilemap + CRAM) and .h (SGDK header).
    set(genesis_outputs
        "h32.bin|--mode genesis-h32"
        "h40.bin|--mode genesis-h40"
        "h40.h|--mode genesis-h40 --symbol genesis_test"
    )
    foreach(spec ${genesis_outputs})
        string(REPLACE "|" ";" spec_parts ${spec})
        list(GET spec_parts 0 ext)
        list(GET spec_parts 1 mode_args)
        separate_arguments(mode_args UNIX_COMMAND "${mode_args}")
        add_test(
            NAME "smoke-genesis-${ext}"
            COMMAND png2amiga ${mode_args}
                    ${PNG2AMIGA_TEST_INPUT}
                    ${CMAKE_BINARY_DIR}/smoke-genesis.${ext}
        )
        add_test(
            NAME "smoke-genesis-${ext}-exists"
            COMMAND ${CMAKE_COMMAND} -E sha256sum
                    ${CMAKE_BINARY_DIR}/smoke-genesis.${ext}
        )
        set_tests_properties("smoke-genesis-${ext}-exists"
                             PROPERTIES DEPENDS "smoke-genesis-${ext}")
    endforeach()

    # SNES Mode 7: 256-mode (.bin tiles+tilemap+palette, .h header) +
    # Direct (.bin: pixels self-decode, no palette emitted).
    set(snes_outputs
        "256.bin|--mode snes-mode7-256"
        "256.h|--mode snes-mode7-256 --symbol snes_test"
        "direct.bin|--mode snes-mode7-direct"
        "direct.h|--mode snes-mode7-direct --symbol snes_direct_test"
    )
    foreach(spec ${snes_outputs})
        string(REPLACE "|" ";" spec_parts ${spec})
        list(GET spec_parts 0 ext)
        list(GET spec_parts 1 mode_args)
        separate_arguments(mode_args UNIX_COMMAND "${mode_args}")
        add_test(
            NAME "smoke-snes-${ext}"
            COMMAND png2amiga ${mode_args}
                    ${PNG2AMIGA_TEST_INPUT}
                    ${CMAKE_BINARY_DIR}/smoke-snes.${ext}
        )
        add_test(
            NAME "smoke-snes-${ext}-exists"
            COMMAND ${CMAKE_COMMAND} -E sha256sum
                    ${CMAKE_BINARY_DIR}/smoke-snes.${ext}
        )
        set_tests_properties("smoke-snes-${ext}-exists"
                             PROPERTIES DEPENDS "smoke-snes-${ext}")
    endforeach()

    # C64 — every (mode, output-format) combo we promise. Bitmap modes:
    # PNG preview, .raw bytes, .prg displayer; .koa for multicolor only;
    # .hir for hires only. Charset modes: PNG, .raw, .h header. PETSCII
    # is text-only (PNG + .prg).
    set(c64_outputs
        "multicolor.png|--mode c64-multicolor"
        "multicolor.raw|--mode c64-multicolor"
        "multicolor.prg|--mode c64-multicolor"
        "multicolor.koa|--mode c64-multicolor"
        "hires.png|--mode c64-hires"
        "hires.raw|--mode c64-hires"
        "hires.prg|--mode c64-hires"
        "hires.hir|--mode c64-hires"
        "fli.png|--mode c64-fli"
        "fli.prg|--mode c64-fli"
        "afli.png|--mode c64-afli"
        "afli.prg|--mode c64-afli"
        "petscii.png|--mode c64-petscii"
        "petscii.prg|--mode c64-petscii"
        "charsethi.png|--mode c64-charset-hires"
        "charsethi.raw|--mode c64-charset-hires"
        "charsethi.h|--mode c64-charset-hires --symbol charsethi_test"
        "charsetmc.png|--mode c64-charset-multicolor"
        "charsetmc.raw|--mode c64-charset-multicolor"
        "charsetmc.h|--mode c64-charset-multicolor --symbol charsetmc_test"
    )
    foreach(spec ${c64_outputs})
        string(REPLACE "|" ";" spec_parts ${spec})
        list(GET spec_parts 0 ext)
        list(GET spec_parts 1 mode_args)
        separate_arguments(mode_args UNIX_COMMAND "${mode_args}")
        add_test(
            NAME "smoke-c64-${ext}"
            COMMAND png2amiga ${mode_args}
                    ${PNG2AMIGA_TEST_INPUT}
                    ${CMAKE_BINARY_DIR}/smoke-c64.${ext}
        )
        add_test(
            NAME "smoke-c64-${ext}-exists"
            COMMAND ${CMAKE_COMMAND} -E sha256sum
                    ${CMAKE_BINARY_DIR}/smoke-c64.${ext}
        )
        set_tests_properties("smoke-c64-${ext}-exists"
                             PROPERTIES DEPENDS "smoke-c64-${ext}")
    endforeach()

    # --- No-output (--preview only / scoring) regression tests ---
    # Bug 2026-05-03: invoking modes without an output path (just
    # `<input>`, no `-o`) used to fail with "PNG write error: Failed
    # to write PNG: " on Genesis / SNES / C64 because their emission
    # blocks fell through to save_preview() with an empty path. Each
    # mode-dispatch path now has an `if (config->output_path.empty())`
    # short-circuit before the emission switch.
    set(no_output_modes
        "genesis-h32|--mode genesis-h32"
        "genesis-h40|--mode genesis-h40"
        "snes-mode7-256|--mode snes-mode7-256"
        "snes-mode7-direct|--mode snes-mode7-direct"
        "c64-multicolor|--mode c64-multicolor"
        "c64-hires|--mode c64-hires"
        "c64-fli|--mode c64-fli"
        "c64-afli|--mode c64-afli"
        "c64-petscii|--mode c64-petscii"
        "c64-charset-hires|--mode c64-charset-hires"
    )
    foreach(spec ${no_output_modes})
        string(REPLACE "|" ";" parts ${spec})
        list(GET parts 0 mname)
        list(GET parts 1 mode_args)
        separate_arguments(mode_args UNIX_COMMAND "${mode_args}")
        add_test(
            NAME "no-output-${mname}"
            COMMAND png2amiga ${mode_args} ${PNG2AMIGA_TEST_INPUT}
        )
        # Must NOT print "PNG write error" — even if the binary
        # exits 0, the stderr leak signals the bug.
        set_tests_properties("no-output-${mname}"
            PROPERTIES FAIL_REGULAR_EXPRESSION "PNG write error")
    endforeach()

    # ----------------------------------------------------------------
    # Reserve-palette + lock-index regression suite.
    #
    # These exercise the failure modes we hit during the May 2026
    # reserve-handling rewrite. Each one corresponds to a specific
    # bug we either fixed or nearly broke; without these, regressing
    # the palette assembly logic is silent until a user notices.
    #
    # tools/check_palette.py runs the encoder with the given args,
    # parses the IFF CMAP, and asserts:
    #   --expect IDX=RRGGBB  exact slot colour
    #   --expect-not-zero N  slot is NOT (0,0,0) — catches the
    #                        "tail unfilled because reserves displaced
    #                         quantizer entries" bug class.
    #   --expect-unique-min  loose lower bound on distinct colours —
    #                        catches "dedupe ate too many quantizer
    #                         entries, palette collapsed".
    set(_check_pal "${PNG2AMIGA_PYTHON};${CMAKE_SOURCE_DIR}/tools/check_palette.py;--bin;$<TARGET_FILE:png2amiga>")
    set(_pal_test_in "${CMAKE_SOURCE_DIR}/examples/lovers.jpg")
    set(_pal_makena_in "${CMAKE_SOURCE_DIR}/examples/makena.jpg")

    # Bug 1 (3bpp + reserve 1-4 ff0000 → all-black image).
    # The original report. Reserves at low indices used to displace
    # the quantizer fill at slots 1-3, leaving slots 5-7 default-
    # black, leaving the dither only black to choose from.
    add_test(NAME "reserve-3bpp-low-reserves-fills-tail"
        COMMAND ${_check_pal}
            --in ${_pal_test_in}
            --out ${CMAKE_BINARY_DIR}/test_reserve_3bpp_low.iff
            --expect 0=000000
            --expect 1=ff0000 --expect 2=ff0000
            --expect 3=ff0000 --expect 4=ff0000
            --expect-not-zero 5 --expect-not-zero 6 --expect-not-zero 7
            --expect-unique-min 4
            --
            --mode lores --depth 3 --reserve-range 1-4 ff0000)

    # Bug 2 (5bpp + 26 reserves to white → 6 colours used, dither has
    # access to a balanced gamut at slots 1-5).
    add_test(NAME "reserve-5bpp-tail-reserves-white"
        COMMAND ${_check_pal}
            --in ${_pal_makena_in}
            --out ${CMAKE_BINARY_DIR}/test_reserve_5bpp_white.iff
            --expect 0=000000
            --expect 6=ffffff --expect 15=ffffff --expect 31=ffffff
            --expect-not-zero 1 --expect-not-zero 2 --expect-not-zero 3
            --expect-not-zero 4 --expect-not-zero 5
            --expect-unique-min 7
            --
            --mode lores --reserve-range 6-31 FFFFFF)

    # Bug 3 (5bpp + reserves at the natural bright tail — web's panel
    # produces this when the user clicks slots 6-31 of an existing
    # palette). Reserves bit-match quantizer naturals, so dedupe-
    # against-locked used to drop them and the unreserved tail came
    # back unfilled. With the reserve-skip mask in assemble, slots
    # 1-5 fill with the natural dark/mid colours.
    add_test(NAME "reserve-5bpp-natural-tail-fills"
        COMMAND ${_check_pal}
            --in ${_pal_makena_in}
            --out ${CMAKE_BINARY_DIR}/test_reserve_5bpp_natural.iff
            --expect 0=000000
            --expect 6=eebb88 --expect 7=aa8877 --expect 31=ffdd66
            --expect-not-zero 1 --expect-not-zero 2 --expect-not-zero 3
            --expect-not-zero 4 --expect-not-zero 5
            --expect-unique-min 18
            --
            --mode lores
            --reserve-range 6 eebb88 --reserve-range 7 aa8877
            --reserve-range 8 77aacc --reserve-range 9 ddaa88
            --reserve-range 10 ffcc77 --reserve-range 11 6688bb
            --reserve-range 12 888888 --reserve-range 13 88aacc
            --reserve-range 14 bbaaaa --reserve-range 15 eeccaa
            --reserve-range 16 ffcc66 --reserve-range 17 bb8877
            --reserve-range 18 bb9999 --reserve-range 19 dd9966
            --reserve-range 20 aabbcc --reserve-range 21 ddbb99
            --reserve-range 22 eeddaa --reserve-range 23 ffcc44
            --reserve-range 24 99bbcc --reserve-range 25 ffaa33
            --reserve-range 26 eeaa44 --reserve-range 27 99ccdd
            --reserve-range 28 eebb99 --reserve-range 29 ffcc55
            --reserve-range 30 eeddbb --reserve-range 31 ffdd66)

    # Bug 4 (5bpp + reserve 1-15 to black — every "dark" slot pinned).
    # Quantizer should produce a 16-color palette tuned for the
    # unreserved-and-unlocked slots (16..31), giving the dither
    # balanced coverage in those slots even though the user nuked
    # the dark gamut.
    add_test(NAME "reserve-5bpp-many-low-blacks-balanced-tail"
        COMMAND ${_check_pal}
            --in ${_pal_makena_in}
            --out ${CMAKE_BINARY_DIR}/test_reserve_5bpp_blacks.iff
            --expect 0=000000
            --expect 1=000000 --expect 8=000000 --expect 15=000000
            --expect-not-zero 16 --expect-not-zero 17
            --expect-not-zero 23 --expect-not-zero 31
            --expect-unique-min 17
            --
            --mode lores --reserve-range 1-15 000000)

    # Bug 5 (slot 0 implicitly locked to black via --lock-color0
    # default; --no-lock-color0 should let the quantizer use slot 0).
    add_test(NAME "reserve-no-lock-color0-allows-quantizer-at-zero"
        COMMAND ${_check_pal}
            --in ${_pal_makena_in}
            --out ${CMAKE_BINARY_DIR}/test_reserve_no_lock0.iff
            --expect 7=ff00ff
            --expect-not-zero 0 --expect-not-zero 1 --expect-not-zero 2
            --expect-unique-min 8
            --
            --mode lores --depth 3 --no-lock-color0 --lock-index 7 ff00ff)

    # Bug 6 (--lock-index works alongside reserves: lock at idx 7,
    # reserves at 1-4, both should land at their requested colours,
    # tail still filled).
    add_test(NAME "reserve-with-lock-index-coexists"
        COMMAND ${_check_pal}
            --in ${_pal_test_in}
            --out ${CMAKE_BINARY_DIR}/test_reserve_with_lock.iff
            --expect 0=000000
            --expect 1=ff0000 --expect 2=ff0000
            --expect 7=ff00ff
            --expect-not-zero 3 --expect-not-zero 4 --expect-not-zero 5
            --expect-not-zero 6
            --expect-unique-min 6
            --
            --mode lores --depth 3
            --reserve-range 1-2 ff0000
            --lock-index 7 ff00ff)

    # Bug 7 (--sliced + reserve 1-15 → CMAP slot 0 + slots 16-31 full,
    # slots 1-15 = reserve colour). Sliced/copper has its own palette
    # build path inside copper.cpp; we patched it to subtract locked
    # count from k1 and skip locked indices when filling so the
    # quantizer's darks land at unreserved slots, not at locked ones
    # that get overwritten.
    add_test(NAME "reserve-sliced-low-reserves-fills-tail"
        COMMAND ${_check_pal}
            --in ${_pal_makena_in}
            --out ${CMAKE_BINARY_DIR}/test_reserve_sliced.iff
            --expect 0=000000
            --expect 1=ff0000 --expect 8=ff0000 --expect 15=ff0000
            --expect-not-zero 16 --expect-not-zero 17
            --expect-not-zero 23 --expect-not-zero 31
            --expect-unique-min 17
            --
            --mode lores --sliced --reserve-range 1-15 ff0000)

    # Bug 8 (--mode ehb + reserve 1-15 → 32-base palette has reserves
    # at 1-15 and quantizer fills 16-31; halfbrite section is hardware-
    # derived, not user-controllable, so only base is asserted).
    add_test(NAME "reserve-ehb-low-reserves-fills-tail"
        COMMAND ${_check_pal}
            --in ${_pal_makena_in}
            --out ${CMAKE_BINARY_DIR}/test_reserve_ehb.iff
            --expect 0=000000
            --expect 1=ff0000 --expect 8=ff0000 --expect 15=ff0000
            --expect-not-zero 16 --expect-not-zero 17
            --expect-not-zero 23 --expect-not-zero 31
            --expect-unique-min 17
            --
            --mode ehb --reserve-range 1-15 ff0000)

    # NOTE: --mode ehb --strips can't write IFF (only PNG/.h/.cpp), so
    # we don't have a CMAP-asserting regression here. The fix lives in
    # strips.cpp's seed_pal builder (subtract reserved_slots.size() from
    # k1 + assemble pattern with reserved-skip) and is exercised by the
    # ehb-sliced and ehb plain tests via the shared encode_copper path.
    # If a future regression shows up, write a PNG-based assertion
    # extension to check_palette.py (count specific colours in
    # rendered output).

    # Bug 9 (--mode ehb --sliced + reserve 1-15 → similar to
    # ehb-strips but via the EHB+sliced encode_once trial loop in
    # api.cpp:1820+; we patched the seed_pal to use
    # assemble_with_reserves AND pass locks+reserves to
    # encode_copper as the locked vector so per-line copper
    # evolution enforces them across every scanline).
    add_test(NAME "reserve-ehb-sliced-low-reserves-fills-tail"
        COMMAND ${_check_pal}
            --in ${_pal_makena_in}
            --out ${CMAKE_BINARY_DIR}/test_reserve_ehb_sliced.iff
            --expect 0=000000
            --expect 1=ff0000 --expect 8=ff0000 --expect 15=ff0000
            --expect-not-zero 16 --expect-not-zero 17
            --expect-not-zero 23 --expect-not-zero 31
            --expect-unique-min 17
            --
            --mode ehb --sliced --reserve-range 1-15 ff0000)

    # ----- Reserve colour-independence tests --------------------------
    # "reserve = DO NOT USE IT": the encoder must produce byte-identical
    # output regardless of which colour is stamped at the reserved slots,
    # because reserved slots never contribute pixels to the rendered
    # image. Past silent regressions:
    #   * build_swap_scratch / refresh_swap_scratch / anchor / change-sort /
    #     column-error feedback in copper.cpp all read locked palette state.
    #   * refine_ehb_base_palette's iterative convergence saw the reserve
    #     colour even when the caller re-stamped after — added a locked_mask
    #     so refine itself is reserve-aware.
    #   * strips planner: per-row base_index rebind, e_strip_dither scorer,
    #     and the swap-eval pixel_min_excl_k inner loop all needed the same
    #     mask.
    # The harness runs the encoder twice with different reserve colours,
    # SHA-256s the output PNG, asserts hashes match.
    set(_indep_chk
        "${PNG2AMIGA_PYTHON};${CMAKE_SOURCE_DIR}/tools/check_reserve_color_indep.py;--bin;$<TARGET_FILE:png2amiga>")

    add_test(NAME "reserve-indep-lores-plain"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 16-31
            --colors 000000 ff0000 cacaca
            -- --mode lores)
    add_test(NAME "reserve-indep-lores-sliced"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 16-31
            --colors 000000 ff0000 cacaca ffffff
            -- --mode lores --sliced)
    add_test(NAME "reserve-indep-hires-sliced"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 8-15
            --colors 000000 ff0000 cacaca
            -- --mode hires --sliced)
    add_test(NAME "reserve-indep-ehb-plain"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 16-31
            --colors 000000 ff0000 cacaca ffffff
            -- --mode ehb)
    add_test(NAME "reserve-indep-ehb-sliced"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 16-31
            --colors 000000 ff0000 cacaca ffffff
            -- --mode ehb --sliced)
    add_test(NAME "reserve-indep-strips-ehb"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 16-31
            --colors 000000 ff0000 cacaca ffffff
            -- --mode ehb --strips)
    add_test(NAME "reserve-indep-vga-13h"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 250-255
            --colors 000000 ff0000 cacaca ff00ff abcdef
            -- --mode vga-13h)
    add_test(NAME "reserve-indep-ega-320"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 12-15
            --colors 000000 ff0000 cacaca ff00ff
            -- --mode ega-320)
    add_test(NAME "reserve-indep-stf-low"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 12-15
            --colors 000000 ff0000 cacaca ff00ff
            -- --mode stf-low)

    # ---------------------------------------------------------------
    # Reserve render-pixel invariants. Stronger than color-indep:
    # these check that the rendered output meets specific per-pixel
    # constraints. Captures bug shapes the user reported in May 2026.
    # ---------------------------------------------------------------
    set(_pixel_chk
        "${PNG2AMIGA_PYTHON};${CMAKE_SOURCE_DIR}/tools/check_pixel_count.py;--bin;$<TARGET_FILE:png2amiga>")

    # User-reported: EHB+sliced + reserve every non-zero base slot
    # (1-31). With lock_color0 also locking slot 0, NO unlocked slot
    # remains, so the dither has no candidates and the image must
    # render entirely as black. Without the api.cpp EHB+sliced fix,
    # the web silently ignored reserves and produced a normal-looking
    # encode — caught here.
    add_test(NAME "reserve-render-ehb-sliced-all-locked-is-black"
        COMMAND ${_pixel_chk}
            --in ${_pal_makena_in}
            --expect-unique-colors 1
            --expect-all-color 000000
            -- --mode ehb --sliced --reserve-range 1-31 ff0000)

    # Same for plain EHB (no sliced).
    add_test(NAME "reserve-render-ehb-all-locked-is-black"
        COMMAND ${_pixel_chk}
            --in ${_pal_makena_in}
            --expect-unique-colors 1
            --expect-all-color 000000
            -- --mode ehb --reserve-range 1-31 ff0000)

    # ---------------------------------------------------------------
    # --joint-input + --lock-index consistency:
    # the same image rendered solo vs as a --ji input must produce
    # byte-identical .idx output (regression: load_and_preprocess's
    # prepared_image bypass dropped tmask, so transparent slot 0 was
    # not excluded from the dither candidate set, off-by-one shifting
    # every opaque pixel's slot routing in --ji renders).
    # ---------------------------------------------------------------
    set(_ji_lock_chk
        "${PNG2AMIGA_PYTHON};${CMAKE_SOURCE_DIR}/tools/check_ji_lock_consistency.py;--bin;$<TARGET_FILE:png2amiga>")

    # logo.png is RGBA — alpha channel exercises the prepared_image
    # tmask synthesis on the --ji path.
    add_test(NAME "ji-lock-consistency-rgba"
        COMMAND ${_ji_lock_chk}
            --in "${CMAKE_SOURCE_DIR}/examples/logo.png"
            --mode lores --depth 5)

    # asterix.png is RGB (no alpha) — exercises the same path with
    # tmask staying empty (no transparency exclusion needed).
    add_test(NAME "ji-lock-consistency-rgb"
        COMMAND ${_ji_lock_chk}
            --in "${CMAKE_SOURCE_DIR}/examples/asterix.png"
            --mode lores --depth 5)

    # --print-palette-json pins[] round-trip: emit JSON with --pin-index-at,
    # re-load it via --palette FILE.json, re-emit JSON, and assert the
    # pins[] section matches the original input. Pins the JSON writer ↔
    # parser contract so a regression in either side (writer drops the
    # field, parser silently ignores it, CLI load skips the merge step)
    # fails immediately.
    add_test(NAME "palette-json-pin-roundtrip"
        COMMAND ${PNG2AMIGA_PYTHON}
            "${CMAKE_SOURCE_DIR}/tools/check_palette_json_roundtrip.py"
            --bin $<TARGET_FILE:png2amiga>
            --in "${CMAKE_SOURCE_DIR}/examples/asterix.png")

    # Lores 5bpp: reserving 1-31 leaves only slot 0 (= black). Output
    # must be all-black.
    add_test(NAME "reserve-render-lores-all-locked-is-black"
        COMMAND ${_pixel_chk}
            --in ${_pal_makena_in}
            --expect-unique-colors 1
            --expect-all-color 000000
            -- --mode lores --reserve-range 1-31 ff0000)

    # Reserve a single slot to magenta (a colour very unlikely to be
    # picked by the K=15 quantizer or as a swap centroid for makena).
    # Assert magenta is NEVER routed through the reserved slot — i.e.
    # no pixel in the output is magenta. Catches a leak where the
    # reserve colour ends up at a different slot via the dither path.
    add_test(NAME "reserve-render-lores-magenta-not-leaked"
        COMMAND ${_pixel_chk}
            --in ${_pal_makena_in}
            --expect-no-color ff00ff
            -- --mode lores --sliced --reserve-range 31 ff00ff)
    add_test(NAME "reserve-render-ehb-magenta-not-leaked"
        COMMAND ${_pixel_chk}
            --in ${_pal_makena_in}
            --expect-no-color ff00ff
            -- --mode ehb --sliced --reserve-range 31 ff00ff)

    # Color-indep also under non-FS dithers (ordered dithering reads
    # palette OKLab via a different code path) and under --best
    # (multi-restart parallel jitter sweep — would surface any
    # reserve-color leak inside the sweep's per-trial encoder).
    add_test(NAME "reserve-indep-lores-sliced-bayer"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 16-31
            --colors 000000 ff0000 cacaca
            -- --mode lores --sliced --dither bayer4x4)
    add_test(NAME "reserve-indep-lores-sliced-best"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 16-31
            --colors 000000 ff0000 cacaca
            -- --mode lores --sliced --best)

    # ---------------------------------------------------------------
    # api-vs-CLI equivalence tests. The amiga modes that have already
    # been fully migrated (or are byte-identical between api::encode_state
    # and main.cpp's inline pipeline) are pinned here. Each phase of
    # the main.cpp/api.cpp merge — see REFACTOR_PLAN.md — adds one or
    # more entries to this set as inline pipelines are migrated.
    #
    # FAILS if the two paths diverge: catches the kind of silent drift
    # that produced the EHB+sliced web reserve bug (May 2026) where
    # api.cpp lacked plumbing main.cpp had.
    #
    # Uses tools/check_api_equiv.py + the api_pipeline_smoke binary,
    # which exercises api::encode_state with a CLI-equivalent option
    # set (--apply-tuning fills in dither_tuning defaults).
    # ---------------------------------------------------------------
    set(_equiv_chk
        "${PNG2AMIGA_PYTHON};${CMAKE_SOURCE_DIR}/tools/check_api_equiv.py;--cli;$<TARGET_FILE:png2amiga>;--api;$<TARGET_FILE:api_pipeline_smoke>")

    add_test(NAME "api-equiv-lores-plain"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores)
    add_test(NAME "api-equiv-lores-sliced"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --sliced)
    add_test(NAME "api-equiv-lores-sliced-reserve"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --sliced --reserve-range 16-31 ff0000)
    add_test(NAME "api-equiv-lores-sliced-d4"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --sliced --depth 4)
    add_test(NAME "api-equiv-lores-sliced-atkinson"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --sliced --dither atkinson)
    add_test(NAME "api-equiv-lores-sliced-bayer"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --sliced --dither bayer4x4)
    add_test(NAME "api-equiv-lores-plain-reserve"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --reserve-range 16-31 ff0000)
    # api::encode_state_image migration (the float-input overload that
    # bypasses the 8-bit PNG round-trip). After the migration, EHB plain
    # and strips EHB byte-match the api smoke. ehb+sliced + hires still
    # use inline pipelines; their api-equiv tests land when those branches
    # migrate too.
    add_test(NAME "api-equiv-ehb-plain"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ehb)
    add_test(NAME "api-equiv-ehb-plain-reserve"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ehb --reserve-range 16-31 ff0000)
    add_test(NAME "api-equiv-strips-ehb"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ehb --strips)
    # hires plain — main.cpp's std-lores path matches api once smoke
    # resolves the per-mode depth default (hires non-AGA → 4, lores → 5).
    add_test(NAME "api-equiv-hires-plain"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode hires)
    add_test(NAME "api-equiv-hires-sliced"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode hires --sliced)
    add_test(NAME "api-equiv-hires-sliced-reserve"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode hires --sliced --reserve-range 8-15 ff0000)
    # Non-Amiga modes — these already routed through api::encode_state and
    # don't have inline pipelines, so byte-equivalence is essentially a
    # smoke test that the option translation between Config and Options
    # is faithful.
    add_test(NAME "api-equiv-stf-low"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode stf-low)
    add_test(NAME "api-equiv-ste-low"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ste-low)
    add_test(NAME "api-equiv-ega-320"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ega-320)
    add_test(NAME "api-equiv-vga-12h"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode vga-12h)
    add_test(NAME "api-equiv-stf-low-reserve"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode stf-low --reserve-range 12-15 ff0000)
    add_test(NAME "api-equiv-ega-320-reserve"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ega-320 --reserve-range 12-15 ff0000)
    add_test(NAME "api-equiv-vga-13h-reserve"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode vga-13h --reserve-range 250-255 ff0000)
    # EHB+sliced — migrated from inline (main.cpp:6498) to api::encode_state_image.
    # Pre-migration the inline path lacked refine_ehb_base_palette +
    # extra_ehb_optimization seed pre-pass, producing 60% pixel-different
    # output and lower PSNR vs the api path. Migration improves CLI quality
    # (PSNR 37.70 → 38.84 dB on makena) AND aligns CLI with web byte-for-byte.
    add_test(NAME "api-equiv-ehb-sliced"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ehb --sliced)
    add_test(NAME "api-equiv-ehb-sliced-reserve"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ehb --sliced --reserve-range 16-31 ff0000)
    # HAM modes — migrated from png_io::encode + encode_state(bytes) to
    # encode_state_image, eliminating the 8-bit precision-loss round-trip.
    add_test(NAME "api-equiv-ham6"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ham6)
    add_test(NAME "api-equiv-ham8"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ham8)
    add_test(NAME "api-equiv-ham6-sliced"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ham6 --sliced)
    add_test(NAME "api-equiv-ham6-strips"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ham6 --strips)
    add_test(NAME "api-equiv-ega-640"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ega-640)
    add_test(NAME "api-equiv-stf-med"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode stf-med)
    # Remaining Atari ST/STE / EGA / VGA / CGA modes — byte-equivalent
    # after the comparison harness's scale_preview_nn inverse fix.
    add_test(NAME "api-equiv-stf-hi"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode stf-hi)
    add_test(NAME "api-equiv-ste-med"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode ste-med)
    add_test(NAME "api-equiv-ste-hi"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode ste-hi)
    add_test(NAME "api-equiv-ega-hi"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode ega-hi)
    add_test(NAME "api-equiv-vga-10h"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode vga-10h)
    add_test(NAME "api-equiv-cga-320"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode cga-320)
    add_test(NAME "api-equiv-cga-640"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode cga-640)
    add_test(NAME "api-equiv-cga-composite"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode cga-composite)
    # Tile / chunky modes — migrated from encode_state(bytes) to
    # encode_state_image (eliminates 8-bit PNG round-trip).
    add_test(NAME "api-equiv-genesis-h40"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode genesis-h40)
    add_test(NAME "api-equiv-genesis-h32"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode genesis-h32)
    add_test(NAME "api-equiv-snes-direct"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode snes-mode7-direct)
    add_test(NAME "api-equiv-snes-256"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode snes-mode7-256)
    add_test(NAME "api-equiv-c64-multicolor"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode c64-multicolor)
    add_test(NAME "api-equiv-c64-hires"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode c64-hires)
    add_test(NAME "api-equiv-c64-fli"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode c64-fli)
    add_test(NAME "api-equiv-c64-afli"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode c64-afli)
    add_test(NAME "api-equiv-c64-petscii"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode c64-petscii)
    add_test(NAME "api-equiv-c64-charset-hires"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode c64-charset-hires)
    add_test(NAME "api-equiv-c64-charset-multicolor"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode c64-charset-multicolor)
    add_test(NAME "api-equiv-cga-text80x100"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode cga-text80x100)
    # gpu_restart_quantize made deterministic in v1.66.0 (GPU argmin
    # only; CPU sum in fixed pixel-index order). Now byte-equivalent.
    add_test(NAME "api-equiv-vga-13h-plain"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode vga-13h)
    # DPF + reserves / locks / pins: --reserve-range, --lock-index, and
    # --pin-index-at all use the 16-entry CLUT view (CLUT[0..7] = PF1
    # zeros, CLUT[8..15] = PF2). The encoder internally translates
    # CLUT 9..15 → PF2 idx 1..7 (slots 0/8 → PF2 idx 0, slots 1..7 =
    # PF1 = silent no-op). The api-equiv check below pins CLI ↔ API
    # parity so both sides apply identical translations.
    add_test(NAME "api-equiv-dpf"
        COMMAND ${_equiv_chk} --in ${_pal_makena_in} -- --mode lores --dpf)
    add_test(NAME "api-equiv-dpf-reserve"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --dpf --reserve-range 15 ff0000)
    add_test(NAME "api-equiv-dpf-lock"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --dpf --lock-index 15 00ff00)
    add_test(NAME "api-equiv-dpf-pin"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --dpf --pin-index-at 15 0 0)

    # DPF + --lock-index actually pins palette[15]: covers plain --dpf
    # AND --dpf --strips so a regression in either path's lock translation
    # surfaces (strips DPF previously had no lock plumbing — locks
    # showed the 🔒 marker in the dump but didn't override the slot's
    # encoded colour). Plain DPF uses IFF CMAP; strips DPF can only
    # output .png/.h/.cpp so we read its palette via --print-palette-json.
    add_test(NAME "dpf-lock-cmap-15-green"
        COMMAND ${_check_pal}
            --in ${_pal_makena_in}
            --out ${CMAKE_BINARY_DIR}/test_dpf_lock_15.iff
            --expect 15=00ff00
            --
            --mode lores --dpf --lock-index 15 00ff00)
    add_test(NAME "dpf-strips-lock-slot-15-green"
        COMMAND ${PNG2AMIGA_PYTHON}
            "${CMAKE_SOURCE_DIR}/tools/check_palette_slot.py"
            --bin $<TARGET_FILE:png2amiga>
            --in ${_pal_makena_in}
            --expect 15=00ff00
            --
            --mode lores --dpf --strips --lock-index 15 00ff00)
    # Coverage gaps surfaced after the api hires palette_diversity bump
    # broke api-equiv-hires-sliced-reserve: each of these exercises a
    # path the prior 51-test set didn't pin.
    add_test(NAME "api-equiv-hires-plain-reserve"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode hires --depth 4 --reserve-range 8-15 ff0000)
    add_test(NAME "api-equiv-lores-lace"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores-lace)
    add_test(NAME "api-equiv-hires-lace"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode hires-lace)
    add_test(NAME "api-equiv-ehb-lace"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ehb-lace)
    add_test(NAME "api-equiv-ham6-lace"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ham6-lace)
    add_test(NAME "api-equiv-ham8-lace"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ham8-lace --chipset aga)
    add_test(NAME "api-equiv-ham8-sliced"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ham8 --sliced --chipset aga)
    add_test(NAME "api-equiv-lores-sliced-aga"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --sliced --chipset aga --depth 8)
    add_test(NAME "api-equiv-strips-dpf"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --strips --dpf --depth 3)
    # DPF variants — non-default chipset / mode / sliced. The original
    # api-equiv-dpf only tested lores OCS depth-3; AGA, hires, and
    # sliced+DPF all silently diverged because the smoke harness's
    # --apply-tuning section didn't mirror main.cpp L5655's DPF depth
    # override before the dither_tuning lookup.
    add_test(NAME "api-equiv-dpf-aga"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --dpf --chipset aga)
    add_test(NAME "api-equiv-dpf-hires"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode hires --dpf)
    add_test(NAME "api-equiv-dpf-hires-aga"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode hires --dpf --chipset aga)
    add_test(NAME "api-equiv-dpf-sliced"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --sliced --dpf)
    # Preprocess + dither variant coverage. Each of these went unverified
    # by the prior 51-test suite — the smoke harness lacked the flags
    # entirely until this batch.
    add_test(NAME "api-equiv-preprocess-gamma"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --gamma 1.5)
    add_test(NAME "api-equiv-preprocess-brightness"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --brightness 0.2)
    add_test(NAME "api-equiv-preprocess-contrast"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --contrast 1.3)
    add_test(NAME "api-equiv-preprocess-saturation"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --saturation 1.2)
    add_test(NAME "api-equiv-preprocess-hue"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --hue-shift 0.1)
    add_test(NAME "api-equiv-preprocess-sharpen"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --sharpen 0.5)
    add_test(NAME "api-equiv-preprocess-match-range"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --match-range)
    add_test(NAME "api-equiv-preprocess-levels"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --black-point 0.05 --white-point 0.95)
    # HAM op-selection metric + greedy fallback.
    add_test(NAME "api-equiv-ham6-srgb-mse"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ham6 --ham-metric srgb-mse)
    add_test(NAME "api-equiv-ham6-fast"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ham6 --ham-fast)
    add_test(NAME "api-equiv-ham6-beam-32"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ham6 --ham-beam 32)
    # Lores depth + chipset variants — closes the gap api-equiv-lores-d6
    # would have surfaced if it'd existed earlier.
    add_test(NAME "api-equiv-lores-d4"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --depth 4)
    add_test(NAME "api-equiv-lores-d6-aga"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --depth 6 --chipset aga)
    add_test(NAME "api-equiv-lores-aga"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --chipset aga --depth 8)
    add_test(NAME "api-equiv-lores-sliced-d6-aga"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --sliced --depth 6 --chipset aga)
    # Plain dither variants (lores).
    add_test(NAME "api-equiv-lores-stucki"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --dither stucki)
    add_test(NAME "api-equiv-lores-jarvis"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --dither jarvis)
    add_test(NAME "api-equiv-lores-yliluoma1"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --dither yliluoma1)
    add_test(NAME "api-equiv-lores-gilbert"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --dither gilbert)
    add_test(NAME "api-equiv-lores-riemersma"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --dither riemersma)
    add_test(NAME "api-equiv-ham6-atkinson"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ham6 --dither atkinson)
    add_test(NAME "api-equiv-ehb-stucki"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode ehb --dither stucki)
    add_test(NAME "api-equiv-lores-sliced-jarvis"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --sliced --dither jarvis)
    add_test(NAME "api-equiv-lores-sliced-sierra-lite"
        COMMAND ${_equiv_chk}
            --in ${_pal_makena_in}
            -- --mode lores --sliced --dither sierra-lite)
    # Color-independence + render-pixel invariants for DPF reserves.
    add_test(NAME "reserve-indep-lores-dpf"
        COMMAND ${_indep_chk}
            --in ${_pal_makena_in} --reserve-range 7
            --colors 000000 ff0000 cacaca
            -- --mode lores --dpf)
    add_test(NAME "reserve-render-dpf-magenta-not-leaked"
        COMMAND ${_pixel_chk}
            --in ${_pal_makena_in}
            --expect-no-color ff00ff
            -- --mode lores --dpf --reserve-range 7 ff00ff)

    # ------------------------------------------------------------------
    # Per-mode SSIMULACRA2 thresholds — assert each encode hits a quality
    # floor on the standard test image (examples/makena.jpg). One score
    # gate per (mode + relevant flags); the output writer's wire format
    # is checked by the round-trip ctests, not here.
    #
    # Replaces an earlier byte-equal SHA256 hash gate that drifted on
    # CI runners across platforms — median-cut + HAM beam are sensitive
    # to thread interleaving + FP rounding, so the SHA256 fingerprint
    # changed even when the perceptual output didn't. The S2 score is
    # the encoder's own self-evaluation against the source (computed in
    # process via ssimulacra2::compute) and tracks human perception, so
    # any real regression shows up here as a drop.
    #
    # Threshold = recorded local-mac S2 minus a 1.0 buffer for
    # cross-platform noise. The encoder prints `S2: <x.xx>` on its final
    # summary line; check_output_score.py parses it.
    set(_score_chk
        ${CMAKE_COMMAND} -E env "PYTHONUNBUFFERED=1"
        ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_output_score.py
        --cli  $<TARGET_FILE:png2amiga>)
    function(_add_score_test NAME OUTNAME SCORE_MIN)
        add_test(NAME "output-score-${NAME}"
            COMMAND ${_score_chk}
                --in    ${_pal_makena_in}
                --out   ${OUTNAME}
                --score-min ${SCORE_MIN}
                -- ${ARGN})
    endfunction()
    _add_score_test(lores         out.png 51.9 --mode lores)
    _add_score_test(hires         out.iff 23.8 --mode hires --depth 4)
    _add_score_test(ehb           out.iff 53.3 --mode ehb)
    _add_score_test(ham6          out.iff 71.4 --mode ham6)
    _add_score_test(ham8          out.iff 83.9 --mode ham8 --chipset aga)
    _add_score_test(lores-sliced  out.iff 54.2 --mode lores --sliced)
    _add_score_test(ehb-sliced    out.iff 63.5 --mode ehb --sliced)
    _add_score_test(ham6-sliced   out.iff 73.6 --mode ham6 --sliced)
    _add_score_test(ham6-strips   out.iff 74.4 --mode ham6 --strips)
    _add_score_test(vga13h        out.iff 78.0 --mode vga-13h)
    _add_score_test(ega320        out.iff -1.0 --mode ega-320)
    _add_score_test(stf-low       out.pi1 25.1 --mode stf-low)
    _add_score_test(ste-low       out.pi1 36.1 --mode ste-low)
    _add_score_test(dpf           out.iff 11.0 --mode lores --dpf)

endif() # NOT EMSCRIPTEN

# Mac-only convenience target: clang-tidy + cppcheck over src/ via tools/lint.sh.
# Configures a separate build-lint/ with homebrew clang on first run so the
# lint compile-DB stays consistent regardless of what compiler the main build
# uses. Requires `brew install llvm cppcheck`.
if(NOT EMSCRIPTEN AND APPLE)
    add_custom_target(lint
        COMMAND ${CMAKE_SOURCE_DIR}/tools/lint.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
        USES_TERMINAL
    )
endif()
