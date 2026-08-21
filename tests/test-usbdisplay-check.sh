#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
checker=$project_dir/tools/usbdisplay-check
test_root=$(mktemp -d)

cleanup()
{
	rm -rf -- "$test_root"
}
trap cleanup EXIT HUP INT TERM

run_check()
{
	USBDISPLAY_SYSFS_ROOT=$test_root/sys \
	USBDISPLAY_PROC_ROOT=$test_root/proc \
	USBDISPLAY_DEV_ROOT=$test_root/dev \
	USBDISPLAY_READY_FILE=$test_root/run/ready \
		sh "$checker" "$@"
}

expect_status()
{
	expected_status=$1
	shift
	actual_status=0
	run_check --quiet "$@" >/dev/null 2>&1 || actual_status=$?
	if [ "$actual_status" -ne "$expected_status" ]; then
		printf 'expected status %s, got %s for: %s\n' \
			"$expected_status" "$actual_status" "$*" >&2
		exit 1
	fi
}

mkdir -p "$test_root/sys" "$test_root/proc" "$test_root/dev" "$test_root/run"
expect_status 1
expect_status 2 --wait 100000

mkdir -p \
	"$test_root/sys/module/usbdisplay" \
	"$test_root/sys/class/graphics/fb1" \
	"$test_root/proc/321"
ln -s /dev/null "$test_root/dev/usbdisplay0"
ln -s /dev/null "$test_root/dev/fb1"
printf '%s\n' usbdisplay > "$test_root/sys/class/graphics/fb1/name"
printf '%s\n' usb-displayd > "$test_root/proc/321/comm"
cat > "$test_root/run/ready" <<'EOF'
pid=321
generation=1
backend=null
physical=0
EOF

expect_status 1
expect_status 0 --allow-diagnostic

cat > "$test_root/run/ready" <<'EOF'
pid=321
generation=1
backend=actions-micro
physical=1
EOF
expect_status 1

mkdir -p "$test_root/sys/bus/usb/devices/1-2"
printf '%s\n' 185b > "$test_root/sys/bus/usb/devices/1-2/idVendor"
printf '%s\n' 2D1D > "$test_root/sys/bus/usb/devices/1-2/idProduct"
expect_status 0

json_result=$(run_check --json)
printf '%s\n' "$json_result" | grep -q '"status":"physical-ready"'
printf '%s\n' "$json_result" | grep -q '"usb_device":"1-2"'
printf '%s\n' "$json_result" | grep -q '"generation":"1"'

cat > "$test_root/run/ready" <<'EOF'
pid=321
generation=1
backend=invalid backend
physical=1
EOF
expect_status 1

cat > "$test_root/run/ready" <<'EOF'
pid=321
generation=1
backend=actions-micro
physical=1
EOF
printf '%s\n' unrelated-process > "$test_root/proc/321/comm"
expect_status 1

printf '%s\n' usb-displayd > "$test_root/proc/321/comm"
cat > "$test_root/run/ready" <<'EOF'
pid=321
backend=actions-micro
physical=1
EOF
expect_status 1

printf '%s\n' 'usbdisplay-check tests passed'
