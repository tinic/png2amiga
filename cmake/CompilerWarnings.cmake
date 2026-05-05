if(MSVC)
    set(PNG2AMIGA_WARNING_FLAGS
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        # Promote useful warnings normally-off-at-/W4 to W1 so /W4 catches
        # them. Curated from cpp-best-practices/cppbestpractices.
        /w14242  # conversion possible loss of data
        /w14254  # operator: bit-width loss of data
        /w14263  # member function does not override virtual
        /w14265  # virtuals + non-virtual destructor
        /w14287  # unsigned/negative constant mismatch
        /we4289  # for-loop control variable used outside loop (force error)
        /w14296  # expression is always true/false
        /w14311  # pointer truncation
        /w14545  # comma evaluates to function w/ missing arg list
        /w14546  # function call before comma missing arg list
        /w14547  # operator before comma has no effect; expected side effect
        /w14549  # operator before comma; did you mean other operator?
        /w14555  # expression has no effect
        /w14619  # pragma warning unknown number
        /w14640  # thread-unsafe static member init
        /w14826  # sign-extended conversion
        /w14905  # wide string literal cast to LPSTR
        /w14906  # string literal cast to LPWSTR
        /w14928  # illegal copy-init: more than one user-defined conversion
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
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND PNG2AMIGA_WARNING_FLAGS
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
        )
    endif()
endif()
