#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

program=${0##*/}
backend=
template_file=
bootstrap_mode=full
ppm_output=/tmp/usbdisplay.ppm
display_width=1920
display_height=1080
enable_service=false
skip_checksum=false
wait_seconds=20
temporary_config=

cleanup()
{
	if [ -n "$temporary_config" ]; then
		rm -f -- "$temporary_config"
	fi
}
trap cleanup EXIT HUP INT TERM

usage()
{
	cat <<EOF
Usage: $program [OPTIONS] PACKAGE.deb

Install USBDisplayStack without using a network package source.

  --backend NAME       actions-micro, null, or ppm
  --template PATH      authorized Actions Micro replay template
  --bootstrap MODE     minimal or full (default: full)
  --ppm-output PATH    PPM diagnostic output (default: /tmp/usbdisplay.ppm)
  --width PIXELS       virtual display width (default: 1920)
  --height PIXELS      virtual display height (default: 1080)
  --enable             load the module, enable/restart the service, and check it
  --wait SECONDS       readiness timeout used with --enable (default: 20)
  --skip-checksum      install without the adjacent .sha256 file
  -h, --help           show this help

Examples:
  sudo ./$program ./usbdisplay-stack_VERSION_ARCH.deb
  sudo ./$program --backend actions-micro --template ./adapter.replay \\
    --enable ./usbdisplay-stack_VERSION_ARCH.deb
EOF
}

fail()
{
	printf '%s: %s\n' "$program" "$1" >&2
	exit 1
}

is_valid_width()
{
	case "$1" in
		''|*[!0-9]*|??????*) return 1 ;;
		*) [ "$1" -ge 1 ] && [ "$1" -le 4096 ] ;;
	esac
}

is_valid_height()
{
	case "$1" in
		''|*[!0-9]*|??????*) return 1 ;;
		*) [ "$1" -ge 1 ] && [ "$1" -le 2160 ] ;;
	esac
}

is_non_negative_integer()
{
	case "$1" in
		''|*[!0-9]*|??????*) return 1 ;;
		*) return 0 ;;
	esac
}

is_safe_backend_path()
{
	case "$1" in
		''|*[!0-9A-Za-z_./+~-]*) return 1 ;;
		*) return 0 ;;
	esac
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--backend)
			[ "$#" -ge 2 ] || { usage >&2; exit 2; }
			backend=$2
			shift 2
			;;
		--template)
			[ "$#" -ge 2 ] || { usage >&2; exit 2; }
			template_file=$2
			shift 2
			;;
		--bootstrap)
			[ "$#" -ge 2 ] || { usage >&2; exit 2; }
			bootstrap_mode=$2
			shift 2
			;;
		--ppm-output)
			[ "$#" -ge 2 ] || { usage >&2; exit 2; }
			ppm_output=$2
			shift 2
			;;
		--width)
			[ "$#" -ge 2 ] || { usage >&2; exit 2; }
			display_width=$2
			shift 2
			;;
		--height)
			[ "$#" -ge 2 ] || { usage >&2; exit 2; }
			display_height=$2
			shift 2
			;;
		--enable)
			enable_service=true
			shift
			;;
		--wait)
			[ "$#" -ge 2 ] || { usage >&2; exit 2; }
			wait_seconds=$2
			shift 2
			;;
		--skip-checksum)
			skip_checksum=true
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		--)
			shift
			break
			;;
		-*)
			printf '%s: unknown option: %s\n' "$program" "$1" >&2
			usage >&2
			exit 2
			;;
		*)
			break
			;;
	esac
done

[ "$#" -eq 1 ] || { usage >&2; exit 2; }
package_file=$1
[ -f "$package_file" ] || fail "package does not exist: $package_file"
is_valid_width "$display_width" || fail 'width must be 1..4096'
is_valid_height "$display_height" || fail 'height must be 1..2160'
is_non_negative_integer "$wait_seconds" ||
	fail 'wait time must be a non-negative integer of at most five digits'

case "$bootstrap_mode" in
	minimal|full) ;;
	*) fail 'bootstrap mode must be minimal or full' ;;
esac
case "$backend" in
	''|null|ppm|actions-micro) ;;
	*) fail 'backend must be actions-micro, null, or ppm' ;;
esac

if $enable_service && [ -z "$backend" ]; then
	fail '--enable requires an explicit --backend'
fi
if [ "$backend" = actions-micro ]; then
	[ -n "$template_file" ] || fail 'actions-micro requires --template'
	[ -f "$template_file" ] || fail "replay template does not exist: $template_file"
	template_dir=$(CDPATH= cd -- "$(dirname -- "$template_file")" && pwd)
	template_file=$template_dir/$(basename -- "$template_file")
	command -v ffmpeg >/dev/null 2>&1 ||
		fail 'actions-micro requires a locally installed ffmpeg executable'
	if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q '[[:space:]]libx264[[:space:]]'; then
		fail 'the installed ffmpeg does not provide the libx264 encoder'
	fi
