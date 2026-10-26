# sshhelper 发布验证与公开文档隐私

二进制由 GitHub Releases 管理，不进入源码提交。每批 Windows ZIP 内的 `dist/build-info.json`
记录实际构建提交、CI URL、工具链、完整测试列表、两个 EXE 的 SHA256 及实际 Authenticode 状态。
这份记录在构建时生成，不再在 Git 中保存会过期的产物快照。

维护者本轮选择跳过 EXE 签名，因此当前产物应标为 `NotSigned`。SHA256 是完整性信息，不能代替签名。
发布流程会核对标签、产品版本和默认分支关系，完整构建、离线测试和最终文件验证全部成功后才上传 Release。

本地已经验证 14 个 CTest，包括首次信任、MCP、用户目录与旧配置迁移；原全局切换快捷键已移除。
另外验证了中文空格临时目录、系统 PATH、隔离数据目录下的启动器、GUI 进程、MCP 初始化与退出、
EXE 摘要和开发运行库加载检查。进程启动检查不等于完整人工 UI 验收；最终指纹按钮人工点击应单独记录。

本地复现（先全量构建、运行 CTest 并记录当前工具链）：

```powershell
ctest --test-dir build --output-on-failure --output-junit ctest-results.xml
powershell -NoProfile -File scripts/package-release.ps1
powershell -NoProfile -File tests/distribution_test.ps1 -PackageDirectory build/package
powershell -NoProfile -File scripts/test-portable.ps1 -PackageDirectory build/package
```

便携测试不使用已有账号配置，临时目录与 `build/portable-smoke.json` 保留在本地供检查。
CI 中这些检查使用 GitHub 托管测试环境，不依赖真实 SSH 服务器或账号。

公开文档中的真实测试设备地址和 SSH 账号已匿名化，技术方案与性能测量数据保留。
历史审计发现旧提交同样含有这些标识；已在本地隔离镜像中验证历史替换和移除旧 dist 文件，
复扫该预览没有剩余标识。远程历史尚未重写：这会改变已有提交编号及旧产物的来源引用，需要维护者确认。
此外，GitHub 缓存/PR 引用与其他人的克隆不由本地强推自动清理，须遵循
[GitHub 敏感数据清理说明](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository)
另行处理。当前文档的匿名化不能被描述为历史数据已经彻底消除。
