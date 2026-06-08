#!/usr/bin/env bash
# Provision a target VM for kernel module debugging.
#
# Installs kernel headers (so the host build can sync them), optionally the
# matching vmlinux debug image + kernel source for step-into-kernel, and
# adds the boot args this lab needs to the guest's GRUB command line:
#   - nokaslr               (both methods; lets vmlinux symbols line up)
#   - kgdboc + sysrq        (kgdb method only)
#   - maxcpus               (only if DEBUG_MAXCPUS is set in the env)
#
# `--uninstall` removes every boot arg the lab added and leaves installed
# packages alone. Reboot the target after either mode for boot args to
# take effect — this script never reboots on its own.
set -euo pipefail

usage() {
	echo "usage: $0 <target> <kgdb|qemu> [--debug-symbols]" >&2
	echo "       $0 <target> --uninstall" >&2
	exit 2
}

target="${1:-}"
arg2="${2:-}"
arg3="${3:-}"

uninstall=0
debug_method=""
symbols=""
case "$arg2" in
	--uninstall)
		uninstall=1
		[[ -z "$arg3" ]] || usage
		;;
	kgdb|qemu)
		debug_method="$arg2"
		case "$arg3" in
			""|--debug-symbols) symbols="$arg3" ;;
			*) usage ;;
		esac
		;;
	*) usage ;;
esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/common.sh
source "$repo_root/scripts/lib/common.sh"
validate_target "$target" "usage: $0 <target> <kgdb|qemu> [--debug-symbols]"

env_file="$(lab_env_file "$repo_root")"
# shellcheck source=/dev/null
source "$env_file"
lab_load_target "$target"

target_os="$(target_cfg "$target" OS)"
kgdb_tty="$(target_cfg "$target" KGDB_TTY)"
kgdb_baud="$(target_cfg "$target" KGDB_BAUD)"
target_os="${target_os:-ubuntu}"
kgdb_tty="${kgdb_tty:-ttyS0}"
kgdb_baud="${kgdb_baud:-115200}"

case "$target_os" in
	ubuntu) ;;
	*) die "unsupported TARGET_OS[$target]='$target_os'; only ubuntu targets are implemented" ;;
esac

# Skip the sudo check — the user might be running 01 *to* configure sudo.
# The provisioning shell below will exercise it and fail with apt's own
# diagnostics if the password is wrong.
lab_check_connection --no-sudo

if (( uninstall )); then
	echo "uninstalling lab GRUB args from $target ($LAB_SSH_TARGET:$LAB_SSH_PORT)"
else
	symbols_blurb="${symbols:+, with debug symbols}"
	echo "provisioning $target ($LAB_SSH_TARGET:$LAB_SSH_PORT) for debug method: $debug_method$symbols_blurb"
fi

# We run the remote script as root via `sudo -A bash -s` so stdin stays a
# clean pipe of env-var-assignments + script body for bash to read. The
# straightforward "pipe password to sudo -S" pattern doesn't work here
# because sudo skips the stdin read whenever policy doesn't require auth
# (NOPASSWD, cached creds), and the password line then leaks into bash as
# its first command. SUDO_ASKPASS routes the password through a helper
# script so stdin is never contended.
askpass="$LAB_REMOTE_DIR/.kmod-askpass-$$"
pwfile="$LAB_REMOTE_DIR/.kmod-sudo-pw-$$"
trap 'lab_ssh "rm -f $askpass $pwfile 2>/dev/null" || true' EXIT

lab_ssh "mkdir -p '$LAB_REMOTE_DIR'; umask 077; cat > '$pwfile'" <<<"$LAB_SUDO_PASS"
lab_ssh "umask 077; cat > '$askpass'; chmod 700 '$askpass'" <<HELPER
#!/bin/sh
exec cat '$pwfile'
HELPER

