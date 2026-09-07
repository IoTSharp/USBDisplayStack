#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=${CC:-cc}
temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/usbdisplay-actions-session.XXXXXX")
binary=$temp_dir/test-actions-session

cleanup()
{
	rm -rf -- "$temp_dir"
}
trap cleanup EXIT HUP INT TERM

compiler_path=$(command -v "$compiler")
[ -x "$compiler_path" ]
"$compiler_path" -I"$project_dir/include" \
	-std=gnu11 -Wall -Wextra -Werror -finput-charset=GBK \
	"$project_dir/tests/test-actions-session.c" -o "$binary"
timeout 20s "$binary"
