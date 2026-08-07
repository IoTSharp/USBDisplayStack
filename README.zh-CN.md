# USBDisplayStack

[English](README.md) | 简体中文

USBDisplayStack 是一个采用 `GPL-2.0-only` 许可证的 Linux USB 显示栈，面向
使用厂商私有传输协议的 USB 显示适配器。应用只需向标准 framebuffer 或
DRM/KMS 设备渲染；协议、压缩和 USB 传输由可替换的用户态后端负责。

> **Alpha 状态：**虚拟 DRM/fbdev 前端和用户态帧通路已在 Linux 4.15
> 工作。Actions Micro `185b:2d1d` 实时后端已经实现，但仍处于实验阶段，
> 默认不启用。

## 架构

```text
LVGL 或其他应用
          |
          +----> /dev/fbN（固定 XRGB8888 framebuffer）
          |
          +----> /dev/dri/cardN（DRM/KMS 虚拟连接器）
                         |
                         v
                usbdisplay 内核模块
                  三缓冲快照 ABI
                         |
                  /dev/usbdisplay0
                         |
                         v
                    usb-displayd
                         |
                 动态加载的后端
                         |
              编码 / HID / USB 传输
```

模块绝不会回退到 `/dev/fb0`。USB 传输或后端缺失时，仅第二显示设备停止，
主显示设备不会受到影响。层间契约参见 [架构](docs/architecture.md)、
[ABI](docs/abi.md) 和 [兼容性](docs/compatibility.md)。

## 组件

- `kernel/usbdisplay_drv.c`：虚拟 DRM/KMS、独立 fbdev 和只读三缓冲帧流。
- `userspace/usb-displayd.c`：单消费者帧调度守护进程。
- `backends/null`：吞吐和链路诊断。
- `backends/ppm`：把最新帧写成 PPM 图片。
- `backends/actions-micro`：Actions Micro `185b:2d1d` 的初始化、心跳、
  FFmpeg H.264 编码和 HID 视频传输。
- `tools/fb-test-pattern`：向 `/dev/fbN` 写入测试图案。
- `tools/drm-probe`：不依赖 libdrm，通过 DRM ioctl 检查连接器和模式。
- `tools/actions-micro-replay`：验证并显式回放捕获的 HID 报文，用于协议研究。
- `examples/lvgl`：LVGL 9 的 fbdev 和 DRM/KMS 示例。
- `examples/csharp`：.NET 8 的 fbdev 和 DRM/KMS 示例。

## 环境要求

当前内核实现针对 Linux 4.15 的 TinyDRM API，需要以下内核配置：

```text
CONFIG_DRM
CONFIG_DRM_KMS_HELPER
CONFIG_DRM_TINYDRM
CONFIG_FB
CONFIG_FB_DEFERRED_IO
```

构建前需安装 C 编译器、make 和与运行内核匹配的头文件。Actions Micro 后端
还需要启用了 `libx264` 编码器的 `ffmpeg`。可选示例分别需要 LVGL 9、
libdrm 开发文件或 .NET 8。

## 构建

```bash
make userspace
make module
# 已安装 LVGL 9 和 libdrm 开发文件时可选：
make examples-lvgl
```

用户态产物位于 `build/`，内核模块为 `kernel/usbdisplay.ko`。

## 手工冒烟测试

```bash
sudo modprobe tinydrm
sudo insmod kernel/usbdisplay.ko width=640 height=360

build/drm-probe /dev/dri/card1 640 360
build/usb-displayd \
  --backend build/usbdisplay-ppm.so \
  --backend-option /tmp/usbdisplay.ppm &
daemon_pid=$!

sudo build/fb-test-pattern /dev/fb1
sleep 1
kill "$daemon_pid"
sudo rmmod usbdisplay
```

第二设备的编号不保证为 `1`。生产脚本应通过 sysfs 中的 `usbdisplay`
framebuffer 名称和 DRM 驱动名称发现节点。

## 应用示例

同一时间只运行一个渲染生产者。两个 LVGL 示例显示同一套仪表盘：

```bash
make -C examples/lvgl
sudo build/examples/lvgl-fbdev-example /dev/fb1
sudo build/examples/lvgl-drm-example /dev/dri/card1
```

C# 示例绘制动态 XRGB8888 图案，并会拒绝不属于 USBDisplayStack 的设备：

```bash
dotnet run --project examples/csharp/Fbdev -- /dev/fb1
dotnet run --project examples/csharp/Drm -- /dev/dri/card1
```

## 安装

`scripts/install.sh` 安装当前内核的模块、用户态程序、后端、udev 规则和
systemd 文件，但不会自动加载模块或启用服务。

```bash
sudo ./scripts/install.sh
sudo systemctl enable --now usb-displayd.service
```

启动前请在 `/etc/modprobe.d/usbdisplay.conf` 设置虚拟分辨率，并在
`/etc/default/usb-displayd` 选择后端。

## Actions Micro 激活

实时后端需要用户有权使用的 `DPRPL001` replay 模板，其中包含适配器的
初始化和心跳命令。默认模式会忽略捕获的视频；对于只有完整捕获流才能退出
等待页的固件，`bootstrap=full` 会明确地发送一次捕获视频。本项目不分发
replay 数据。参考适配器配置如下：

```text
USBDISPLAY_BACKEND=/usr/lib/usbdisplay/usbdisplay-actions-micro.so
USBDISPLAY_BACKEND_ARGS=--backend-option template=/var/lib/usbdisplay/actions-micro.replay,bootstrap=full
```

后端会自动发现并校验两个 `185b:2d1d` hidraw 接口，发送初始化序列，在
静止画面期间持续发送心跳，并将新的 fbdev 或 DRM 帧编码成 baseline H.264
和厂商 HID 报文。模块和服务启动后，LVGL 写入 USBDisplayStack framebuffer
就会自动进入这条激活与显示通路。

完整选项、安全规则和当前验证边界参见
[Actions Micro 185b:2d1d](docs/actions-micro-185b-2d1d.md)。已捕获的 Windows
画面和实时生成的 LVGL 画面均已在参考设备上显示；后端仍需完成重复重连和
长时间运行验证，因此继续标记为实验性。

完整的硬件型号、USB ID 和验证状态参见[支持设备清单](docs/supported-devices.zh-CN.md)。

![绿联 USB 转 HDMI 转换器](docs/assets/devices/ugreen-usb-hdmi-185b-2d1d.png)

## 许可证

USBDisplayStack 使用 `GPL-2.0-only` 许可证，详见 [LICENSE](LICENSE)。
