if(MSVC)
    set(PNG2AMIGA_WARNING_FLAGS
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        # Silence noise from C++23/STL: /W4 flags some perfectly fine
        # constructs in headers we can't change.
        /wd4324  # structure padded due to alignment specifier
        /wd4458  # declaration hides class member (we use this style intentionally)
        /wd5030  # attribute 'gnu::*' not recognized (kept for GCC/Clang builds)
    )
else()
    set(PNG2AMIGA_WARNING_FLAGS
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
    )
endif()
