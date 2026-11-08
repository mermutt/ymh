# 96-D1..96-D7 (git revision): configure-time build stamp derived from git.
#
# The displayed version is
#
#     v<YYMMDD>-<8 hex digits of HEAD>[x]
#
# where <YYMMDD> is the local configure (build) date, the hash is taken from the
# current HEAD, and a trailing `x` marks a work tree with uncommitted changes
# (staged, unstaged, or untracked). Example: `v261108-02cc0645x`.
#
# Nothing is read from or written to the source tree; the stamp is a pure
# function of HEAD, the work-tree state, and the date. If git is unavailable or
# the source is not a work tree, the hash degrades to `unknown` and configure
# still succeeds (never fatal).

# Validate a YYMMDD date (month/day ranges only; calendar exactness is not
# needed for a build identifier).
function(_ymh_valid_date value out_var)
    if(value MATCHES "^[0-9][0-9][0-9][0-9][0-9][0-9]$")
        string(SUBSTRING "${value}" 2 2 _ymh_mm)
        string(SUBSTRING "${value}" 4 2 _ymh_dd)
        if(_ymh_mm MATCHES "^(0[1-9]|1[0-2])$"
           AND _ymh_dd MATCHES "^(0[1-9]|[12][0-9]|3[01])$")
            set(${out_var} TRUE PARENT_SCOPE)
            return()
        endif()
    endif()
    set(${out_var} FALSE PARENT_SCOPE)
endfunction()

# Resolve the stamp date. YMH_VERSION_DATE may be YYMMDD, YYYYMMDD, or the old
# DD-MM-YYYY spelling (kept so existing callers and tests keep working). A
# malformed or out-of-range override is ignored with a warning.
function(_ymh_stamp_date out_var)
    set(_ymh_raw "")
    if(DEFINED YMH_VERSION_DATE)
        string(STRIP "${YMH_VERSION_DATE}" _ymh_raw)
    endif()

    set(_ymh_date "")
    if(_ymh_raw MATCHES "^[0-9][0-9][0-9][0-9][0-9][0-9]$")
        set(_ymh_date "${_ymh_raw}")
    elseif(_ymh_raw MATCHES "^[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]$")
        string(SUBSTRING "${_ymh_raw}" 2 6 _ymh_date)
    elseif(_ymh_raw MATCHES "^[0-9][0-9]-[0-9][0-9]-[0-9][0-9][0-9][0-9]$")
        string(SUBSTRING "${_ymh_raw}" 0 2 _ymh_dd)
        string(SUBSTRING "${_ymh_raw}" 3 2 _ymh_mm)
        string(SUBSTRING "${_ymh_raw}" 6 4 _ymh_yyyy)
        string(SUBSTRING "${_ymh_yyyy}" 2 2 _ymh_yy)
        set(_ymh_date "${_ymh_yy}${_ymh_mm}${_ymh_dd}")
    endif()

    set(_ymh_date_valid FALSE)
    if(_ymh_date)
        _ymh_valid_date("${_ymh_date}" _ymh_date_valid)
    endif()
    if(NOT _ymh_date_valid)
        if(NOT _ymh_raw STREQUAL "")
            message(WARNING
                "ymh: ignoring malformed YMH_VERSION_DATE='${_ymh_raw}'; using the local date")
        endif()
        string(TIMESTAMP _ymh_date "%y%m%d")
    endif()

    set(${out_var} "${_ymh_date}" PARENT_SCOPE)
endfunction()

