# Building the V1 baseline with CMake

## Scope

Phase 1 provides a reproducible CMake/C++20 build around the existing V1 implementation. It does not rename the daemon, change the session-bus contract, redesign collectors, or establish that the current eBPF observations are semantically correct.

The production and example targets do not require Python or any package under `optional/experimental/`.

## Requirements

Always required:

- Linux;
- CMake 3.20 or newer;
- a C and C++ compiler with C++20 support;
- Threads;
- pkg-config;
- dbus-1 development files;
- glog development files.

Required with `ENABLE_EBPF=ON`:

- Clang with BPF code-generation support;
- libbpf, libelf, and zlib development files;
- either a readable pre-generated `vmlinux.h`, or readable kernel BTF plus bpftool.

Ninja is the recommended generator. Make remains available only as a temporary compatibility wrapper over CMake.

## Standard build

```bash
cmake -S . -B build/default -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_EBPF=ON \
  -DBUILD_TESTING=ON

cmake --build build/default --parallel
ctest --test-dir build/default --output-on-failure
```

Products are written under the build tree:

```text
build/default/bin/weaknet-dbus-server
build/default/bin/test-client
build/default/bin/example-client
build/default/bin/ping-example
build/default/lib/libweaknet.so
build/default/generated/bpf/vmlinux.h
build/default/libexec/weaknet/flow_rate.bpf.o
```

`test-client` remains a manual V1 validation tool. It is not registered as automated CTest evidence because several modes require a running session-bus service, privileges, or external hosts and do not reliably propagate failures.

## eBPF modes

### Enabled (default)

```bash
cmake -S . -B build/ebpf -G Ninja -DENABLE_EBPF=ON
```

Configuration fails rather than silently disabling eBPF when Clang/BPF support, libbpf, libelf, zlib, or the selected BTF/header input is missing.

The default generation input is:

```text
WEAKNET_VMLINUX_BTF=/sys/kernel/btf/vmlinux
```

Override it with another BTF file:

```bash
cmake -S . -B build/ebpf -G Ninja \
  -DENABLE_EBPF=ON \
  -DWEAKNET_VMLINUX_BTF=/path/to/vmlinux.btf
```

Or provide a pre-generated header and avoid BTF generation:

```bash
cmake -S . -B build/ebpf -G Ninja \
  -DENABLE_EBPF=ON \
  -DWEAKNET_VMLINUX_HEADER=/path/to/vmlinux.h
```

The selected input is copied/generated atomically as `build/ebpf/generated/bpf/vmlinux.h`. CMake never generates `server/vmlinux.h`.

The build maps the configured target processor to libbpf's `__TARGET_ARCH_*` macro and selects `bpfel` or `bpfeb` by byte order. The x86_64-to-`__TARGET_ARCH_x86` mapping is verified on the current Phase 1 host. Other declared mappings are not yet claimed as tested.

The BPF C source and hooks are otherwise unchanged. Successful compilation or skeleton parsing is not proof that the V1 kprobes attach or measure traffic correctly.

### Disabled

```bash
cmake -S . -B build/no-ebpf -G Ninja \
  -DENABLE_EBPF=OFF \
  -DBUILD_TESTING=ON
cmake --build build/no-ebpf --parallel
ctest --test-dir build/no-ebpf --output-on-failure
```

This mode does not discover, include, or directly link libbpf, libelf, or zlib. The existing V1 non-eBPF stubs are compiled, and the daemon continues in its established degraded traffic-analysis mode.

## Runtime BPF object lookup

When eBPF is enabled, the existing traffic analyzer resolves `flow_rate.bpf.o` in this order:

1. `WEAKNET_BPF_OBJECT` environment override;
2. executable-relative `../libexec/weaknet/flow_rate.bpf.o` in the build or install tree;
3. executable-relative `../build/flow_rate.bpf.o` for temporary `server/bin` Make staging;
4. the legacy V1 fallback `server/build/flow_rate.bpf.o`, relative to the process working directory.

Only this BPF-object path was hardened in Phase 1. Existing log and `.bin` persistence paths remain working-directory-relative technical debt.

## Tests

With `BUILD_TESTING=ON`, CTest builds deterministic, unprivileged tests for:

- serializer round trips and malformed input;
- the current network-quality baseline;
- C11 and C++20 public-header inclusion;
- client version/build metadata;
- the exact V1 exported C symbol set;
- the static V1 D-Bus endpoint/member/signature fixture;
- retained shell-script syntax;
- BPF object ELF and skeleton parsing when eBPF is enabled.

No Phase 1 CTest loads eBPF, changes the host network, requires a default route, contacts the Internet, or starts the daemon.

Disable tests with:

```bash
cmake -S . -B build/release -G Ninja \
  -DENABLE_EBPF=OFF \
  -DBUILD_TESTING=OFF
```

## Installation

Install into a staging prefix without modifying system directories:

```bash
cmake --install build/default --prefix /tmp/weaknet-install
```

Installed files include the unchanged daemon/client tool names, `libweaknet.so`, the public C header, supported examples, and (when enabled) `libexec/weaknet/flow_rate.bpf.o`.

## Temporary Make compatibility

The root and `server/` Makefiles configure and invoke CMake. They stage compatibility artifacts in the historical ignored paths:

```bash
make ENABLE_EBPF=ON BUILD_TESTING=ON all
make test-lib
make -C server all
```

The staged paths are:

```text
server/bin/weaknet-dbus-server
server/build/flow_rate.bpf.o
client/lib/libweaknet.so
client/bin/test-client
```

`make test-lib` runs deterministic CTest contract/header checks. Other `test-*` Make targets retain the manual V1 `test-client` behavior and are not automated test evidence.

## Dependency inspection

`install.sh` retains its host-mutating `--install-deps` behavior, but Phase 1 adds a non-mutating check:

```bash
./install.sh --check-deps
ENABLE_EBPF=OFF ./install.sh --check-deps
```

The default is `ENABLE_EBPF=ON`. No packages are installed by `--check-deps`.

## Known local toolchain risk

The Phase 1 WSL2 development host has bpftool 7.7.0 under `/usr/local/sbin`, built with bundled libbpf 1.7, while pkg-config selects development libbpf 1.3.0. BTF generation and object parsing work, and CMake reports the mismatch as a warning. This is not a hermetic reference toolchain and must not be hidden in release evidence.

## Warnings policy

Project C++ targets compile with `-Wall -Wextra -Wpedantic` on GCC and Clang. Phase 1 does not promote warnings to errors because existing V1 sources contain known non-blocking warnings. New warnings should not be introduced; the existing set is technical debt to handle in the milestone that owns the affected behavior.
