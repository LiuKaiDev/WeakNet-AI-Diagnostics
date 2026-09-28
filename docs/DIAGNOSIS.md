# 诊断模型

WeakNet 将“观测到的异常”和“可能的原因”分成两个确定性层：

```text
typed observation -> IncidentEngine -> incident
incident + topology/probe/Wi-Fi context -> RootCauseEngine -> hypothesis
```

Incident 不是 root cause。Hypothesis 也不是未经条件限制的因果证明。

## IncidentEngine

`IncidentEngine` 消费 committed observations，在 bounded state machine 中管理 `Pending`、`Active` 和 `Resolved` lifecycle。每个 incident 具有 daemon-local ID、typed scope、severity、timestamps、validity、evidence completeness 和 structured evidence。

当前 incidents：

| 类型 | Scope | 激活条件摘要 |
| --- | --- | --- |
| `HighTcpRtt` | `SocketId` | TCP-estimated RTT 连续超过阈值 |
| `ElevatedTcpRetransmission` | `SocketId` | 有效发送 interval 的 retransmission-segment ratio 连续升高 |
| `RouteUnavailable` | `SocketId` | 权威 route context 明确为 `NoModeledRoute` |
| `SocketRouteConflict` | `SocketId` | `diag_ifindex` 与 modeled route interface 明确冲突 |
| `UplinkUnavailable` | `NetnsId` | 权威 topology 明确没有可用 selected uplink |

默认 RTT policy 在 `200000 us` 连续两个 valid samples 后激活，在 `150000 us` 或以下连续两个 samples 后恢复。Retransmission policy 默认在 ratio `0.10`、至少十个 data segments、连续两个 valid intervals 后激活，在 `0.05` 或以下恢复。阈值是可审查的 engineering defaults，不是所有网络的普适标准。

Hysteresis band 内的值不会同时推进激活和恢复。Unavailable、stale、partial、counter reset、zero denominator 或 identity change 不会被当作正常样本。

socket generation 是 scope 的一部分；tuple 被复用后会产生新的 incident identity。Resolved condition 再次出现时获得新的 incident ID。

## RootCauseEngine

`RootCauseEngine` 消费 active incident transition，并按 namespace/socket scope 组合 authoritative topology、最新 gateway/remote probe 和最新 current-interface Wi-Fi observation。

当前 hypotheses：

| 类型 | 主要证据边界 |
| --- | --- |
| `UplinkAvailabilityProblem` | 权威 `UplinkUnavailable` |
| `LocalRoutingProblem` | `RouteUnavailable` 或 `SocketRouteConflict` |
| `NetworkPathDegradation` | socket-specific RTT/retransmission incident |
| `RemoteOrUpstreamDegradation` | local gateway 正常且更远 target 出现退化证据 |
| `LocalLinkSuspected` | path incident 与当前 Wi-Fi association/signal + gateway context 相关 |
| `InsufficientEvidence` | incident 存在，但 probe/topology context 缺失或过期 |

Confidence 为 `Low`、`Medium`、`High` enum，不是概率。它由证据 completeness 和独立证据组合决定。

## Evidence roles

每个 hypothesis 独立保存：

- `Supporting`：与 hypothesis 一致的事实；
- `Contradicting`：与 hypothesis 冲突或削弱它的事实；
- `Missing`：必要 capability/context 不可用、过期或不适用。

Missing evidence 既不是 support，也不是 contradiction。Collector unavailable 不能自动支持任何故障，也不能证明健康。

Evidence 包含 kind、role、source、provenance、scope、timestamp、optional value/unit、capability 和 validity。Correlation 使用 binary address、ifindex、namespace、socket generation 等 typed identity，不依赖 printable name。

## Path degradation rules

- 单个 `HighTcpRtt` 或 `ElevatedTcpRetransmission` 产生 Low `NetworkPathDegradation`。
- 两种独立 TCP incidents 同时存在时可提升为 Medium。
- fresh gateway high RTT 可把 Low path hypothesis 提升为 Medium；一次 gateway timeout 只增加 weak support。
- gateway 正常且 remote high RTT/timeout 时，可产生 Medium `RemoteOrUpstreamDegradation`。
- fresh remote normal success 会反驳 broad upstream interpretation，但不会消除 socket-specific path hypothesis，因为 probe target 不是 socket peer。
- remote timeout 不会单独支持 High confidence。
- 缺失或 stale probe context 会生成 `InsufficientEvidence`，而不是假设 timeout 或 success。

“beyond local gateway” 只表示当前证据模式相对更符合 immediate gateway 之外的退化，不等同于确认 ISP congestion 或 remote server failure。

## ActiveProbe semantics

Gateway target 只来自 authoritative modeled topology。没有唯一 gateway、multipath ambiguity、on-link default、topology unavailable 或没有 selected uplink 时，结果为 `NoTarget`，不会发明地址。

Probe freshness 默认使用 15 秒 monotonic window。Gateway evidence 必须匹配当前 binary gateway、family 和 selected interface。Gateway change 会立即使旧 sample 不适用；remote target replacement 也不会保留旧 target 结果。

状态解释：

- `Success`：只证明该 target 在该 sample time 可达；
- `Timeout`：weak degradation evidence，不是 loss rate 或 host-unreachable proof；
- `TransportUnavailable` / `NoTarget`：missing capability/context；
- `InvalidReply` / `Error`：低质量 unavailable evidence；
- `Unreachable`：当前 transport 尚未验证 ICMP unreachable，因此不作为 root-cause failure evidence。

Gateway success 不证明所有 local component 或 Internet 健康。Configured remote 也不代表每个 socket endpoint。

## Wi-Fi semantics

`RootCauseEngine` 每个 `NetnsId + ifindex` 只保留最新 Wi-Fi observation。Observation 必须：

- 在默认 10 秒 monotonic freshness window 内；
- 匹配当前 route/uplink interface；
- generation 一致（双方均提供时）；
- capability/status 对当前 station 有意义。

`Available + Associated` 可提供 association、signal 和 optional bitrate context。`NotAssociated` 只有在当前 authoritative Wi-Fi uplink 上才有诊断意义。`NotWifi`、`Unsupported`、`PermissionDenied`、`TransportUnavailable`、`Error` 和 `NoTarget` 都是 missing/not-applicable evidence。

默认 signal categories：

- weak：`<= -70 dBm`，恢复阈值 `>= -67 dBm`；
- very weak：`<= -80 dBm`，恢复阈值 `>= -77 dBm`。

RSSI alone 不会创建 hypothesis。`LocalLinkSuspected` 要求 active path degradation，再结合 current-interface `NotAssociated`，或 weak signal 与 degraded gateway evidence。该 hypothesis confidence 上限为 Medium。

Cumulative retries/failures 不会在此处直接转换成 interval retry ratio 或 packet loss。引擎不声称 AP health、RF interference、roaming history 或 endpoint-specific loss。

## Recovery 与 recomputation

Incident condition 连续进入 recovery threshold 后转为 `Resolved`。只有 active incidents 参与 active root-cause hypotheses；incident resolution 会使对应 hypotheses resolve。

新的 probe、Wi-Fi 或 topology evidence 会对相关 namespace/socket 执行 bounded recomputation。只改变 timestamp/sequence 而没有改变 confidence、role、kind、value、capability 或 meaning 时，不重复发布等价 hypothesis event。

## 不提供的诊断

当前 deterministic engine 不提供 DNS、HTTP/TLS、traceroute、jitter、packet-loss window、probabilistic scoring 或自动 remediation。缺少这些能力会作为 limitation/missing evidence 表达，而不是用 unsupported causal claim 填补。
