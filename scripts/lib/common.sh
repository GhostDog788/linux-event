#!/usr/bin/env bash
# Shared helpers sourced by every numbered script.
#
# `die`, `validate_target`, `target_*` are state-free.
# `lab_*` helpers rely on globals set by `lab_load_target`.
# `LAB_*` globals are set by `lab_load_target`. Use the helpers, not the
# raw ssh/scp/sshpass binaries.

die() {
	echo "$*" >&2
	exit 1
}

# --- Target validation ----------------------------------------------------

validate_target() {
	local target="$1"
	local usage="$2"

	[[ -n "$target" ]] || die "$usage"
	[[ "$target" =~ ^[A-Za-z][A-Za-z0-9_-]*$ ]] ||
		die "invalid target '$target'; use letters, numbers, '_' or '-', starting with a letter"
}

target_is_configured() {
	local target="$1"
	local known

	declare -p TARGETS >/dev/null 2>&1 ||
		die "missing TARGETS array in lab.local.env"

	for known in "${TARGETS[@]}"; do
		[[ "$known" == "$target" ]] && return 0
	done

	die "unknown target '$target'; add it to TARGETS in lab.local.env"
}

target_cfg() {
	local target="$1"
	local key="$2"
	local map_name="TARGET_${key}"

	declare -p "$map_name" >/dev/null 2>&1 || return 0
	local -n map="$map_name"
	printf '%s' "${map[$target]:-}"
}

target_require_cfg() {
	local target="$1"
	local key="$2"
	local value

	value="$(target_cfg "$target" "$key")"
	[[ -n "$value" ]] || die "missing TARGET_${key}[$target] in lab.local.env"
	printf '%s' "$value"
}

require_debian_host() {
	command -v apt-get >/dev/null 2>&1 ||
		die "the development host must be Debian-based (apt-get not found)"
}

# --- Lab env loading ------------------------------------------------------

# Callers `source` the returned path directly so TARGET_* assignments land
# in script-global scope (sourcing from inside a function would scope them
# to the function).
lab_env_file() {
	local env_file="$1/lab.local.env"
	[[ -f "$env_file" ]] || die "missing $env_file; copy lab.example.env to lab.local.env and edit it for your machines"
	printf '%s' "$env_file"
}

# --- Per-target connection setup ------------------------------------------

# Read TARGET_* config and populate LAB_* globals: LAB_TARGET, LAB_SSH_HOST,
# LAB_SSH_PORT, LAB_SSH_USER, LAB_SSH_PASS, LAB_SUDO_PASS, LAB_REMOTE_DIR,
# LAB_SSH_TARGET, LAB_SSH_CMD[], LAB_SCP_CMD[], LAB_RSYNC_RSH.
#
# LAB_SSH_CMD / LAB_SCP_CMD are flag-only — they do NOT include the host.
# Helpers (and the few direct callers) append it themselves, so options
# like `-t` can be inserted before the host (ssh treats anything after
# the host as the remote command).
lab_load_target() {
	local target="$1"
	target_is_configured "$target"

	# shellcheck disable=SC2034  # LAB_TARGET is consumed by sourcing scripts.
	LAB_TARGET="$target"
	LAB_SSH_HOST="$(target_require_cfg "$target" SSH_HOST)"
	LAB_SSH_PORT="$(target_cfg "$target" SSH_PORT)"
	LAB_SSH_USER="$(target_require_cfg "$target" SSH_USER)"
	LAB_SSH_PASS="$(target_cfg "$target" SSH_PASS)"
	LAB_SUDO_PASS="$(target_cfg "$target" SUDO_PASS)"
	LAB_REMOTE_DIR="$(target_cfg "$target" REMOTE_DIR)"

	LAB_SSH_PORT="${LAB_SSH_PORT:-22}"
	LAB_SUDO_PASS="${LAB_SUDO_PASS:-$LAB_SSH_PASS}"
	LAB_REMOTE_DIR="${LAB_REMOTE_DIR:-/tmp/kmod-debug-lab-$LAB_SSH_USER}"

	local ssh_bin scp_bin sshpass_bin
	ssh_bin="${SSH_BIN:-ssh}"
	scp_bin="${SCP_BIN:-scp}"
	sshpass_bin="${SSHPASS_BIN:-sshpass}"

	if [[ -n "$LAB_SSH_PASS" ]] && ! command -v "$sshpass_bin" >/dev/null 2>&1; then
		die "TARGET_SSH_PASS[$target] is set but '$sshpass_bin' isn't installed; install with: sudo apt-get install -y sshpass"
	fi

	LAB_SSH_TARGET="$LAB_SSH_USER@$LAB_SSH_HOST"

	# ConnectTimeout: fail fast on dead targets instead of multi-minute TCP wait.
	# ServerAliveInterval: keep long-lived connections (e.g. the polling loop)
	# alive through NAT idle timers.
	# ControlMaster: multiplex every ssh/scp/rsync for a target over ONE
	# connection. The first connection authenticates (so password-auth targets
	# prompt exactly once instead of once per command, which otherwise breaks
	# scripts that make several connections in a row); the rest reuse it. The
	# socket persists briefly so back-to-back script runs stay authenticated.
	# %C hashes user/host/port into a short, per-target, collision-free name.
	local cm_path="${TMPDIR:-/tmp}/lab-ssh-cm-%C"
	local ssh_opts=(
		-o StrictHostKeyChecking=accept-new
		-o ConnectTimeout=10
		-o ServerAliveInterval=15
		-o ControlMaster=auto
		-o "ControlPath=$cm_path"
		-o ControlPersist=120
	)
	LAB_SSH_CMD=("$ssh_bin" "${ssh_opts[@]}" -p "$LAB_SSH_PORT")
	LAB_SCP_CMD=("$scp_bin" "${ssh_opts[@]}" -P "$LAB_SSH_PORT")
	LAB_RSYNC_RSH="$ssh_bin ${ssh_opts[*]} -p $LAB_SSH_PORT"
	if [[ -n "$LAB_SSH_PASS" ]]; then
		LAB_SSH_CMD=("$sshpass_bin" -e "${LAB_SSH_CMD[@]}")
		LAB_SCP_CMD=("$sshpass_bin" -e "${LAB_SCP_CMD[@]}")
		LAB_RSYNC_RSH="$sshpass_bin -e $LAB_RSYNC_RSH"
	fi
}

