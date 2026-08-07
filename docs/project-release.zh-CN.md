# USBDisplayStack：把私有协议 USB 显示适配器接入 Linux 标准显示栈

项目地址：[gitee.com/IoTSharp/USBDisplayStack](https://gitee.com/IoTSharp/USBDisplayStack)

许可证：`GPL-2.0-only`

当前阶段：Alpha。Linux 虚拟显示前端与用户态帧通路已经完成验证，绿联 USB
转 HDMI 参考设备（Actions Micro `185b:2d1d`）的实时硬件后端仍为实验性功能。

## 从一个常见但棘手的问题开始

市面上有很多被统称为“USB 转 HDMI”的设备，但这个名称只描述了产品形态，
并不代表它们使用同一种协议。DisplayLink、Trigger、MacroSilicon、Fresco
Logic 和 Actions Micro 的设备，在 USB 接口、初始化流程、视频编码和报文格式上
都可能完全不同。

这给 Linux 应用带来了一个很现实的问题：上层程序只是想显示一块仪表盘、状态页
或第二屏内容，却可能被迫理解某个适配器的 HID 报文、H.264 封装和心跳机制。

USBDisplayStack 希望把这两件事分开：应用继续使用标准的 framebuffer 或
DRM/KMS 接口，具体设备的压缩、初始化和 USB 传输，则交给可替换的用户态后端。

## USBDisplayStack 是什么

USBDisplayStack 是一套面向 Linux 的开源 USB 显示栈。它在系统中创建独立的
虚拟 DRM/KMS 显示设备和 fbdev 设备，再通过只读帧流把画面交给用户态守护进程。
守护进程动态加载与硬件匹配的后端，完成像素转换、编码和厂商协议传输。

整个数据通路如下：

```text
LVGL、.NET 或其他图形应用
            |
            +----> /dev/fbN（固定模式 XRGB8888 framebuffer）
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

它不是 EVDI 或 DisplayLink 的替代实现，也不会声称所有“USB 转 HDMI”设备都能
直接使用。USBDisplayStack 提供的是一套清晰的显示前端、帧传递机制和后端扩展
边界；每一种私有协议仍然需要与之匹配的后端。

## 几个关键设计

### 1. 应用只面对标准 Linux 显示接口

内核模块同时提供一个固定模式的 XRGB8888 framebuffer 和一个带固定虚拟模式的
DRM/KMS 连接器。LVGL 等现有 fbdev 应用可以直接写入 `/dev/fbN`，DRM 应用则
可以通过标准 KMS 流程完成显示，不需要链接厂商 SDK，也不需要接触 hidraw、
libx264 或私有报文。

项目还提供了 LVGL 9 和 .NET 8 示例，分别演示 fbdev 与 DRM/KMS 两条接入路径。

### 2. 内核只负责稳定地交付画面

内核与用户态之间使用只读三缓冲快照。每次更新都会写入一个未被消费者持有的
槽位，并通过单调递增的序号以及 `poll(2)`、`read(2)` 通知守护进程。

这条通路保证消费者读到的是完整帧。后端过慢时，中间帧可以被较新的帧替换，
因为显示系统更关心“尽快显示最新画面”，而不是把每次更新都当作不可丢失的事件
日志。

### 3. 厂商协议留在用户态

`usb-displayd` 通过 `dlopen(3)` 加载一个共享库后端。后端 ABI 带有版本号和
结构体大小，方便在保持兼容的前提下继续扩展。

设备发现、像素转换、视频压缩、初始化、心跳、USB/HID 传输、热插拔和帧率控制
都属于后端职责。支持另一款适配器时，通常只需在 `backends/` 下增加一个新后端，
不必再写一套 framebuffer 驱动。

### 4. 不接管主显示设备

模块只创建新的设备节点，绝不会把输出重定向到 `/dev/fb0` 或已有的 DRM 卡。
USB 适配器断开、后端加载失败或编码器退出时，受影响的是这条第二显示通路，而不是
主显示设备。

## 当前已经包含什么

| 组件 | 用途 | 当前状态 |
| --- | --- | --- |
| 虚拟 DRM/KMS 与 fbdev 前端 | 为应用提供标准显示接口 | 已在 Linux 4.15 验证 |
| `/dev/usbdisplay0` 三缓冲帧流 | 把一致的最新帧交给用户态 | 已验证 |
| `usb-displayd` | 调度帧并动态加载后端 | 已验证 |
| Null 后端 | 不访问硬件，检查帧通路和吞吐 | 可用 |
| PPM 后端 | 把最新帧写成图片，完成端到端诊断 | 可用 |
| 绿联参考设备的 Actions Micro `185b:2d1d` 后端 | H.264 编码与双 hidraw 传输 | 实验性 |
| `fb-test-pattern` 与 `drm-probe` | 检查 fbdev、DRM 连接器与显示模式 | 可用 |
| LVGL 9 与 .NET 8 示例 | 演示两种标准应用接入方式 | 可用 |

## 从协议回放走到实时画面

当前仓库包含针对一款绿联 USB 转 HDMI 转换器（黑色 USB-A 款）的实验性后端。
参考设备的 USB ID 为 `185b:2d1d`，描述符为 `Compro` / `Mirroring Suit`。
它暴露两个 HID 输出接口，通过厂商消息流传输 H.264。

![绿联 USB 转 HDMI 参考设备](assets/devices/ugreen-usb-hdmi-185b-2d1d.png)

图片用于识别参考设备的外壳，不代表外观相似的绿联其他批次使用相同芯片或协议。
选择后端前，应先用 `lsusb -d 185b:2d1d` 核对 USB 身份。项目实现了以下
完整链路：

- 从用户有权使用的 `DPRPL001` 模板中提取初始化命令和 `_PPA` 心跳；
- 自动发现并校验两个对应的 hidraw 接口；
- 使用 FFmpeg 与 libx264 持续编码 baseline H.264；
- 生成四字节 Annex-B 起始码并封装为 `RRIM/TADV` 视频消息；
- 将消息拆分为 4096 字节 HID 报告，并在两个接口间交替发送；
- 在画面静止时继续发送心跳，保持设备会话活跃。

在参考设备上，项目先通过约五秒的授权捕获回放唤醒了 HDMI 输出，随后完成了
`bootstrap=full` 到实时编码画面的连续切换。物理断电重连后，1920×1080 的
LVGL 实时界面已经在显示器上得到目视确认。

这里需要特别说明：这证明了参考设备上的完整链路可行，但还不等于对所有同 VID/PID
设备作出通用支持承诺。重复热插拔、背压处理和长时间运行仍需继续验证，因此该后端
已经随项目安装，但默认不会启用。项目也不分发包含厂商消息和录屏数据的 replay
模板；使用者必须基于自己有权分析的硬件和数据生成模板。

## 快速体验

当前内核实现针对 Ubuntu 的 Linux `4.15.0-60-generic` x86_64 环境开发和
验证，并依赖该版本的 TinyDRM API。较新内核已经迁移到其他 DRM helper，当前
项目尚未宣称向前兼容。

构建前需要 C 编译器、make，以及与运行内核匹配的头文件：

```bash
git clone https://gitee.com/IoTSharp/USBDisplayStack.git
cd USBDisplayStack

make userspace
make module
```

可以先使用 PPM 后端完成一轮不依赖 USB 硬件的冒烟测试：

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

这里的 `/dev/fb1` 和 `/dev/dri/card1` 只是示例编号。实际脚本应通过 sysfs 中
名为 `usbdisplay` 的 framebuffer，或 DRM 驱动名称来发现设备，不能假定节点
编号固定。

需要安装到系统时，可以运行：

```bash
sudo ./scripts/install.sh
```

脚本会安装内核模块、守护进程、后端、udev 规则和 systemd 服务，但不会自动
加载模块或启动服务。启用前请检查 `/etc/modprobe.d/usbdisplay.conf` 中的
虚拟分辨率，以及 `/etc/default/usb-displayd` 中选择的后端。默认配置使用
Null 后端，不会访问 USB 硬件。

## 应用接入示例

LVGL 9 示例使用同一套仪表盘分别验证 fbdev 和 DRM/KMS：

```bash
make -C examples/lvgl
sudo build/examples/lvgl-fbdev-example /dev/fb1
sudo build/examples/lvgl-drm-example /dev/dri/card1
```

.NET 8 示例会绘制动态 XRGB8888 图案，并在写入前确认目标设备确实属于
USBDisplayStack：

```bash
dotnet run --project examples/csharp/Fbdev -- /dev/fb1
dotnet run --project examples/csharp/Drm -- /dev/dri/card1
```

fbdev 与 DRM 是两个独立的画面生产者，但最终进入同一条后端帧流，因此同一时间
应只运行一个渲染生产者。

## 已完成的验证与明确的边界

在参考 Linux 4.15 环境中，项目已经验证：

- 用户态代码在 `-Wall -Wextra -Werror` 下构建通过；
- 内核模块可针对运行内核完成构建和加载；
- 模块会创建第二张 DRM 卡、名为 `usbdisplay` 的 fbdev 和
  `/dev/usbdisplay0`；
- fbdev 测试图案可以经过守护进程和 PPM 后端生成预期图片；
- DRM 能报告一个已连接的虚拟连接器和一个首选固定模式；
- 重复加载、卸载不会改变原有 DRM 卡、framebuffer 和正在运行的应用；
- LVGL 9 与 .NET 8 的 fbdev、DRM 示例均已走通实时后端软件链路。

项目当前不声称支持 DisplayLink/EVDI 或 MacroSilicon MS912x/MS9132，也不把
不同品牌的 USB 显示适配器视为协议兼容。对于 Actions Micro 后端，目前准确的
表述仍然是“参考设备上完成实时画面验证的实验性支持”。完整硬件身份和验证状态
可查阅[支持设备清单](supported-devices.zh-CN.md)。

## 下一步

接下来最重要的工作包括：

- 将内核前端迁移到较新 Linux 内核使用的 DRM helper；
- 完善 Actions Micro 后端的热插拔、状态输入、背压和主动丢帧策略；
- 扩大物理断电、重复重连和长时间稳定性测试；
- 在现有后端 ABI 上增加更多适配器协议实现；
- 补充更多硬件样本和可复现的兼容性记录。

如果你正在做 Linux 嵌入式界面、LVGL 第二屏、DRM/KMS 应用，或者正在研究一款
没有 Linux 主机驱动的 USB 显示适配器，欢迎试用、提交硬件信息、测试记录或新的
协议后端。

USBDisplayStack 已在 Gitee 开源：
[https://gitee.com/IoTSharp/USBDisplayStack](https://gitee.com/IoTSharp/USBDisplayStack)
