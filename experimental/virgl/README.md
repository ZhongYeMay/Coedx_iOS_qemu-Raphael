# VirtIO-GPU virgl 传输验证（v0.2，显式启用）

本目录提供 QEMU 虚拟 3D 命令链路的实验配置。[下载 v0.2 编译包与完整测试日志](../../downloads/qemu-virgl-transport-v0.2-arm64.tar.gz)。新增 XNU 代码已完成上下文、3D 资源、命令提交和像素读回测试；它不是 Metal/OpenGL API 实现，也没有把 CoreAnimation 接入该 3D 路径。

## 已验证内容

在 QEMU 10.0.13、virglrenderer 1.1.0 与宿主 Mesa llvmpipe 上实际执行：

1. 协商 `VIRTIO_GPU_F_VIRGL`，查询并验证 virgl v1 capset（308 字节）及 BGRA8 render-target 支持。
2. 创建上下文及独立的 64 × 64 BGRA8 3D 资源，注册后备内存并关联上下文。
3. 提交 virgl `CREATE_OBJECT(surface)`、`SET_FRAMEBUFFER_STATE` 和 `CLEAR` 命令。
4. 通过 `TRANSFER_FROM_HOST_3D` 读回全部 4096 个像素，分别验证红、绿两轮输出；每轮都先写入不同的无关哨兵值，防止把残留后备内存误判为成功。
5. 两轮像素不匹配数均为零，随后释放上下文关联、上下文与测试资源。
6. 测试后继续启动完整 iOS，SpringBoard 完成初始化并可交互；显示传输与 fence 持续完成。

这里验证的是**虚拟 3D 传输与清屏执行**。没有验证顶点/片元着色器、几何绘制、OpenGL ES 或 Metal，更没有证明物理 GPU 加速或桌面帧率提升。

## 启动参数

| 参数 | 默认 | 行为 |
|---|---|---|
| `virtio_gpu=1` | 显式启用 | 启用虚拟显示驱动 |
| `virtio_gpu_virgl=1` | 关闭 | 宿主提供 VIRGL 时协商 virgl v1；否则保留 2D 路径 |
| `virtio_gpu_3d_test=1` | 关闭 | 请求 VIRGL 并执行启动期的两轮清屏/读回测试 |

普通 `scripts/run-qemu.sh` 默认仍选择 `virtio-gpu-device`。具有正常 DRM/EGL 宿主的环境可使用：

```sh
VIRGL=1 VIRGL_SELFTEST=1 bash scripts/run-qemu.sh work/hackios 900
```

这会选择 `virtio-gpu-gl-device` 与 `egl-headless`。宿主需要 QEMU 的 OpenGL 模块、virglrenderer 和可用 EGL 环境。没有 VIRGL 时的2D回退不表示3D自检通过，应以 `VIRGL_3D_SELFTEST PASS` 记录为准。

## 无物理 GPU 的云端软件测试

本次环境没有 `/dev/dri`。补齐 QEMU OpenGL 模块后，标准 `egl-headless` 仍因缺少 DRM render node 无法初始化，因此另提供**进程内、显式启用的测试适配器**：

```sh
bash scripts/run-virgl-software-test.sh work/hackios 900
```

脚本仅为该 QEMU 进程设置 `LD_PRELOAD`，使用 EGL surfaceless 创建 OpenGL core 上下文，并强制 Mesa 软件渲染。它不修改 QEMU 可执行文件、系统库或设备配置。日志必须标识类似：

```text
surfaceless-test: EGL 1.5, GL_RENDERER=llvmpipe (...); SOFTWARE HOST TEST ONLY
virtio-gpu: VIRGL_CLEAR pass 1: ... mismatches=0/4096 ...
virtio-gpu: VIRGL_CLEAR pass 2: ... mismatches=0/4096 ...
virtio-gpu: VIRGL_3D_SELFTEST PASS: ... no CoreAnimation acceleration
```

最初的完整iOS会话使用初版适配器；错误处理加固版随后通过原生错误路径、EGL初始化及最小XNU的同一virgl像素读回测试。

适配器依赖 QEMU **10.0.x** 的导出符号与 EGL 接口，本次只验证10.0.13；不能视为通用显示后端。它拒绝不支持的模式、缺失的 ABI 与非 llvmpipe/softpipe 渲染器，并使用 QEMU Error 接口报告失败。宿主仍需自行安装相应QEMU模块、virglrenderer及Mesa软件驱动；脚本不自动下载依赖。

## 截图限制与证据来源

QEMU 10.0.x 的 `screendump(device=vgpu)` 对本次 GL 纹理扫描输出返回 `no surface`。不能把它当成已有可用的GL截图，也不能把ramfb画面标为GL输出。

本轮完整系统截图明确来自默认 **ramfb 镜像控制台**：

```sh
python3 scripts/screenshot.py --boot-framebuffer boot-screen.png
```

[本轮桌面镜像](../../docs/images/virgl-v0.2-ramfb-desktop.png)与[关于本机镜像](../../docs/images/virgl-v0.2-ramfb-about.png)可供核对。

该截图证明系统界面仍可运行；独立的 virgl 清屏结果由 3D 资源像素读回及 fence 校验证明。原先README里的 `device=vgpu` 截图仍属于 v0.1 的实际2D会话，两类证据不混用。

## 后续工作

需要继续实现可供用户态调用的上下文/资源/同步接口，接入 Mesa 或其他图形 API 后端，并验证着色器及绘制路径，才能进一步对接桌面合成。当前原型仅使用一个隔离测试上下文，不提供通用用户态提交接口，也未实现热插拔、休眠恢复或多客户端资源隔离。
