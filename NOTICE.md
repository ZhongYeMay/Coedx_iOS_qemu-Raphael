# 来源

上游作者：OOSEX GANG。

- https://github.com/oosexgang/hackios-xnu-kernel
- https://github.com/oosexgang/hackios-xnu-userland
- https://github.com/apple-oss-distributions/xnu/tree/xnu-8796.121.2

寄存器与固件格式核对：Linux v6.12 的 sm8150.dtsi、gpucc-sm8150.c、gcc-sm8150.c、a6xx_gmu.c、a6xx_catalog.c；VirtIO 1.2 GPU规范及QEMU 10.0.13。

- https://github.com/torvalds/linux/tree/v6.12
- https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html
- https://www.qemu.org/docs/master/system/devices/virtio-gpu.html

公开A640固件仅用于云端解析测试，没有包含到本仓库。源码保留原有头部和归属，补丁不会改变第三方原有许可。Apple系统、SDK、Qualcomm固件不是本项目的再分发内容。
