add_library(stb STATIC stb_impl.c)
target_include_directories(stb SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

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
