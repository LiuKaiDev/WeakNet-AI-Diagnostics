include(TestBigEndian)

set(WEAKNET_VMLINUX_HEADER "" CACHE FILEPATH
    "Use a pre-generated vmlinux.h instead of generating one from kernel BTF")
set(WEAKNET_VMLINUX_BTF "/sys/kernel/btf/vmlinux" CACHE FILEPATH
    "BTF file used to generate the build-tree vmlinux.h")
set(WEAKNET_CLANG_EXECUTABLE "" CACHE FILEPATH "Clang executable with BPF support")
set(BPFTOOL_EXECUTABLE "" CACHE FILEPATH "bpftool executable")

function(_weaknet_map_bpf_arch output)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _processor)
    if(_processor MATCHES "^(x86_64|amd64|i[3-6]86)$")
        set(_arch x86)
    elseif(_processor MATCHES "^(aarch64|arm64)$")
        set(_arch arm64)
    elseif(_processor MATCHES "^arm")
        set(_arch arm)
    elseif(_processor MATCHES "^s390")
        set(_arch s390)
    elseif(_processor MATCHES "^(ppc|powerpc)")
        set(_arch powerpc)
    elseif(_processor MATCHES "^mips")
        set(_arch mips)
    elseif(_processor MATCHES "^riscv")
        set(_arch riscv)
    elseif(_processor MATCHES "^sparc")
        set(_arch sparc)
    elseif(_processor MATCHES "^arc")
        set(_arch arc)
    elseif(_processor MATCHES "^loongarch")
        set(_arch loongarch)
    else()
        message(FATAL_ERROR
            "ENABLE_EBPF=ON does not support CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'")
    endif()
    set(${output} "${_arch}" PARENT_SCOPE)
endfunction()

