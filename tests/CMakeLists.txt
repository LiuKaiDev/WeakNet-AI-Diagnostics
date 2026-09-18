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

weaknet_add_cpp_test(runtime_foundation runtime_foundation_test.cpp)
target_link_libraries(runtime_foundation PRIVATE weaknet_runtime weaknet_server_core)

weaknet_add_cpp_test(ping_executor_lifecycle ping_executor_test.cpp)
target_link_libraries(ping_executor_lifecycle PRIVATE weaknet_server_core)

add_executable(ping_helper_fixture ping_helper_fixture.cpp)
target_link_libraries(ping_helper_fixture PRIVATE weaknet_build_options Threads::Threads)

add_executable(ping_helper_lifecycle ping_helper_lifecycle_test.cpp)
target_link_libraries(ping_helper_lifecycle PRIVATE weaknet_server_core)
add_test(NAME ping_helper_lifecycle
    COMMAND $<TARGET_FILE:ping_helper_lifecycle> $<TARGET_FILE:ping_helper_fixture>)
set_tests_properties(ping_helper_lifecycle PROPERTIES LABELS "phase2")

add_executable(test_daemon_fixture test_daemon_fixture.cpp)
target_link_libraries(test_daemon_fixture PRIVATE weaknet_server_core)

add_executable(application_lifecycle application_lifecycle_test.cpp)
target_link_libraries(application_lifecycle PRIVATE weaknet_server_core)

add_executable(daemon_process_lifecycle daemon_process_test.cpp)
target_link_libraries(daemon_process_lifecycle PRIVATE weaknet_build_options PkgConfig::DBUS)

add_executable(client_lifetime client_lifetime_test.cpp)
target_link_libraries(client_lifetime PRIVATE
    weaknet_build_options weaknet_client Threads::Threads)
set_target_properties(client_lifetime PROPERTIES BUILD_RPATH "$ORIGIN/../lib")

find_program(DBUS_RUN_SESSION_EXECUTABLE NAMES dbus-run-session REQUIRED)
add_test(NAME application_lifecycle
    COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" -- $<TARGET_FILE:application_lifecycle>)
set_tests_properties(application_lifecycle PROPERTIES TIMEOUT 45 LABELS "phase2;process")

add_test(NAME daemon_process_lifecycle
    COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" --
        $<TARGET_FILE:daemon_process_lifecycle>
        $<TARGET_FILE:weaknet-dbus-server>
        $<TARGET_FILE:test_daemon_fixture>)
set_tests_properties(daemon_process_lifecycle PROPERTIES TIMEOUT 45 LABELS "phase2;process")

add_test(NAME client_lifetime
    COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" -- $<TARGET_FILE:client_lifetime>)
set_tests_properties(client_lifetime PROPERTIES TIMEOUT 30 LABELS "phase2;client")

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
