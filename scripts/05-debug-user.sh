#!/usr/bin/env bash
# Deploy the usermode demo to <target> and start it under gdbserver, ready for
# a host-side gdb (the VS Code "User: demo (remote)" launch) to attach.
#
# The demo talks to /dev/event, so it has to run on the target where event.ko
# is loaded -- not on the dev host. This script:
#   1. uploads code/example/demo to the target,
#   2. makes sure /dev/event exists (insmod's the built event.ko if not),
#   3. (re)starts `gdbserver :<port> ./demo <args>` on the target, detached,
#   4. writes .gdb/current-user.gdb (+ the current-user-program symlink) that
#      the cppdbg launch sources to `target remote` into that gdbserver.
#
# Env knobs:
#   USERDEBUG_PORT   (2345)   TCP port gdbserver listens on (target-side)
#   DEMO_ARGS        (3)      args passed to ./demo (its listener count)
#   GDBSERVER_WAIT   (10)     seconds to wait for the port to come up
set -euo pipefail

target="${1:-}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/common.sh
source "$repo_root/scripts/lib/common.sh"
validate_target "$target" "usage: $0 <target>"
# shellcheck source=/dev/null
source "$(lab_env_file "$repo_root")"
lab_load_target "$target"
lab_check_connection --no-sudo

port="${USERDEBUG_PORT:-2345}"
demo_args="${DEMO_ARGS:-3}"
wait_secs="${GDBSERVER_WAIT:-10}"

demo_bin="$repo_root/code/example/demo"
[[ -x "$demo_bin" ]] ||
	die "no built demo at $demo_bin; run the 'User: Build demo' task first (or: make -C code/example)"

remote_dir="$LAB_REMOTE_DIR"
remote_demo="$remote_dir/demo"

echo "deploying demo to $LAB_SSH_TARGET:$remote_demo"
lab_ssh "mkdir -p '$remote_dir'" || die "could not create $remote_dir on $LAB_SSH_TARGET (ssh failed)"
lab_scp_to "$demo_bin" "$remote_demo" || die "could not upload demo to $LAB_SSH_TARGET:$remote_demo (scp failed)"
lab_ssh "chmod +x '$remote_demo'" || die "could not chmod $remote_demo on $LAB_SSH_TARGET"

# gdbserver must be present on the target.
lab_ssh "command -v gdbserver >/dev/null 2>&1" ||
	die "gdbserver is not installed on $LAB_SSH_TARGET; install it there: sudo apt-get install -y gdbserver"

# The demo needs /dev/event. If the module isn't loaded, load the built .ko.
if ! lab_ssh "test -e /dev/event"; then
	kernel_file="$repo_root/kernel-cache/$target/kernel.release"
	[[ -f "$kernel_file" ]] ||
		die "/dev/event missing on $LAB_SSH_TARGET and no kernel cache for '$target' to locate event.ko; run scripts/04-deploy-and-prep-gdb.sh $target <method> to load it"
	kernel="$(<"$kernel_file")"
	ko="$repo_root/build/artifacts/$target/$kernel/event.ko"
	[[ -f "$ko" ]] ||
		die "/dev/event missing and no $ko; build it (scripts/03-build-module.sh $target) or load it via scripts/04-deploy-and-prep-gdb.sh $target <method>"
	echo "/dev/event not present; loading $(basename "$ko") on the target"
	lab_scp_to "$ko" "$remote_dir/event.ko"
	lab_ssh_sudo "insmod '$remote_dir/event.ko'" ||
		die "insmod event.ko failed on $LAB_SSH_TARGET; check: ssh $LAB_SSH_TARGET sudo dmesg | tail"
fi

fwd_spec="127.0.0.1:$port:127.0.0.1:$port"
log="$remote_dir/gdbserver.log"
# Clear any stale gdbserver (target) and forwarding (host-side SSH master).
# The '[g]' bracket keeps the pattern from matching the shell running pkill
# (whose own command line contains this very string) -- otherwise pkill -f
# SIGTERMs its own ssh session and the call fails with 255.
lab_ssh "pkill -f '[g]dbserver 127.0.0.1:$port' 2>/dev/null || true"
SSHPASS="$LAB_SSH_PASS" "${LAB_SSH_CMD[@]}" -O cancel -L "$fwd_spec" "$LAB_SSH_TARGET" 2>/dev/null || true

echo "starting gdbserver 127.0.0.1:$port ./demo $demo_args on the target (detached)"
# Bind gdbserver to the target's loopback only; we reach it through the SSH
# forward, so it is never exposed on the target's network. The ( ... & )
# subshell + setsid + full redirects detach gdbserver from this ssh channel --
# without the subshell, ssh waits on the backgrounded process and never returns.
lab_ssh "cd '$remote_dir' && ( setsid gdbserver 127.0.0.1:$port ./demo $demo_args \
	> '$log' 2>&1 < /dev/null & ) ; echo gdbserver-launched"

# Wait for gdbserver to actually be listening on the target before we connect.
echo "waiting up to ${wait_secs}s for gdbserver to come up"
deadline=$((SECONDS + wait_secs))
until lab_ssh "grep -q 'Listening on port' '$log' 2>/dev/null"; do
	(( SECONDS < deadline )) ||
		die "gdbserver did not start within ${wait_secs}s;
- target log: ssh -p $LAB_SSH_PORT $LAB_SSH_TARGET cat '$log'
- confirm gdbserver is installed and /dev/event is present"
	sleep 0.3
done

# Forward host 127.0.0.1:$port -> the target's gdbserver over the multiplexed
# SSH master. Using the existing master (-O forward) avoids a separate ssh -f
# process -- which sshpass cannot drive -- and works through NAT/firewalls.
echo "forwarding $fwd_spec over the SSH master"
SSHPASS="$LAB_SSH_PASS" "${LAB_SSH_CMD[@]}" -O forward -L "$fwd_spec" "$LAB_SSH_TARGET" ||
	die "could not set up SSH port-forward $fwd_spec (is the SSH master up?)"
endpoint="127.0.0.1:$port"

# Emit the gdb init the cppdbg launch sources, plus a stable symlink for the
# launch's `program` (local copy carries the symbols; same build as the target).
gdb_dir="$repo_root/.gdb"
mkdir -p "$gdb_dir"
cat >"$gdb_dir/current-user.gdb" <<EOF
# Generated by scripts/05-debug-user.sh -- do not edit by hand.
set pagination off
set breakpoint pending on
# gdbserver is running ./demo on $LAB_SSH_TARGET
target remote $endpoint
EOF
ln -sf "$demo_bin" "$gdb_dir/current-user-program"

echo "ready: gdb endpoint $endpoint, demo args '$demo_args'"
echo "next: F5 'User: demo (remote)' in VS Code, or:"
echo "  gdb $demo_bin -x $gdb_dir/current-user.gdb"
