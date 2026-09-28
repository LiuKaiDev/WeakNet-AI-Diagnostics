# 构建与测试

WeakNet 使用 CMake 3.20+ 和 C++20。确定性 daemon、`weaknetctl` 与 C++ tests 不依赖 Python、LLM 或 RAG packages。

## 基础依赖

所有构建都需要：

- Linux；
- CMake 3.20 或更新版本；
- 支持 C++20 的 C/C++ compiler；
- Threads；
- pkg-config；
- dbus-1 development files；
- glog development files。

Ninja 是推荐 generator，但不是强制要求。

## eBPF OFF

这是最容易复现的完整核心构建：

```bash
cmake -S . -B build/no-ebpf -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_EBPF=OFF \
  -DBUILD_TESTING=ON

cmake --build build/no-ebpf --parallel
ctest --test-dir build/no-ebpf --output-on-failure
```

该模式不查找或链接 libbpf、libelf 和 zlib。Daemon 保留显式 non-eBPF/degraded capability path，其他 collectors 与 diagnosis engines 仍可运行。

## eBPF ON

额外依赖：

- Clang，且包含 BPF code-generation backend；
- libbpf development files；
- libelf development files；
- zlib development files；
- `bpftool`；
- 可读的 kernel BTF，或预生成 `vmlinux.h`。

构建命令：

```bash
cmake -S . -B build/ebpf -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_EBPF=ON \
  -DBUILD_TESTING=ON

cmake --build build/ebpf --parallel
ctest --test-dir build/ebpf --output-on-failure
```

默认 BTF input：

```text
/sys/kernel/btf/vmlinux
```

使用其他 BTF：

```bash
cmake -S . -B build/ebpf -G Ninja \
  -DENABLE_EBPF=ON \
  -DWEAKNET_VMLINUX_BTF=/path/to/vmlinux.btf
```

使用预生成 header：

```bash
cmake -S . -B build/ebpf -G Ninja \
  -DENABLE_EBPF=ON \
  -DWEAKNET_VMLINUX_HEADER=/path/to/vmlinux.h
```

CMake 在 build tree 中生成或复制 `generated/bpf/vmlinux.h`，不会修改 source tree header。编译成功只证明 object 可构建；实际 load/attach 仍取决于 kernel、BTF、capability 和安全策略。

## 主要产物

以 `build/no-ebpf` 为例：

```text
build/no-ebpf/bin/weaknet-dbus-server
build/no-ebpf/bin/weaknetctl
build/no-ebpf/bin/test-client
build/no-ebpf/bin/example-client
build/no-ebpf/bin/ping-example
build/no-ebpf/lib/libweaknet.so
build/no-ebpf/libexec/weaknet/weaknet-ping-helper
```

eBPF-enabled build 还会产生：

```text
build/ebpf/generated/bpf/vmlinux.h
build/ebpf/libexec/weaknet/flow_rate.bpf.o
```

## CMake options

| Option | 说明 |
| --- | --- |
| `ENABLE_EBPF=ON|OFF` | 是否构建 eBPF traffic observer |
| `BUILD_TESTING=ON|OFF` | 是否构建和注册 CTest |
| `WEAKNET_SANITIZER=none` | 默认，无 sanitizer |
| `WEAKNET_SANITIZER=address-undefined` | ASan + UBSan |
| `WEAKNET_SANITIZER=thread` | TSan，必须使用独立 build tree |
| `WEAKNET_VMLINUX_BTF=<path>` | 指定 BTF input |
| `WEAKNET_VMLINUX_HEADER=<path>` | 指定预生成 `vmlinux.h` |

## C++ tests

```bash
ctest --test-dir build/no-ebpf --output-on-failure
```

测试覆盖：

- serialization 与 public C/C++ header；
- runtime/application/process/client lifecycle；
- `NetworkEvent`、EventBus、MetricStore；
- RTNETLINK parser、topology policy 与 namespace integration；
- socket lifecycle、SOCK_DIAG parser、`TCP_INFO` metrics；
- socket-route attribution；
- ActiveProbe 与 ping helper；
- nl80211 parser/collector；
- IncidentEngine、RootCauseEngine、DiagnosticsQueryService；
- D-Bus contract、`weaknetctl` 和 C ABI contract。

