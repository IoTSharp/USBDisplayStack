#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
loader=$project_dir/tools/usbdisplay-load
service=$project_dir/packaging/usb-displayd.service
modules=$project_dir/packaging/usbdisplay.modules-load.conf

[ -f "$loader" ]

# The loader is deliberately one-shot. systemd bounds each failed start with
# TimeoutStartSec and retries the service, so the helper must not poll.
if grep -Eq '(^|[[:space:]])(while|for)[[:space:]]' "$loader"; then
	printf '%s\n' 'usbdisplay-load must not contain an unbounded polling loop' >&2
	exit 1
fi
for required in \
	'fb0/name' \
	'fb1/name' \
	'/dev/fb1' \
	'/dev/usbdisplay0' \
	'modprobe usbdisplay' \
	'/sys/bus/platform/drivers_probe' \
	'efi-framebuffer' \
	'simple-framebuffer' \
	'vesa'; do
	grep -Fq "$required" "$loader"
done

grep -Fq 'TimeoutStartSec=10s' "$service"
grep -Fq 'StartLimitIntervalSec=0' "$service"
grep -Fq 'ExecStartPre=/usr/bin/usbdisplay-load' "$service"
grep -Fq 'RestartSec=2' "$service"
if grep -Fxq 'usbdisplay' "$modules"; then
	printf '%s\n' 'usbdisplay must not be loaded by modules-load.d' >&2
	exit 1
fi

printf '%s\n' 'usbdisplay-load contract tests passed'
