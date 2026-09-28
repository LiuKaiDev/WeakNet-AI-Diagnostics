# WeakNet Lab

WeakNet Lab 是可复现的网络场景与诊断评估 harness。它在隔离的 Linux network namespaces 中创建 veth topology，启动真实 `weaknet-dbus-server`，生成本地 TCP traffic，仅对 Lab 自有 interface 注入有界故障，然后通过 D-Bus 读取结构化诊断并写出 machine-readable evaluation artifact。

Lab 不注入 incident 或 root-cause hypothesis；所有诊断结果都来自 production C++ engines。

## 安全边界

- Namespace 名称固定使用 `wnlab-c-<token>` 和 `wnlab-s-<token>` ownership pattern。
- Veth 名称使用 `wnc-<token>` 和 `wns-<token>`。
- Fault 只应用于上述 namespaces 内的 Lab-owned veth。
- 不修改 host default route 或 host qdisc。
- 所有 commands 使用 bounded timeout，并记录 owned process PID/start-time pair。
- Cleanup 在删除前校验 resource name，拒绝操作不符合 ownership pattern 的对象。
- Cleanup privilege 不足且 resource 仍存在时，state 会保留，供之后进行 privileged retry。
- Generated state 与 artifacts 位于 `lab/.state/` 和 `lab/output/`，均不应提交。

不要手动把 state entry 指向非 Lab resource。

## 前置条件

先构建 C++ targets，并准备 repository `.venv`：

```bash
cmake -S . -B build/no-ebpf -G Ninja \
  -DENABLE_EBPF=OFF -DBUILD_TESTING=ON
cmake --build build/no-ebpf --parallel

python3 -m venv .venv
.venv/bin/pip install -r ai/requirements-runtime.txt
```

指定 build tree 并检查环境：

```bash
export WEAKNET_LAB_BUILD_DIR=build/no-ebpf
./lab/weaknet-lab doctor
```

Live scenarios 需要 Linux、`ip`、`tc`、`dbus-daemon`、built daemon/CLI、`.venv` 中的 `dbus-next`，以及以下之一：

- root；
- effective `CAP_NET_ADMIN` + `CAP_SYS_ADMIN`；
- passwordless `sudo -n`。

缺少 capability 时，`run` 会写出包含缺失原因的 `SKIP` evaluation，并且不执行 network mutation。`doctor` 只报告 boolean/configuration state，不打印 credential。

## Commands

```bash
./lab/weaknet-lab list
./lab/weaknet-lab doctor
./lab/weaknet-lab run healthy --no-ai
./lab/weaknet-lab run high-rtt --no-ai
./lab/weaknet-lab up uplink-unavailable
./lab/weaknet-lab status
./lab/weaknet-lab down
./lab/cleanup.sh
```

`run` 在 pass、failure 或 timeout 后都会尝试 scoped cleanup。`up` 会保留一个 active scenario 供检查，完成后使用 `down`。同一时刻只允许一个 active Lab，避免 ownership 冲突。

## Scenarios

| ID | 真实 setup/fault | 预期 deterministic behavior |
| --- | --- | --- |
| `healthy` | 本地 TCP echo，无 fault | 无 incident/hypothesis |
| `high-rtt` | server-egress 添加 300 ms netem delay | `HighTcpRtt`、`NetworkPathDegradation`，随后 recovery |
| `retransmission` | server-egress 添加 25% netem loss | `ElevatedTcpRetransmission`、`NetworkPathDegradation` |
| `uplink-unavailable` | 仅移除 client namespace default route | `UplinkUnavailable`、`UplinkAvailabilityProblem`，随后 recovery |
| `observability-gap` | eBPF-off，physical Wi-Fi telemetry unavailable | `Degraded` 或 `Unknown`，不制造 incident/hypothesis |

Scenario manifests 位于 `lab/scenarios/`，定义 required capabilities、workload、fault parameters、time windows、expected incidents/hypotheses、status constraints 和 recovery expectations。

## Evaluation artifacts

每次 `run` 创建：

```text
lab/output/<run-id>/evaluation.json
```

Schema identifier 为 `weaknet.lab.evaluation.v1`。Artifact 记录：

- setup `pass`/`skip` 与最终 `PASS`/`FAIL`/`SKIP`；
- expected、observed、hit、missing incidents/hypotheses；
- unexpected authoritative escalation；
- detection/recovery latency；
- 显式请求时的 optional AI schema/grounding result。

Schema reference 位于 `lab/expected/evaluation.schema.json`。CLI capture、daemon log 和 optional structured AI report 会保存在同一 run directory。结果只代表该次运行，不外推为通用 benchmark 或准确率。

## 可选 AI checks

AI 默认关闭：

```bash
./lab/weaknet-lab run high-rtt --explain
./lab/weaknet-lab run high-rtt --advise
```

这些模式要求显式配置 provider environment。Lab 不读取 `.env.local`，不打印 secret，普通测试也不调用外部 provider。它先保存 deterministic `weaknetctl diagnose` output，再检查 AI report schema、unchanged deterministic type/confidence/status、grounding、citation 和 CLI exit behavior。

离线演示：

```bash
./lab/weaknet-lab fixture-demo --mode advise
```

`fixture-demo` 始终标记 `SIMULATION`，使用 canonical fixture 和 fake provider，不代表 Linux telemetry 或 privileged live validation。

## Tests

Offline tests 不修改 network，也不访问外部 provider：

```bash
.venv/bin/python -m unittest -v lab.tests.test_lab
```

Privileged integration 为显式 opt-in：

```bash
WEAKNET_RUN_LAB_INTEGRATION=1 \
  .venv/bin/python -m unittest -v \
  lab.tests.test_lab.PrivilegedIntegrationTests
```

仅在 disposable/approved Linux host 上运行。若 `doctor` 报告 namespace 或 privilege 不可用，`SKIP` 是正确结果，不应被描述为 live pass。
