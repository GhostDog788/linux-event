#!/usr/bin/env bash
# Deploy the built module to a target, load it, and write the GDB init files
# VS Code (or `gdb -tui`) needs to attach and source-debug it.
#
# Phases:
#   1. Best-effort kill of any local gdb holding the debug-endpoint TCP
#      port (KGDB serial bridge and QEMU gdbstub each serve one client).
#   2. Upload module/<name>.ko, verify size matches.
#   3. Insmod; poll /sys/module/<name>/sections/ for runtime addresses,
#      stage a section-relocated copy of the .ko, and add-symbol-file it.
#   4. Emit .gdb/<target>-<method>.gdb (+ -attached variant) and the
#      current-* symlinks the IDE launch configs point at.
#
# Env knobs:
#   DEBUG_LOAD_DELAY_MS (5000)  passed to the module if it declares debug_delay_ms
#   INSMOD_WAIT_SECS    (30)    poll deadline for /sys/module/.../sections/.text
set -euo pipefail

usage() {
	echo "usage: $0 <target> <kgdb|qemu>" >&2
	exit 2
}

target="${1:-}"
debug_method="${2:-}"
case "$debug_method" in kgdb|qemu) ;; *) usage ;; esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/common.sh
source "$repo_root/scripts/lib/common.sh"
validate_target "$target" "usage: $0 <target> <kgdb|qemu>"

env_file="$(lab_env_file "$repo_root")"
# shellcheck source=/dev/null
source "$env_file"
lab_load_target "$target"

debug_endpoint="$(target_require_cfg "$target" "DEBUG_ENDPOINT_${debug_method^^}")"
debug_delay_ms="${DEBUG_LOAD_DELAY_MS:-5000}"
insmod_wait_secs="${INSMOD_WAIT_SECS:-30}"

# --- Locate artifact + vmlinux, verify target is on the cached kernel ----

cache_dir="$repo_root/kernel-cache/$target"
kernel_file="$cache_dir/kernel.release"
vmlinux="$cache_dir/vmlinux"

[[ -f "$kernel_file" ]] ||
	die "missing kernel cache for $target; run: scripts/02-setup-host-build.sh $target"
[[ -f "$vmlinux" ]] ||
	die "missing $vmlinux — source debugging needs the matching vmlinux from the target.
- install + sync it with:
    scripts/01-provision-target.sh $target $debug_method --debug-symbols
    scripts/02-setup-host-build.sh $target"

kernel="$(<"$kernel_file")"

lab_check_connection
live_kernel="$(lab_ssh 'uname -r')"
if [[ "$live_kernel" != "$kernel" ]]; then
	die "kernel mismatch: target now runs '$live_kernel' but cache has '$kernel'.
- if the target rebooted into a new kernel, re-sync:
    scripts/01-provision-target.sh $target $debug_method --debug-symbols   # for a new vmlinux
    scripts/02-setup-host-build.sh $target
- or reboot the target back into $kernel"
fi

artifact_dir="$repo_root/build/artifacts/$target/$kernel"
intermediate_dir="$repo_root/build/intermediate/$target/$kernel"
module_dir="$repo_root/module"

mapfile -t artifacts < <(find "$artifact_dir" -maxdepth 1 -name '*.ko' -type f | sort)
case ${#artifacts[@]} in
	0) die "no .ko under $artifact_dir; run: scripts/03-build-module.sh $target" ;;
	1) ;;
	*)
		echo "expected one .ko under $artifact_dir; found:" >&2
		printf '  %s\n' "${artifacts[@]}" >&2
		die "the lab assumes a single obj-m per build. Merge into one module (obj-m += foo.o; foo-y := a.o b.o ...) or split into separate module/ trees."
		;;
esac
artifact="${artifacts[0]}"
module_name="$(basename "$artifact" .ko)"

# --- Output paths ---------------------------------------------------------

gdb_dir="$repo_root/.gdb"
mkdir -p "$gdb_dir"

symbols_file="$gdb_dir/$target-$debug_method-module-symbols.gdb"
gdb_file="$gdb_dir/$target-$debug_method.gdb"
gdb_attached_file="$gdb_dir/$target-$debug_method-attached.gdb"
loader_log="$gdb_dir/$target-$debug_method-loader.log"

rm -f "$symbols_file" "$loader_log"

