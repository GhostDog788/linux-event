#!/usr/bin/env bash
# Sync the target's kernel build tree to the host so an out-of-tree module
# can be cross-built against the target's exact kernel.
#
# Populates kernel-cache/<target>/ with:
#   build/                  -> synced linux-headers tree
#   source/                 -> synced linux-source tree (if --debug-symbols was used in 01)
#   vmlinux                 -> debug-symbol vmlinux from the target (if available)
#   kernel.release          -> running kernel release
#   remote.{build,header}.* -> bookkeeping
# Also updates kernel-cache/current -> <target> so plain `make` and the
# IntelliSense config track the most recently synced target.
set -euo pipefail

target="${1:-}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/common.sh
source "$repo_root/scripts/lib/common.sh"
validate_target "$target" "usage: $0 <target>"
require_debian_host

env_file="$(lab_env_file "$repo_root")"
# shellcheck source=/dev/null
source "$env_file"
lab_load_target "$target"

target_os="$(target_cfg "$target" OS)"
target_os="${target_os:-ubuntu}"
case "$target_os" in
	ubuntu) ;;
	*) die "unsupported TARGET_OS[$target]='$target_os'; only ubuntu targets are implemented" ;;
esac

# --- Install dev-host build prerequisites ---------------------------------

apt_get() {
	sudo apt-get \
		-o Acquire::Retries=3 \
		-o Acquire::http::Timeout=20 \
		-o Acquire::https::Timeout=20 \
		"$@"
}

echo "[1/5] installing host build prerequisites"
apt_get update
apt_get install -y \
	bc bison build-essential dwarves flex gdb \
	libelf-dev libssl-dev openssh-client rsync sshpass

# --- Discover the target's kernel + header layout -------------------------

echo "[2/5] discovering target kernel"
lab_check_connection
kernel="$(lab_ssh 'uname -r')"
echo "       target $target is running $kernel"

cache_dir="$repo_root/kernel-cache/$target"
usr_src_dir="$cache_dir/usr-src"
build_dir="$cache_dir/build"
remote_vmlinux="/usr/lib/debug/boot/vmlinux-$kernel"

mkdir -p "$cache_dir" "$usr_src_dir"

build_real="$(lab_ssh "readlink -f '/lib/modules/$kernel/build'")"
if ! lab_ssh "test -d '$build_real'"; then
	die "missing target build directory $build_real;
- on the target, check: ls -la /lib/modules/$kernel/build
- if it's missing, re-run: scripts/01-provision-target.sh $target <method>"
fi

mapfile -t remote_header_dirs < <(lab_ssh "bash -s" <<REMOTE
set -euo pipefail
kernel='$kernel'
build_real='$build_real'
printf '%s\n' "\$build_real"

if [[ -e "/lib/modules/\$kernel/source" ]]; then
	source_real="\$(readlink -f "/lib/modules/\$kernel/source")"
	[[ -d "\$source_real" ]] && printf '%s\n' "\$source_real"
fi

base="\${kernel%-generic}"
base="\${base%-lowlatency}"
for d in "/usr/src/linux-headers-\$base"; do
	[[ -d "\$d" ]] && printf '%s\n' "\$d"
done
REMOTE
)

# Kernel source is found separately: it can be a tarball-file or a dir
# (sometimes with the tarball inside it). Classify each path as file|dir so
# the sync loop below stays trivial.
mapfile -t remote_source_entries < <(lab_ssh "bash -s" <<'REMOTE'
set -euo pipefail
shopt -s nullglob
seen=()
for p in /usr/src/linux-source-*.tar.* /usr/src/linux-source-*; do
	[[ -e "$p" ]] || continue
	dup=
	for s in "${seen[@]}"; do [[ "$s" == "$p" ]] && dup=1 && break; done
	[[ -n "$dup" ]] && continue
	seen+=("$p")
	if [[ -d "$p" ]]; then printf '%s\tdir\n' "$p"
	elif [[ -f "$p" ]]; then printf '%s\tfile\n' "$p"
	fi
done
REMOTE
)

# --- Sync ----------------------------------------------------------------

sync_header_dir() {
	local remote_dir="$1" dest
	dest="$usr_src_dir/$(basename "$remote_dir")"
	echo "       $remote_dir -> ${dest#"$repo_root"/}"
	# Preserve symlinks inside Ubuntu's header trees (e.g. dangling rust
	# links — harmless for external C builds).
	lab_rsync_from "$remote_dir/" "$dest/" --delete
}

sync_source_path() {
	local remote_path="$1" kind="$2" dest
	dest="$usr_src_dir/$(basename "$remote_path")"
	echo "       $remote_path -> ${dest#"$repo_root"/}"
	if [[ "$kind" == "dir" ]]; then
		lab_rsync_from "$remote_path/" "$dest/" --delete
	else
		lab_rsync_from "$remote_path" "$dest"
	fi
}

echo "[3/5] syncing kernel headers and source"
# Dedup discovered paths — two map entries can canonicalize to the same
# dir via symlink chains.
printf '%s\n' "${remote_header_dirs[@]}" | awk 'NF && !seen[$0]++' |
while IFS= read -r remote_dir; do
	sync_header_dir "$remote_dir"
