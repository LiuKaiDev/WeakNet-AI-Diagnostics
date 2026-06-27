# 日志检索分析工具使用指南

## 概述

本指南说明 `optional/experimental/log-analysis-tools/` 中实验性日志分析工具的使用方式。工具可以实时捕获 WeakNet Diagnostics 服务端日志，解析网络指标，并结合本地知识库或外部模型服务生成时间点分析结果。

该目录不是 WeakNet Diagnostics 的核心运行链路。主项目功能以服务端、客户端动态库、DBus 通信、网卡检测、健康检查和 Ping 功能为准。

## 系统架构

```text
弱网服务日志 -> log_capture.py -> 分析脚本 -> DashScope API 或本地规则 -> 分析报告
                    |
                    v
            network_knowledge_base.py
```

## 核心组件

1. `log_capture.py`
   实时捕获弱网服务终端输出，并提取结构化网络指标。

2. `local_vector_rag_analyzer.py`
   使用本地嵌入和 FAISS 向量库检索网络知识库，并生成分析结果。

3. `interactive_rag.py`
   提供交互式命令行界面，支持日志捕获、时间点查询和分析。

4. `network_knowledge_base.py`
   保存指标阈值、症状分析和故障排查建议。

## 安装与配置

### 环境要求

- Python 3.7+
- FAISS 等 Python 依赖
- 使用外部模型服务时需要稳定网络连接和 DashScope API Key

### 安装依赖

```bash
cd optional/experimental/log-analysis-tools
chmod +x install_rag.sh
./install_rag.sh
```

### 配置 API Key

使用外部模型服务前，在相关脚本中设置 DashScope API Key：

```python
api_key = "sk-your-api-key-here"
```

请勿将真实密钥提交到版本控制系统。

## 使用方法

### 直接运行分析脚本

```bash
python3 local_vector_rag_analyzer.py
```

### 交互式模式

```bash
python3 interactive_rag.py
```

交互式命令：

| 命令 | 功能 | 示例 |
|------|------|------|
| `1` 或 `capture [duration]` | 启动日志捕获 | `1 300` |
| `2` 或 `stop` | 停止日志捕获 | `2` |
| `3` 或 `times` | 显示可用时间点 | `3` |
| `4` 或 `analyze [time]` | 分析特定时间点 | `4 00:13:24` |
| `5` 或 `help` | 显示帮助 | `5` |
| `6` 或 `quit` | 退出程序 | `6` |

### 命令行模式

```bash
python3 interactive_rag.py analyze 00:13:24
python3 interactive_rag.py capture 300
```

## 支持的日志格式

```text
[00:13:24] RTT监控: eth0 = 15ms (质量:1, 使用:YES, 目标:223.5.5.5)
[00:13:34] TCP详细: eth0 = 0.5% (发送:137, 重传:0, 等级:good)
[00:13:24] 流量监控: eth0 = 2.5MB/s (连接:15, 包/秒:1200)
[00:13:24] RSSI监控: wlan0 = -65dBm (质量:2, 使用:NO)
[00:13:24] 接口汇总: eth0 = RTT:15ms, 质量:1, RSSI:-1000dBm, TCP丢包:0.5%, 流量:2.5MB/s
[00:13:30] 网络质量: eth0 = good (分数:85.5)
```

## 分析报告结构

分析结果通常包含以下内容：

1. 整体网络健康状况
2. 各接口具体表现
3. 发现的问题或异常
4. 可能的原因分析
5. 改进建议
6. 是否需要进一步监控

## 网络指标阈值

| 指标 | 优秀 | 良好 | 一般 | 较差 |
|------|------|------|------|------|
| RTT 延迟 | <10ms | 10-30ms | 30-50ms | >50ms |
| TCP 丢包率 | 0% | 0-0.5% | 0.5-1% | >1% |
| Wi-Fi 信号 | -30~-50dBm | -50~-60dBm | -60~-70dBm | <-70dBm |
| 质量评分 | 90+ | 80-90 | 70-80 | <70 |

## 故障排除

### API 连接问题

1. 运行 `python3 test_dashscope_api.py` 检查 API Key。
2. 确认网络连接、DNS 和防火墙设置。
3. 检查 API Key 权限和调用额度。

### 日志解析问题

1. 确认日志时间戳格式为 `HH:MM:SS`。
2. 确认指标字段与解析规则匹配。
3. 使用交互式模式的 `times` 命令查看可用时间点。

### 向量库问题

1. 检查 FAISS 依赖是否安装。
2. 检查 `network_knowledge_vectorstore_local/` 权限。
3. 必要时删除该目录后重新生成向量库。

## 安全注意事项

- 使用环境变量或本地配置管理 API Key，避免硬编码真实密钥。
- 不要提交包含真实密钥、访问令牌或私有日志的文件。
- 该目录为实验性工具，部署前应单独评估依赖、网络访问和数据合规要求。
