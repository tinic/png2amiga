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
