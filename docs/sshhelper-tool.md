# sshhelper-tool：Codex 即时监控接口

`sshhelper-tool.exe` 是原生 Windows 本地 MCP 服务，复用 sshhelper 的 Linux SSH 探针、
Windows 本机采样及 DPAPI 配置读取代码。Codex 通过 stdin/stdout 的逐行 JSON-RPC 调用它；
不监听网络端口，无需 Node.js、Python 或 OpenAI API Key。悬浮窗无需运行。

## 安装

[下载 Windows Release ZIP](https://github.com/itsuki689/sshhelper/releases/latest) 并完整解压，运行 `powershell -File scripts/verify-dist.ps1` 核验；来源、EXE SHA256 和实际签名状态见 `dist/build-info.json`。源码仓库不包含 EXE，自行构建的结果在 `build` 中。
先在 sshhelper 保存 SSH 配置，通过可信渠道核对首次确认窗口中的主机、端口和 SHA256 指纹，明确接受并成功保存后再连接。

在项目根目录运行：

```powershell
powershell -File scripts/register-codex-tool.ps1
```

该脚本使用 Codex 官方 `codex mcp add` 命令，将 `sshhelper-tool` 注册到当前用户的 Codex
配置，不改其他 MCP 项。重新打开会话或重新加载 MCP 连接后使用。验证注册：

```powershell
codex mcp get sshhelper-tool
```

手动注册等价命令（按实际安装位置修改）：

```powershell
codex mcp add sshhelper-tool -- C:\Tools\sshhelper\dist\sshhelper-tool.exe --stdio
```

默认配置和主机指纹来自 `%LOCALAPPDATA%\sshhelper`，不要求程序目录可写。GUI 与 MCP 可通过 `--data-dir` 或 `SSHHELPER_DATA_DIR` 选择同一个独立目录。需要旧版显式配置时可用 `-CredentialsPath` / `--credentials`，旧文件名及其旁边的固定指纹仍兼容读取。
工具在每次调用时重新读取配置，服务器凭据、名称或刷新间隔变化后会重建相应采样器。
配置损坏或不能解密会明确报错，不会静默改成仅本机。

## 在 Codex 中使用

- “使用 sshhelper-tool 查看所有服务器和本机的即时监控。”
- “使用 sshhelper-tool 查看 example.invalid 的 CPU 温度、功耗、显存和网络速度。”
- “使用 sshhelper-tool 查看本机 CPU 负载和内存占用。”

工具列表：

| 工具 | 参数 | 行为 |
| --- | --- | --- |
| `list_servers` | 无 | 返回 `server_id`、名称、地址、端口、本机标记、刷新间隔；不连接 SSH |
| `get_monitoring` | `server` 可选 | 不填查询全部；可填 `local`、`server-1` 等 ID、精确配置名称或主机地址 |
| 同上 | `fresh`，默认 `true` | 请求本次调用后开始的新采样；`false` 直接读取最近的后台快照 |
| 同上 | `wait_seconds`，默认 20，范围 0–30 | 所有主机共用一个总等待时限；超时返回 pending 或旧快照 |

同名或同地址对应多台配置时，必须用 `server_id` 消除歧义。`server-N` 依照当前配置顺序生成，
配置增删后应重新 `list_servers`。首次成功采样后短暂追加一次采样，建立 CPU、网络和功耗差分基线。
之后按各主机设置的间隔在后台更新；`fresh=true` 可以唤醒长间隔等待。
每个 MCP 进程独立维护自己的采样器与 SSH 连接，仅对查询过的主机启动后台采样。
SSH 失败后间隔约 3 秒重试。退出 MCP 进程会取消任务并释放连接。

## 返回数据

结果同时提供 MCP `structuredContent` 和等价的 JSON 文本。示例外层格式：

```json
{
  "schema_version": 1,
  "requested_fresh": true,
  "generated_at": "2026-10-18T10:00:00Z",
  "servers": [
    {
      "server_id": "local",
      "name": "本机",
      "local": true,
      "host": null,
      "port": null,
      "refresh_interval_seconds": 2,
      "state": "online",
      "connected": true,
      "status": "Local",
      "host_name": "MY-PC",
      "sampled_at": "2026-10-18T10:00:00Z",
      "age_ms": 1,
      "is_fresh": true,
      "rates_ready": true,
      "metrics": {}
    }
  ]
}
```

示例省略 `metrics` 的具体内容。实际包含：

| 字段 | 单位或含义 |
| --- | --- |
| `cpu.usage_percent`、`cpu.clock_mhz` | CPU 忙碌占比、操作系统报告的动态频率，目前由本机采样提供 |
| `cpu.temperature_c`、`cpu.power_w` | CPU 传感器温度、封装功耗 |
| `cpu.thermal_zone_c` | Windows 固件温区，单独保留，不冒充 CPU Die/Package 温度 |
| `cpu.core_temperatures_c`、`cpu.chiplet_temperatures_c` | 带标签的核心或 CCD 温度数组 |
| `gpus` | GPU 名称、温度、功耗、风扇、显存频率和容量 |
| `fans_rpm` | 带标签的风扇转速数组 |
| `memory`、`swap`、`disk`、`gpus[].memory` | `used_bytes`、`total_bytes`、`used_percent` |
| `memory_configured_rate_mts` | BIOS 配置内存传输速率，MT/s |
| `ssd_temperature_c` | 系统盘或远端 NVMe 温度 |
| `network.down_bytes_per_second`、`network.up_bytes_per_second` | 默认网卡接收/发送速率，字节/秒 |
| `network.link_mbps` | 网卡协商速率，Mbps |
| `ssh_latency_ms` | 远端探针命令首字节耗时；本机为 `null` |

`sampled_at` 和 `generated_at` 均使用 UTC。缺失指标使用 `null`，不使用伪造的零值。
`rates_ready=false` 时网络速率和本机 CPU 负载尚未建立差分基线，显示为 `null`。
`is_fresh=true` 表示结果来自本次调用后开始的采样；缓存读取通常为 `false`。
新采样超时可能返回旧数据，此时必须结合 `age_ms`、`is_fresh` 判断。
`state=offline` 表示采样/连接失败，`metrics=null`；`pending` 表示尚无完成的采样。
单台主机离线不会使整个结果失败。

## 命令行 JSON 接口

```powershell
.\dist\sshhelper-tool.exe --list-servers
.\dist\sshhelper-tool.exe --snapshot --server local
.\dist\sshhelper-tool.exe --snapshot --server server-1
.\dist\sshhelper-tool.exe --snapshot
```

默认配置为 `%LOCALAPPDATA%\sshhelper\sshhelper.credentials`；可用 `--data-dir 完整目录`，或显式 `--credentials 完整路径`。`--show-data-dir` 显示所用目录。
无配置文件时提供本机监控。标准输出只有 JSON，启动错误写入标准错误并以非零状态退出。

## 安全与限制

仅接受保存的主机配置，不能传入任意 SSH 命令、上传、下载或修改远端设置。
MCP 服务不返回密码、用户名、私钥路径或口令；主机指纹必须事先存在，变化时拒绝连接，
查询不会新增或修改指纹记录。主机名称、硬件标签、状态字符串按不可信数据处理。
采样权限和可用传感器与 sshhelper 相同；远端硬件缺失或无读取权限的指标为 `null`。
本接口不读取 GUI 的传输进度，也不提供长期历史数据。

## 验证

`ctest --test-dir build --output-on-failure` 新增监控 API 与 MCP 协议测试，覆盖并行等待、
新鲜度、缓存、配置变化、歧义选择、凭据不泄露、缺失值、错误、请求大小与逐行消息。
如开发机有 Node.js，额外执行 `mcp_stdio`：启动真实原生 MCP 子进程，完成握手、发现工具、
读取 Windows 实际监控并退出。Node.js 只用于这项开发测试。

Codex 接入方式参考：[OpenAI 官方 MCP 文档](https://developers.openai.com/codex/mcp)。
