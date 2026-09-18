cmake_minimum_required(VERSION 3.20)

project(WeakNetAIDiagnostics VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CTest)

option(ENABLE_EBPF "Build the existing V1 eBPF traffic observer" ON)
set(WEAKNET_SANITIZER "none" CACHE STRING
    "Project-code sanitizer: none, address-undefined, or thread")
set_property(CACHE WEAKNET_SANITIZER PROPERTY STRINGS none address-undefined thread)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(GLOG REQUIRED IMPORTED_TARGET libglog)

add_library(weaknet_build_options INTERFACE)
target_compile_features(weaknet_build_options INTERFACE cxx_std_20)
target_compile_options(weaknet_build_options INTERFACE
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

if(WEAKNET_SANITIZER STREQUAL "address-undefined")
    target_compile_options(weaknet_build_options INTERFACE
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fsanitize=address,undefined;-fno-omit-frame-pointer>)
    target_link_options(weaknet_build_options INTERFACE
        $<$<LINK_LANG_AND_ID:CXX,GNU,Clang>:-fsanitize=address,undefined>)
elseif(WEAKNET_SANITIZER STREQUAL "thread")
    target_compile_options(weaknet_build_options INTERFACE
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fsanitize=thread;-fno-omit-frame-pointer>)
    target_link_options(weaknet_build_options INTERFACE
        $<$<LINK_LANG_AND_ID:CXX,GNU,Clang>:-fsanitize=thread>)
elseif(NOT WEAKNET_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unknown WEAKNET_SANITIZER='${WEAKNET_SANITIZER}'")
endif()

set(WEAKNET_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${WEAKNET_GENERATED_DIR}/weaknet")

if(ENABLE_EBPF)
    set(WEAKNET_ENABLE_EBPF_VALUE 1)
else()
    set(WEAKNET_ENABLE_EBPF_VALUE 0)
endif()

set(WEAKNET_BPF_RELATIVE_PATH
    "${CMAKE_INSTALL_LIBEXECDIR}/weaknet/flow_rate.bpf.o")
set(WEAKNET_PING_HELPER_RELATIVE_PATH
    "${CMAKE_INSTALL_LIBEXECDIR}/weaknet/weaknet-ping-helper")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/weaknet_build_config.hpp.in"
    "${WEAKNET_GENERATED_DIR}/weaknet/build_config.hpp"
    @ONLY
)

if(ENABLE_EBPF)
    pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)
    pkg_check_modules(LIBELF REQUIRED IMPORTED_TARGET libelf)
    pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/WeakNetBpf.cmake")
    weaknet_add_v1_bpf_target(WEAKNET_BPF_OBJECT)
endif()

add_library(weaknet_serialization STATIC
    server/src/serializer.cpp
)
target_link_libraries(weaknet_serialization PUBLIC weaknet_build_options)
target_include_directories(weaknet_serialization
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/server/include>
)
set_target_properties(weaknet_serialization PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(weaknet_runtime STATIC
    server/src/runtime_config.cpp
)
target_link_libraries(weaknet_runtime PUBLIC weaknet_build_options)
target_include_directories(weaknet_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/server/include>
)
set_target_properties(weaknet_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(weaknet_quality STATIC
    server/src/network_quality_assessor.cpp
)

add_library(weaknet_data_plane STATIC
    server/src/clock.cpp
    server/src/network_event.cpp
    server/src/event_bus.cpp
    server/src/metric_store.cpp
    server/src/v1_observation_adapter.cpp
)
target_link_libraries(weaknet_data_plane
    PUBLIC
        weaknet_build_options
        Threads::Threads
)
target_include_directories(weaknet_data_plane
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/server/include>
)
target_link_libraries(weaknet_quality
    PUBLIC weaknet_build_options
    PRIVATE PkgConfig::GLOG
)
target_include_directories(weaknet_quality
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/server/include>
)

set(WEAKNET_SERVER_SOURCES
    server/src/application.cpp
    server/src/runtime_health.cpp
    server/src/ping_executor.cpp
    server/src/server.cpp
    server/src/dbus_service.cpp
    server/src/looper.cpp
    server/src/net_iface.cpp
    server/src/net_info.cpp
    server/src/weak_netmgr.cpp
    server/src/using_iface.cpp
    server/src/net_ping.cpp
    server/src/rtt_monitor.cpp
    server/src/rssi_monitor.cpp
    server/src/net_wifiriss.cpp
    server/src/net_tcp.cpp
    server/src/tcp_loss_monitor.cpp
    server/src/event_manager.cpp
    server/src/logger.cpp
    server/src/traffic_analyzer.cpp
    server/src/net_traffic.cpp
    server/src/traffic_anomaly_detector.cpp
)

add_library(weaknet_server_core STATIC ${WEAKNET_SERVER_SOURCES})
target_link_libraries(weaknet_server_core
    PUBLIC
        weaknet_build_options
        weaknet_serialization
        weaknet_quality
        weaknet_runtime
        weaknet_data_plane
    PRIVATE
        Threads::Threads
        PkgConfig::DBUS
        PkgConfig::GLOG
)
target_include_directories(weaknet_server_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/server/include>
    PRIVATE
        "${WEAKNET_GENERATED_DIR}"
)

if(ENABLE_EBPF)
    target_link_libraries(weaknet_server_core PRIVATE
        PkgConfig::LIBBPF
        PkgConfig::LIBELF
        PkgConfig::ZLIB
    )
endif()

add_executable(weaknet-dbus-server server/src/main.cpp)
target_link_libraries(weaknet-dbus-server PRIVATE weaknet_server_core)
set_target_properties(weaknet-dbus-server PROPERTIES
    BUILD_RPATH "$ORIGIN/../lib"
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
)
if(ENABLE_EBPF)
    add_dependencies(weaknet-dbus-server weaknet_bpf)
endif()

add_executable(weaknet-ping-helper server/src/ping_helper_main.cpp)
target_link_libraries(weaknet-ping-helper PRIVATE weaknet_server_core)
set_target_properties(weaknet-ping-helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBEXECDIR}/weaknet")

add_library(weaknet_client SHARED client/client.cpp)
set_target_properties(weaknet_client PROPERTIES OUTPUT_NAME weaknet)
target_link_libraries(weaknet_client
    PRIVATE
        weaknet_build_options
        weaknet_serialization
        weaknet_runtime
        Threads::Threads
        PkgConfig::DBUS
        PkgConfig::GLOG
)
target_include_directories(weaknet_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/client>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/server/include"
)

function(weaknet_add_client_tool target source)
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE
        weaknet_build_options
        weaknet_client
        Threads::Threads
        PkgConfig::GLOG
    )
    set_target_properties(${target} PROPERTIES
        BUILD_RPATH "$ORIGIN/../lib"
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
endfunction()

weaknet_add_client_tool(test-client client/test_client.cpp)
target_include_directories(test-client PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/server/include")
weaknet_add_client_tool(example-client client/example_usage.cpp)
weaknet_add_client_tool(ping-example client/ping_example.cpp)

install(TARGETS
    weaknet-dbus-server
    weaknet_client
    test-client
    example-client
    ping-example
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(TARGETS weaknet-ping-helper
    RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/weaknet")
install(FILES client/weaknet_client.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(ENABLE_EBPF)
    install(FILES "${WEAKNET_BPF_OBJECT}"
        DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/weaknet")
endif()

set(_legacy_server_commands
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/server/bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/server/build"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:weaknet-dbus-server>"
        "${CMAKE_CURRENT_SOURCE_DIR}/server/bin/weaknet-dbus-server"
)
set(_legacy_server_dependencies weaknet-dbus-server)
list(APPEND _legacy_server_commands
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/server/libexec/weaknet"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:weaknet-ping-helper>"
        "${CMAKE_CURRENT_SOURCE_DIR}/server/libexec/weaknet/weaknet-ping-helper"
)
list(APPEND _legacy_server_dependencies weaknet-ping-helper)
if(ENABLE_EBPF)
    list(APPEND _legacy_server_commands
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${WEAKNET_BPF_OBJECT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/server/build/flow_rate.bpf.o"
    )
    list(APPEND _legacy_server_dependencies weaknet_bpf)
else()
    list(APPEND _legacy_server_commands
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "${CMAKE_CURRENT_SOURCE_DIR}/server/build/flow_rate.bpf.o"
    )
endif()

add_custom_target(weaknet_server_legacy_stage
    ${_legacy_server_commands}
    DEPENDS ${_legacy_server_dependencies}
    VERBATIM
)

add_custom_target(weaknet_legacy_stage
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/client/bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/client/lib"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:weaknet_client>"
        "${CMAKE_CURRENT_SOURCE_DIR}/client/lib/libweaknet.so"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:test-client>"
        "${CMAKE_CURRENT_SOURCE_DIR}/client/bin/test-client"
    DEPENDS
        weaknet_server_legacy_stage
        weaknet_client
        test-client
    VERBATIM
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

message(STATUS "WeakNet Phase 1 configuration")
message(STATUS "  C++ standard: C++20")
message(STATUS "  ENABLE_EBPF: ${ENABLE_EBPF}")
message(STATUS "  BUILD_TESTING: ${BUILD_TESTING}")
message(STATUS "  WEAKNET_SANITIZER: ${WEAKNET_SANITIZER}")
message(STATUS "  dbus-1: ${DBUS_VERSION}")
message(STATUS "  glog: ${GLOG_VERSION}")
if(ENABLE_EBPF)
    message(STATUS "  libbpf: ${LIBBPF_VERSION}")
    message(STATUS "  libelf: ${LIBELF_VERSION}")
    message(STATUS "  zlib: ${ZLIB_VERSION}")
endif()
