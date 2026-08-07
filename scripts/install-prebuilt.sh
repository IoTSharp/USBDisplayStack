#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

if [ "$(id -u)" -ne 0 ]; then
	printf '%s\n' "install-prebuilt.sh must run as root" >&2
	exit 1
fi

package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
package_kernel=$(cat "$package_dir/KERNEL_RELEASE")
running_kernel=$(uname -r)

if [ "$package_kernel" != "$running_kernel" ]; then
	printf 'Package kernel %s does not match running kernel %s.\n' \
		"$package_kernel" "$running_kernel" >&2
	exit 1
fi

install -d "/lib/modules/$running_kernel/extra" /usr/bin \
	/usr/lib/usbdisplay /usr/lib/systemd/system /etc/udev/rules.d \
	/etc/modules-load.d /etc/modprobe.d /etc/default
install -m 0644 "$package_dir/module/usbdisplay.ko" \
	"/lib/modules/$running_kernel/extra/usbdisplay.ko"
install -m 0755 "$package_dir/bin/usb-displayd" /usr/bin/usb-displayd
install -m 0755 "$package_dir/bin/fb-test-pattern" \
	/usr/bin/usbdisplay-fb-test-pattern
install -m 0755 "$package_dir/bin/drm-probe" \
	/usr/bin/usbdisplay-drm-probe
install -m 0755 "$package_dir/lib/usbdisplay-null.so" \
	/usr/lib/usbdisplay/usbdisplay-null.so
install -m 0755 "$package_dir/lib/usbdisplay-ppm.so" \
	/usr/lib/usbdisplay/usbdisplay-ppm.so
install -m 0755 "$package_dir/lib/usbdisplay-actions-micro.so" \
	/usr/lib/usbdisplay/usbdisplay-actions-micro.so
install -m 0644 "$package_dir/packaging/usb-displayd.service" \
	/usr/lib/systemd/system/usb-displayd.service
install -m 0644 "$package_dir/packaging/60-usbdisplay.rules" \
	/etc/udev/rules.d/60-usbdisplay.rules
install -m 0644 "$package_dir/packaging/usbdisplay.modules-load.conf" \
	/etc/modules-load.d/usbdisplay.conf

if [ ! -e /etc/modprobe.d/usbdisplay.conf ]; then
	install -m 0644 "$package_dir/packaging/usbdisplay.conf" \
		/etc/modprobe.d/usbdisplay.conf
fi
if [ ! -e /etc/default/usb-displayd ]; then
	install -m 0644 "$package_dir/packaging/usb-displayd.default" \
		/etc/default/usb-displayd
fi

depmod -a "$running_kernel"
if command -v systemctl >/dev/null 2>&1; then
	systemctl daemon-reload
fi
if command -v udevadm >/dev/null 2>&1; then
	udevadm control --reload-rules
fi

printf 'Installed USBDisplayStack for kernel %s.\n' "$running_kernel"
printf '%s\n' 'The module and service were not started automatically.'
