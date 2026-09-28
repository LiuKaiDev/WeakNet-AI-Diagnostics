# 系统架构

WeakNet 的核心是一个 C++20/Linux 原生的只读观测与确定性诊断 runtime。Python AI/RAG 服务位于独立进程，不参与 kernel data collection、incident state 或 root-cause authority。

## 组件关系

```text
Linux kernel/network state
  ├─ RTNETLINK topology
  ├─ NETLINK_SOCK_DIAG / TCP_INFO
  ├─ ActiveProbe
  ├─ nl80211 Wi-Fi
  └─ optional eBPF
        ↓
NetworkEvent / MetricSample
        ↓
EventBus / MetricStore
        ↓
IncidentEngine
        ↓
RootCauseEngine
        ↓
DiagnosticsQueryService
        ↓
D-Bus / weaknetctl
```

`weaknet-dbus-server` 组合并拥有 collector、store、engine 和 D-Bus adapter。`weaknetctl` 是同步只读客户端。核心进程不会启动 Python、下载模型或调用 LLM。

## 类型、身份和时间

内部 `NetworkEvent` header 包含：

- schema version、event ID 和全局 publication sequence；
- realtime timestamp 与 monotonic timestamp；
- `NetnsId`；
- 可选 `InterfaceId`、`SocketId`；
- event kind、source、validity 和 status。

关键 identity 规则：

- network namespace 是对象身份的一部分；
- interface 以 namespace + ifindex 标识，名称只是可变 metadata；
- socket 以 namespace、可选 kernel cookie 和 daemon-local generation 标识；
- 相同 local/remote tuple 不能跨生命周期复用历史。

realtime 用于人类关联，monotonic time 用于 interval、freshness 和 ordering。

## 有效性和权威性

观测值不会只用一个数字表达。`Valid`、`Unavailable`、`Stale`、`Partial` 和 `Reset` 是数据 contract 的一部分：

- 没有样本时不返回数值零；
- collector capability 缺失时不返回 healthy；
- counter reset 会清除 baseline，而不是产生负 delta；
- 非权威 topology 不会触发“没有路由/没有 uplink”的确定结论；
- partial route attribution 会保留 ambiguity 和 limitation。

## RTNETLINK topology

`NetlinkCollector` 拥有一个 nonblocking `NETLINK_ROUTE` socket，订阅 link、IPv4/IPv6 address 与 route group，并周期性执行完整 reconciliation。

每次 dump 都校验 kernel sender、request sequence、multipart completion、`NLMSG_ERROR`、`NLM_F_DUMP_INTR`、truncation 和 attribute length。候选 topology 只有在 link/address/IPv4 route/IPv6 route 全部完成后才原子提交。Dump 期间出现相关 notification 会使候选失效并触发 bounded resync，避免旧 dump 覆盖更新后的事实。

`TopologySnapshot` 保留 link、address、route、multipath nexthop、table、metric、gateway、preferred source、authority 和 generation。Uplink policy 对建模 default route 做确定性排序；on-link route 不会伪造 gateway。

当前模型不实现完整 `ip rule`、fwmark、source-policy routing、VRF 或 per-flow ECMP hashing。

## SocketTracker 与 route attribution

`SocketTracker` 使用一个 `NETLINK_SOCK_DIAG` socket 对 IPv4/IPv6 TCP socket 执行 transactional snapshot。两种 family 都完整成功后才提交 lifecycle change；partial/failed candidate 不会关闭活跃 socket 或修改 TCP baseline。

`SocketDiagParser` 读取 binary endpoint、port、TCP state、`idiag_if`、kernel cookie 和 variable-length `TCP_INFO`。每个字段仅在 payload size 覆盖其完整布局时才标记 available。

Interval metrics 只在以下条件满足时计算：

- 完全相同的 `SocketId`/generation；
- monotonic timestamp 严格递增；
- 所需 counters 均 available 且兼容；
- counter 没有回退；
- denominator 有效且非零。

`SocketRouteAttributor` 在 committed topology 上进行 binary longest-prefix matching，保留 route identity、table、metric、gateway/on-link、所有可能 interface、multipath、source-address context、`diag_ifindex` 与 selected-uplink relation。该结果是 modeled explanation，而不是 kernel 实际 forwarding 的保证。

## ActiveProbe

