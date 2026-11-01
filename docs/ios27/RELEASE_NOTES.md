# v0.3.0-pre.1 · iOS 27 启动前检查工具

这是**开发工具预发布版**，不是可启动的 iOS 系统，也不是手机刷机包。
发布来源为 `ios27-development` 分支，不代表已合并到 `main`。

## 本次进展

- 离线检查 IPSW 的 BuildManifest：读取版本、构建号、设备和板级信息；拒绝错误主版本及异常清单。
- 检查定制 QEMU 的 darwin 机器、TCG 与启动选项，校验固件是否齐全及 SPTM/TXM 是否成对。
- 添加有超时限制的无界面启动入口、串口日志捕获、进程回收及日志防覆盖。
- 建立测试通过后才发布的工作流，附件包含源码、测试日志、宿主检查、提交号和 SHA-256。
- 写入“小进展预发布、大进展经实测后正式发布”的项目规则。

## 验证范围

Linux x86_64 本地 30 项测试通过。测试使用合成固件和假 QEMU，覆盖元数据解析、
参数校验、真实子进程调用与超时回收；**没有运行 Apple 固件，也没有启动 iOS**。
GitHub Actions 在发布前重跑测试，实际日志见附件 `tests.log`。
`host-preflight.json` 缺失 QEMU/固件时应报告阻塞，这不是系统启动测试通过。

## 附件

- `ios27-tooling-v0.3.0-pre.1.tar.gz`：工具、测试、说明及许可。
- `tests.log`：GitHub Actions 测试日志。
- `host-preflight.json`：发布环境检查结果。
- `SOURCE_COMMIT.txt`：对应源码提交。
- `SHA256SUMS`：附件校验值，可使用 `sha256sum -c SHA256SUMS` 验证。

## 尚未完成

未编译定制 QEMU，未验证 iOS 27 内核、root shell 或 SpringBoard 启动。
没有新增 GPU 驱动或 Linux ramdisk 修改器，既有 iOS 16.5 路径未在本轮重测。
附件不包含 Apple 固件、SDK、系统镜像、密钥或凭据。

运行方法与限制详见 `docs/ios27/README.md`。