{
	cat <<REMOTE_ENV
UNINSTALL='$uninstall'
TARGET_OS='$target_os'
DEBUG_METHOD='$debug_method'
KGDB_TTY='$kgdb_tty'
KGDB_BAUD='$kgdb_baud'
INSTALL_DEBUG_SYMBOLS='$symbols'
DEBUG_MAXCPUS='${DEBUG_MAXCPUS:-}'
REMOTE_ENV
	cat <<'REMOTE_SCRIPT'
set -euo pipefail

# Already root via `sudo -A bash -s` — no inner sudo needed.
apt_retry() {
	apt-get \
		-o Acquire::Retries=3 \
		-o Acquire::http::Timeout=20 \
		-o Acquire::https::Timeout=20 \
		"$@"
}

. /etc/os-release
kernel="$(uname -r)"
codename="${VERSION_CODENAME:-noble}"
[[ "${TARGET_OS:-ubuntu}" == "ubuntu" ]] ||
	{ echo "unsupported target OS: ${TARGET_OS:-}" >&2; exit 1; }
[[ "${ID:-}" == "ubuntu" ]] ||
	{ echo "target is not Ubuntu (os-release ID=$ID); only ubuntu targets are implemented" >&2; exit 1; }

grub_file=/etc/default/grub
# Lab-managed boot args; stripped on every run so reruns replace rather
# than append, and `--uninstall` cleans them all out.
grub_managed_keys='kgdboc=|maxcpus=|sysrq_always_enabled=|nokaslr'

read_current_cmdline() {
	local raw stripped
	raw="$(sed -n 's/^GRUB_CMDLINE_LINUX_DEFAULT="\{0,1\}\([^"]*\)"\{0,1\}/\1/p' "$grub_file" | head -1)"
	stripped="$(printf '%s' "$raw" | sed -E "s/(^| )($grub_managed_keys)([^ ]*)?/ /g; s/  */ /g; s/^ +//; s/ +$//")"
	printf '%s' "$stripped"
}

write_cmdline() {
	cp "$grub_file" "$grub_file.kmod-debug-lab.$(date +%Y%m%d%H%M%S).bak"
	if grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' "$grub_file"; then
		sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT=\"$1\"|" "$grub_file"
	else
		printf 'GRUB_CMDLINE_LINUX_DEFAULT="%s"\n' "$1" >> "$grub_file"
	fi
	update-grub
}

if [[ "${UNINSTALL:-0}" == "1" ]]; then
	echo "stripping lab boot args from $grub_file"
	current="$(read_current_cmdline)"
	write_cmdline "$current"
	echo
	echo "done. reboot to drop the lab boot args."
	echo "  boot args now: $current"
	exit 0
fi

echo "[1/3] installing kernel headers for running kernel $kernel"
apt_retry update
apt_retry install -y "linux-headers-$kernel" rsync
[[ -d "/lib/modules/$kernel/build" ]] ||
	{ echo "missing /lib/modules/$kernel/build after installing headers; check apt output above" >&2; exit 1; }

if [[ "$INSTALL_DEBUG_SYMBOLS" == "--debug-symbols" ]]; then
	echo "[2/3] installing vmlinux debug image + kernel source for step-into-kernel"
	vmlinux="/usr/lib/debug/boot/vmlinux-$kernel"
	if [[ ! -r "$vmlinux" ]]; then
		apt_retry install -y ubuntu-dbgsym-keyring
		cat > /etc/apt/sources.list.d/ddebs.list <<DDEBS
deb http://ddebs.ubuntu.com $codename main restricted universe multiverse
deb http://ddebs.ubuntu.com ${codename}-updates main restricted universe multiverse
DDEBS
		apt-get clean
		apt_retry update
		apt_retry install -y "linux-image-${kernel}-dbgsym" ||
			apt_retry install -y "linux-image-unsigned-${kernel}-dbgsym" ||
			{ echo "failed to install dbgsym for $kernel; retry later if ddebs.ubuntu.com is returning 503" >&2; exit 1; }
	fi
	[[ -r "$vmlinux" ]] || echo "warning: $vmlinux still missing after install; kernel source debugging will be unavailable"

	# Try the major.minor.patch-suffixed package first (linux-source-6.8.0),
	# falling back to the meta-package. We don't extract here — script 02
	# does it host-side to avoid needing tar on the target.
	short_kver="$(printf '%s' "$kernel" | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+' || true)"
	src_pkg=""
	for candidate in "linux-source-$short_kver" "linux-source"; do
		[[ -z "$candidate" || "$candidate" == "linux-source-" ]] && continue
		apt_retry install -y "$candidate" && { src_pkg="$candidate"; break; }
	done
	if [[ -z "$src_pkg" ]]; then
		echo "warning: could not install a linux-source package; step-into-kernel will only show disassembly" >&2
	elif ls -1 /usr/src/linux-source-*.tar.* /usr/src/linux-source-*/linux-source-*.tar.* 2>/dev/null | head -1 >/dev/null; then
		echo "       kernel source tarball is in /usr/src; script 02 will sync and extract it"
	else
		echo "warning: $src_pkg installed but no linux-source tarball found under /usr/src/" >&2
	fi
else
	echo "[2/3] skipping debug symbols (pass --debug-symbols to enable step-into-kernel)"
fi

echo "[3/3] updating GRUB command line"
current="$(read_current_cmdline)"

args=("nokaslr")
[[ "$DEBUG_METHOD" == "kgdb" ]] && args+=("kgdboc=${KGDB_TTY},${KGDB_BAUD}" "sysrq_always_enabled=1")
[[ -n "${DEBUG_MAXCPUS:-}" ]] && args+=("maxcpus=${DEBUG_MAXCPUS}")
for arg in "${args[@]}"; do
	case " $current " in *" $arg "*) ;; *) current="${current:+$current }$arg" ;; esac
done

write_cmdline "$current"

echo
echo "done. reboot the target VM before debugging."
echo "  boot args: $current"
REMOTE_SCRIPT
} | SSHPASS="$LAB_SSH_PASS" "${LAB_SSH_CMD[@]}" "$LAB_SSH_TARGET" "SUDO_ASKPASS='$askpass' sudo -A bash -s"

echo
if (( uninstall )); then
	echo "uninstall complete. reboot the $target VM to drop the lab boot args."
else
	echo "provisioning complete. reboot the $target VM, then:"
	echo "next: scripts/02-setup-host-build.sh $target"
fi
