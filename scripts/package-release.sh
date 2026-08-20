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
kernel_release=$(uname -r)
architecture=$(uname -m)
package_name=usbdisplay-stack-$version-linux-$architecture-$kernel_release
temporary_dir=$(mktemp -d)
package_dir=$temporary_dir/$package_name

cleanup()
{
	rm -rf -- "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

case "$version" in
	*[!A-Za-z0-9._-]*|'')
		printf '%s\n' 'VERSION contains unsupported characters' >&2
		exit 2
		;;
esac

for required_file in \
	"$build_dir/usb-displayd" \
	"$build_dir/fb-test-pattern" \
	"$build_dir/drm-probe" \
	"$build_dir/actions-micro-replay" \
	"$build_dir/usbdisplay-null.so" \
	"$build_dir/usbdisplay-ppm.so" \
	"$build_dir/usbdisplay-actions-micro.so" \
	"$project_dir/kernel/usbdisplay.ko"; do
	if [ ! -f "$required_file" ]; then
		printf 'Missing build artifact: %s\n' "$required_file" >&2
		exit 1
	fi
done

mkdir -p "$package_dir/bin" "$package_dir/experimental" \
	"$package_dir/lib" "$package_dir/module" "$package_dir/packaging" \
	"$package_dir/docs" "$package_dir/examples/lvgl/common" \
	"$package_dir/examples/lvgl/fbdev" "$package_dir/examples/lvgl/drm" \
	"$package_dir/examples/csharp/Fbdev" \
	"$package_dir/examples/csharp/Drm" "$output_dir"
install -m 0755 "$build_dir/usb-displayd" "$package_dir/bin/"
install -m 0755 "$build_dir/fb-test-pattern" "$package_dir/bin/"
install -m 0755 "$build_dir/drm-probe" "$package_dir/bin/"
install -m 0755 "$project_dir/tools/usbdisplay-check" \
	"$package_dir/bin/"
install -m 0755 "$build_dir/actions-micro-replay" \
	"$package_dir/experimental/"
install -m 0755 "$build_dir/usbdisplay-null.so" "$package_dir/lib/"
install -m 0755 "$build_dir/usbdisplay-ppm.so" "$package_dir/lib/"
install -m 0755 "$build_dir/usbdisplay-actions-micro.so" \
	"$package_dir/lib/"
install -m 0644 "$project_dir/kernel/usbdisplay.ko" "$package_dir/module/"
install -m 0755 "$project_dir/scripts/install-prebuilt.sh" "$package_dir/"
for packaging_file in \
	60-usbdisplay.rules \
	usb-displayd.default \
	usb-displayd.service \
	usbdisplay.conf \
	usbdisplay.modules-load.conf; do
	install -m 0644 "$project_dir/packaging/$packaging_file" \
		"$package_dir/packaging/"
done
install -m 0644 "$project_dir"/docs/*.md "$package_dir/docs/"
install -m 0644 "$project_dir/examples/lvgl/Makefile" \
	"$project_dir/examples/lvgl/README.md" "$package_dir/examples/lvgl/"
install -m 0644 "$project_dir/examples/lvgl/common/demo_ui.h" \
	"$package_dir/examples/lvgl/common/"
install -m 0644 "$project_dir/examples/lvgl/fbdev/main.c" \
	"$package_dir/examples/lvgl/fbdev/"
install -m 0644 "$project_dir/examples/lvgl/drm/main.c" \
	"$package_dir/examples/lvgl/drm/"
install -m 0644 "$project_dir/examples/csharp/README.md" \
	"$package_dir/examples/csharp/"
install -m 0644 "$project_dir/examples/csharp/Fbdev/FbdevExample.csproj" \
	"$project_dir/examples/csharp/Fbdev/Program.cs" \
	"$package_dir/examples/csharp/Fbdev/"
install -m 0644 "$project_dir/examples/csharp/Drm/DrmExample.csproj" \
	"$project_dir/examples/csharp/Drm/Program.cs" \
	"$package_dir/examples/csharp/Drm/"
install -m 0644 "$project_dir/README.md" "$project_dir/README.zh-CN.md" \
	"$project_dir/LICENSE" "$project_dir/CHANGELOG.md" "$package_dir/"
printf '%s\n' "$kernel_release" > "$package_dir/KERNEL_RELEASE"
printf '%s\n' "$version" > "$package_dir/VERSION"

archive_path=$output_dir/$package_name.tar.gz
tar -C "$temporary_dir" -czf "$archive_path" "$package_name"
(
	cd "$output_dir"
	sha256sum "$package_name.tar.gz" > "$package_name.tar.gz.sha256"
)
printf '%s\n' "$archive_path"
printf '%s\n' "$archive_path.sha256"
