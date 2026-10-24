# 实测记录（2026-10-24）

本仓库整理的是一次实际云工作区的适配结果。

- ARM64 XNU链接完成，未解析符号数0。
- make最终退出码2：既有DriverKit导出、IOKit/Libkern/Private symbolset检查失败。没有声称整体构建通过。
- VirtIO-GPU设备16，现代virtio-mmio，VERSION_1协商通过；不协商VIRGL/3D。
- 显示1125×2048 BGRX，9216000字节直接注册为物理后备内存；实际合并成一个物理连续区间。
- 设备确认第240帧，fence488完成；未观察到控制队列超时、错误完成响应或内核panic。
- 实际进入桌面，并通过QMP触摸打开设置→通用→关于本机。
- CoreAnimation仍是软件渲染。没有主张帧率提升或Metal/3D硬件加速。
- GT9886、I2C、A640、VirtIO-GPU与UFS策略主机测试通过；没有SM8150真机测试。
- 新发布目录使用本地固定版本镜像实际执行了准备脚本，补丁接入成功；五组主机测试再次通过。

编译包及其完整日志见 `downloads/`；SHA256见仓库根目录。脚本只准备源码，复现完整系统启动还需要自行准备工具链、SDK和系统镜像。

## v0.2：virgl v1 传输验证

新增运行时证据见 `virgl-v0.2.log`，详细环境与限制见 `experimental/virgl/README.md`。

- 补齐QEMU OpenGL模块及virglrenderer后，使用显式进程级EGL测试适配器，在没有DRM节点的宿主创建llvmpipe上下文。
- XNU完成virgl v1 capset、上下文、3D资源、清屏提交、fence、读回与资源清理测试。
- 红、绿两轮各4096像素校验，差异均为0；不是着色器或几何绘制测试。
- 完整iOS继续启动，SpringBoard完成初始化，CoreAnimation仍为软件渲染。
- GL扫描输出的QMP截图返回`no surface`。本轮界面截图标记为ramfb镜像，不当作GL输出证据。
- 普通2D模式保持默认；新主机测试还覆盖virgl报文及EGL适配器错误契约。
