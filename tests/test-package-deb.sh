#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d)
build_dir=$test_root/build
output_dir=$test_root/dist
kernel_release=4.15.0-usbdisplay-test
package_version=0.2.0

cleanup()
{
	rm -rf -- "$test_root"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$build_dir"
for artifact in \
	usb-displayd \
	fb-test-pattern \
	drm-probe \
	usbdisplay-null.so \
	usbdisplay-ppm.so \
	usbdisplay-actions-micro.so; do
	printf '#!/bin/sh\nexit 0\n' > "$build_dir/$artifact"
	chmod 0755 "$build_dir/$artifact"
done
printf 'test kernel module\nvermagic=%s SMP mod_unload\n' "$kernel_release" > \
	"$test_root/usbdisplay.ko"

if BUILD_DIR=$build_dir \
   KERNEL_MODULE=$test_root/usbdisplay.ko \
   KERNEL_RELEASE=$kernel_release \
   DEB_ARCH=amd64 \
	sh "$project_dir/scripts/package-deb.sh" invalid_version \
	"$output_dir" >/dev/null 2>&1; then
	printf '%s\n' 'package builder accepted an invalid Debian version' >&2
	exit 1
fi

if BUILD_DIR=$build_dir \
   KERNEL_MODULE=$test_root/usbdisplay.ko \
   KERNEL_RELEASE=4.15.0-wrong-kernel \
   DEB_ARCH=amd64 \
	sh "$project_dir/scripts/package-deb.sh" "$package_version" \
	"$output_dir" >/dev/null 2>&1; then
	printf '%s\n' 'package builder accepted mismatched module vermagic' >&2
	exit 1
fi

BUILD_DIR=$build_dir \
KERNEL_MODULE=$test_root/usbdisplay.ko \
KERNEL_RELEASE=$kernel_release \
DEB_ARCH=amd64 \
	sh "$project_dir/scripts/package-deb.sh" "$package_version" \
	"$output_dir" >/dev/null

package_file=$output_dir/usbdisplay-stack_${package_version}+kernel.${kernel_release}_amd64.deb
package_contents=$test_root/package-contents.txt
[ -f "$package_file" ]
[ -f "$package_file.sha256" ]
[ -x "$output_dir/install-usbdisplay-offline.sh" ]

[ "$(dpkg-deb -f "$package_file" Package)" = usbdisplay-stack ]
[ "$(dpkg-deb -f "$package_file" Architecture)" = amd64 ]
[ "$(dpkg-deb -f "$package_file" X-USBDisplay-Kernel)" = "$kernel_release" ]
depends=$(dpkg-deb -f "$package_file" Depends)
for dependency in kmod systemd udev ffmpeg; do
	printf '%s\n' "$depends" | grep -q "\\b$dependency\\b"
done
dpkg-deb -c "$package_file" > "$package_contents"
grep -q "./lib/modules/$kernel_release/extra/usbdisplay.ko" \
	"$package_contents"
grep -q './usr/bin/usbdisplay-check' "$package_contents"
grep -q './lib/systemd/system/usb-displayd.service' "$package_contents"
grep -q './usr/share/doc/usbdisplay-stack/docs/assets/usbdisplay-splash.png' \
	"$package_contents"
for install_script in install.sh install-prebuilt.sh install-offline-deb.sh \
		uninstall.sh; do
	grep -q "./usr/share/usbdisplay/install-scripts/$install_script" \
		"$package_contents"
done
(
	cd "$output_dir"
	sha256sum -c "${package_file##*/}.sha256" >/dev/null
)

printf '%s\n' 'offline Debian package tests passed'
