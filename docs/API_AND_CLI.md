# 接口与命令行

WeakNet 通过 session D-Bus 暴露只读诊断状态，并由 C++ `weaknetctl` 提供同步命令行查询。API 不提供配置修改、incident acknowledgement、remediation 或 daemon control。

## D-Bus endpoint

```text
service:   com.example.WeakNet
object:    /com/example/WeakNet/V2
interface: com.example.WeakNet.Diagnostics2
```

`/V2` 与 `Diagnostics2` 是当前稳定实现使用的 literal identifier，不表示文档中的迁移流程。

## Methods

所有 methods 都没有输入参数：

| Method | Reply signature | 内容 |
| --- | --- | --- |
| `GetStatus` | `a{sv}` | 总体状态、topology authority、collector degradation、uplink 和 active counts |
| `ListActiveIncidents` | `aa{sv}` | active incident records |
| `ListRootCauseHypotheses` | `aa{sv}` | active root-cause hypotheses 与 evidence summaries |
| `GetDiagnosis` | `a{sv}` | status、incidents、hypotheses 的组合 snapshot |
| `GetTopologySummary` | `a{sv}` | 当前 topology 与 selected uplink summary |

Dictionary value 使用 D-Bus variant。Internal C++ variants 和 kernel structs 不属于 wire contract。

## 状态语义

`GetStatus` 的 `state`：

- `Healthy`：diagnosis engines 正常运行、authoritative topology 可用、没有 active incident/hypothesis，也没有已知 runtime degradation；
- `Degraded`：authoritative state 可用，但存在 active diagnosis 或 collector/runtime degradation；
- `Unknown`：必要 engine 未运行或 authoritative topology 不可用。

Unavailable telemetry 不会被转换为 `Healthy`。

常见 status fields 包括：

- `topology_authoritative`
- `topology_degraded`
- `socket_tracker_degraded`
- `uplink` / selected ifindex（可用时）
- `active_incidents`
- `active_hypotheses`
- realtime query timestamp

## Incident records

Incident record 包含：

```text
id
type
state
scope
opened_at_ms
last_updated_at_ms
```

Scope 保留 namespace 与 socket generation identity。Interface name 只是 metadata，不是对象 identity。

## Root-cause records

Hypothesis record 包含：

```text
occurrence
type
state
confidence
scope
reason
supporting_evidence
contradicting_evidence
missing_evidence
```

Evidence kind 使用稳定 descriptive name，例如 `GatewayProbeReachable`、`RemoteProbeTimeout`、`ProbeEvidenceStale`、`WifiAssociated`、`WifiSignalWeak` 和 `WifiNotAssociated`。Missing evidence 不会混入 contradiction。

Timestamp 使用 realtime Unix milliseconds。查询从各组件复制 snapshot 后组装响应，不触发 collector scan、probe 或 EventBus publication。

## weaknetctl

```text
weaknetctl status
weaknetctl incidents
weaknetctl hypotheses
weaknetctl diagnose
weaknetctl diagnose --explain
weaknetctl diagnose --advise
```

### `status`

打印总体状态、topology authority/degradation、socket tracker degradation、selected uplink、active incidents 和 hypothesis count。

退出码：

| 条件 | Exit code |
| --- | ---: |
| `Healthy` | 0 |
| `Degraded` | 1 |
| `Unknown` | 2 |
| D-Bus / usage / client error | 3 或更高 |

### `incidents`

列出 active incidents。列表为空或非空都表示查询成功，因此退出 0；incident 本身不是 CLI error。

### `hypotheses`

列出 active hypotheses，包括 confidence、scope 以及 supporting/contradicting/missing summaries。列表是否为空不改变查询成功状态。

### `diagnose`

按顺序打印 deterministic status、active incidents 和 active hypotheses。它不生成 LLM prose，也不接触 AI runtime。

### `diagnose --explain`

先打印与 `diagnose` 相同的确定性内容，再向 loopback AI runtime 的 explanation endpoint 发出一次有界请求并显示 `ExplanationReport`。

AI timeout、provider error、invalid schema 或 grounding violation 会单独显示；确定性输出和 Healthy/Degraded/Unknown exit code 保持不变。

### `diagnose --advise`

先打印确定性内容，再调用 grounded RAG advice endpoint，显示：

- retrieval mode（`lexical` 或 `hybrid`）；
- cited knowledge explanation；
- read-only recommended checks；
- limitations 与 exact citation IDs。

没有 active root-cause hypothesis 时返回 `not_applicable`，不会进行 retrieval 或 provider call。Advice failure 不会成为 deterministic diagnosis failure。

## Timeout 和错误边界

- D-Bus method call timeout：3 秒；
- AI request 默认总 timeout：38 秒；
- `WEAKNET_AI_TIMEOUT_SECONDS` 可配置 AI timeout，上限为 40 秒；
- runtime/provider/retrieval/grounding errors 与 network state 分离。

`--explain` 和 `--advise` 是不同模式；advice 不会隐式替代 explanation。

## 启动示例

```bash
dbus-run-session -- bash
./build/no-ebpf/bin/weaknet-dbus-server
```

在继承同一 `DBUS_SESSION_BUS_ADDRESS` 的终端中：

```bash
./build/no-ebpf/bin/weaknetctl status
./build/no-ebpf/bin/weaknetctl diagnose
```

## C library

仓库还构建 `libweaknet.so` 和 `client/weaknet_client.h`，用于需要 C ABI 的现有集成。该 library 提供初始化、interface/health/Ping 查询、event polling、quality callback 和 build metadata。它不是 `weaknetctl` 的实现依赖，详细使用方式见 [`../client/README.md`](../client/README.md)。
