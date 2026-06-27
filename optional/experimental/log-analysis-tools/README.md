# Experimental Log Analysis Tools

本目录保存实验性日志分析工具，用于解析 WeakNet AI Diagnostics 服务端输出、按时间点整理网络指标，并尝试通过本地知识库和外部模型服务生成分析报告。

这些工具不属于主项目核心运行链路。WeakNet AI Diagnostics 的服务端、客户端动态库、DBus 通信、网卡检测、健康检查和 Ping 功能均不依赖本目录。

## 主要功能

- 实时日志捕获：从服务端终端输出中提取网络指标。
- 多指标解析：支持 RTT、TCP 丢包率、流量、RSSI、网络质量等字段。
- 时间点查询：按时间点整理日志中的网络状态。
- 向量检索实验：使用 FAISS 保存和检索网络知识库。
- 外部模型服务集成：部分脚本可调用 DashScope 的 OpenAI 兼容接口；不可用时回退到本地规则分析。

## 文件结构

- `log_capture.py`：弱网服务日志截取工具。
- `local_vector_rag_analyzer.py`：本地向量检索分析脚本。
- `interactive_rag.py`：交互式日志分析入口。
- `network_knowledge_base.py`：网络分析知识库。
- `vector_rag_analyzer.py`：基于外部嵌入接口的向量检索分析脚本。
- `simple_rag_analyzer.py`：基于文本匹配的简化分析脚本。
- `optimized_network_rag.py`：早期优化版分析脚本，保留用于对比。
- `test_dashscope_api.py`：DashScope API 连接测试。
- `install_rag.sh`：实验工具依赖安装脚本。
- `requirements.txt`：Python 依赖列表。
- `RAG_SERVICE_GUIDE.md`：详细使用说明。

## 使用前提

使用外部模型服务前，需要将脚本中的 `YOUR_DASHSCOPE_API_KEY_HERE` 替换为真实 API Key。请勿将真实密钥提交到版本控制系统。

```bash
cd optional/experimental/log-analysis-tools
chmod +x install_rag.sh
./install_rag.sh
python3 test_dashscope_api.py
```

## 运行示例

```bash
# 本地向量检索分析
python3 local_vector_rag_analyzer.py

# 交互式模式
python3 interactive_rag.py

# 日志捕获
python3 log_capture.py
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

## 注意事项

- 该目录属于实验性工具，不代表主项目功能全部经过同等部署验证。
- 外部模型服务需要网络连接、API Key 和相应调用额度。
- FAISS 向量库文件位于 `network_knowledge_vectorstore_local/`，可按需删除后重新生成。
- 日志解析依赖服务端输出格式，服务端日志格式变化时需要同步调整解析规则。
