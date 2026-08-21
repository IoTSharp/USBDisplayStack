#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
	printf 'Usage: %s VERSION [OUTPUT_DIR]\n' "$0" >&2
	exit 2
fi

version=$1
output_dir=${2:-dist}
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-$project_dir/build}
kernel_release=${KERNEL_RELEASE:-$(uname -r)}
kernel_module=${KERNEL_MODULE:-$project_dir/kernel/usbdisplay.ko}
package_architecture=${DEB_ARCH:-$(dpkg --print-architecture)}
package_name=usbdisplay-stack

if ! command -v dpkg-deb >/dev/null 2>&1 ||
   ! command -v dpkg >/dev/null 2>&1; then
	printf '%s\n' 'dpkg and dpkg-deb are required to build the package' >&2
	exit 1
fi

case "$version" in
	[0-9]*) ;;
	*)
		printf '%s\n' 'VERSION must start with a digit' >&2
		exit 2
		;;
esac
case "$version" in
	*[!0-9A-Za-z.+~-]*)
		printf '%s\n' 'VERSION is not a valid Debian version string' >&2
		exit 2
		;;
esac
if ! dpkg --compare-versions "$version" ge 0 2>/dev/null; then
	printf '%s\n' 'VERSION is not accepted by dpkg' >&2
	exit 2
fi

case "$kernel_release" in
	''|*[!0-9A-Za-z.+~-]*)
		printf '%s\n' 'KERNEL_RELEASE contains unsupported characters' >&2
		exit 2
		;;
esac
case "$package_architecture" in
	''|*[!0-9a-z-]*)
		printf '%s\n' 'DEB_ARCH contains unsupported characters' >&2
		exit 2
		;;
esac

for required_file in \
	"$build_dir/usb-displayd" \
	"$build_dir/fb-test-pattern" \
	"$build_dir/drm-probe" \
	"$build_dir/usbdisplay-null.so" \
	"$build_dir/usbdisplay-ppm.so" \
	"$build_dir/usbdisplay-actions-micro.so" \
	"$kernel_module" \
	"$project_dir/tools/usbdisplay-check"; do
	if [ ! -f "$required_file" ]; then
		printf 'Missing build artifact: %s\n' "$required_file" >&2
		exit 1
	fi
done

module_kernel=
if command -v modinfo >/dev/null 2>&1; then
	module_kernel=$(modinfo -F vermagic "$kernel_module" 2>/dev/null |
		awk 'NR == 1 { print $1 }')
fi
if [ -z "$module_kernel" ] && command -v strings >/dev/null 2>&1; then
	module_kernel=$(strings "$kernel_module" |
		sed -n 's/^vermagic=\([^ ][^ ]*\).*/\1/p' | head -n 1)
fi
[ -n "$module_kernel" ] || {
	printf 'Cannot read vermagic from kernel module: %s\n' "$kernel_module" >&2
	exit 1
}
[ "$module_kernel" = "$kernel_release" ] || {
	printf 'Kernel module vermagic %s does not match KERNEL_RELEASE %s.\n' \
		"$module_kernel" "$kernel_release" >&2
	exit 1
}

mkdir -p "$output_dir"
output_dir=$(CDPATH= cd -- "$output_dir" && pwd)
temporary_dir=$(mktemp -d)
package_root=$temporary_dir/root

