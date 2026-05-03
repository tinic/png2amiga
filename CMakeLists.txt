cmake_minimum_required(VERSION 3.28)
project(png2amiga VERSION 1.60.0 LANGUAGES C CXX)

if(EMSCRIPTEN)
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
    add_executable(png2amiga_wasm
        ${PNG2AMIGA_CORE_SOURCES}
        src/wasm_bindings.cpp
    )
    target_include_directories(png2amiga_wasm PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(png2amiga_wasm PRIVATE stb spng webp_headers webpdecode webpdspdecode webputilsdecode constixel)
    # -pthread enables std::thread / std::jthread / std::mutex / std::atomic
    # in WASM via Emscripten's pthreads-on-web-workers shim. Requires the
    # browser to be cross-origin isolated (COOP/COEP headers, see
    # service/nginx.conf and vite.config.js). Pool size = the number of
    # cores reported by navigator.hardwareConcurrency, allocated lazily.
    target_compile_options(png2amiga_wasm PRIVATE
        -O3 -msimd128 -flto
        -pthread)
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
    target_include_directories(png2amiga PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(png2amiga PRIVATE stb spng webp_headers webpdecode webpdspdecode webputilsdecode constixel)
    target_compile_options(png2amiga PRIVATE ${PNG2AMIGA_WARNING_FLAGS})

    if(MINGW)
        # std::print's terminal helpers (__open_terminal, __write_to_terminal)
        # live in libstdc++exp.a on MinGW rather than the main libstdc++.a.
        # Must come AFTER object files in the link line, so use
        # target_link_libraries (not CMAKE_EXE_LINKER_FLAGS, which is positioned
        # before objects and won't pull any symbols from a static archive).
        target_link_libraries(png2amiga PRIVATE -lstdc++exp)
    endif()

    enable_testing()

    # Single shared test input. Lives under examples/ (tracked) so a
    # fresh checkout has everything ctest needs — no more local test.png
    # in the repo root that has to be hand-restored after a checkout.
    # Matches recoil_roundtrip.py's default so smoke + RECOIL tests
    # exercise the same source image.
    set(PNG2AMIGA_TEST_INPUT "${CMAKE_SOURCE_DIR}/examples/lovers.jpg")

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
        COMMAND python3
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
            COMMAND python3
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
        COMMAND python3 -c
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
        COMMAND python3 -c
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

    # Pin targets a locked slot
    add_test(
        NAME "pin-index-targets-locked"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 5 ff0000
                --pin-index-at 5 100 50
                ${PNG2AMIGA_TEST_INPUT}
                ${CMAKE_BINARY_DIR}/test_reject_pin_locked.iff
    )
    set_tests_properties("pin-index-targets-locked" PROPERTIES WILL_FAIL TRUE)

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
            COMMAND python3 -c
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
            COMMAND python3 -c
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

endif() # NOT EMSCRIPTEN
