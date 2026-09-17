find_program(BASH_EXECUTABLE NAMES bash REQUIRED)

function(weaknet_add_cpp_test target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE weaknet_build_options)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

weaknet_add_cpp_test(serializer_roundtrip serializer_test.cpp)
target_link_libraries(serializer_roundtrip PRIVATE weaknet_serialization)

weaknet_add_cpp_test(network_quality_baseline network_quality_assessor_test.cpp)
target_link_libraries(network_quality_baseline PRIVATE weaknet_quality PkgConfig::GLOG)

add_executable(c_header_smoke c_header_smoke.c)
target_include_directories(c_header_smoke PRIVATE "${PROJECT_SOURCE_DIR}/client")
set_target_properties(c_header_smoke PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)
add_test(NAME c_header_smoke COMMAND c_header_smoke)

weaknet_add_cpp_test(cxx_header_smoke cxx_header_smoke.cpp)
target_include_directories(cxx_header_smoke PRIVATE "${PROJECT_SOURCE_DIR}/client")

weaknet_add_cpp_test(v1_client_metadata v1_client_metadata_test.cpp)
target_link_libraries(v1_client_metadata PRIVATE weaknet_client)
set_target_properties(v1_client_metadata PROPERTIES
    BUILD_RPATH "$ORIGIN/../lib"
)

add_executable(v1_dbus_contract v1_dbus_contract_test.cpp)
target_link_libraries(v1_dbus_contract PRIVATE weaknet_build_options)
target_include_directories(v1_dbus_contract PRIVATE
    "${PROJECT_SOURCE_DIR}/server/include")
target_link_libraries(v1_dbus_contract PRIVATE PkgConfig::DBUS)
add_test(NAME v1_dbus_contract_fixture
    COMMAND v1_dbus_contract
        "${CMAKE_CURRENT_SOURCE_DIR}/contracts/v1_dbus.contract")

add_test(NAME v1_c_abi_contract
    COMMAND "${CMAKE_COMMAND}"
        "-DNM_EXECUTABLE:FILEPATH=${CMAKE_NM}"
        "-DLIBRARY_PATH:FILEPATH=$<TARGET_FILE:weaknet_client>"
        "-DEXPECTED_FILE:FILEPATH=${CMAKE_CURRENT_SOURCE_DIR}/contracts/v1_c_abi.symbols"
        -P "${PROJECT_SOURCE_DIR}/cmake/CheckCAbi.cmake")

add_test(NAME shell_install_syntax
    COMMAND "${BASH_EXECUTABLE}" -n "${PROJECT_SOURCE_DIR}/install.sh")
add_test(NAME shell_rag_install_syntax
    COMMAND "${BASH_EXECUTABLE}" -n
        "${PROJECT_SOURCE_DIR}/optional/experimental/log-analysis-tools/install_rag.sh")

if(ENABLE_EBPF)
    find_program(READELF_EXECUTABLE NAMES readelf REQUIRED)
    add_test(NAME bpf_object_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DBPF_OBJECT:FILEPATH=${WEAKNET_BPF_OBJECT}"
            "-DBPFTOOL_EXECUTABLE:FILEPATH=${WEAKNET_BPFTOOL_EXECUTABLE}"
            "-DREADELF_EXECUTABLE:FILEPATH=${READELF_EXECUTABLE}"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckBpfObject.cmake")
endif()