`ActiveProbe` 由一个 stop-aware worker 驱动，对当前 modeled gateway 和 configured numeric IPv4 remote target 顺序执行 bounded probe。Transport 使用 Linux IPv4 ICMP datagram socket，校验 source、echo identifier 和 sequence。

结果是 typed `Success`、`Timeout`、`InvalidReply`、`TransportUnavailable`、`NoTarget`、`Error` 等状态。RTT 只在 matching reply 成功时存在。Target identity 包含 namespace、address、interface 和 topology generation，旧 target 的 evidence 不能泄漏到新路径。

## nl80211 Wi-Fi collector

`WifiCollector` 通过 Generic Netlink control family 解析 `nl80211` family ID，并读取当前 selected interface 的 station information。它不调用 `iw`/`nmcli`，不启动或配置 wpa_supplicant，不扫描 AP，也不执行 reconnect/roaming。

Observation 可包含 association state、BSSID、SSID bytes、frequency、signal/average signal、TX/RX bitrate 和 kernel 暴露的可选 counters。缺失属性保持 absent；signed dBm 不会被当作 unsigned value。

## 可选 eBPF

eBPF 对象由 CMake 从 CO-RE source 构建，使用 build-tree `vmlinux.h` 和 libbpf。runtime 查找 build/install tree 中的 `flow_rate.bpf.o`，也允许显式 `WEAKNET_BPF_OBJECT` override。

eBPF 是 optional collector。BTF、permission、load、verifier、attach 或 object lookup 失败会记录 degraded capability；其他 collector 和诊断仍可运行。

## EventBus

`EventBus` 是进程内有界 multi-producer queue，由一个 owned `std::jthread` dispatcher 建立 publication order。Admission 时分配非零 sequence；callbacks 在 registry lock 之外执行。

可替换 gauge 在队列满时只对完全相同的 scope key 进行 coalesce；不可替换 state/event 在无法入队时显式计数 drop。Telemetry 暴露 capacity、depth、high-water mark、accepted、dispatched、drops、coalesces 和 callback failures。

Subscription token 使用 RAII。外部 unsubscribe 会等待已运行 callback 完成；self-unsubscribe 不会死锁。`stop()` 拒绝新 publication、drain finite queue、join dispatcher，并保持 idempotent。

## MetricStore

`MetricStore` 以 namespace、metric name/unit、source、可选 interface/socket 和 interval 为 key，保存 bounded latest/history samples。每个 sample 包含 ID、validity、value、realtime/monotonic time、provenance 和 optional status。

Store 有明确的 series、per-series、total-sample、age 和 query bounds。Eviction、rejection、stale results 和 cardinality 都可观测。读取返回 immutable copies，不把内部可变引用交给调用方。

## 生命周期与线程

`DaemonApplication` 按依赖顺序构造和启动组件，失败时执行 reverse-order rollback，停止时先终止 producers，再 drain EventBus，最后释放 D-Bus 和共享 state。

主要 execution contexts：

- application/signal thread：组件 ownership、start/stop；
- D-Bus dispatch：解析请求并 marshal snapshot，不运行长期 collector；
- RTNETLINK、SocketTracker、ActiveProbe、Wi-Fi workers；
- EventBus dispatcher；
- bounded Ping executor 与独立 helper process。

长生命周期线程均有 ownership 和 cancellation strategy；没有依赖 detached thread 保持核心对象存活。Blocking I/O、probe 和 sampling 不在共享 state lock 下执行。

## 查询边界

`DiagnosticsQueryService` 从 topology、IncidentEngine 和 RootCauseEngine 复制 snapshot 后组装响应。D-Bus query 不触发新 scan、probe、EventBus publication 或网络修改。多个组件的结果是同一查询时间点附近的 near-consistent snapshot，而不是跨所有 collector 的全局事务。

## 当前限制

- daemon 只观测其所在 network namespace；跨 namespace collection 由 Lab 通过在 namespace 中启动 daemon 实现。
- route model 不等同于完整 Linux policy routing/FIB lookup。
- ActiveProbe 当前仅支持 numeric IPv4 target。
- Wi-Fi evidence 依赖真实 nl80211 station environment。
- eBPF 能力依赖 kernel、BTF、libbpf、attachment strategy 和权限。
- session D-Bus 是当前运行路径；system-bus/systemd packaging 尚未提供。
