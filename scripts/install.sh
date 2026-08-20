#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf '%s\n' "install.sh must run as root" >&2
	exit 1
fi

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel_release=${KERNEL_RELEASE:-$(uname -r)}
kernel_build_dir=${KDIR:-/lib/modules/$kernel_release/build}
module_dir=/lib/modules/$kernel_release/extra

if [ ! -x "$project_dir/build/usb-displayd" ] ||
   [ ! -f "$project_dir/build/usbdisplay-actions-micro.so" ] ||
   [ ! -f "$project_dir/kernel/usbdisplay.ko" ]; then
	make -C "$project_dir" userspace
	make -C "$project_dir" module KDIR="$kernel_build_dir"
fi

install -d "$module_dir"
install -m 0644 "$project_dir/kernel/usbdisplay.ko" \
	"$module_dir/usbdisplay.ko"
install -d /usr/bin /usr/lib/usbdisplay
install -m 0755 "$project_dir/build/usb-displayd" /usr/bin/usb-displayd
install -m 0755 "$project_dir/build/fb-test-pattern" \
	/usr/bin/usbdisplay-fb-test-pattern
install -m 0755 "$project_dir/build/drm-probe" \
	/usr/bin/usbdisplay-drm-probe
install -m 0755 "$project_dir/tools/usbdisplay-check" \
	/usr/bin/usbdisplay-check
install -m 0755 "$project_dir/build/usbdisplay-null.so" \
	/usr/lib/usbdisplay/usbdisplay-null.so
install -m 0755 "$project_dir/build/usbdisplay-ppm.so" \
	/usr/lib/usbdisplay/usbdisplay-ppm.so
install -m 0755 "$project_dir/build/usbdisplay-actions-micro.so" \
	/usr/lib/usbdisplay/usbdisplay-actions-micro.so

install -d /usr/lib/systemd/system /etc/udev/rules.d /etc/modules-load.d \
	/etc/modprobe.d /etc/default
install -m 0644 "$project_dir/packaging/usb-displayd.service" \
	/usr/lib/systemd/system/usb-displayd.service
install -m 0644 "$project_dir/packaging/60-usbdisplay.rules" \
	/etc/udev/rules.d/60-usbdisplay.rules
install -m 0644 "$project_dir/packaging/usbdisplay.modules-load.conf" \
	/etc/modules-load.d/usbdisplay.conf

if [ ! -e /etc/modprobe.d/usbdisplay.conf ]; then
	install -m 0644 "$project_dir/packaging/usbdisplay.conf" \
		/etc/modprobe.d/usbdisplay.conf
fi
if [ ! -e /etc/default/usb-displayd ]; then
	install -m 0644 "$project_dir/packaging/usb-displayd.default" \
		/etc/default/usb-displayd
fi

depmod -a "$kernel_release"
if command -v systemctl >/dev/null 2>&1; then
	systemctl daemon-reload
fi
if command -v udevadm >/dev/null 2>&1; then
	udevadm control --reload-rules
fi

printf 'Installed USBDisplayStack for kernel %s.\n' "$kernel_release"
printf '%s\n' 'Review /etc/modprobe.d/usbdisplay.conf and /etc/default/usb-displayd.'
printf '%s\n' 'The module and service were not started automatically.'
