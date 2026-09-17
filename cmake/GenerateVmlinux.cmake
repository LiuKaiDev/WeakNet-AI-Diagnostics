if(NOT DEFINED OUTPUT_FILE OR OUTPUT_FILE STREQUAL "")
    message(FATAL_ERROR "GenerateVmlinux.cmake requires OUTPUT_FILE")
endif()

get_filename_component(_output_dir "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_dir}")
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _suffix)
set(_temporary_file "${OUTPUT_FILE}.tmp-${_suffix}")

if(DEFINED INPUT_HEADER AND NOT INPUT_HEADER STREQUAL "")
    if(NOT EXISTS "${INPUT_HEADER}")
        message(FATAL_ERROR "WEAKNET_VMLINUX_HEADER does not exist: ${INPUT_HEADER}")
    endif()
    configure_file("${INPUT_HEADER}" "${_temporary_file}" COPYONLY)
else()
    if(NOT DEFINED BTF_FILE OR BTF_FILE STREQUAL "" OR NOT EXISTS "${BTF_FILE}")
        message(FATAL_ERROR "Readable kernel BTF is required: ${BTF_FILE}")
    endif()
    if(NOT DEFINED BPFTOOL_EXECUTABLE OR BPFTOOL_EXECUTABLE STREQUAL "")
        message(FATAL_ERROR "bpftool is required to generate vmlinux.h from BTF")
    endif()

    execute_process(
        COMMAND "${BPFTOOL_EXECUTABLE}" btf dump file "${BTF_FILE}" format c
        RESULT_VARIABLE _result
        OUTPUT_FILE "${_temporary_file}"
        ERROR_VARIABLE _error
    )
    if(NOT _result EQUAL 0)
        file(REMOVE "${_temporary_file}")
        message(FATAL_ERROR "bpftool failed to generate vmlinux.h: ${_error}")
    endif()
endif()

file(SIZE "${_temporary_file}" _size)
if(_size EQUAL 0)
    file(REMOVE "${_temporary_file}")
    message(FATAL_ERROR "Generated vmlinux.h is empty")
endif()

file(RENAME "${_temporary_file}" "${OUTPUT_FILE}")
