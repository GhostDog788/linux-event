# Linux Kernel Module Debug Lab

A template for building and **source-debugging** out-of-tree Linux kernel
modules against one or more target VMs. Drop your module under `module/`,
point the lab at a target, press F5 — step through your code and into
kernel code from VS Code or `gdb -tui`.

What you get:

- **One slot, any module.** `module/` is your project; the lab is generic.
- **Per-target builds.** Cross-build the same source against multiple
  target kernels without polluting your tree.
- **Breakpoints just work.** `-ffile-prefix-map` is injected via `KCFLAGS`
  so IDE breakpoints by absolute path bind to your real source files.
- **Step into the kernel.** With `--debug-symbols`, the lab pulls down
  `vmlinux` + kernel source and wires GDB `substitute-path` so you can
  step from your module's read handler into `vfs_read`.
- **Two debug methods.** In-kernel KGDB over serial (`kgdb`) and any
  hypervisor GDB stub (`qemu`, also VMware's `debugStub`).

The repo ships with a worked example (`examples/chuck_norise/`) so you
can run the full flow end-to-end before writing a line of module code.

---

## Quickstart

You need a **Debian-based dev host** (Debian, Ubuntu, WSL Ubuntu) and at
least one **Ubuntu target VM** reachable over SSH.

```bash
# 0. clone + configure
git clone <this-repo> kmod-debug-lab && cd kmod-debug-lab
cp lab.example.env lab.local.env
$EDITOR lab.local.env                                       # TARGET_SSH_*, endpoints, etc.

# optional preflight (non-mutating)
./scripts/00-check-target.sh server qemu

# 1. target setup: headers, vmlinux dbg, kernel source (one-time)
./scripts/01-provision-target.sh server qemu --debug-symbols
# ...reboot the target VM so new GRUB args take effect...

# 2. host setup: sync everything to kernel-cache/server/ (one-time)
./scripts/02-setup-host-build.sh server

# 3. try the example (or skip and drop your own module under module/)
make use-example NAME=chuck_norise
./scripts/03-build-module.sh server          # or in VS Code: Ctrl+Shift+B ("Kernel: Build")

# 4. deploy + load on the target, prep GDB init files
./scripts/04-deploy-and-prep-gdb.sh server qemu

# 5. open the repo in VS Code and press F5 ("Kernel: cppdbg")
code .
```

Set a breakpoint in `module/src/hello.c` and one in `vfs_read` — both
bind. You're stepping through kernel code from your module.

**Inner loop in VS Code:** `Ctrl+Shift+B` runs the build (script 03);
`F5` runs deploy + attach (script 04 as the launch's pre-task, then GDB).
You don't need a terminal for either after the one-time `01 + 02`.

---

## Repo layout

```
module/                              your module project (Makefile + sources)
examples/chuck_norise/               worked example; `make use-example` copies it into module/

scripts/00-check-target.sh           preflight: ssh / sudo / headers / vmlinux / endpoint
scripts/01-provision-target.sh       target-side: headers, vmlinux dbg, kernel source, GRUB args
                                     (`--uninstall` to undo the GRUB args)
scripts/02-setup-host-build.sh       dev-host: sync headers + vmlinux + source into kernel-cache/
scripts/03-build-module.sh           dev-host: build module/ against the synced headers
scripts/04-deploy-and-prep-gdb.sh    dev-host: upload, insmod, write .gdb/<target>-<method>.gdb (does NOT attach gdb)
scripts/lib/common.sh                shared helpers (lab_ssh, lab_ssh_sudo, lab_rsync_from, ...)

kernel-cache/<target>/              synced build/source/vmlinux (gitignored)
build/                               per-target intermediate + final .ko (gitignored)
.gdb/                                generated GDB init files (gitignored, regenerated each F5)

host/                                optional VMware-on-Windows helpers
lab.example.env -> lab.local.env     per-machine config (gitignored copy)
```

Top-level `Makefile` exposes a few non-build targets too:
`make use-example NAME=<name>` (copy `examples/<name>/` into `module/`),
`make clean-module` (reset `module/` to just its placeholder README),
`make help` (full var/target listing).

The numbered scripts are designed to be run in order. After the one-time
01 + 02 against a target, the inner loop is **build → deploy → debug**:
script 03 (or `Ctrl+Shift+B`), then F5 — which runs 04 as the launch's
pre-task and then attaches GDB. F5 does *not* rebuild; if you edited
sources, build again first.

---

## The `module/` contract

`module/` is the lab's only slot for your module. The lab does not know
its name, source layout, or behavior. Expectations:

- **`module/Makefile`** (or `Kbuild`) following standard out-of-tree
  conventions:

  ```makefile
  obj-m += my_module.o
  my_module-y := src/main.o src/util.o
  ccflags-y := -I$(src)/include -g -DDEBUG
  ```

  `examples/chuck_norise/Makefile` shows the canonical shape, including
  the optional `ifndef KERNELRELEASE` wrapper that lets `make` work
  directly in `module/`.

- **Exactly one `obj-m` entry per build.** The lab discovers the module
  name from the produced `<name>.ko`.

- **Optional `debug_delay_ms` module parameter** so the host has time to
  attach GDB before init runs:

  ```c
  static unsigned int debug_delay_ms = 5000;
  module_param(debug_delay_ms, uint, 0644);
  ```

  Script 04 passes `DEBUG_LOAD_DELAY_MS` (from `lab.local.env`) on
  `insmod`. Modules that don't declare the parameter fall back to a
  plain `insmod` automatically.

Source layout is yours. The lab passes through whatever your Kbuild file
declares.

---

## Configuring the lab

Targets are **data, not variable prefixes**. Add a profile name to
`TARGETS`, then add entries to the `TARGET_*` maps under that name:

```bash
TARGETS=(desktop server)

declare -A TARGET_SSH_HOST=([desktop]=ubuntu-desktop.local [server]=ubuntu-server.local)
declare -A TARGET_SSH_USER=([desktop]=user [server]=user)
declare -A TARGET_SSH_PASS=([desktop]= [server]=)            # empty when using SSH keys

declare -A TARGET_DEBUG_ENDPOINT_QEMU=(
  [desktop]=127.0.0.1:1234
  [server]=127.0.0.1:1234
)
```

Endpoints are split per debug method (`TARGET_DEBUG_ENDPOINT_KGDB`,
`TARGET_DEBUG_ENDPOINT_QEMU`); either map can be empty per target. The
VS Code task picker accepts any target name you type — no edits to
`.vscode/` when adding targets.

---

## Debug methods

- **`qemu`** — the hypervisor's built-in GDB stub
  (QEMU's `-gdb tcp::PORT` or `-s`, VMware's `debugStub.listen.guest64`).
  No KGDB in the guest; the hypervisor halts the vCPU directly.
- **`kgdb`** — in-kernel KGDB over the guest's serial port. Script 01
  adds `kgdboc=ttyS0,115200` and `sysrq_always_enabled=1` to GRUB. Break
  with `echo g | sudo tee /proc/sysrq-trigger` on the target. Useful
  when you can't change the hypervisor's command line (VMware
  Workstation is the common case).

Neither method assumes any in-module `kgdb_breakpoint()` call. In both,
you break manually after 04 finishes.

**QEMU stub:** `qemu-system-x86_64 ... -gdb tcp::1234` (`-S` to pause
at boot).

**VMware debug stub:** in the VM's `.vmx`:

```
debugStub.listen.guest64 = "TRUE"
debugStub.port.guest64 = "8864"
debugStub.listen.guest64.remote = "TRUE"
debugStub.hideBreakpoints = "FALSE"
```

Then set `TARGET_DEBUG_ENDPOINT_QEMU[server]=127.0.0.1:8864`.

**KGDB over serial → TCP:** bridge the guest's `/dev/ttyS0` to a TCP
listener. For QEMU: `-serial tcp:127.0.0.1:5520,server,nowait`. For
VMware Workstation on Windows: named-pipe serial port +
`host/bridge-kgdb.ps1 -PipeName kgdb-server -Port 5520`. `TARGET_KGDB_TTY`
and `TARGET_KGDB_BAUD` in `lab.local.env` must match the GRUB args
script 01 adds.

Sanity check before F5: `nc -vz 127.0.0.1 <port>`.

---

## Stepping into kernel code

Pass `--debug-symbols` to script 01 (installs the matching `vmlinux`
debug image and the `linux-source-X` package). Script 02 syncs and
extracts both. Script 04 then asks `addr2line` where `start_kernel`
lives in `vmlinux` (always `<build-prefix>/init/main.c`) and emits the
corresponding `set substitute-path` into the generated `.gdb` file.

That's it — set a breakpoint in `vfs_read` and step through it.

Gracefully degrades: missing `vmlinux` is fatal (script 04 errors with
remediation); missing kernel source prints a one-line note and module
debugging still works (kernel step-into shows disassembly instead of
source).

---

## Undoing target-side changes

`./scripts/01-provision-target.sh <target> --uninstall` removes every
boot arg the lab added (`nokaslr`, `kgdboc=`, `sysrq_always_enabled=1`,
`maxcpus=`) and re-runs `update-grub`. Installed packages are left in
place. A timestamped GRUB backup is left on the target under
`/etc/default/grub.kmod-debug-lab.<ts>.bak`.

---

## Troubleshooting

**`sshpass: command not found`.** `TARGET_SSH_PASS` is set. On the dev
host: `sudo apt-get install -y sshpass`.

**F5 fails with `target remote ... Connection timed out`.** Endpoint
not reachable from the dev host. `nc -vz <host> <port>` to confirm. On
WSL, the Windows-host address is in `/etc/resolv.conf`'s `nameserver`
line. Re-run F5 after fixing `lab.local.env` so 04 regenerates the
`.gdb/` files.

**GDB connects but the module never loads.** Inspect
`.gdb/<target>-<method>-loader.log`. Reproduce by hand:
`ssh <target> sudo insmod /tmp/kmod-debug-lab-<user>/<name>.ko` then
`ssh <target> sudo dmesg | tail -40`.

**Breakpoints bind under `build/intermediate/...`.** Re-run F5; 04
regenerates `.gdb/current-debug.gdb` with the correct
`substitute-path`. Don't edit `.gdb/` by hand.

**VS Code shows include squiggles for `linux/module.h`.** Script 02
hasn't synced the headers yet, or you switched targets and
IntelliSense is caching old paths. Re-run script 02 and run
`C/C++: Reset IntelliSense Database`.

**`/sys/module/<name>/sections/.text` didn't appear within 30s.** Your
module failed to load, or it's slow to init. Loader log shows insmod
output. Bump with `INSMOD_WAIT_SECS=60 ./scripts/04-... ...` if needed.

**`Cannot access memory at address …` when stepping into the kernel.**
Either vmlinux is missing (re-run `01 --debug-symbols && 02`) or the
cached vmlinux is for a different kernel than the target now runs
(script 04 normally catches this — if you bypassed the guard, re-sync).

**Spaces in your repo path.** Clone to a path without spaces. GDB's
`set substitute-path FROM TO` splits on whitespace; Kbuild is also
fragile with spaces.

---

## Tool versions

Dev host: bash 5+, GNU coreutils, GDB ≥ 10, gcc ≥ 8 (for
`-ffile-prefix-map`). Target VM: Ubuntu 22.04 / 24.04 (kernel 5.15 /
6.8 tested; CI also exercises ubuntu-24.04 runners with 6.17). macOS
dev hosts need `brew install coreutils` and `make CP=gcp` for the
recursive-symlink staging.

---

## Adding a new target OS

Only `TARGET_OS[*]=ubuntu` is implemented in scripts 01 and 02. Adding
(say) Fedora means teaching script 01 to install headers / dbgsym /
source via `dnf`, and teaching script 02 to discover the header tree
paths under `/usr/src/`. Both scripts check `target_os` near the top
and fail fast on unknown values — that's where to add a backend. PRs
welcome.

---

## License

MIT — see [LICENSE](LICENSE). The lab's license does not impose terms
on your module under `module/`.