Namespace integration 使用 CTest skip code 77。当 runner 不允许 `unshare(CLONE_NEWNET)` 时，该 test 显式 skip，不会修改 host namespace。

## Sanitizer build

```bash
cmake -S . -B build/asan -G Ninja \
  -DENABLE_EBPF=OFF \
  -DBUILD_TESTING=ON \
  -DWEAKNET_SANITIZER=address-undefined

cmake --build build/asan --parallel
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/asan --output-on-failure
```

TSan 使用：

```bash
-DWEAKNET_SANITIZER=thread
```

不要在同一 build tree 中组合 TSan 与 ASan/UBSan。某些 virtualized/WSL environments 的 TSan runtime 可能在 test code 运行前失败；这种情况应记录为环境限制，不能声明 pass。

## Python AI runtime

创建 repository-local environment：

```bash
python3 -m venv .venv
.venv/bin/pip install -r ai/requirements-runtime.txt
```

`ai/requirements-runtime.txt` 仅包含 current-D-Bus source 所需的 optional `dbus-next`。Caller-supplied snapshot、fake provider 和大部分 offline tests 使用 Python standard library 即可。

运行 Python tests：

```bash
.venv/bin/python -m compileall -q ai lab
.venv/bin/python -m unittest discover -v -s ai/tests
```

普通 tests 不进行外部 network/provider calls。

## 可选 dense RAG dependencies

只有需要 BGE/FAISS/reranker 时才安装：

```bash
.venv/bin/pip install -r ai/requirements-rag.txt
```

该 requirements file 包含 CPU-oriented PyTorch、NumPy、FAISS 和 sentence-transformers。安装体积较大，且真实 model execution 需要可用的本地/远程 model weights。普通 AI runtime import 不会自动下载模型。

## Lab tests

```bash
.venv/bin/python -m unittest -v lab.tests.test_lab
./lab/weaknet-lab list
WEAKNET_LAB_BUILD_DIR=build/no-ebpf ./lab/weaknet-lab doctor
```

Privileged integration 只有在显式 opt-in 且 host capability 满足时运行。`doctor` 会报告 namespace、privilege、D-Bus、binary、venv 和 `dbus-next` readiness，不打印 credential。

## 安装

```bash
cmake --install build/no-ebpf --prefix /tmp/weaknet-install
```

Installed files 包括 daemon、`weaknetctl`、C library/header、examples 和 private ping helper。eBPF object 仅在 `ENABLE_EBPF=ON` 时安装。

## Runtime paths

Daemon 不依赖 launch working directory。以下 variables 可覆盖 writable paths：

```text
WEAKNET_STATE_DIR
WEAKNET_LOG_DIR
WEAKNET_RUNTIME_DIR
```

eBPF object lookup 可用：

```text
WEAKNET_BPF_OBJECT
```

AI runtime variables：

```text
WEAKNET_LLM_PROVIDER
WEAKNET_LLM_MODEL
DASHSCOPE_API_KEY
WEAKNET_AI_HOST
WEAKNET_AI_PORT
```

所有 credential 只通过 environment/secret management 提供，不应写入 tracked files。

## Dependency check 与 Make wrapper

非修改性 dependency check：

```bash
./install.sh --check-deps
ENABLE_EBPF=OFF ./install.sh --check-deps
```

`./install.sh --install-deps` 会修改 host package state，应只在合适环境使用。

Root/server Makefiles 是 CMake 的便捷 wrapper，可复制 artifacts 到 manual-tool paths；当前文档和自动化以 CMake build tree 为准。

## 常见环境限制

- session D-Bus 需要相关命令运行在同一个 `DBUS_SESSION_BUS_ADDRESS` 下；
- eBPF 需要 kernel/BTF/toolchain/capability 共同满足；
- namespace tests 需要允许 `unshare`；
- physical Wi-Fi tests 需要 nl80211 station device；
- dense RAG 需要 compatible Python packages 和 model weights；
- live Qwen tests 需要显式 opt-in 和 provider credential，普通 regression 不会调用它们。
