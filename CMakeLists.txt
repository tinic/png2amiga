cmake_minimum_required(VERSION 3.28)
project(png2amiga VERSION 1.10.10 LANGUAGES C CXX)

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
    src/scap.cpp
    src/dither_tuning.cpp
    src/palette_locks.cpp
    src/cga_text.cpp
    src/cheader_dos_c.cpp
    src/api.cpp
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
    target_link_libraries(png2amiga_wasm PRIVATE stb spng webp_headers webpdecode webpdspdecode webputilsdecode)
    target_compile_options(png2amiga_wasm PRIVATE -O3 -msimd128 -flto)
    target_link_options(png2amiga_wasm PRIVATE
        --bind
        -O3
        -flto
        -msimd128
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORT_ES6=1
        -sMODULARIZE=1
        -sEXPORT_NAME=createPng2Amiga
        -sENVIRONMENT=web,worker
        -sNO_FILESYSTEM=1
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
    target_link_libraries(png2amiga PRIVATE stb spng webp_headers webpdecode webpdspdecode webputilsdecode)
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

    # Basic build test: verify the binary runs and shows help
    add_test(
        NAME "help"
        COMMAND png2amiga --help
    )

    # RECOIL round-trip suite. Encodes one input through every IFF-
    # producing combo, decodes each IFF via Piotr Fusik's recoil2png,
    # and PSNR-compares against png2amiga's own preview. Skips cleanly
    # if recoil2png isn't installed. Run with:
    #     cmake --build build --target recoil_check
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

    # --- Raw bitplane output ---
    add_test(
        NAME "raw-output"
        COMMAND png2amiga ${CMAKE_SOURCE_DIR}/test.png
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
        COMMAND png2amiga ${CMAKE_SOURCE_DIR}/test.png
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
                ${CMAKE_SOURCE_DIR}/test.png
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
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_from_gpl_palette.png
    )

    # --- HAM6 greedy (fast) ---
    add_test(
        NAME "ham6-fast"
        COMMAND png2amiga --mode ham6
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_ham6_fast.iff
    )

    # --- HAM6 DP optimal ---
    add_test(
        NAME "ham6-optimal"
        COMMAND png2amiga --mode ham6 --ham-beam 32
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_ham6_optimal.iff
    )

    # --- HAM6 DP optimal with preview PNG ---
    add_test(
        NAME "ham6-optimal-png"
        COMMAND png2amiga --mode ham6 --ham-beam 16
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_ham6_optimal.png
    )

    # --- HAM8 DP optimal ---
    add_test(
        NAME "ham8-optimal"
        COMMAND png2amiga --mode ham8 --ham-beam 16
                ${CMAKE_SOURCE_DIR}/test.png
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
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_ham6_dithered_fast.png
    )

    # --- HAM6 dithered (DP optimal, Floyd-Steinberg) ---
    add_test(
        NAME "ham6-dithered-optimal"
        COMMAND png2amiga --mode ham6 --ham-beam 16
                --dither floyd-steinberg
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_ham6_dithered_optimal.png
    )

    # --- HAM8 dithered (greedy, Atkinson) ---
    add_test(
        NAME "ham8-dithered-atkinson"
        COMMAND png2amiga --mode ham8 --dither atkinson
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_ham8_dithered_atkinson.iff
    )

    # IFF CMAP loading: generate an IFF file, then load its palette
    add_test(
        NAME "palette-iff-gen"
        COMMAND png2amiga --depth 4
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_iff_for_pal.iff
    )
    add_test(
        NAME "palette-load-iff"
        COMMAND png2amiga --palette ${CMAKE_BINARY_DIR}/test_iff_for_pal.iff
                --depth 4
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_from_iff_palette.png
    )
    set_tests_properties("palette-load-iff" PROPERTIES DEPENDS "palette-iff-gen")

    # ----- --lock-index / --pin-index-at -----

    # Single lock at index 0 in lores: replaces the implicit black-at-0
    add_test(
        NAME "lock-index-lores-single"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 0 ff00ff
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_lock_lores_single.iff
    )

    # Multiple locks in lores
    add_test(
        NAME "lock-index-lores-multi"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 0 000000
                --lock-index 5 ff0000
                --lock-index 31 ffffff
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_lock_lores_multi.iff
    )

    # Round-trip: load the locked IFF as a palette, depth 5 mode
    add_test(
        NAME "lock-index-roundtrip"
        COMMAND png2amiga --mode lores --depth 5
                --palette ${CMAKE_BINARY_DIR}/test_lock_lores_multi.iff
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_lock_roundtrip.iff
    )
    set_tests_properties("lock-index-roundtrip" PROPERTIES DEPENDS "lock-index-lores-multi")

    # Pin in lores: deterministic ordered dither so the source pixel resolves
    # the same on every run.
    add_test(
        NAME "pin-index-lores"
        COMMAND png2amiga --mode lores --depth 5 --dither bayer4x4
                --pin-index-at 5 100 50
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_pin_lores.iff
    )

    # Lock + pin combination
    add_test(
        NAME "lock-and-pin-lores"
        COMMAND png2amiga --mode lores --depth 5 --dither bayer4x4
                --lock-index 0 ff00ff
                --lock-index 31 ffff00
                --pin-index-at 5 100 50
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_lock_and_pin.iff
    )

    # Hex prefix variants (#rrggbb, 0xrrggbb, 3-digit shorthand)
    add_test(
        NAME "lock-index-hex-prefix"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 0 "#ff00ff"
                --lock-index 1 0xff0000
                --lock-index 2 f0c
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_lock_hex_prefix.iff
    )

    # Lock in EHB mode (operates on the 32 base colors)
    add_test(
        NAME "lock-index-ehb"
        COMMAND png2amiga --mode ehb
                --lock-index 0 000000
                --lock-index 1 ff0000
                --lock-index 31 ffffff
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_lock_ehb.iff
    )

    # Pin in EHB: deterministic dither so the resolved index is stable
    add_test(
        NAME "pin-index-ehb"
        COMMAND png2amiga --mode ehb --dither bayer4x4
                --pin-index-at 5 100 50
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_pin_ehb.iff
    )

    # Lock applied on top of a loaded user palette
    add_test(
        NAME "lock-index-with-user-palette"
        COMMAND png2amiga --mode lores --depth 4
                --palette ${CMAKE_BINARY_DIR}/test_palette.txt
                --lock-index 0 ffffff
                --lock-index 15 000000
                ${CMAKE_SOURCE_DIR}/test.png
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
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_reject_ham_lock.iff
    )
    set_tests_properties("lock-index-ham-rejected" PROPERTIES WILL_FAIL TRUE)

    # HAM rejects pins
    add_test(
        NAME "pin-index-ham-rejected"
        COMMAND png2amiga --mode ham6
                --pin-index-at 5 100 50
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_reject_ham_pin.iff
    )
    set_tests_properties("pin-index-ham-rejected" PROPERTIES WILL_FAIL TRUE)

    # Copper supports locks (fixed palette slots for blitter objects)
    add_test(
        NAME "lock-index-copper-accepted"
        COMMAND png2amiga --mode lores --depth 5 --copper
                --lock-index 0 ff0000
                ${CMAKE_SOURCE_DIR}/test.png
                -o ${CMAKE_BINARY_DIR}/test_copper_lock.png
    )

    # Out-of-range lock index (depth 5 → max_colors 32, index 32 invalid)
    add_test(
        NAME "lock-index-out-of-range"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 32 ff0000
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_reject_lock_oob.iff
    )
    set_tests_properties("lock-index-out-of-range" PROPERTIES WILL_FAIL TRUE)

    # Duplicate lock index
    add_test(
        NAME "lock-index-duplicate"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 5 ff0000
                --lock-index 5 00ff00
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_reject_lock_dup.iff
    )
    set_tests_properties("lock-index-duplicate" PROPERTIES WILL_FAIL TRUE)

    # Pin out of image bounds (test.png scales to 320x119 by default)
    add_test(
        NAME "pin-index-out-of-bounds"
        COMMAND png2amiga --mode lores --depth 5
                --pin-index-at 5 9999 9999
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_reject_pin_oob.iff
    )
    set_tests_properties("pin-index-out-of-bounds" PROPERTIES WILL_FAIL TRUE)

    # Pin targets a locked slot
    add_test(
        NAME "pin-index-targets-locked"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 5 ff0000
                --pin-index-at 5 100 50
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_reject_pin_locked.iff
    )
    set_tests_properties("pin-index-targets-locked" PROPERTIES WILL_FAIL TRUE)

    # Bad hex format
    add_test(
        NAME "lock-index-bad-hex"
        COMMAND png2amiga --mode lores --depth 5
                --lock-index 0 zzzzzz
                ${CMAKE_SOURCE_DIR}/test.png
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
                    ${CMAKE_SOURCE_DIR}/test.png
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
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/smoke.pi1
    )
    add_test(
        NAME "smoke-degas-pi2"
        COMMAND png2amiga --mode ste-med
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/smoke.pi2
    )
    add_test(
        NAME "smoke-degas-pi3"
        COMMAND png2amiga --mode ste-hi
                ${CMAKE_SOURCE_DIR}/test.png
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

endif() # NOT EMSCRIPTEN
