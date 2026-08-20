#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf '%s\n' "uninstall.sh must run as root" >&2
	exit 1
fi

kernel_release=${KERNEL_RELEASE:-$(uname -r)}

if command -v systemctl >/dev/null 2>&1; then
	systemctl disable --now usb-displayd.service 2>/dev/null || true
fi
if grep -q '^usbdisplay ' /proc/modules; then
	modprobe -r usbdisplay
fi

rm -f \
	/usr/bin/usb-displayd \
	/usr/bin/usbdisplay-fb-test-pattern \
	/usr/bin/usbdisplay-drm-probe \
	/usr/bin/usbdisplay-check \
	/usr/lib/usbdisplay/usbdisplay-null.so \
	/usr/lib/usbdisplay/usbdisplay-ppm.so \
	/usr/lib/usbdisplay/usbdisplay-actions-micro.so \
	/usr/lib/systemd/system/usb-displayd.service \
	/etc/udev/rules.d/60-usbdisplay.rules \
	/etc/modules-load.d/usbdisplay.conf \
	"/lib/modules/$kernel_release/extra/usbdisplay.ko"

rmdir /usr/lib/usbdisplay 2>/dev/null || true
depmod -a "$kernel_release"
if command -v systemctl >/dev/null 2>&1; then
	systemctl daemon-reload
fi
if command -v udevadm >/dev/null 2>&1; then
	udevadm control --reload-rules
fi

printf '%s\n' 'Removed USBDisplayStack binaries and service files.'
printf '%s\n' 'Local settings in /etc/default and /etc/modprobe.d were preserved.'
