# 96-D1..96-D7: configure-time build stamp.
#
# `.version_info` holds one canonical line `<DD>-<MM>-<YYYY>:<NN>` and is
# gitignored. Every configure advances the counter (same local date) or resets
# it (new date, missing, empty, or malformed file), then the display value is
# `<DD>-<Mon>-<YYYY>:#<NN>` (fixed English month table, locale independent).
#
# All file I/O goes through `execute_process`, so a read-only file or directory
# warns instead of aborting the configure (96-D6/96-D7). No `file(LOCK)`, no
# `file(SIZE)`.

# Strip leading zeroes from a decimal digit string while keeping at least one
# digit, so `math(EXPR)` never sees an octal-looking literal.
function(_ymh_norm_num out_var value)
    if(value MATCHES "^0+([0-9]+)$")
        set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    elseif(value MATCHES "^0+$")
        set(${out_var} "0" PARENT_SCOPE)
    else()
        set(${out_var} "${value}" PARENT_SCOPE)
    endif()
endfunction()

function(ymh_stamp_version file out_var)
    # 96-D5: date source. A malformed or out-of-range override is ignored with
    # a warning; the local date is used instead.
    set(_ymh_date "")
    if(DEFINED YMH_VERSION_DATE)
        if("${YMH_VERSION_DATE}" STREQUAL "")
            message(WARNING "ymh: ignoring empty YMH_VERSION_DATE; using the local date")
        elseif(YMH_VERSION_DATE MATCHES "^[0-9][0-9]-[0-9][0-9]-[0-9][0-9][0-9][0-9]$")
            string(SUBSTRING "${YMH_VERSION_DATE}" 0 2 _ymh_ov_dd)
            string(SUBSTRING "${YMH_VERSION_DATE}" 3 2 _ymh_ov_mm)
            if(_ymh_ov_dd MATCHES "^(0[1-9]|[12][0-9]|3[01])$"
               AND _ymh_ov_mm MATCHES "^(0[1-9]|1[0-2])$")
                set(_ymh_date "${YMH_VERSION_DATE}")
            else()
                message(WARNING
                    "ymh: ignoring out-of-range YMH_VERSION_DATE='${YMH_VERSION_DATE}'; using the local date")
            endif()
        else()
            message(WARNING
                "ymh: ignoring malformed YMH_VERSION_DATE='${YMH_VERSION_DATE}'; using the local date")
        endif()
    endif()
    if(NOT _ymh_date)
        string(TIMESTAMP _ymh_date "%d-%m-%Y")
    endif()
    string(SUBSTRING "${_ymh_date}" 0 2 _ymh_dd)
    string(SUBSTRING "${_ymh_date}" 3 2 _ymh_mm)
    string(SUBSTRING "${_ymh_date}" 6 4 _ymh_yyyy)
    _ymh_norm_num(_ymh_dd_num "${_ymh_dd}")
    _ymh_norm_num(_ymh_mm_num "${_ymh_mm}")
    _ymh_norm_num(_ymh_yyyy_num "${_ymh_yyyy}")

    # 96-D7: "cmake -E cat" is non-fatal for a missing, unreadable, or
    # directory path; any failure (or an empty result) is treated as absent.
    set(_ymh_body "")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E cat "${file}"
        OUTPUT_VARIABLE _ymh_body
        RESULT_VARIABLE _ymh_read_rc
        ERROR_QUIET
    )
    string(STRIP "${_ymh_body}" _ymh_body)
    if(NOT _ymh_read_rc EQUAL 0)
        set(_ymh_body "")
    endif()

    # 96-D1: lenient read, canonical write. CMake's regex engine has no
    # `{m,n}` quantifier, so the repeat groups are explicit.
    set(_ymh_n 1)
    if(_ymh_body MATCHES "^([0-9][0-9]?)-([0-9][0-9]?)-([0-9][0-9][0-9][0-9]):([0-9]+)$")
        set(_ymh_fdd "${CMAKE_MATCH_1}")
        set(_ymh_fmm "${CMAKE_MATCH_2}")
        set(_ymh_fyyyy "${CMAKE_MATCH_3}")
        set(_ymh_fcounter "${CMAKE_MATCH_4}")
        _ymh_norm_num(_ymh_fdd_num "${_ymh_fdd}")
        _ymh_norm_num(_ymh_fmm_num "${_ymh_fmm}")
        _ymh_norm_num(_ymh_fyyyy_num "${_ymh_fyyyy}")
        if(_ymh_fdd_num STREQUAL _ymh_dd_num
           AND _ymh_fmm_num STREQUAL _ymh_mm_num
           AND _ymh_fyyyy_num STREQUAL _ymh_yyyy_num)
            _ymh_norm_num(_ymh_counter "${_ymh_fcounter}")
            string(LENGTH "${_ymh_counter}" _ymh_counter_len)
            if(_ymh_counter_len GREATER 4)
                set(_ymh_n 9999)
                message(WARNING "ymh: build counter is above 9999; reusing #9999")
            else()
                math(EXPR _ymh_next "${_ymh_counter} + 1")
                if(_ymh_next GREATER 9999)
                    set(_ymh_n 9999)
                    message(WARNING "ymh: build counter is above 9999; reusing #9999")
                else()
                    set(_ymh_n "${_ymh_next}")
                endif()
            endif()
        endif()
    endif()

    # 96-D2: minimum two digits for day and counter, fixed English month table.
    string(LENGTH "${_ymh_n}" _ymh_n_len)
    if(_ymh_n_len LESS 2)
        set(_ymh_nn "0${_ymh_n}")
    else()
        set(_ymh_nn "${_ymh_n}")
    endif()
    math(EXPR _ymh_month_index "${_ymh_mm_num} - 1")
    set(_ymh_months Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec)
    list(GET _ymh_months ${_ymh_month_index} _ymh_mon)
    set(_ymh_display "${_ymh_dd}-${_ymh_mon}-${_ymh_yyyy}:#${_ymh_nn}")

    # 96-D6: write a temp file, read it back, and only then rename it over the
    # stamp. Every step uses execute_process, so failures warn and continue.
    string(RANDOM _ymh_suffix)
    set(_ymh_tmp "${file}.tmp-${_ymh_suffix}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E echo "${_ymh_date}:${_ymh_nn}"
        OUTPUT_FILE "${_ymh_tmp}"
        RESULT_VARIABLE _ymh_echo_rc
        ERROR_QUIET
    )
    set(_ymh_tmp_body "")
    set(_ymh_cat_rc 1)
    if(EXISTS "${_ymh_tmp}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E cat "${_ymh_tmp}"
            OUTPUT_VARIABLE _ymh_tmp_body
            RESULT_VARIABLE _ymh_cat_rc
            ERROR_QUIET
        )
    endif()
    string(STRIP "${_ymh_tmp_body}" _ymh_tmp_body)
    if(NOT _ymh_cat_rc EQUAL 0 OR NOT _ymh_tmp_body STREQUAL "${_ymh_date}:${_ymh_nn}")
        message(WARNING "ymh: cannot write a complete ${_ymh_tmp}; version not persisted")
        file(REMOVE "${_ymh_tmp}")
    else()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E rename "${_ymh_tmp}" "${file}"
            RESULT_VARIABLE _ymh_rename_rc
            ERROR_QUIET
        )
        if(NOT _ymh_rename_rc EQUAL 0)
            message(WARNING "ymh: cannot replace ${file} (rename ${_ymh_rename_rc})")
            file(REMOVE "${_ymh_tmp}")
        endif()
    endif()

    set(${out_var} "${_ymh_display}" PARENT_SCOPE)

    # 96-D13: exactly one status line per configure, never in `cmake -P` mode.
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        message(STATUS "ymh: build version ${_ymh_display} (from .version_info)")
    endif()
endfunction()

# 96-D14: standalone script mode for the tests.
if(CMAKE_SCRIPT_MODE_FILE)
    ymh_stamp_version("${YMH_VERSION_FILE}" _ymh_out)
    file(WRITE "${YMH_VERSION_OUT}" "${_ymh_out}\n")
endif()
