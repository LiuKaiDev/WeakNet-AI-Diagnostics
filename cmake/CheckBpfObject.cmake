if(NOT DEFINED BPF_OBJECT OR NOT EXISTS "${BPF_OBJECT}")
    message(FATAL_ERROR "BPF object is missing: ${BPF_OBJECT}")
endif()
file(SIZE "${BPF_OBJECT}" _size)
if(_size EQUAL 0)
    message(FATAL_ERROR "BPF object is empty: ${BPF_OBJECT}")
endif()

execute_process(
    COMMAND "${READELF_EXECUTABLE}" -h "${BPF_OBJECT}"
    RESULT_VARIABLE _readelf_result
    OUTPUT_VARIABLE _readelf_output
    ERROR_VARIABLE _readelf_error
)
if(NOT _readelf_result EQUAL 0 OR NOT _readelf_output MATCHES "Linux BPF")
    message(FATAL_ERROR "readelf did not recognize a Linux BPF object: ${_readelf_error}")
endif()

if(DEFINED BPFTOOL_EXECUTABLE AND NOT BPFTOOL_EXECUTABLE STREQUAL "")
    execute_process(
        COMMAND "${BPFTOOL_EXECUTABLE}" gen skeleton "${BPF_OBJECT}"
        RESULT_VARIABLE _bpftool_result
        OUTPUT_QUIET
        ERROR_VARIABLE _bpftool_error
    )
    if(NOT _bpftool_result EQUAL 0)
        message(FATAL_ERROR "bpftool could not parse the BPF object: ${_bpftool_error}")
    endif()
    message(STATUS "BPF object compiled and skeleton parsing succeeded (${_size} bytes)")
else()
    message(STATUS "BPF object compiled and ELF parsing succeeded (${_size} bytes); bpftool unavailable")
endif()
