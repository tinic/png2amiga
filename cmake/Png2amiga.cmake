# Png2amiga.cmake — drop-in CMake helpers for asset pipelines.
#
# Usage:
#   include(/path/to/png2amiga/cmake/Png2amiga.cmake)
#   set(PNG2AMIGA /usr/local/bin/png2amiga)         # or the build/png2amiga path
#
#   png2amiga_add_image(
#     TARGET   sprites
#     INPUT    ${CMAKE_CURRENT_SOURCE_DIR}/art/title.png
#     OUTPUT   ${CMAKE_CURRENT_BINARY_DIR}/title.h
#              ${CMAKE_CURRENT_BINARY_DIR}/title.iff
#     MODE     ham6
#     OPTIONS  --cap --ham-beam 32
#     PALETTE  ${CMAKE_CURRENT_SOURCE_DIR}/palette.gpl   # optional, tracked via depfile
#   )
#
# Effect:
#   * One add_custom_command per OUTPUT path (run in parallel under -jN).
#   * Each command emits a Make-format depfile so changes to PALETTE
#     trigger a rebuild even though it isn't listed as a CMake DEPENDS.
#   * A target named ${TARGET} aggregates all outputs so other targets
#     can DEPENDS on a single name.
#
# Requires png2amiga >= 1.10.8 (--quiet, --depfile, exit-code-by-category).

if(NOT DEFINED PNG2AMIGA)
    find_program(PNG2AMIGA NAMES png2amiga
                 DOC "Path to the png2amiga executable")
    if(NOT PNG2AMIGA)
        message(FATAL_ERROR
            "png2amiga not found. Set -DPNG2AMIGA=<path> or add it to PATH.")
    endif()
endif()

function(png2amiga_add_image)
    set(_options "")
    set(_one     TARGET INPUT MODE PALETTE)
    set(_multi   OUTPUT OPTIONS)
    cmake_parse_arguments(P2A "${_options}" "${_one}" "${_multi}" ${ARGN})

    if(NOT P2A_TARGET)
        message(FATAL_ERROR "png2amiga_add_image: TARGET is required")
    endif()
    if(NOT P2A_INPUT)
        message(FATAL_ERROR "png2amiga_add_image: INPUT is required")
    endif()
    if(NOT P2A_OUTPUT)
        message(FATAL_ERROR "png2amiga_add_image: at least one OUTPUT is required")
    endif()

    set(_mode_arg "")
    if(P2A_MODE)
        set(_mode_arg --mode ${P2A_MODE})
    endif()

    set(_palette_arg "")
    if(P2A_PALETTE)
        set(_palette_arg --palette ${P2A_PALETTE})
    endif()

    foreach(_out IN LISTS P2A_OUTPUT)
        set(_depfile "${_out}.d")
        # Ninja consumes depfiles with DEPFILE; Make falls back to IMPLICIT_DEPENDS.
        # We pass the depfile to png2amiga so it emits the right format.
        add_custom_command(
            OUTPUT  ${_out}
            COMMAND ${PNG2AMIGA}
                    --quiet
                    --depfile "${_depfile}"
                    ${_mode_arg}
                    ${_palette_arg}
                    ${P2A_OPTIONS}
                    "${P2A_INPUT}"
                    "${_out}"
            DEPENDS ${P2A_INPUT}
                    ${P2A_PALETTE}
            DEPFILE "${_depfile}"
            COMMENT "png2amiga ${P2A_MODE}: ${_out}"
            VERBATIM
        )
        list(APPEND _all_outputs "${_out}")
    endforeach()

    add_custom_target(${P2A_TARGET} ALL DEPENDS ${_all_outputs})
endfunction()

# Convenience: build a list of png2amiga options from individual variables.
# Lets users keep the option list readable instead of one long string.
#
#   png2amiga_options(opts
#       MODE ham6 CAP CAP_BEST DEPTH 5 DITHER ostromoukhov)
#
function(png2amiga_options out_var)
    set(_opts "")
    set(_one_value MODE DEPTH DITHER DITHER_STRENGTH ERROR_CLAMP HAM_BEAM
                   CAP_CHANGES CHIPSET PALETTE_DIVERSITY LAYOUT)
    set(_flag      CAP CAP_BEST DPF SCAP HAM_FAST INTERLACE FADE_IN
                   NATIVE_PAR NO_RESERVE_COLOR0 MATCH_RANGE NO_SCALE)
    cmake_parse_arguments(O "${_flag}" "${_one_value}" "" ${ARGN})

    if(O_MODE)
        list(APPEND _opts --mode ${O_MODE})
    endif()
    if(O_DEPTH)
        list(APPEND _opts --depth ${O_DEPTH})
    endif()
    if(O_DITHER)
        list(APPEND _opts --dither ${O_DITHER})
    endif()
    if(O_DITHER_STRENGTH)
        list(APPEND _opts --dither-strength ${O_DITHER_STRENGTH})
    endif()
    if(O_ERROR_CLAMP)
        list(APPEND _opts --error-clamp ${O_ERROR_CLAMP})
    endif()
    if(O_HAM_BEAM)
        list(APPEND _opts --ham-beam ${O_HAM_BEAM})
    endif()
    if(O_CAP_CHANGES)
        list(APPEND _opts --cap-changes ${O_CAP_CHANGES})
    endif()
    if(O_CHIPSET)
        list(APPEND _opts --chipset ${O_CHIPSET})
    endif()
    if(O_PALETTE_DIVERSITY)
        list(APPEND _opts --palette-diversity ${O_PALETTE_DIVERSITY})
    endif()
    if(O_LAYOUT)
        list(APPEND _opts --layout ${O_LAYOUT})
    endif()
    if(O_NO_SCALE)
        list(APPEND _opts --no-scale)
    endif()
    if(O_CAP)
        list(APPEND _opts --cap)
    endif()
    if(O_CAP_BEST)
        list(APPEND _opts --cap-best)
    endif()
    if(O_DPF)
        list(APPEND _opts --dpf)
    endif()
    if(O_SCAP)
        list(APPEND _opts --scap)
    endif()
    if(O_HAM_FAST)
        list(APPEND _opts --ham-fast)
    endif()
    if(O_INTERLACE)
        list(APPEND _opts --interlace)
    endif()
    if(O_FADE_IN)
        list(APPEND _opts --fade-in)
    endif()
    if(O_NATIVE_PAR)
        list(APPEND _opts --native-par)
    endif()
    if(O_NO_RESERVE_COLOR0)
        list(APPEND _opts --no-reserve-color0)
    endif()
    if(O_MATCH_RANGE)
        list(APPEND _opts --match-range)
    endif()

    set(${out_var} "${_opts}" PARENT_SCOPE)
endfunction()
