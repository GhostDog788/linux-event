#!/usr/bin/env bash
# Build module/ against the synced headers for <target>.
#
# Delegates to the top-level Makefile, which stages module/ into
# build/intermediate/<target>/<kernel>/ and runs Kbuild from there with
# -ffile-prefix-map injected via KCFLAGS. Result: one .ko under
# build/artifacts/<target>/<kernel>/.
set -euo pipefail

target="${1:-}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/common.sh
source "$repo_root/scripts/lib/common.sh"
validate_target "$target" "usage: $0 <target>"

cache_dir="$repo_root/kernel-cache/$target"
kernel_file="$cache_dir/kernel.release"
kdir="$cache_dir/build"

[[ -f "$kernel_file" && -d "$kdir" ]] ||
	die "missing kernel cache for $target; run: scripts/02-setup-host-build.sh $target"

module_dir="$repo_root/module"
[[ -e "$module_dir/Makefile" || -e "$module_dir/Kbuild" ]] ||
	die "no Makefile or Kbuild under $module_dir; drop your module project there (see $module_dir/README.md), or try the example: make use-example NAME=chuck_norise"

kernel="$(<"$kernel_file")"
intermediate_dir="$repo_root/build/intermediate/$target/$kernel"
artifact_dir="$repo_root/build/artifacts/$target/$kernel"

echo "building $module_dir for $target kernel $kernel"
make -C "$repo_root" \
	KDIR="$kdir" \
	BUILD_ID="$target/$kernel" \
	INTERMEDIATE_DIR="$intermediate_dir" \
	ARTIFACT_DIR="$artifact_dir" \
	MODULE_DIR="$module_dir" \
	clean modules

mapfile -t kos < <(find "$artifact_dir" -maxdepth 1 -name '*.ko' -type f | sort)
[[ ${#kos[@]} -gt 0 ]] || die "build produced no .ko under $artifact_dir; check the make output above"

for ko in "${kos[@]}"; do
	echo "built $ko"
done
echo "next: scripts/04-deploy-and-prep-gdb.sh $target <kgdb|qemu>"
