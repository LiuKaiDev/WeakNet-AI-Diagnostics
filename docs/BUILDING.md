# Building WeakNet

WeakNet uses CMake and C++20. The deterministic daemon, CLI and tests do not
require Python or the optional AI/RAG packages.

## Requirements

Always required:

- Linux and CMake 3.20 or newer;
- a C and C++ compiler with C++20 support;
- Threads, pkg-config, dbus-1 development files and glog development files.

Ninja is recommended but not required.

With `ENABLE_EBPF=ON`, also provide Clang with BPF code generation, libbpf,
libelf, zlib, and either readable kernel BTF plus `bpftool` or a pre-generated
`vmlinux.h`. These are feature requirements, not claims about every validated
host configuration.

## Reproducible build without eBPF

```bash
cmake -S . -B build/no-ebpf -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_EBPF=OFF \
  -DBUILD_TESTING=ON
cmake --build build/no-ebpf --parallel
ctest --test-dir build/no-ebpf --output-on-failure
```

This mode avoids libbpf/libelf/zlib discovery and builds the daemon with its
explicit non-eBPF/degraded capability path.

## Build with eBPF

```bash
cmake -S . -B build/default -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_EBPF=ON \
  -DBUILD_TESTING=ON
cmake --build build/default --parallel
ctest --test-dir build/default --output-on-failure
```

The default BTF input is `/sys/kernel/btf/vmlinux`. Override it with:

```bash
cmake -S . -B build/ebpf -G Ninja \
  -DENABLE_EBPF=ON \
  -DWEAKNET_VMLINUX_BTF=/path/to/vmlinux.btf
```

or provide a header directly:

```bash
cmake -S . -B build/ebpf -G Ninja \
  -DENABLE_EBPF=ON \
  -DWEAKNET_VMLINUX_HEADER=/path/to/vmlinux.h
```

CMake generates/copies the selected header under the build tree and builds
`libexec/weaknet/flow_rate.bpf.o`. Successful compilation does not by itself
prove that a particular kernel permits every attachment; runtime capability is
reported separately.

## Build products

For a build directory named `build/no-ebpf`:

```text
build/no-ebpf/bin/weaknet-dbus-server
build/no-ebpf/bin/weaknetctl
build/no-ebpf/bin/test-client
build/no-ebpf/bin/example-client
build/no-ebpf/bin/ping-example
build/no-ebpf/lib/libweaknet.so
build/no-ebpf/libexec/weaknet/weaknet-ping-helper
```

An eBPF-enabled build additionally produces generated BTF headers and
`libexec/weaknet/flow_rate.bpf.o`.

## Tests

CTest includes deterministic coverage for serialization, runtime lifecycle,
events/metrics, RTNETLINK topology, socket lifecycle and `TCP_INFO`, route
attribution, ActiveProbe, nl80211 parsing, IncidentEngine, RootCauseEngine,
D-Bus queries, V1 contracts and client lifetime.

The namespace integration test returns CTest skip code 77 when the host lacks
the required capabilities. Process tests create a private session bus and do
not require a system-bus installation. No ordinary CTest contacts an external
AI provider.

Optional Python tests:

```bash
.venv/bin/python -m unittest discover -v -s ai/tests
.venv/bin/python -m unittest -v lab.tests.test_lab
```

Live provider and privileged lab tests are opt-in and should be run only on an
approved environment with the documented variables/capabilities.

## Sanitizers

```bash
cmake -S . -B build/asan -G Ninja \
  -DENABLE_EBPF=OFF -DBUILD_TESTING=ON \
  -DWEAKNET_SANITIZER=address-undefined
cmake --build build/asan --parallel
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/asan --output-on-failure
```

Use `-DWEAKNET_SANITIZER=thread` in a separate build for TSan.

## Installation

```bash
cmake --install build/no-ebpf --prefix /tmp/weaknet-install
```

The install includes `weaknet-dbus-server`, `weaknetctl`, the compatibility
library/tools, public C header and the private ping helper. The eBPF object is
installed only when enabled.

## Runtime paths and compatibility entry points

The daemon resolves writable directories independently of its launch
directory. `WEAKNET_STATE_DIR`, `WEAKNET_LOG_DIR` and `WEAKNET_RUNTIME_DIR`
override the defaults. The eBPF object can be overridden with
`WEAKNET_BPF_OBJECT`.

The root and `server/` Makefiles remain compatibility wrappers over CMake and
may copy compatibility artifacts into `server/bin`, `server/build`, `server/libexec`,
`client/bin` and `client/lib`. Current documentation and automation should use
the CMake build tree unless compatibility behavior is specifically being
tested.

`./install.sh --check-deps` performs a non-mutating dependency check. The
separate `--install-deps` behavior changes the host and should be used only in
an appropriate environment.
