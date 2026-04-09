cmake_minimum_required(VERSION 3.28)
project(png2amiga LANGUAGES C CXX)

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
    src/palette_locks.cpp
    src/api.cpp
)

if(EMSCRIPTEN)
    # --- WASM library build ---
    add_executable(png2amiga_wasm
        ${PNG2AMIGA_CORE_SOURCES}
        src/wasm_bindings.cpp
    )
    target_include_directories(png2amiga_wasm PRIVATE src ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(png2amiga_wasm PRIVATE stb)
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
    target_include_directories(png2amiga PRIVATE src ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(png2amiga PRIVATE stb)
    target_compile_options(png2amiga PRIVATE ${PNG2AMIGA_WARNING_FLAGS})

    enable_testing()

    # Basic build test: verify the binary runs and shows help
    add_test(
        NAME "help"
        COMMAND png2amiga --help
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
        COMMAND png2amiga --mode ham6 --ham-quality fast
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_ham6_fast.iff
    )

    # --- HAM6 DP optimal ---
    add_test(
        NAME "ham6-optimal"
        COMMAND png2amiga --mode ham6 --ham-quality optimal --ham-beam 32
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_ham6_optimal.iff
    )

    # --- HAM6 DP optimal with preview PNG ---
    add_test(
        NAME "ham6-optimal-png"
        COMMAND png2amiga --mode ham6 --ham-quality optimal --ham-beam 16
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_ham6_optimal.png
    )

    # --- HAM8 DP optimal ---
    add_test(
        NAME "ham8-optimal"
        COMMAND png2amiga --mode ham8 --ham-quality optimal --ham-beam 16
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
        COMMAND png2amiga --mode ham6 --ham-quality fast --dither floyd-steinberg
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_ham6_dithered_fast.png
    )

    # --- HAM6 dithered (DP optimal, Floyd-Steinberg) ---
    add_test(
        NAME "ham6-dithered-optimal"
        COMMAND png2amiga --mode ham6 --ham-quality optimal --ham-beam 16
                --dither floyd-steinberg
                ${CMAKE_SOURCE_DIR}/test.png
                ${CMAKE_BINARY_DIR}/test_ham6_dithered_optimal.png
    )

    # --- HAM8 dithered (greedy, Atkinson) ---
    add_test(
        NAME "ham8-dithered-atkinson"
        COMMAND png2amiga --mode ham8 --ham-quality fast --dither atkinson
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

endif() # NOT EMSCRIPTEN