done

for entry in "${remote_source_entries[@]}"; do
	[[ -z "$entry" ]] && continue
	sync_source_path "${entry%$'\t'*}" "${entry##*$'\t'}"
done

build_name="$(basename "$build_real")"
rm -rf "$build_dir"
ln -s "usr-src/$build_name" "$build_dir"
[[ -f "$build_dir/Makefile" ]] || die "synced build tree is missing Makefile: $build_dir"

# --- vmlinux -------------------------------------------------------------

echo "[4/5] checking vmlinux debug image"
if lab_ssh_sudo "test -r '$remote_vmlinux'"; then
	# rsync-over-sudo: skips unchanged, restartable, checksummed. Replaces
	# an earlier cat-over-ssh approach that silently corrupted vmlinux on
	# any sudo banner or connection drop (file is ~415MB).
	echo "       rsync $remote_vmlinux -> ${cache_dir#"$repo_root"/}/vmlinux"
	lab_rsync_from "$remote_vmlinux" "$cache_dir/vmlinux" --rsync-path='sudo rsync'
else
	rm -f "$cache_dir/vmlinux"
	echo "       no vmlinux on target ($remote_vmlinux is missing)"
	echo "       module debugging will work; kernel source debugging will not"
	echo "       to enable: scripts/01-provision-target.sh $target <method> --debug-symbols"
fi

# --- Kernel source resolution --------------------------------------------
#
# Ubuntu has shipped three linux-source layouts:
#   1. /usr/src/linux-source-X.tar.bz2          (tarball, no enclosing dir)
#   2. /usr/src/linux-source-X/<source files>   (extracted, files at top)
#   3. /usr/src/linux-source-X/linux-source-X/  (extracted, nested one deep)
# Plus the variant where the tarball lives inside the same-named dir.
#
# Strategy: look for a Makefile + init/main.c (the kernel-source markers)
# at depth ≤ 3 under usr-src/. If found, symlink to it. Otherwise extract
# a tarball into source-tree/ on the host (self-healing on rerun, and
# avoids needing tar on the target).

echo "[5/5] resolving kernel source for step-into-kernel"
source_link="$cache_dir/source"
source_tree="$cache_dir/source-tree"
rm -f "$source_link"

is_kernel_source_root() {
	[[ -f "$1/Makefile" && -d "$1/init" && -f "$1/init/main.c" ]]
}

# Prefer the linux-source-X dir whose X matches the live kernel; fall
# back to the highest version installed.
short_kver="$(printf '%s' "$kernel" | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+' || true)"
found_root=""
matching_root=""
while IFS= read -r mf; do
	candidate="$(dirname "$mf")"
	if is_kernel_source_root "$candidate"; then
		[[ -z "$found_root" ]] && found_root="$candidate"
		if [[ -n "$short_kver" && "$candidate" == *"linux-source-$short_kver"* ]]; then
			matching_root="$candidate"
			break
		fi
	fi
done < <(find "$usr_src_dir" -mindepth 1 -maxdepth 3 -name Makefile -path '*linux-source-*' 2>/dev/null | sort -Vr)
found_root="${matching_root:-$found_root}"

if [[ -n "$found_root" ]]; then
	rm -rf "$source_tree"
	rel="${found_root#"$cache_dir"/}"
	ln -s "$rel" "$source_link"
	echo "       kernel source: $source_link -> $rel"
else
	shopt -s nullglob
	tarballs=("$usr_src_dir"/linux-source-*.tar.* "$usr_src_dir"/linux-source-*/linux-source-*.tar.*)
	shopt -u nullglob
	if [[ ${#tarballs[@]} -gt 0 ]]; then
		tarball="${tarballs[0]}"
		if is_kernel_source_root "$source_tree"; then
			echo "       kernel source already extracted at $source_tree"
		else
			echo "       extracting $(basename "$tarball") -> source-tree/ (one-time, ~30s)"
			rm -rf "$source_tree"
			mkdir -p "$source_tree"
			tar -C "$source_tree" --strip-components=1 -xf "$tarball"
		fi
		ln -s source-tree "$source_link"
		echo "       kernel source: $source_link -> source-tree"
	else
		rm -rf "$source_tree"
		echo "       no kernel source on target; kernel step-into will show disassembly only"
		echo "       to enable: scripts/01-provision-target.sh $target <method> --debug-symbols"
	fi
fi

# --- Bookkeeping ---------------------------------------------------------

printf '%s\n' "$kernel" > "$cache_dir/kernel.release"
printf '%s\n' "$build_real" > "$cache_dir/remote.build.path"
printf '%s\n' "${remote_header_dirs[@]}" > "$cache_dir/remote.header.paths"
ln -sfn "$target" "$repo_root/kernel-cache/current"

echo
echo "synced $target kernel $kernel"
echo "  build  -> $build_dir"
[[ -L "$source_link" ]] && echo "  source -> $(readlink -f "$source_link")"
[[ -f "$cache_dir/vmlinux" ]] && echo "  vmlinux: $cache_dir/vmlinux"
echo "next: scripts/03-build-module.sh $target"
