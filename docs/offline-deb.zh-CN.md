# 离线 Debian 包

USBDisplayStack 的内核模块必须与目标机正在运行的内核完全匹配。因此每个
`.deb` 都绑定一个 `uname -r` 和一个 Debian 架构；安装脚本会在写入系统前
同时检查内核、架构和 SHA-256，避免把不兼容的模块装入车道机。

## 生成包

应在具有目标内核准确头文件的 Linux 构建环境中执行：

```bash
make userspace
make module KDIR=/lib/modules/$(uname -r)/build
sh scripts/package-deb.sh 0.2.0 dist
```

也可以显式指定内核和架构。`kernel/usbdisplay.ko` 必须已经用同一套
`KERNEL_RELEASE` 头文件构建，脚本不会把其他版本的模块伪装成目标版本：

```bash
KERNEL_RELEASE=4.15.0-60-generic DEB_ARCH=amd64 \
  sh scripts/package-deb.sh 0.2.0 dist
```

输出目录包含：

- `usbdisplay-stack_<版本>+kernel.<内核>_<架构>.deb`；
- 同名 `.sha256` 校验文件；
- `install-usbdisplay-offline.sh` 离线安装脚本。

`.deb` 内含 `usbdisplay.ko`、`usb-displayd`、三个后端、诊断工具、udev 和
systemd 配置，以及 `usbdisplay-check`。它不包含初始化 replay，也不捆绑
FFmpeg。Actions Micro 实体后端要求目标机已经有带 `libx264` 编码器的
`ffmpeg`；安装脚本只检查本机状态，绝不会联网补依赖。

## 仅安装

把 `.deb`、`.sha256` 和安装脚本放在同一目录后运行：

```bash
sudo sh ./install-usbdisplay-offline.sh \
  ./usbdisplay-stack_0.2.0+kernel.4.15.0-60-generic_amd64.deb
```

这一步安装文件并刷新 `depmod`、udev 和 systemd，但不会加载模块、启动服务
或改动车道应用。若确实无法提供校验文件，可以显式使用 `--skip-checksum`；
现场正常交付不应跳过校验。

## 配置并启用实体适配器

replay 必须来自有权使用的参考设备，不得提交到仓库。下面的显式 `--enable`
才会加载模块、启用并重启 `usb-displayd`，随后等待物理链路就绪：

```bash
sudo sh ./install-usbdisplay-offline.sh \
  --backend actions-micro \
  --template ./actions-micro.replay \
  --bootstrap full \
  --width 1920 --height 1080 \
  --enable \
  ./usbdisplay-stack_0.2.0+kernel.4.15.0-60-generic_amd64.deb
```

安装脚本把 replay 以 `0600` 权限放到
`/var/lib/usbdisplay/actions-micro.replay`。如果服务未能在超时时间内打开
`185b:2d1d` 的两个 HID 接口，脚本以非零状态结束，但已经安装的软件不会被
自动回滚。此时应保留现场，运行检测命令并查看服务日志。

从源码安装脚本升级时，旧版本可能在 `/usr/lib/systemd/system` 留下未受包
管理的同名服务并遮蔽 Debian 包的 `/lib/systemd/system` 单元。离线安装脚本
只会迁移未被其他包拥有的旧单元，并把原文件备份到
`/var/backups/usbdisplay-stack/legacy-usb-displayd.service`；若旧单元属于其他
软件包则拒绝继续，避免覆盖其他包的所有权。

Null 和 PPM 后端只用于诊断，不代表 HDMI 已经连通：

```bash
sudo sh ./install-usbdisplay-offline.sh --backend null --enable PACKAGE.deb
sudo sh ./install-usbdisplay-offline.sh \
  --backend ppm --ppm-output /tmp/usbdisplay.ppm --enable PACKAGE.deb
```

## 检测程序

默认检查必须同时满足以下条件才返回 `0`：

- `usbdisplay` 内核模块、流设备和同名 framebuffer 均存在；
- `/run/usbdisplay/ready` 格式有效，其中的 PID 仍是 `usb-displayd`；
- 后端声明 `physical=1`；
- USB 总线上仍存在 `185b:2d1d` 适配器。

```bash
usbdisplay-check
usbdisplay-check --wait 20
usbdisplay-check --json
```

退出码 `0` 表示就绪，`1` 表示未就绪，`2` 表示命令行错误。自动化程序应以
退出码为准，并可解析 `--json` 输出。仅检查 null/PPM 诊断链路时必须显式使用
`--allow-diagnostic`，防止把虚拟 framebuffer 误报成真实 HDMI 输出。

## 升级与卸载

直接再次运行离线安装脚本即可升级同一内核的包。dpkg 会保留本地修改过的
`/etc/default/usb-displayd` 和 `/etc/modprobe.d/usbdisplay.conf`；只要命令中
再次提供 `--backend`，安装脚本就会按显式参数重写它们。

```bash
sudo dpkg -r usbdisplay-stack
```

卸载会停止并禁用 `usb-displayd`，移除包管理的文件，但保留
`/var/lib/usbdisplay/actions-micro.replay`，避免无提示删除现场授权数据。
