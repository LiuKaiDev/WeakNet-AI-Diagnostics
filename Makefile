# Temporary V1 compatibility wrapper around the Phase 1 CMake build.

CMAKE ?= cmake
CTEST ?= ctest
CMAKE_GENERATOR ?= Ninja
CMAKE_BUILD_DIR ?= build/make-compat
CMAKE_BUILD_TYPE ?= RelWithDebInfo
ENABLE_EBPF ?= ON
BUILD_TESTING ?= ON

CMAKE_CONFIGURE = $(CMAKE) -S . -B "$(CMAKE_BUILD_DIR)" \
	-G "$(CMAKE_GENERATOR)" \
	-DCMAKE_BUILD_TYPE="$(CMAKE_BUILD_TYPE)" \
	-DENABLE_EBPF="$(ENABLE_EBPF)" \
	-DBUILD_TESTING="$(BUILD_TESTING)"

.PHONY: all configure dirs server-client-lib server-client clean \
	run-server run-client test-client test-lib test-events test-all \
	test-ping test-performance

all: server-client-lib

configure:
	@$(CMAKE_CONFIGURE)

dirs: configure

server-client-lib: configure
	@$(CMAKE) --build "$(CMAKE_BUILD_DIR)" --target weaknet_legacy_stage --parallel

server-client: server-client-lib

clean:
	@$(CMAKE) -E rm -rf "$(CMAKE_BUILD_DIR)" \
		server/bin server/build server/libexec client/bin client/lib

run-server: server-client-lib
	@DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS \
		./server/bin/weaknet-dbus-server

test-client: server-client-lib
	@if [ "$(COMMAND)" = "" ]; then \
		echo "用法: make test-client COMMAND=[all|get|health|file|ping|check|events|event-types|test-*]"; \
		echo "示例: make test-client COMMAND=get"; \
	else \
		echo "运行客户端手动验证工具: $(COMMAND)"; \
		LD_LIBRARY_PATH=./client/lib:$$LD_LIBRARY_PATH \
		DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS \
		./client/bin/test-client $(COMMAND); \
	fi

test-lib: server-client-lib
	@$(CMAKE) --build "$(CMAKE_BUILD_DIR)" --parallel --target \
		c_header_smoke cxx_header_smoke v1_client_metadata
	@$(CTEST) --test-dir "$(CMAKE_BUILD_DIR)" --output-on-failure \
		-R '^(c_header_smoke|cxx_header_smoke|v1_c_abi_contract|v1_client_metadata)$$'

test-events: server-client-lib
	@LD_LIBRARY_PATH=./client/lib:$$LD_LIBRARY_PATH \
		DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS \
		./client/bin/test-client test-events

test-all: server-client-lib
	@LD_LIBRARY_PATH=./client/lib:$$LD_LIBRARY_PATH \
		DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS \
		./client/bin/test-client all

test-ping: server-client-lib
	@LD_LIBRARY_PATH=./client/lib:$$LD_LIBRARY_PATH \
		DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS \
		./client/bin/test-client test-ping

test-performance: server-client-lib
	@LD_LIBRARY_PATH=./client/lib:$$LD_LIBRARY_PATH \
		DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS \
		./client/bin/test-client test-performance

run-client: server-client-lib
	@LD_LIBRARY_PATH=./client/lib:$$LD_LIBRARY_PATH \
		DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS \
		./client/bin/test-client subscribe
