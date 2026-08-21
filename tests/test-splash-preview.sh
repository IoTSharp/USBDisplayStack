#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
preview=$(mktemp)
expected_version=${USBDISPLAY_EXPECTED_VERSION:-development}

cleanup()
{
	rm -f -- "$preview"
}
trap cleanup EXIT HUP INT TERM

"$project_dir/build/splash-preview" "$preview"
[ "$(sed -n '1p' "$preview")" = P6 ]
[ "$(sed -n '2p' "$preview")" = '960 540' ]
[ "$(sed -n '3p' "$preview")" = 255 ]
[ "$(wc -c < "$preview")" -gt 1555200 ]
od -v -An -tx1 "$preview" | grep -q '3a c7 f2'
od -v -An -tx1 "$preview" | grep -q '6b d1 46'
od -v -An -tx1 "$preview" | grep -q 'f4 f7 f8'
strings "$project_dir/build/splash-preview" | grep -q '^QQ:100860505$'
strings "$project_dir/build/splash-preview" |
	grep -Fqx "VERSION $expected_version"
strings "$project_dir/build/usb-displayd" |
	grep -Fqx "VERSION $expected_version"

printf '%s\n' 'splash preview tests passed'
