# Launchpad PPA 发布

USBDisplayStack 的 Launchpad 目标为：

```text
ppa:maikebing/usbdisplaystack
```

PPA 不接受 GitHub Release 中已经编译完成的 `.deb`。GitHub Actions 会为每个
`vX.Y.Z` 标签生成并签名两套 Debian 源码包，再由 Launchpad builder 编译和发布：

| Ubuntu | 架构 | 内核目标 | Debian revision |
| --- | --- | --- | --- |
| 18.04 Bionic | i386 | `4.15.0-60-generic` | `X.Y.Z-1~bionic1` |
| 26.04 Resolute | amd64 | 构建时最新 `linux-headers-generic` | `X.Y.Z-1~resolute1` |

## 一次性配置

1. 登录 `maikebing` Launchpad 账户，导入专用于自动发布的 OpenPGP 公钥。
2. 创建名为 `usbdisplaystack` 的公开 PPA，并确认已启用 i386 和 amd64
   processor。
3. 在 GitHub 仓库的 `Settings -> Secrets and variables -> Actions` 中添加：

| 名称 | 类型 | 内容 |
| --- | --- | --- |
| `LAUNCHPAD_GPG_FINGERPRINT` | Repository secret | Launchpad 已登记的完整 GPG 指纹 |
| `LAUNCHPAD_GPG_PRIVATE_KEY` | Repository secret | 对应的 ASCII armored 私钥 |

建议生成只用于此仓库发布的独立签名密钥。当前 workflow 为完全无人值守发布，
因此该专用私钥不能带交互式口令；它只能保存在 GitHub Actions secret 中，不能
提交到仓库、Release artifact 或日志。

导出私钥时使用：

```bash
gpg --armor --export-secret-keys FULL_FINGERPRINT
```

将完整输出（包括 `BEGIN/END PGP PRIVATE KEY BLOCK`）保存为
`LAUNCHPAD_GPG_PRIVATE_KEY`。公开密钥必须先导入 Launchpad，否则上传会被拒绝。

## 自动发布

推送标签后，现有 `build` workflow 会同时：

- 构建并上传 GitHub Release 的内核绑定 `.deb`；
- 生成 Bionic i386 和 Resolute amd64 两个 `_source.changes`；
- 使用专用 GPG 密钥签名 `.dsc` 和 `_source.changes`；
- 执行 `dput ppa:maikebing/usbdisplaystack ...`；
- 保存签名后的源包为 GitHub Actions artifact，保留 30 天。

示例：

```bash
git tag -a v0.2.4 -m "v0.2.4"
git push origin v0.2.4
```

Launchpad 接收上传后会异步构建。通过以下地址查看接受、构建和发布状态：

```text
https://launchpad.net/~maikebing/+archive/ubuntu/usbdisplaystack
```

每个 PPA 源包版本只能上传一次。修复打包问题时必须增加 Debian revision，例如
从 `0.2.4-1~bionic1` 改为 `0.2.4-1~bionic2`，不能覆盖已经接受的版本。

## 发布 v0.2.4

把本次改动提交后，在该提交创建并推送 `v0.2.4` 标签。workflow 会从标签读取
源码和 `debian/` 元数据，确保程序闪屏、包内 `VERSION`、GitHub Release 和
Launchpad 源码包都使用 `0.2.4`，然后依次上传：

```text
usbdisplay-stack_0.2.4-1~bionic1_source.changes
usbdisplay-stack_0.2.4-1~resolute1_source.changes
```

## 用户安装

Launchpad 构建和发布完成后，Bionic i386 与 Resolute amd64 用户都执行：

```bash
sudo add-apt-repository ppa:maikebing/usbdisplaystack
sudo apt update
sudo apt install usbdisplay-stack
```

每个 PPA 二进制包仍然严格绑定其 `X-USBDisplay-Kernel` 记录的内核 ABI。
Bionic 包固定为 `4.15.0-60-generic`；Resolute 包使用 Launchpad 构建时安装的
最新 generic headers。安装脚本会拒绝在其他运行内核上配置软件包；安装完成后
也不会自动加载模块或启动服务。
