# WeakNet AI Diagnostics 项目说明

本文档是早期项目说明的归档版本，主项目介绍以根目录 `README.md` 为准。

WeakNet AI Diagnostics 是一个面向 Linux 环境的网络诊断与弱网监控系统，基于 eBPF 和 DBus 提供网络接口状态监控、流量分析、网络质量评估、事件通知和客户端调用接口。仓库标识为 `WeakNet-AI-Diagnostics`。

## 快速开始

### 自动安装

```bash
# 安装依赖并编译
./install.sh

# 仅安装系统依赖
./install.sh --install-deps
```

### 手动安装

```bash
# 1. 安装依赖 (Alibaba Cloud Linux / RHEL)
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y gcc gcc-c++ make cmake clang llvm pkgconf pkgconf-pkg-config pkgconfig dbus dbus-devel dbus-x11 glog glog-devel elfutils-libelf elfutils-libelf-devel zlib zlib-devel libcap libcap-devel kernel-headers kernel-devel libbpf libbpf-devel bpftool

# 2. 编译项目
make all

# 3. 启动服务器
./server/bin/weaknet-dbus-server

# 4. 运行测试
LD_LIBRARY_PATH=./client/lib:$LD_LIBRARY_PATH ./client/bin/test-client get
```

## 项目结构

```text
.
├── server/                 # DBus 服务端和网络监控核心逻辑
├── client/                 # 客户端动态库、调用接口和测试程序
├── docs/                   # 项目文档
├── logs/                   # 运行日志目录
├── optional/experimental/  # 实验性工具，不属于核心运行链路
├── config.mk               # 项目配置
├── Makefile                # 主 Makefile
└── install.sh              # 安装脚本
```

## 功能特性

### 服务端功能

- 网络接口监控
- RTT 延迟监控
- RSSI 信号读取
- TCP 丢包率监控
- 基于 eBPF 的基础流量分析
- 网络质量评估
- 基于 DBus 的事件通知机制

### 客户端功能

- C/C++ API
- 动态库
- `test-client` 命令行测试工具
- 事件订阅
- 健康检查
- Ping 测试

## 使用方法

### 启动服务端

```bash
./server/bin/weaknet-dbus-server
```

### 客户端测试

```bash
# 获取网络接口信息
LD_LIBRARY_PATH=./client/lib:$LD_LIBRARY_PATH ./client/bin/test-client get

# 网络健康检查
LD_LIBRARY_PATH=./client/lib:$LD_LIBRARY_PATH ./client/bin/test-client health

# Ping 测试
LD_LIBRARY_PATH=./client/lib:$LD_LIBRARY_PATH ./client/bin/test-client ping 223.5.5.5
```

### C/C++ 编程接口

```cpp
#include "client/weaknet_client.h"

if (!weaknet_init()) {
    std::cerr << "初始化失败" << std::endl;
    return -1;
}

char buffer[1024], error_buffer[256];
if (weaknet_get_interfaces(buffer, sizeof(buffer), error_buffer, sizeof(error_buffer))) {
    std::cout << "网络接口: " << buffer << std::endl;
}

weaknet_cleanup();
```

## 编译选项

```bash
# 编译所有组件
make all

# 清理编译产物
make clean

# 运行测试
make test-all
make test-events
make test-performance
```

## 监控指标

### 网络接口指标

- 接口名称和状态
- IP 地址和子网掩码
- 网络标志位
- 当前使用状态

### 网络质量指标

- RTT
- TCP 丢包率
- RSSI
- 流量统计
- 综合质量评分

### 事件类型

- `InterfaceChanged`: 网络接口变化
- `ConnectionModeChanged`: 上网方式变化
- `NetworkQualityChanged`: 网络质量变化

## 实验性日志分析工具

仓库中保留了一组实验性 Python 日志分析工具，位于 `optional/experimental/log-analysis-tools/` 目录。该目录用于离线解析服务端日志和尝试检索式分析流程，不属于 WeakNet AI Diagnostics 的核心运行链路。

```bash
cd optional/experimental/log-analysis-tools
```

使用这些工具前需要单独安装 Python 依赖并配置外部模型服务密钥。核心服务端、客户端、DBus 通信、网卡检测、健康检查和 Ping 功能不依赖该目录。

## 文档

- [客户端 API 文档](../client/README_CLIENT.md)
- [动态库使用指南](../client/README_LIBRARY.md)
- [事件系统文档](events.md)
- [Ping 功能文档](ping-feature.md)
- [实验性日志分析工具](../optional/experimental/log-analysis-tools/README.md)

## 相关链接

- [eBPF 官方文档](https://ebpf.io/)
- [DBus 官方文档](https://dbus.freedesktop.org/)
- [libbpf 项目](https://github.com/libbpf/libbpf)
