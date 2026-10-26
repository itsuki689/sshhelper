# 便携程序交付验收（2026-10-26）

本批两个 EXE 直接提取自构建源码提交 `9d7262f8b811add693db425793fc960984d33f30` 的
[Windows CI 40277892828](https://github.com/itsuki689/sshhelper/actions/runs/40277892828)，运行尝试为 1，
工件 ID 为 `12631160354`。已核对运行成功、head_sha、工件所属运行和下载 ZIP 的摘要；
两个 EXE 各自的摘要、大小、工具链版本及全部 13 项通过的 CTest 名称由导入脚本写入
[build-info.json](../dist/build-info.json)，用户可用 [SHA256SUMS](../dist/SHA256SUMS) 核验。

这是对现有成功构建的交付修正，没有修改主机信任逻辑、程序源码或编译参数。
随后提交 EXE、脚本和说明的交付提交与上述构建源码提交不同；使用
`git log -1 --format=%H -- dist/build-info.json` 或 GitHub 文件历史定位交付提交。

| 检查 | 实际结果 |
| --- | --- |
| 来源构建与回归 | 指定 CI 的完整构建及 13 个 CTest 成功，包含 `host_trust`、`app_host_trust`、`mcp_stdio` |
| 交付文件一致性 | 两个 EXE 来自同一个已核验工件，提取后分别计算摘要，与 JSON 和校验文件一致 |
| 启动器 | 正常 Windows 用户会话中，在包含中文和空格的独立临时目录运行 `start.bat`，退出码为 0；交付 GUI 保持运行，并在回环测试配置下观察到非零主窗口句柄 |
| 开发环境依赖 | 子进程 PATH 仅包含 Windows/System32/Wbem；两个 EXE 的导入表与实际模块列表均未出现 MinGW/MSYS2 的运行时 DLL |
| MCP 启动 | 同一临时目录的交付 EXE 完成 MCP 初始化、仅含本机的隔离配置发现及正常退出 |
| 拒绝不一致文件 | 分发回归验证 EXE 篡改、CI/source_commit 不一致、额外校验项及缺少伴随 EXE；篡改时 `start.bat` 返回失败 |
| 最终 GUI 首次信任点击 | 未完成：窗口观察工具多次报告最终程序没有可定位窗口，无法检查或点击指纹提示。已准备同一 Windows 会话的回环 SSH 桩和一次性虚拟账号，桩未收到认证；不能据此声称交互冒烟通过。对应源码的信任回归由上述成功 CI 证明 |

可复现命令：

```powershell
powershell -NoProfile -File scripts/verify-dist.ps1
powershell -NoProfile -File tests/distribution_test.ps1
powershell -NoProfile -File scripts/test-portable.ps1
```

便携验收会保留临时目录和 `build/portable-smoke.json`，不读取现有配置。交互复核可在该临时目录的
设置中录入自己的隔离测试服务器，核对控制台读取的指纹后进行接受或拒绝操作；不要复用个人配置进行自动测试。
本轮没有进行代码签名、UI 重构或完整 Release 系统建设。
