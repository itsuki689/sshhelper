# AirSFTP

一个精简的苹果风格 SSH/SFTP 文件传输助手。后端只依赖 `ssh2`，支持上传、下载、实时进度、速度、耗时和取消任务。

## 运行

双击 `start.bat`，或在命令行运行：

```powershell
npm install
npm start
```

访问 `http://127.0.0.1:4173`。服务默认只监听本机，不会保存或记录密码、私钥内容。

## 创建上传任务

```http
POST /api/transfers
Content-Type: application/json

{
  "direction": "upload",
  "host": "server.example.com",
  "port": 22,
  "username": "ubuntu",
  "password": "your-password",
  "sourcePath": "D:\\build\\app.zip",
  "destinationPath": "/var/www/app.zip"
}
```

下载时使用 `"direction": "download"`，此时 `sourcePath` 是远程文件，`destinationPath` 是本地文件。认证也可使用 `privateKeyPath`，或设置 `"useAgent": true` 使用系统 SSH Agent。

响应包含任务 ID，以及状态和 SSE 地址：

- `GET /api/transfers/:id`：查询当前状态。
- `GET /api/transfers/:id/events`：订阅实时进度。
- `DELETE /api/transfers/:id`：取消传输。

进度数据包含 `progress`、`transferredBytes`、`totalBytes`、`bytesPerSecond` 和 `elapsedSeconds`。

## 便携 EXE

直接运行 `dist\AirSFTP.exe`，无需安装 Node.js；程序启动后会自动打开浏览器，并只监听本机地址。

如需重新打包，双击 `build-exe.bat`，或运行：

```powershell
npm run build:portable
```