elif [ -n "$template_file" ]; then
	fail '--template is valid only with --backend actions-micro'
fi

if [ "$backend" = ppm ] && ! is_safe_backend_path "$ppm_output"; then
	fail 'PPM output path contains unsupported characters'
fi

package_dir=$(CDPATH= cd -- "$(dirname -- "$package_file")" && pwd)
package_base=$(basename -- "$package_file")
package_file=$package_dir/$package_base
checksum_file=$package_file.sha256

if ! $skip_checksum; then
	[ -f "$checksum_file" ] ||
		fail "checksum file is missing: $checksum_file (or use --skip-checksum)"
	(
		cd "$package_dir"
		sha256sum -c "${checksum_file##*/}"
	) || fail 'package checksum verification failed'
fi

command -v dpkg >/dev/null 2>&1 || fail 'dpkg is not installed'
command -v dpkg-deb >/dev/null 2>&1 || fail 'dpkg-deb is not installed'
package_name=$(dpkg-deb -f "$package_file" Package)
[ "$package_name" = usbdisplay-stack ] ||
	fail "unexpected Debian package: $package_name"
package_kernel=$(dpkg-deb -f "$package_file" X-USBDisplay-Kernel)
[ -n "$package_kernel" ] || fail 'package does not declare X-USBDisplay-Kernel'
running_kernel=$(uname -r)
[ "$package_kernel" = "$running_kernel" ] ||
	fail "package kernel $package_kernel does not match running kernel $running_kernel"

package_architecture=$(dpkg-deb -f "$package_file" Architecture)
host_architecture=$(dpkg --print-architecture)
architecture_supported=false
if [ "$package_architecture" = all ] ||
   [ "$package_architecture" = "$host_architecture" ]; then
	architecture_supported=true
else
	for foreign_architecture in $(dpkg --print-foreign-architectures); do
		if [ "$package_architecture" = "$foreign_architecture" ]; then
			architecture_supported=true
			break
		fi
	done
fi
$architecture_supported ||
	fail "package architecture $package_architecture is not enabled on $host_architecture"

[ "$(id -u)" -eq 0 ] || fail 'installation must run as root'

dpkg -i "$package_file" ||
	fail 'dpkg installation failed; no network dependency fallback was attempted'

if [ -n "$backend" ]; then
	temporary_config=$(mktemp)
	case "$backend" in
		actions-micro)
			install -d -m 0750 /var/lib/usbdisplay
			install -m 0600 "$template_file" \
				/var/lib/usbdisplay/actions-micro.replay
			cat > "$temporary_config" <<EOF
# Generated by $program for the physical Actions Micro backend.
USBDISPLAY_BACKEND=/usr/lib/usbdisplay/usbdisplay-actions-micro.so
USBDISPLAY_BACKEND_ARGS=--backend-option template=/var/lib/usbdisplay/actions-micro.replay,bootstrap=$bootstrap_mode
EOF
			;;
		ppm)
			cat > "$temporary_config" <<EOF
# Generated by $program for diagnostic PPM output.
USBDISPLAY_BACKEND=/usr/lib/usbdisplay/usbdisplay-ppm.so
USBDISPLAY_BACKEND_ARGS=--backend-option $ppm_output
EOF
			;;
		null)
			cat > "$temporary_config" <<EOF
# Generated by $program for a diagnostic sink without physical output.
USBDISPLAY_BACKEND=/usr/lib/usbdisplay/usbdisplay-null.so
USBDISPLAY_BACKEND_ARGS=
EOF
			;;
	esac
	install -m 0644 "$temporary_config" /etc/default/usb-displayd
	printf 'options usbdisplay width=%s height=%s\n' \
		"$display_width" "$display_height" > "$temporary_config"
	install -m 0644 "$temporary_config" /etc/modprobe.d/usbdisplay.conf
fi

if $enable_service; then
	command -v modprobe >/dev/null 2>&1 || fail 'modprobe is not installed'
	command -v systemctl >/dev/null 2>&1 || fail 'systemctl is not installed'
	modprobe usbdisplay || fail 'failed to load usbdisplay kernel module'
	systemctl enable usb-displayd.service >/dev/null ||
		fail 'failed to enable usb-displayd.service'
	systemctl restart usb-displayd.service ||
		fail 'failed to start usb-displayd.service'

	check_option=
	if [ "$backend" != actions-micro ]; then
		check_option=--allow-diagnostic
	fi
	if ! /usr/bin/usbdisplay-check $check_option --wait "$wait_seconds"; then
		fail 'package was installed, but USBDisplayStack did not become ready'
	fi
fi

printf 'Installed %s for kernel %s.\n' "$package_name" "$package_kernel"
if ! $enable_service; then
	printf '%s\n' 'The module and service were not started; use --enable for explicit activation.'
fi
