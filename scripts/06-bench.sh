#!/usr/bin/env bash
# Build, deploy, and run the event benchmark (code/bench) on <target>, then
# fetch the raw-sample CSV back into results/, named by the git revision the
# tree was at -- so every implementation iteration leaves a comparable,
# attributable artifact:
#
#   scripts/06-bench.sh server                      # full default sweep
#   scripts/06-bench.sh server -s wake -N 1,64,256  # extra args go to ./bench
#
#   python3 code/bench/compare.py results/bench-server-<revA>-*.csv \
#                                 results/bench-server-<revB>-*.csv
#
# The benchmark needs /dev/event on the target; like scripts/05, this script
# insmods the built event.ko if the device is missing. It does NOT rebuild or
# reload the module if it is already loaded -- after changing event.c, run
# scripts/03-build-module.sh and scripts/04-deploy-and-prep-gdb.sh (or rmmod
# on the target) first, or you will benchmark the OLD implementation. The
# script warns when the loaded module's srcversion differs from the built .ko.
set -euo pipefail

target="${1:-}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/common.sh
source "$repo_root/scripts/lib/common.sh"
validate_target "$target" "usage: $0 <target> [bench args...]"
shift
# shellcheck source=/dev/null
source "$(lab_env_file "$repo_root")"
lab_load_target "$target"
lab_check_connection --no-sudo

echo "building code/bench on the host"
make -C "$repo_root/code/bench"

remote_dir="$LAB_REMOTE_DIR"
remote_bench="$remote_dir/bench"
remote_csv="$remote_dir/bench.csv"

echo "deploying bench to $LAB_SSH_TARGET:$remote_bench"
lab_ssh "mkdir -p '$remote_dir'" || die "could not create $remote_dir on $LAB_SSH_TARGET"
lab_scp_to "$repo_root/code/bench/bench" "$remote_bench" || die "could not upload bench"
lab_ssh "chmod +x '$remote_bench'"

# The bench needs /dev/event. If the module isn't loaded, load the built .ko
# (same dance as scripts/05-debug-user.sh).
kernel_file="$repo_root/kernel-cache/$target/kernel.release"
ko=""
if [[ -f "$kernel_file" ]]; then
	ko="$repo_root/build/artifacts/$target/$(<"$kernel_file")/event.ko"
fi
if ! lab_ssh "test -e /dev/event"; then
	[[ -n "$ko" && -f "$ko" ]] ||
		die "/dev/event missing on $LAB_SSH_TARGET and no built event.ko; run scripts/03-build-module.sh $target first"
	echo "/dev/event not present; loading $(basename "$ko") on the target"
	lab_scp_to "$ko" "$remote_dir/event.ko"
	lab_ssh_sudo "insmod '$remote_dir/event.ko'" ||
		die "insmod event.ko failed on $LAB_SSH_TARGET; check: ssh $LAB_SSH_TARGET sudo dmesg | tail"
elif [[ -n "$ko" && -f "$ko" ]]; then
	# Catch the classic mistake: editing event.c but benchmarking the module
	# that is still loaded from before. srcversion is a hash of the sources.
	built_src="$(modinfo -F srcversion "$ko" 2>/dev/null || true)"
	loaded_src="$(lab_ssh "cat /sys/module/event/srcversion 2>/dev/null" || true)"
	if [[ -n "$built_src" && -n "$loaded_src" && "$built_src" != "$loaded_src" ]]; then
		echo "WARNING: the event.ko loaded on the target ($loaded_src) is not the" >&2
		echo "         one last built ($built_src) -- you are about to benchmark a" >&2
		echo "         STALE implementation. Reload it first:" >&2
		echo "           scripts/04-deploy-and-prep-gdb.sh $target <method>" >&2
	fi
fi

# Label every sample with the revision of the working tree, so a CSV is
# forever attributable to the implementation that produced it.
rev="$(git -C "$repo_root" describe --always --dirty 2>/dev/null || echo unknown)"
stamp="$(date +%Y%m%d-%H%M%S)"
out_dir="$repo_root/results"
out="$out_dir/bench-$target-$rev-$stamp.csv"

echo "running bench on the target (label '$rev', extra args: ${*:-none})"
lab_ssh "cd '$remote_dir' && ./bench -c '$remote_csv' -l '$rev' $*" ||
	die "bench failed on $LAB_SSH_TARGET"

mkdir -p "$out_dir"
lab_rsync_from "$remote_csv" "$out" || die "could not fetch $remote_csv"
echo ""
echo "raw samples: $out"
echo "compare against a previous run with:"
echo "  python3 code/bench/compare.py results/<baseline>.csv $out"