cleanup()
{
	rm -rf -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

package_version=$version+kernel.$kernel_release
package_file=$output_dir/${package_name}_${package_version}_${package_architecture}.deb

install -d \
	"$package_root/DEBIAN" \
	"$package_root/etc/default" \
	"$package_root/etc/modprobe.d" \
	"$package_root/etc/modules-load.d" \
	"$package_root/etc/udev/rules.d" \
	"$package_root/lib/modules/$kernel_release/extra" \
	"$package_root/lib/systemd/system" \
	"$package_root/usr/bin" \
	"$package_root/usr/lib/usbdisplay" \
	"$package_root/usr/share/doc/$package_name/docs/assets" \
	"$package_root/usr/share/usbdisplay" \
	"$package_root/usr/share/usbdisplay/install-scripts" \
	"$package_root/var/lib/usbdisplay"

install -m 0644 "$kernel_module" \
	"$package_root/lib/modules/$kernel_release/extra/usbdisplay.ko"
install -m 0755 "$build_dir/usb-displayd" \
	"$package_root/usr/bin/usb-displayd"
install -m 0755 "$build_dir/fb-test-pattern" \
	"$package_root/usr/bin/usbdisplay-fb-test-pattern"
install -m 0755 "$build_dir/drm-probe" \
	"$package_root/usr/bin/usbdisplay-drm-probe"
install -m 0755 "$project_dir/tools/usbdisplay-check" \
	"$package_root/usr/bin/usbdisplay-check"
install -m 0755 "$build_dir/usbdisplay-null.so" \
	"$package_root/usr/lib/usbdisplay/usbdisplay-null.so"
install -m 0755 "$build_dir/usbdisplay-ppm.so" \
	"$package_root/usr/lib/usbdisplay/usbdisplay-ppm.so"
install -m 0755 "$build_dir/usbdisplay-actions-micro.so" \
	"$package_root/usr/lib/usbdisplay/usbdisplay-actions-micro.so"

install -m 0644 "$project_dir/packaging/usb-displayd.default" \
	"$package_root/etc/default/usb-displayd"
install -m 0644 "$project_dir/packaging/usbdisplay.conf" \
	"$package_root/etc/modprobe.d/usbdisplay.conf"
install -m 0644 "$project_dir/packaging/usbdisplay.modules-load.conf" \
	"$package_root/etc/modules-load.d/usbdisplay.conf"
install -m 0644 "$project_dir/packaging/60-usbdisplay.rules" \
	"$package_root/etc/udev/rules.d/60-usbdisplay.rules"
install -m 0644 "$project_dir/packaging/usb-displayd.service" \
	"$package_root/lib/systemd/system/usb-displayd.service"

install -m 0644 "$project_dir/README.md" "$project_dir/README.zh-CN.md" \
	"$project_dir/CHANGELOG.md" "$project_dir/docs/offline-deb.zh-CN.md" \
	"$package_root/usr/share/doc/$package_name/"
install -m 0644 "$project_dir/LICENSE" \
	"$package_root/usr/share/doc/$package_name/copyright"
install -m 0644 "$project_dir/docs/assets/usbdisplay-splash.png" \
	"$package_root/usr/share/doc/$package_name/docs/assets/usbdisplay-splash.png"
for install_script in install.sh install-prebuilt.sh install-offline-deb.sh \
		uninstall.sh; do
	install -m 0755 "$project_dir/scripts/$install_script" \
		"$package_root/usr/share/usbdisplay/install-scripts/$install_script"
done
printf '%s\n' "$kernel_release" > \
	"$package_root/usr/share/usbdisplay/KERNEL_RELEASE"
printf '%s\n' "$version" > "$package_root/usr/share/usbdisplay/VERSION"

installed_size=$(du -sk "$package_root" | awk '{print $1}')
cat > "$package_root/DEBIAN/control" <<EOF
Package: $package_name
Version: $package_version
Architecture: $package_architecture
Maintainer: USBDisplayStack maintainers <noreply@iotsharp.net>
Installed-Size: $installed_size
Depends: libc6 (>= 2.17), kmod, systemd, udev, ffmpeg
Section: kernel
Priority: optional
X-USBDisplay-Kernel: $kernel_release
Homepage: https://github.com/IoTSharp/USBDisplayStack
Description: USB display virtual DRM/fbdev stack
 Kernel module, frame transport daemon, Actions Micro USB-HDMI backend,
 diagnostic tools, installation scripts, and a physical-readiness checker.
 This package is tied to Linux kernel $kernel_release; install it with APT so
 FFmpeg and its runtime libraries are resolved automatically.
EOF

cat > "$package_root/DEBIAN/conffiles" <<'EOF'
/etc/default/usb-displayd
/etc/modprobe.d/usbdisplay.conf
EOF

cat > "$package_root/DEBIAN/preinst" <<EOF
#!/bin/sh
set -e
package_kernel='$kernel_release'
running_kernel=\$(uname -r)
case "\${1:-}" in
	install|upgrade)
		if [ "\$running_kernel" != "\$package_kernel" ]; then
			printf 'usbdisplay-stack: package kernel %s does not match running kernel %s.\n' \
				"\$package_kernel" "\$running_kernel" >&2
			exit 1
		fi
		;;
esac
exit 0
EOF

cat > "$package_root/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
kernel_release='$kernel_release'
if command -v depmod >/dev/null 2>&1; then
	depmod -a "\$kernel_release"
fi
if command -v systemctl >/dev/null 2>&1; then
	systemctl daemon-reload >/dev/null 2>&1 || true
fi
if command -v udevadm >/dev/null 2>&1; then
	udevadm control --reload-rules >/dev/null 2>&1 || true
fi
if ! command -v ffmpeg >/dev/null 2>&1 ||
   ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q '[[:space:]]libx264[[:space:]]'; then
	printf '%s\n' 'usbdisplay-stack: ffmpeg with the libx264 encoder is required.' >&2
	exit 1
fi
printf '%s\n' 'usbdisplay-stack installed; module and service were not started automatically.'
exit 0
EOF

cat > "$package_root/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
case "${1:-}" in
	remove|deconfigure)
		if command -v systemctl >/dev/null 2>&1; then
			systemctl disable --now usb-displayd.service >/dev/null 2>&1 || true
		fi
		;;
esac
exit 0
EOF

cat > "$package_root/DEBIAN/postrm" <<EOF
#!/bin/sh
set -e
kernel_release='$kernel_release'
if command -v depmod >/dev/null 2>&1; then
	depmod -a "\$kernel_release"
fi
if command -v systemctl >/dev/null 2>&1; then
	systemctl daemon-reload >/dev/null 2>&1 || true
fi
if command -v udevadm >/dev/null 2>&1; then
	udevadm control --reload-rules >/dev/null 2>&1 || true
fi
exit 0
EOF

chmod 0755 "$package_root/DEBIAN/preinst" \
	"$package_root/DEBIAN/postinst" \
	"$package_root/DEBIAN/prerm" \
	"$package_root/DEBIAN/postrm"

(
	cd "$package_root"
	find etc lib usr -type f -print | LC_ALL=C sort | xargs md5sum > DEBIAN/md5sums
)

if dpkg-deb --help 2>&1 | grep -q -- '--root-owner-group'; then
	dpkg-deb --build --root-owner-group "$package_root" "$package_file"
elif command -v fakeroot >/dev/null 2>&1; then
	fakeroot dpkg-deb --build "$package_root" "$package_file"
elif [ "$(id -u)" -eq 0 ]; then
	dpkg-deb --build "$package_root" "$package_file"
else
	printf '%s\n' \
		'dpkg-deb lacks --root-owner-group; install fakeroot or run as root' >&2
	exit 1
fi

(
	cd "$output_dir"
	sha256sum "${package_file##*/}" > "${package_file##*/}.sha256"
)
install -m 0755 "$project_dir/scripts/install-offline-deb.sh" \
	"$output_dir/install-usbdisplay-offline.sh"

printf '%s\n' "$package_file"
printf '%s\n' "$package_file.sha256"
printf '%s\n' "$output_dir/install-usbdisplay-offline.sh"
