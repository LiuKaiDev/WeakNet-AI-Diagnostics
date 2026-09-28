# Client 与 C Library

WeakNet 的主要用户接口是 `weaknetctl`。仓库同时构建 `libweaknet.so` 与 `weaknet_client.h`，供需要 C ABI 的现有 application 集成。

## weaknetctl

```bash
./build/no-ebpf/bin/weaknetctl status
./build/no-ebpf/bin/weaknetctl incidents
./build/no-ebpf/bin/weaknetctl hypotheses
./build/no-ebpf/bin/weaknetctl diagnose
./build/no-ebpf/bin/weaknetctl diagnose --explain
./build/no-ebpf/bin/weaknetctl diagnose --advise
```

完整 D-Bus、exit code、timeout 和 AI behavior 见 [`../docs/API_AND_CLI.md`](../docs/API_AND_CLI.md)。

## 构建产物

```text
build/<name>/bin/weaknetctl
build/<name>/bin/test-client
build/<name>/bin/example-client
build/<name>/bin/ping-example
build/<name>/lib/libweaknet.so
client/weaknet_client.h
```

`test-client` 是手动 C-library/API validation tool，不是当前诊断 CLI。

## C API

Header：

```c
#include "weaknet_client.h"
```

主要 functions：

```c
bool weaknet_init(void);
void weaknet_cleanup(void);
bool weaknet_is_connected(void);

bool weaknet_get_interfaces(char*, size_t, char*, size_t);
bool weaknet_health_check(char*, size_t, char*, size_t);
bool weaknet_get_from_file(char*, size_t, char*, size_t);
bool weaknet_ping_host(const char*, char*, size_t, char*, size_t);

bool weaknet_check_changes(char*, size_t, int32_t*, char*, size_t);
bool weaknet_get_event_types(char*, size_t, char*, size_t);
bool weaknet_check_events(char*, size_t, char*, size_t,
                          int32_t*, char*, size_t, char*, size_t);

bool weaknet_get_version(char*, size_t);
bool weaknet_get_build_info(char*, size_t);
```

Callback registration functions 也在 header 中声明。以 `client/weaknet_client.h` 为最终 signature source，不要根据文档重新声明类型。

## 最小示例

```c
#include "weaknet_client.h"
#include <stdio.h>

int main(void) {
    char result[4096];
    char error[256];

    if (!weaknet_init()) {
        return 1;
    }

    if (weaknet_get_interfaces(result, sizeof(result),
                               error, sizeof(error))) {
        printf("%s\n", result);
    } else {
        fprintf(stderr, "%s\n", error);
    }

    weaknet_cleanup();
    return 0;
}
```

从 build tree 编译：

```bash
cc -I./client example.c \
  -L./build/no-ebpf/lib -lweaknet \
  -Wl,-rpath,"$PWD/build/no-ebpf/lib" \
  -o example
```

运行前必须存在可访问的 session D-Bus 与正在运行的 `weaknet-dbus-server`。

## 使用约束

- 先调用 `weaknet_init()`，结束前调用 `weaknet_cleanup()`；
- function 返回 `false` 时读取对应 error buffer；
- caller 提供的 buffer size 必须与实际 allocation 一致；
- `weaknet_get_from_file` 和部分 event/quality functions 属于 C ABI surface，不代表 `weaknetctl` 的 deterministic diagnosis model；
- TCP retransmission-derived fields 不应被解释为权威 packet-loss rate。

Public header、exported symbol set、version/build metadata 和 client lifecycle 都由 CTest contract tests 覆盖。
