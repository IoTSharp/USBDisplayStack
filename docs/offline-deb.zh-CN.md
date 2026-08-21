# 单文件 Debian 包与离线安装

USBDisplayStack 的内核模块必须与目标机正在运行的内核完全匹配。因此每个
`.deb` 都绑定一个 `uname -r` 和一个 Debian 架构；安装脚本会在写入系统前
同时检查内核、架构和 SHA-256，避免把不兼容的模块装入车道机。

## 使用 PCCT 生成包

内核模块必须先使用目标内核的准确头文件构建。用户态程序和 DEB 由 PCCT
i386 镜像编译、打包；构建脚本通过 `docker cp` 复制源码，不会把当前仓库
bind mount 到容器中：

```powershell
pwsh ./scripts/build-pcct-deb.ps1 \
  -Version 0.2.4 \
  -KernelModule D:\path\to\usbdisplay.ko \
  -KernelRelease 4.15.0-60-generic \
  -Architecture i386
```

默认镜像为 `ghcr.io/iotsharp/pcct-build-x86:latest`。传入的
`usbdisplay.ko` 必须已经用同一套 `KERNEL_RELEASE` 头文件构建，打包脚本会
读取 `vermagic`，不会把其他版本的模块伪装成目标版本。

如果已经位于合适的 Linux/PCCT 容器内，也可以直接执行：

```bash
make userspace USBDISPLAY_VERSION=0.2.4
KERNEL_MODULE=/path/to/usbdisplay.ko \
KERNEL_RELEASE=4.15.0-60-generic DEB_ARCH=i386 \
  sh scripts/package-deb.sh 0.2.4 dist
```

`build-pcct-deb.ps1` 会把同一个 `-Version` 值注入 `usb-displayd` 内置 Splash，
因此设备画面中的版本号和 DEB 元数据保持一致，不需要长期硬编码。

输出目录包含：

- `usbdisplay-stack_<版本>+kernel.<内核>_<架构>.deb`；
- 同名 `.sha256` 校验文件；
- `install-usbdisplay-offline.sh` 离线安装脚本。

`.deb` 内含 `usbdisplay.ko`、`usb-displayd`、三个后端、诊断工具、udev 和
systemd 配置、`usbdisplay-check`，以及项目的全部安装/卸载脚本。它不包含
初始化 replay，也不直接复制某个发行版的 FFmpeg 动态库；DEB 通过标准
`Depends` 声明 `ffmpeg`，由 APT 选择与目标系统兼容的 `libx264` 和其他
运行库。`postinst` 会再次验证 FFmpeg 是否实际提供 `libx264` 编码器。

## 单 DEB 在线安装

目标机能够访问正确的 APT 软件源时，用户只需下载一个 `.deb`：

```bash
sudo apt install ./usbdisplay-stack_0.2.4+kernel.4.15.0-60-generic_i386.deb
```

APT 会安装 `ffmpeg`、`systemd`、`udev`、`kmod` 及其动态库依赖，再执行 DEB
内的 `preinst/postinst`。直接运行 `dpkg -i` 不会下载依赖，不应作为在线一键
安装命令。安装会刷新 `depmod`、udev 和 systemd，但不会加载模块、启动服务
或改动车道应用。

## 完全离线安装

完全离线时，目标机必须预先具备 DEB 声明的依赖。把 `.deb`、`.sha256` 和
构建输出的离线脚本放在同一目录后运行：

```bash
sudo sh ./install-usbdisplay-offline.sh \
  ./usbdisplay-stack_0.2.4+kernel.4.15.0-60-generic_i386.deb
```

离线脚本不会联网补依赖。若确实无法提供校验文件，可以显式使用
`--skip-checksum`；现场正常交付不应跳过校验。安装脚本也随 DEB 保存在
`/usr/share/usbdisplay/install-scripts/`，用于包内容审计和恢复场景。

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
  ./usbdisplay-stack_0.2.4+kernel.4.15.0-60-generic_i386.deb
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
- `/run/usbdisplay/ready` 格式有效，其中代数大于零且 PID 仍是 `usb-displayd`；
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
