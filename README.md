# HackiOS Raphael 与 QEMU 实验适配

基于 [OOSEX GANG 的 HackiOS](https://github.com/oosexgang/hackios-xnu-kernel)，整理 Redmi K20 Pro / raphael / SM8150 的实验驱动，以及云端 QEMU ARM64 的实际运行结果。

**目前：QEMU 能进入 iOS 16.5 桌面与设置，VirtIO-GPU 2D 显示已验证。没有 Metal／3D 硬件加速，没有验证 K20 Pro 真机启动或从 UFS 启动 iOS。**

## 实际状态

| 部分 | 实现与验证 |
|---|---|
| QEMU VirtIO-GPU 2D | 驱动已编译进 XNU；1125×2048输出，实测至少240帧传输、fence488完成 |
| 桌面与设置 | 实际进入 SpringBoard → Settings → General → About |
| 3D／Metal 加速 | 未实现；CoreAnimation 仍是 CPU 软件渲染 |
| Adreno 640 | 分阶段固件解析、时钟恢复与 GMU/SMMU 探测；模型与编译验证，未真机验证 |
| GT9886 触摸 | 实验性 GPIO I2C 与报告解析；模型与编译验证，未真机验证 |
| UFS | 只读保护及已有 UEFI 路径复用；未完成原生 UFS 冷初始化或根分区启动 |
| 构建 | ARM64内核已链接，无未解析符号；整体make仍有既有DriverKit导出及symbolset检查错误 |

“240帧已传输”是累计完成次数，**不是240 FPS**，也不能证明比软件显示更流畅。当前云环境没有暴露物理GPU，QEMU安装也没有virgl GL设备后端；3D加速还缺宿主后端与iOS图形API、上下文及命令提交适配。

## 实测画面

以下图片由 QMP 指定 `device=vgpu` 截取，来自新 VirtIO-GPU 2D 驱动的输出：

<p>
<img src="docs/images/virtio-desktop.png" width="260" alt="VirtIO-GPU实际桌面截图">
<img src="docs/images/virtio-about.png" width="260" alt="VirtIO-GPU实际关于本机截图">
</p>

[启动过程视频](docs/video/ios165-boot-and-settings.mp4)来自**此前ramfb会话**，不是VirtIO-GPU或3D加速演示。视频中用提示卡标明切换录制连接造成的间隔；详见[录制说明](docs/video/RECORDING.md)。关于本机显示的 iPhone X 身份来自 HackiOS 兼容配置。

## 目录

- `overlay/`：新增驱动及头文件，按上游源码目录组织。
- `patches/`：固定上游版本的内核接入和用户态修复。
- `scripts/prepare.py`：生成独立的可构建源码工作区。
- `tests/`、`scripts/test-host.sh`：触摸、I2C、A640、VirtIO-GPU、UFS策略模型测试。
- `evidence/`：实际测试输出、硬件能力记录及内核构建报告。
- `downloads/`：此前生成的实验内核、源码、日志打包；不是手机可直接刷入的boot.img。
- `upstream.json`：上游仓库与精确提交版本。

仓库不包含 iOS 镜像、Apple SDK 或 Qualcomm 固件。编译产物来自开源XNU及这些实验补丁。已有工作区中抽取的系统文件没有发布到仓库。

## 获取完整源码工作区

需要 Git 与 Python 3.12+。在本仓库目录运行：

```sh
python3 scripts/prepare.py work/hackios
```

脚本从固定版本下载两个开源上游，保留原有许可，应用本项目补丁并复制新增驱动。目标目录必须不存在；脚本不会覆盖已有工作区，不下载系统镜像或SDK。

上游版本：

- 内核：`oosexgang/hackios-xnu-kernel`，`d82c64a3a31cd27a0a0afbf6e213e467e44b1b4c`
- 用户态：`oosexgang/hackios-xnu-userland`，`a79ff0772a3454fd1ef127e181e5b3bce6c9ce18`

构建前使用 `umask 022`，避免新建的公共镜像目录继承仅root可访问的权限。

接着按[上游构建文档](https://github.com/oosexgang/hackios-xnu-kernel)准备 Linux 编译工具链与自行取得的SDK，执行 `xnu/apply.sh` 创建XNU树，再编译。准备脚本不自动安装这些工具，也不保证任意新工具链版本都可构建。

本次云端还使用了本地工具链路径和宿主头文件路径调整；这些路径不是可移植构建要求，没有写入公共补丁。实际产物与检查限制见[验证报告](evidence/RESULTS.md)。

## 主机测试

```sh
bash scripts/test-host.sh
```

需要 GCC 和 ASan/UBSan。GitHub Actions 也会执行这五组测试。主机模型测试不等于手机硬件验证，CI不会启动Apple系统或证明GPU硬件加速。

## QEMU启动

在内核、loader及用户态镜像已经构建好后：

```sh
bash scripts/run-qemu.sh work/hackios 900
python3 scripts/screenshot.py virtio-screen.png
```

需要支持 `virtio-gpu-device` 的 QEMU 和 AArch64 UEFI。Debian的固件默认路径是 `/usr/share/AAVMF/`，其他环境可设置 `QEMU_EFI`、`QEMU_VARS`；非系统安装可另设 `QEMU_MODULE_DIR`、`QEMU_DATA_DIR`。

脚本增加 `virtio_gpu=1` 和 `-device virtio-gpu-device,id=vgpu,xres=1125,yres=2048`。原有ramfb保留给loader使用。纯QEMU运行不要设置 `raphael=1`，因为手机分支会跳过虚拟输入。QMP默认监听本机4445端口；同一时刻只允许一个客户端连接。

## 下一步

- QEMU：实现并验证virgl/rutabaga与iOS图形接口，才有可能打通3D路径。
- 真机：验证loader、供电与寄存器访问，再推进GMU的SMMU映射、启动握手、CP/SQE命令提交。
- UFS：冷初始化、块设备接口和根文件系统启动链仍需继续实现。

## 来源与许可

保留上游作者与许可；内核相关代码及修改遵循原有APSL-2.0条款，用户态上游MIT许可见 `licenses/USERLAND-MIT.txt`。本项目不是Apple官方项目，也没有将模型测试或QEMU结果表述为手机硬件成功。