function(ymh_stamp_version source_dir out_var)
    _ymh_stamp_date(_ymh_date)

    # HEAD -> eight hex digits. `rev-parse --verify` fails cleanly on a repo
    # without commits; the fallback keeps the configure non-fatal.
    execute_process(
        COMMAND git -C "${source_dir}" rev-parse --verify HEAD
        OUTPUT_VARIABLE _ymh_head
        RESULT_VARIABLE _ymh_head_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(_ymh_hash "unknown")
    set(_ymh_dirty "")
    if(_ymh_head_rc EQUAL 0 AND _ymh_head MATCHES "^[0-9a-fA-F]+$")
        string(SUBSTRING "${_ymh_head}" 0 8 _ymh_hash)
        execute_process(
            COMMAND git -C "${source_dir}" status --porcelain --untracked-files=normal
            OUTPUT_VARIABLE _ymh_status
            RESULT_VARIABLE _ymh_status_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_ymh_status_rc EQUAL 0 AND NOT _ymh_status STREQUAL "")
            set(_ymh_dirty "x")
        endif()
    endif()

    set(_ymh_display "v${_ymh_date}-${_ymh_hash}${_ymh_dirty}")
    set(${out_var} "${_ymh_display}" PARENT_SCOPE)

    # Exactly one status line per configure, never in `cmake -P` mode.
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        message(STATUS "ymh: build version ${_ymh_display} (from git)")
    endif()
endfunction()

# Register the git state that the stamp depends on, so an ordinary
# `cmake --build` re-runs configure (and therefore the stamp) after a commit,
# checkout, stage, or tracked-file edit.
function(ymh_watch_version_inputs source_dir)
    if(CMAKE_SCRIPT_MODE_FILE)
        return()
    endif()

    set(_ymh_watch "${source_dir}")
    execute_process(
        COMMAND git -C "${source_dir}" rev-parse --git-path HEAD --git-path index
        OUTPUT_VARIABLE _ymh_git_paths
        RESULT_VARIABLE _ymh_git_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_ymh_git_rc EQUAL 0)
        string(REPLACE "\n" ";" _ymh_git_paths "${_ymh_git_paths}")
        foreach(_ymh_path IN LISTS _ymh_git_paths)
            get_filename_component(_ymh_abs "${source_dir}/${_ymh_path}" ABSOLUTE)
            list(APPEND _ymh_watch "${_ymh_abs}")
        endforeach()
    endif()

    execute_process(
        COMMAND git -C "${source_dir}" ls-files
        OUTPUT_VARIABLE _ymh_tracked
        RESULT_VARIABLE _ymh_ls_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_ymh_ls_rc EQUAL 0 AND NOT _ymh_tracked STREQUAL "")
        string(REPLACE "\n" ";" _ymh_tracked "${_ymh_tracked}")
        foreach(_ymh_file IN LISTS _ymh_tracked)
            set(_ymh_abs "${source_dir}/${_ymh_file}")
            list(APPEND _ymh_watch "${_ymh_abs}")
            # The containing directory catches untracked files being created or
            # deleted, which also have to flip the dirty marker.
            get_filename_component(_ymh_dir "${_ymh_abs}" DIRECTORY)
            list(APPEND _ymh_watch "${_ymh_dir}")
        endforeach()
    endif()

    if(_ymh_watch)
        list(REMOVE_DUPLICATES _ymh_watch)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ymh_watch})
    endif()
endfunction()

# Standalone script mode for the tests:
#
#   cmake -DYMH_VERSION_DIR=<repo> [-DYMH_VERSION_DATE=<date>]
#         [-DYMH_VERSION_OUT=<path>] -P cmake/version_stamp.cmake
#
# YMH_SOURCE_DIR / YMH_VERSION_SOURCE_DIR are accepted as aliases.
if(CMAKE_SCRIPT_MODE_FILE)
    if(DEFINED YMH_VERSION_DIR)
        set(_ymh_source_dir "${YMH_VERSION_DIR}")
    elseif(DEFINED YMH_VERSION_SOURCE_DIR)
        set(_ymh_source_dir "${YMH_VERSION_SOURCE_DIR}")
    elseif(DEFINED YMH_SOURCE_DIR)
        set(_ymh_source_dir "${YMH_SOURCE_DIR}")
    else()
        get_filename_component(_ymh_source_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    endif()

    ymh_stamp_version("${_ymh_source_dir}" _ymh_out)

    if(DEFINED YMH_VERSION_OUT AND NOT YMH_VERSION_OUT STREQUAL "")
        get_filename_component(_ymh_out_dir "${YMH_VERSION_OUT}" DIRECTORY)
        if(NOT _ymh_out_dir STREQUAL "")
            file(MAKE_DIRECTORY "${_ymh_out_dir}")
        endif()
        file(WRITE "${YMH_VERSION_OUT}" "${_ymh_out}\n")
    endif()

    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${_ymh_out}")
endif()