# --- Remote command helpers -----------------------------------------------
# All require a prior `lab_load_target` call.

# Inherits stdin from the caller (so a caller can pipe data into the remote
# command — see lab_ssh_sudo).
lab_ssh() {
	SSHPASS="$LAB_SSH_PASS" "${LAB_SSH_CMD[@]}" "$LAB_SSH_TARGET" "$@"
}

# Run a command on the target as root. The password reaches `sudo -S` via
# ssh's stdin, so it never appears in argv on either host.
#
# `-k` ignores any cached sudo timestamp (e.g. left by `scripts/00-check-target.sh`)
# for THIS invocation only — without it, sudo would skip the stdin read and
# the password would leak into the next reader. It does NOT invalidate the
# user's existing sudo cache.
#
# For callers where the remote command itself reads stdin (e.g. `bash -s`),
# this helper is not enough — NOPASSWD policy also skips the stdin read,
# leaking the password. Use SUDO_ASKPASS instead (see scripts/01 for the
# pattern).
lab_ssh_sudo() {
	printf '%s\n' "$LAB_SUDO_PASS" |
		SSHPASS="$LAB_SSH_PASS" "${LAB_SSH_CMD[@]}" "$LAB_SSH_TARGET" "sudo -k -S -p '' $*"
}

lab_scp_to() {
	SSHPASS="$LAB_SSH_PASS" "${LAB_SCP_CMD[@]}" "$1" "$LAB_SSH_TARGET:$2"
}

# Extra rsync flags can be appended — notably `--rsync-path='sudo rsync'`
# for paths only root can read (vmlinux), and `--delete` for tree mirroring.
lab_rsync_from() {
	local remote_src="$1"
	local local_dst="$2"
	shift 2
	SSHPASS="$LAB_SSH_PASS" "${RSYNC_BIN:-rsync}" -a -e "$LAB_RSYNC_RSH" "$@" \
		"$LAB_SSH_TARGET:$remote_src" "$local_dst"
}

# --- Preflight ------------------------------------------------------------

# `--no-sudo` skips the sudo test, for scripts/01 which may be running TO
# configure sudo for the first time.
# shellcheck disable=SC2120  # --no-sudo arg is optional.
lab_check_connection() {
	local check_sudo=1
	[[ "${1:-}" == "--no-sudo" ]] && check_sudo=0

	if ! lab_ssh 'true' 2>/dev/null; then
		die "cannot reach $LAB_SSH_TARGET over ssh (port $LAB_SSH_PORT).
- check TARGET_SSH_HOST/PORT/USER for '$LAB_TARGET' in lab.local.env
- try by hand: ssh -p $LAB_SSH_PORT $LAB_SSH_TARGET
- if using passwords, confirm TARGET_SSH_PASS[$LAB_TARGET] is set"
	fi
	if (( check_sudo )) && ! lab_ssh_sudo 'true' 2>/dev/null; then
		die "ssh reaches $LAB_SSH_TARGET but sudo doesn't work there.
- check TARGET_SUDO_PASS[$LAB_TARGET] in lab.local.env (defaults to TARGET_SSH_PASS)
- on the target, confirm: sudo -n -v
- consider NOPASSWD for the lab user on long-lived dev targets"
	fi
}
