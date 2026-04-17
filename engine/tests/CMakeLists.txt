# Host-only smoke tests — validate that the engine's concepts match the
# png2amiga-assets generator's output.
enable_testing()

add_executable(pa-concept-test
    concept_test.cpp
    ../examples/hello_tiles/assets/level_3_1.cpp
)
target_include_directories(pa-concept-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../examples/hello_tiles
)
target_link_libraries(pa-concept-test PRIVATE pa-engine)
add_test(NAME pa-concept-test COMMAND pa-concept-test)
