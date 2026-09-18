# Temporary server-only V1 compatibility wrapper around the Phase 1 CMake build.

CMAKE ?= cmake
CMAKE_GENERATOR ?= Ninja
CMAKE_BUILD_DIR ?= ../build/server-make-compat
CMAKE_BUILD_TYPE ?= RelWithDebInfo
ENABLE_EBPF ?= ON
BUILD_TESTING ?= OFF

CMAKE_CONFIGURE = $(CMAKE) -S .. -B "$(CMAKE_BUILD_DIR)" \
	-G "$(CMAKE_GENERATOR)" \
	-DCMAKE_BUILD_TYPE="$(CMAKE_BUILD_TYPE)" \
	-DENABLE_EBPF="$(ENABLE_EBPF)" \
	-DBUILD_TESTING="$(BUILD_TESTING)"

.PHONY: all configure clean run

all: configure
	@$(CMAKE) --build "$(CMAKE_BUILD_DIR)" \
		--target weaknet_server_legacy_stage --parallel

configure:
	@$(CMAKE_CONFIGURE)

clean:
	@$(CMAKE) -E rm -rf "$(CMAKE_BUILD_DIR)" bin build libexec

run: all
	@DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS \
		./bin/weaknet-dbus-server
