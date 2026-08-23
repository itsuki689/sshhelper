# AirSFTP Monitor

一个约 3 MB 的 Windows 原生 SSH 文件传输与远程主机悬浮监控工具。没有 WebUI、Electron、Node.js 或 Python 运行时。

## 功能

- SFTP 上传/下载：指定本地与远程完整路径，显示实时进度、累计字节和平均速度。
- SSH 遥测：CPU 封装温度、各核心温度、机箱风扇、GPU 温度与风扇、内存、根分区存储、默认网卡上下行速度、SSH 命令首字节延迟。
- 紧凑悬浮窗：Win32 + Direct2D，默认置顶；双击标题或拖到顶部收成横向延迟条，拖到左右边缘收成竖向状态条。收缩后可直接拖动，松手会按最近边缘自动切换横/竖形态，单击任意位置以平滑动画恢复。动画遵循 Windows 辅助功能设置。核心温度按高到低显示前三项；风扇较多时每 6 秒轮播。
- 原生苹果风格设置页：圆角无边框面板、自绘主次按钮与紧凑字段分组，支持顶部拖动和跨显示器 DPI。
- 密码或私钥认证：密码框在私钥模式下兼作密钥口令；敏感内容只保存在当前进程内，不写入磁盘。
- 主机密钥固定：首次连接自动记录 `AirSFTP.known_hosts`，以后密钥改变会拒绝连接。

## 直接使用

1. 双击 `start.bat`，或直接运行 `dist\\AirSFTP.exe`。
2. 点“设置”，填写主机、端口、用户名以及密码或私钥路径；加密私钥可在密码框填写口令。
3. 点“保存并监控”开始每 2 秒采样。
4. 传输时填写本地文件和远程文件的完整路径，然后点“上传”或“下载”。

远端必须是 Linux 且可通过当前 SSH 用户执行 `sensors`。你已安装的 `lm-sensors` 足够提供 CPU/主板传感器；NVIDIA 指标额外使用 `nvidia-smi`，AMD/Nouveau/Intel GPU 会尝试从 `lm-sensors` 芯片读取。硬件驱动没有暴露的项目会显示为 `—`，例如部分 AMD CPU 不提供逐核心温度。

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

## 技术方案

- UI：纯 Win32 + Direct2D/DirectWrite。
- SSH/SFTP：静态 libssh2，Windows CNG 加密后端。
- 遥测：在现有 SSH 会话中执行只读 POSIX `sh` 探针，解析 `sensors -u`、`/proc`、`df` 和可选 `nvidia-smi`。
- 安全：传感器字段不拼接用户输入；文件路径通过 SFTP API 传递，不交给远端 shell。