function(weaknet_add_v1_bpf_target output_variable)
    if(NOT WEAKNET_CLANG_EXECUTABLE)
        find_program(_weaknet_clang NAMES clang)
        if(NOT _weaknet_clang)
            message(FATAL_ERROR "ENABLE_EBPF=ON requires Clang with a BPF backend")
        endif()
        set(WEAKNET_CLANG_EXECUTABLE "${_weaknet_clang}" CACHE FILEPATH
            "Clang executable with BPF support" FORCE)
    elseif(NOT EXISTS "${WEAKNET_CLANG_EXECUTABLE}")
        message(FATAL_ERROR "WEAKNET_CLANG_EXECUTABLE does not exist: ${WEAKNET_CLANG_EXECUTABLE}")
    endif()

    _weaknet_map_bpf_arch(_bpf_arch)
    test_big_endian(_is_big_endian)
    if(_is_big_endian)
        set(_bpf_target bpfeb)
    else()
        set(_bpf_target bpfel)
    endif()

    set(_probe_source "${CMAKE_BINARY_DIR}/CMakeFiles/weaknet_bpf_probe.c")
    set(_probe_object "${CMAKE_BINARY_DIR}/CMakeFiles/weaknet_bpf_probe.o")
    file(WRITE "${_probe_source}" "int weaknet_bpf_probe(void) { return 0; }\n")
    execute_process(
        COMMAND "${WEAKNET_CLANG_EXECUTABLE}" -target "${_bpf_target}"
            -O2 -c "${_probe_source}" -o "${_probe_object}"
        RESULT_VARIABLE _probe_result
        ERROR_VARIABLE _probe_error
    )
    file(REMOVE "${_probe_source}" "${_probe_object}")
    if(NOT _probe_result EQUAL 0)
        message(FATAL_ERROR
            "ENABLE_EBPF=ON requires a working Clang BPF backend: ${_probe_error}")
    endif()

    if(WEAKNET_VMLINUX_HEADER)
        if(NOT EXISTS "${WEAKNET_VMLINUX_HEADER}")
            message(FATAL_ERROR
                "WEAKNET_VMLINUX_HEADER does not exist: ${WEAKNET_VMLINUX_HEADER}")
        endif()
        set(_vmlinux_source "pre-generated header: ${WEAKNET_VMLINUX_HEADER}")
        set(_vmlinux_args
            "-DINPUT_HEADER:FILEPATH=${WEAKNET_VMLINUX_HEADER}")
    else()
        if(NOT EXISTS "${WEAKNET_VMLINUX_BTF}")
            message(FATAL_ERROR
                "ENABLE_EBPF=ON requires WEAKNET_VMLINUX_HEADER or readable BTF at ${WEAKNET_VMLINUX_BTF}")
        endif()
        if(NOT BPFTOOL_EXECUTABLE)
            find_program(_weaknet_bpftool NAMES bpftool)
            if(NOT _weaknet_bpftool)
                message(FATAL_ERROR
                    "ENABLE_EBPF=ON requires bpftool when generating vmlinux.h from BTF")
            endif()
            set(BPFTOOL_EXECUTABLE "${_weaknet_bpftool}" CACHE FILEPATH
                "bpftool executable" FORCE)
        elseif(NOT EXISTS "${BPFTOOL_EXECUTABLE}")
            message(FATAL_ERROR "BPFTOOL_EXECUTABLE does not exist: ${BPFTOOL_EXECUTABLE}")
        endif()
        set(_vmlinux_source "BTF: ${WEAKNET_VMLINUX_BTF}")
        set(_vmlinux_args
            "-DBTF_FILE:FILEPATH=${WEAKNET_VMLINUX_BTF}"
            "-DBPFTOOL_EXECUTABLE:FILEPATH=${BPFTOOL_EXECUTABLE}")
    endif()

    set(_generated_bpf_dir "${CMAKE_BINARY_DIR}/generated/bpf")
    set(_vmlinux_header "${_generated_bpf_dir}/vmlinux.h")
    add_custom_command(
        OUTPUT "${_vmlinux_header}"
        COMMAND "${CMAKE_COMMAND}"
            "-DOUTPUT_FILE:FILEPATH=${_vmlinux_header}"
            ${_vmlinux_args}
            -P "${CMAKE_SOURCE_DIR}/cmake/GenerateVmlinux.cmake"
        DEPENDS
            "${CMAKE_SOURCE_DIR}/cmake/GenerateVmlinux.cmake"
            $<$<BOOL:${WEAKNET_VMLINUX_HEADER}>:${WEAKNET_VMLINUX_HEADER}>
            $<$<NOT:$<BOOL:${WEAKNET_VMLINUX_HEADER}>>:${WEAKNET_VMLINUX_BTF}>
        COMMENT "Generating build-tree vmlinux.h"
        VERBATIM
    )

    set(_bpf_output_dir "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBEXECDIR}/weaknet")
    set(_bpf_object "${_bpf_output_dir}/flow_rate.bpf.o")
    set(_bpf_include_args "-I${_generated_bpf_dir}")
    foreach(_include_dir IN LISTS LIBBPF_INCLUDE_DIRS)
        list(APPEND _bpf_include_args "-I${_include_dir}")
    endforeach()
    add_custom_command(
        OUTPUT "${_bpf_object}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bpf_output_dir}"
        COMMAND "${WEAKNET_CLANG_EXECUTABLE}"
            -g -O2 -target "${_bpf_target}"
            "-D__TARGET_ARCH_${_bpf_arch}"
            ${_bpf_include_args}
            -c "${CMAKE_SOURCE_DIR}/server/src/flow_rate.bpf.c"
            -o "${_bpf_object}"
        DEPENDS
            "${CMAKE_SOURCE_DIR}/server/src/flow_rate.bpf.c"
            "${_vmlinux_header}"
        COMMENT "Building V1 flow_rate eBPF object for ${_bpf_arch}/${_bpf_target}"
        VERBATIM
    )
    add_custom_target(weaknet_bpf ALL DEPENDS "${_bpf_object}")

    execute_process(
        COMMAND "${WEAKNET_CLANG_EXECUTABLE}" --version
        OUTPUT_VARIABLE _clang_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REGEX MATCH "^[^\n]+" _clang_version_line "${_clang_version}")

    set(_bpftool_version "not required (pre-generated header selected)")
    if(BPFTOOL_EXECUTABLE)
        execute_process(
            COMMAND "${BPFTOOL_EXECUTABLE}" version
            OUTPUT_VARIABLE _bpftool_version
            ERROR_VARIABLE _bpftool_version_error
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_bpftool_version STREQUAL "")
            set(_bpftool_version "${_bpftool_version_error}")
        endif()
        string(REPLACE "\n" "; " _bpftool_version "${_bpftool_version}")

        string(REGEX MATCH "using libbpf v([0-9]+\.[0-9]+)" _bundled_match "${_bpftool_version}")
        if(_bundled_match)
            set(_bpftool_libbpf "${CMAKE_MATCH_1}")
            string(REGEX MATCH "^[0-9]+\.[0-9]+" _system_libbpf "${LIBBPF_VERSION}")
            if(NOT _bpftool_libbpf STREQUAL _system_libbpf)
                message(WARNING
                    "bpftool reports bundled libbpf ${_bpftool_libbpf}, while the development library is ${LIBBPF_VERSION}. "
                    "Header generation is supported, but this local toolchain mismatch is a reproducibility risk.")
            endif()
        endif()
    endif()

    message(STATUS "  BPF architecture: ${_bpf_arch} (${_bpf_target})")
    if(_bpf_arch STREQUAL "x86" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
        message(STATUS "  BPF architecture verification: x86_64 mapping verified on this host")
    else()
        message(STATUS "  BPF architecture verification: mapping configured but not verified on this host")
    endif()
    message(STATUS "  Clang BPF compiler: ${WEAKNET_CLANG_EXECUTABLE} (${_clang_version_line})")
    message(STATUS "  vmlinux.h source: ${_vmlinux_source}")
    message(STATUS "  generated vmlinux.h: ${_vmlinux_header}")
    message(STATUS "  bpftool: ${BPFTOOL_EXECUTABLE}")
    message(STATUS "  bpftool version: ${_bpftool_version}")
    message(STATUS "  BPF object: ${_bpf_object}")

    set(${output_variable} "${_bpf_object}" PARENT_SCOPE)
    set(WEAKNET_BPFTOOL_EXECUTABLE "${BPFTOOL_EXECUTABLE}" PARENT_SCOPE)
endfunction()
