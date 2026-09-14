# AirSFTP Monitor

一个约 3 MB 的 Windows 原生 SSH 文件传输与远程主机悬浮监控工具。没有 WebUI、Electron、Node.js 或 Python 运行时。

## 功能

- SFTP 文件夹上传/下载：指定本地和远程文件夹，递归传输全部内容，或从复选清单选择部分文件/子目录；保留相对路径与空目录，显示总进度、文件数量和平均速度。
- SSH 遥测：CPU 封装温度与功耗、各核心温度、机箱风扇、GPU 温度/风扇/功耗、内存、根分区存储、默认网卡上下行速度、SSH 命令首字节延迟。
- 紧凑悬浮窗：Win32 + Direct2D，默认置顶；双击标题或拖到顶部收成横向延迟条，拖到左右边缘收成竖向状态条。收缩后可直接拖动，松手会按最近边缘自动切换横/竖形态，单击任意位置以平滑动画恢复。动画遵循 Windows 辅助功能设置。核心温度按高到低显示前三项；风扇较多时每 6 秒轮播。
- 原生苹果风格设置页：圆角无边框面板、自绘主次按钮与紧凑字段分组，支持顶部拖动和跨显示器 DPI。
- 密码或私钥认证：密码框在私钥模式下兼作密钥口令；主机、端口、用户名、密码和私钥路径使用 Windows DPAPI 加密保存在 EXE 同目录的 `AirSFTP.credentials`，仅当前 Windows 用户可解密。
- 主机密钥固定：首次连接自动记录 `AirSFTP.known_hosts`，以后密钥改变会拒绝连接。

连接与读写操作设有 15 秒超时；切换连接、开始新的传输或退出时会取消旧任务，等待过程在后台完成。传输只有在读取、写入、关闭及已知文件大小检查通过后才显示完成；失败或取消可能留下部分目标文件。网卡切换或采样缺失后会重新建立速率基线。

## 直接使用

1. 双击 `start.bat`，或直接运行 `dist\\AirSFTP.exe`。
2. 点“设置”，填写主机、端口、用户名以及密码或私钥路径；加密私钥可在密码框填写口令。
3. 点“保存并监控”开始每 2 秒采样。
4. 填写两个**文件夹路径**，例如本地 `D:\data`、远程 `/home/user/backup`。本地“浏览”按钮打开文件夹选择器；下载目标文件夹也可以手动输入尚未创建的完整路径。
5. 选择“整个文件夹”后点击“上传”或“下载”，递归传输源文件夹内的所有普通文件和子目录。
6. 选择“选择部分文件 / 子目录”后点击方向按钮：上传列出本地内容，下载列出远程内容。勾选需要的项目，再点“开始上传”或“开始下载”；支持全选、全不选和取消扫描。勾选目录会同时勾选其当前内容，也可以取消其中某些文件。

上传方向：本地文件夹 → 远程文件夹；下载方向相反。源文件夹内的 `sub/a.txt` 会落在目标文件夹的 `sub/a.txt`，不会额外套一层源文件夹名称。目标目录不存在时自动创建；同名普通文件直接覆盖，目标中未涉及的文件保留。

不跟随符号链接或 Windows 目录联接，跳过特殊设备文件。下载选中的文件名若不兼容 Windows（如保留设备名、大小写冲突）会报错。远程文件夹须为绝对路径且不包含 `.` 或 `..` 路径段。单次扫描最多 100,000 个项目、128 层子目录；失败时停止本批任务，已完成文件保留，当前文件可能只传输了一部分。

远端必须是 Linux 且可通过当前 SSH 用户执行 `sensors`。你已安装的 `lm-sensors` 足够提供 CPU/主板传感器；功耗读取 `power*_average` 或 `power*_input`。NVIDIA 指标额外使用 `nvidia-smi` 的 `power.draw`，AMD/Nouveau/Intel GPU 会尝试从 `lm-sensors` 芯片读取。硬件驱动没有暴露的项目会显示为 `—`，例如部分 AMD CPU 不提供逐核心温度或封装功耗。

程序会读取 `/proc/cpuinfo` 的 CPU 名称自动选择功耗策略：Intel/Xeon 优先对 `/sys/class/powercap` 的 RAPL `energy_uj` 做采样差分，并自动汇总双路 CPU；其他处理器及 RAPL 不可用时继续使用上述 `lm-sensors` 策略。硬件驱动没有暴露或当前 SSH 用户无权读取的项目会显示为 `—`。

网络速度取默认路由网卡的 `/sys/class/net/.../statistics` 计数差；存储占用取根分区 `/`。SSH 延迟是启动远程采样命令到收到首字节的耗时，因此比 ICMP ping 更贴近实际 SSH 交互。

## 一键构建便携 EXE

开发机需要 CMake 3.24+、Ninja 和 MinGW-w64 GCC。双击：

```text
build-exe.bat
```

脚本会下载锁定版本的 libssh2，静态链接 WinCNG、C/C++ 运行库，执行测试，并输出 `dist\\AirSFTP.exe`。最终程序仅依赖 Windows 系统 DLL。

命令行等价操作：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

## 回归测试

`ctest --test-dir build --output-on-failure` 覆盖遥测解析、凭据加密、回环 SSH 超时与取消、中文私钥路径，以及 MinGW 下的 SFTP 故障注入（短写、读写失败、关闭失败、文件大小不符和取消）、文件夹递归与部分选择上传/下载、空目录、中文路径及文件名冲突检查。测试不需要真实 SSH 服务器或登录凭据。

## 技术方案

- UI：纯 Win32 + Direct2D/DirectWrite。
- SSH/SFTP：静态 libssh2，Windows CNG 加密后端。
- 遥测：在现有 SSH 会话中执行只读 POSIX `sh` 探针，解析 `sensors -u`、`/proc`、`df` 和可选 `nvidia-smi`。
- 安全：传感器字段不拼接用户输入；文件路径通过 SFTP API 传递，不交给远端 shell。