# --- Free the debug endpoint from stale GDB clients ----------------------
#
# Best-effort: lsof to find sockets on the endpoint, ps to confirm comm=gdb,
# kill -9. Errors here never fail the deploy.
free_debug_endpoint() {
	command -v lsof >/dev/null 2>&1 || return 0
	local host="${debug_endpoint%:*}" port="${debug_endpoint##*:}"
	local pids
	pids="$(lsof -ti "@$host:$port" 2>/dev/null || true)"
	local gdb_pids=() pid
	for pid in $pids; do
		[[ "$(ps -p "$pid" -o comm= 2>/dev/null || true)" == "gdb" ]] && gdb_pids+=("$pid")
	done
	if [[ ${#gdb_pids[@]} -gt 0 ]]; then
		echo "killing leftover gdb clients on $debug_endpoint:"
		ps -p "${gdb_pids[@]}" -o pid,cmd 2>/dev/null | tail -n +2 | sed 's/^/  /' || true
		kill -9 "${gdb_pids[@]}" 2>/dev/null || true
		sleep 0.3
	fi
}
free_debug_endpoint || true

# --- Upload + load -------------------------------------------------------

remote_module="$LAB_REMOTE_DIR/$module_name.ko"

echo "uploading $artifact -> $LAB_SSH_TARGET:$remote_module"
lab_ssh "mkdir -p '$LAB_REMOTE_DIR'"
lab_scp_to "$artifact" "$remote_module"
# Verify size — a truncated .ko produces cryptic "invalid module format" errors.
local_size="$(stat -c %s "$artifact")"
remote_size="$(lab_ssh "stat -c %s '$remote_module' 2>/dev/null" | tr -d '[:space:]')"
[[ "$remote_size" == "$local_size" ]] ||
	die "upload size mismatch: local $local_size bytes, remote ${remote_size:-missing} bytes; retry scripts/04 or check disk space on the target"

# Background insmod on the target so we can poll for sections in parallel
# with the module's debug_delay_ms sleep.
#
# Three load-bearing details:
#   - `<&0` keeps sudo's stdin attached to the SSH-inherited pipe (where
#     the password arrives). Non-job-control bash auto-redirects async
#     stdin to /dev/null without an explicit redirection — sudo would
#     see EOF and silently fail to authenticate.
#   - `& sleep 1` keeps the SSH session open long enough for sudo to
#     authenticate and exec nohup before the channel closes.
#   - `nohup` + non-interactive bash → the orphaned chain survives
#     session exit (bash doesn't huponexit non-interactively).
#
# `|| insmod ...` falls back when the module doesn't declare a
# debug_delay_ms parameter (kv arg would otherwise reject with EINVAL).
load_and_discover_symbols() {
	echo "loading $module_name on $target (debug_delay_ms=$debug_delay_ms)"
	lab_ssh_sudo "rmmod '$module_name' >/dev/null 2>&1 || true"
	lab_ssh_sudo "nohup sh -c 'insmod \"$remote_module\" debug_delay_ms=\"$debug_delay_ms\" 2>/dev/null || insmod \"$remote_module\"' > '$LAB_REMOTE_DIR/insmod.log' 2>&1 <&0 & sleep 1"

	echo "polling /sys/module/$module_name/sections/ (timeout ${insmod_wait_secs}s)"

	# Dump every readable file under sections/. Dynamic discovery handles
	# non-standard sections (.text.hot, custom __ksymtab subsections, ...).
	#
	# The sh -c body MUST be single-quoted in the SSH command so the outer
	# remote bash doesn't expand $(ls -A) and $f before sh -c sees them —
	# with double quotes the outer bash would expand in its own CWD and
	# the for loop would iterate over the wrong filenames.
	local dump_body
	# shellcheck disable=SC2016  # $(...) is intentionally deferred to sh -c.
	dump_body='cd "/sys/module/'"$module_name"'/sections" 2>/dev/null && for f in $(ls -A 2>/dev/null); do [ -r "$f" ] && printf "%s %s\n" "$f" "$(cat "$f")"; done'

	local deadline=$((SECONDS + insmod_wait_secs)) tmp
	tmp="$(mktemp)"
	while ((SECONDS < deadline)); do
		# No sleep — each SSH round-trip already takes 0.3-1s, which is
		# the right polling cadence.
		if lab_ssh_sudo "sh -c '$dump_body'" > "$tmp" 2>/dev/null; then
			local text_addr
			text_addr="$(awk '$1 == ".text" { print $2 }' "$tmp")"
			if [[ -n "$text_addr" ]]; then
				# Pre-relocate the .ko: write each section's runtime
				# address into its sh_addr so BFD bakes correct addresses into DWARF.
				local relocated="$gdb_dir/$target-$debug_method-$module_name-relocated.ko"
				local objcopy_args=()
				local existing
				existing="$(objdump -h "$artifact" | awk '$1 ~ /^[0-9]+$/ {print $2}')"
				while read -r sec addr; do
					[[ -z "$addr" || -z "$sec" ]] && continue
					grep -qxF "$sec" <<< "$existing" || continue
					objcopy_args+=("--change-section-address" "$sec=$addr")
				done < "$tmp"
				rm -f "$relocated"
				objcopy --no-change-warnings "${objcopy_args[@]}" \
					"$artifact" "$relocated" \
					|| die "objcopy failed to produce $relocated"
				{
					printf 'add-symbol-file %s %s\n' "$relocated" "$text_addr"
					printf 'echo loaded %s module symbols for %s\\n\n' "$module_name" "$target"
				} > "$symbols_file"
				rm -f "$tmp"
				echo "wrote $symbols_file"
				return 0
			fi
		fi
	done

	rm -f "$tmp"
	echo "timeout: /sys/module/$module_name/sections/.text did not appear within ${insmod_wait_secs}s"
	echo "remote insmod.log (empty = insmod produced no output; module may still have failed to load):"
	lab_ssh "cat '$LAB_REMOTE_DIR/insmod.log' 2>/dev/null || true" | sed 's/^/  /'
	echo "hint: ssh into the target and try the load by hand:"
	echo "  ssh $LAB_SSH_TARGET sudo insmod $remote_module"
	echo "  ssh $LAB_SSH_TARGET sudo dmesg | tail -40"
	echo "  ssh $LAB_SSH_TARGET ls -la /sys/module/$module_name/sections/ 2>/dev/null"
	return 1
}

if load_and_discover_symbols > "$loader_log" 2>&1; then
	tail -1 "$loader_log"
else
	echo "module load / symbol discovery failed (full log: $loader_log):"
	sed 's/^/  /' "$loader_log"
	exit 1
fi

# --- GDB init files ------------------------------------------------------
#
# If the kernel source has been synced (scripts/01 --debug-symbols +
# scripts/02), discover Ubuntu's build-time source prefix from vmlinux so
# GDB can remap DWARF references to our local copy. addr2line on
# `start_kernel` returns `<build-prefix>/init/main.c`; stripping the
# known suffix yields the prefix.
kernel_substitute_line=""
kernel_directory_line=""
kernel_src_link="$cache_dir/source"
if [[ -L "$kernel_src_link" || -d "$kernel_src_link" ]]; then
	kernel_src_root="$(readlink -f "$kernel_src_link")"
	kernel_directory_line="directory $kernel_src_root"
	if command -v nm >/dev/null 2>&1 && command -v addr2line >/dev/null 2>&1; then
		# `|| true` because awk's `exit` SIGPIPEs nm, and pipefail would
		# kill the script. We still capture awk's output before exit.
		sym_addr="$(nm "$vmlinux" 2>/dev/null | awk 'NF==3 && $3=="start_kernel" {print $1; exit}' || true)"
		if [[ -n "$sym_addr" ]]; then
			sym_loc="$(addr2line -e "$vmlinux" "$sym_addr" 2>/dev/null | head -1 | cut -d: -f1 || true)"
			build_prefix="${sym_loc%/init/main.c}"
			if [[ -n "$build_prefix" && "$build_prefix" != "$sym_loc" ]]; then
				kernel_substitute_line="set substitute-path $build_prefix $kernel_src_root"
				echo "kernel source mapping: $build_prefix -> $kernel_src_root"
			else
				echo "warning: addr2line returned unexpected location for start_kernel: $sym_loc"
			fi
		else
			echo "warning: could not find start_kernel in vmlinux; kernel step-into will show disassembly"
		fi
	fi
else
	echo "note: no kernel source synced; kernel step-into will show disassembly"
	echo "      to enable: scripts/01-... --debug-symbols && scripts/02-... $target"
fi

{
	cat <<EOF
set confirm off
set pagination off
set breakpoint pending on
set mi-async on
set print pretty on
set print thread-events off
set disassemble-next-line on
set tcp connect-timeout 60
set remotetimeout 60
set architecture i386:x86-64
set substitute-path $intermediate_dir $module_dir
directory $module_dir
EOF
	[[ -n "$kernel_substitute_line" ]] && printf '%s\n' "$kernel_substitute_line"
	[[ -n "$kernel_directory_line" ]] && printf '%s\n' "$kernel_directory_line"
	cat <<EOF
symbol-file $vmlinux
target remote $debug_endpoint
source $symbols_file
EOF
} > "$gdb_file"

# Native Debug and cppdbg-with-miDebuggerServerAddress already called
# `target remote` and loaded the executable by the time they source this
# script — so commands that change global gdb state error with
# "Cannot change this setting while the inferior is running". Strip them.
grep -vE '^(target remote |set mi-async |set target-async |set tcp connect-timeout |set remotetimeout |set architecture |symbol-file )' \
	"$gdb_file" > "$gdb_attached_file"

ln -sfn "$target-$debug_method.gdb" "$gdb_dir/current-debug.gdb"
ln -sfn "$target-$debug_method-attached.gdb" "$gdb_dir/current-debug-attached.gdb"
ln -sfn "../kernel-cache/$target/vmlinux" "$gdb_dir/current-vmlinux"

echo
echo "ready. GDB endpoint: $debug_endpoint"
echo "  gdb script:  $gdb_file"
echo "  loader log:  $loader_log"
echo "next: F5 in VS Code (Kernel: cppdbg) — or: gdb -tui $vmlinux -x $gdb_file"
