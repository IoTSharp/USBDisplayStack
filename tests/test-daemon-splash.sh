#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=${CC:-cc}
temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/usbdisplay-daemon-splash.XXXXXX")
binary=$temp_dir/test-daemon-splash

cleanup()
{
	rm -rf -- "$temp_dir"
}
trap cleanup EXIT HUP INT TERM

compiler_path=$(command -v "$compiler")
[ -x "$compiler_path" ]
"$compiler_path" -I"$project_dir/include" -I"$project_dir/userspace" \
	-std=gnu11 -Wall -Wextra -Werror -finput-charset=GBK \
	-DUSBDISPLAY_VERSION='"runtime-test"' \
	"$project_dir/tests/test-daemon-splash.c" \
	"$project_dir/userspace/splash.c" -o "$binary" -ldl -lrt
timeout 10s "$binary"
