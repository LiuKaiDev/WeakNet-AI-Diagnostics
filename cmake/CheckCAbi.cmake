foreach(_required NM_EXECUTABLE LIBRARY_PATH EXPECTED_FILE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "CheckCAbi.cmake requires ${_required}")
    endif()
endforeach()

execute_process(
    COMMAND "${NM_EXECUTABLE}" -D --defined-only "${LIBRARY_PATH}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error
)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "nm failed for ${LIBRARY_PATH}: ${_error}")
endif()

string(REPLACE "\r\n" "\n" _output "${_output}")
string(REPLACE "\n" ";" _lines "${_output}")
set(_actual)
foreach(_line IN LISTS _lines)
    string(REGEX MATCH "[A-Za-z_][A-Za-z0-9_]*$" _symbol "${_line}")
    if(_symbol MATCHES "^weaknet_")
        list(APPEND _actual "${_symbol}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _actual)
list(SORT _actual)

file(STRINGS "${EXPECTED_FILE}" _expected)
list(FILTER _expected EXCLUDE REGEX "^[ \t]*(#|$)")
list(SORT _expected)

if(NOT _actual STREQUAL _expected)
    string(JOIN "\n" _actual_text ${_actual})
    string(JOIN "\n" _expected_text ${_expected})
    message(FATAL_ERROR
        "V1 C ABI mismatch\nExpected:\n${_expected_text}\nActual:\n${_actual_text}")
endif()

list(LENGTH _actual _count)
message(STATUS "V1 C ABI contract matched ${_count} exported symbols")
