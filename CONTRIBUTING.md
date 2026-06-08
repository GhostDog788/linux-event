# Contributing

Bug reports, fixes, and improvements welcome.

## What belongs here

This is a **template** for building and source-debugging out-of-tree
Linux kernel modules. Lab tooling lives in `Makefile`, `scripts/`,
`.vscode/`, `host/`. The example under `examples/chuck_norise/`
demonstrates the workflow end-to-end.

**PRs that belong here:**

- Bugs in the lab tooling: broken scripts, wrong assumptions, scripts
  that fail on re-run, unclear errors.
- New target-OS backends (currently only Ubuntu — see the `TARGET_OS`
  checks in scripts 01 and 02).
- New debug methods that fit the `<endpoint> → GDB` shape.
- Docs fixes.
- Quality-of-life: better errors, idempotent re-runs, faster sync.

**PRs that do not belong here**

- Changes to `module/` (user's slot; stays as just a placeholder README).
- New examples under `examples/` (one focused example beats many).
- Project-management features (work logs, task tracking).

## Reporting bugs

Open an issue with:

- Dev host (distro, version, kernel) and target VM (distro, version,
  kernel, hypervisor).
- Debug method tried (`kgdb` or `qemu`) and endpoint configured.
- Command run and full output. For deploy failures, attach
  `.gdb/<target>-<method>-loader.log`.
- What you expected vs what happened.

## Submitting changes

1. Fork, branch from `main`.
2. Focused commits — one logical change per commit.
3. Keep scripts shellcheck-clean and `bash -n`-clean:
   `shellcheck -x scripts/lib/common.sh scripts/0*.sh` and
   `for f in scripts/lib/*.sh scripts/0*.sh; do bash -n "$f"; done`.
4. Update docs in the same PR — if you touched a script, check
   `README.md` and `module/README.md` for anything the change made
   inaccurate.
5. PR description explains *why*, not just what.

## Style

- **Shell:** tabs for indent. `set -euo pipefail` at the top. Errors via
  the `die` helper. Use `lab_*` helpers from `scripts/lib/common.sh` —
  don't call ssh/scp/sshpass binaries directly from numbered scripts.
- **Makefile:** tabs for recipes; explicit `.PHONY`.
- **Markdown:** wrap at ~80; fenced code blocks with language tags.
- **JSON / YAML:** 2-space indent, trailing newline.

`.editorconfig` captures these — most editors apply it automatically.

## Testing locally

No automated test suite for the build/debug flow (it needs real VMs).
Before sending a PR, smoke-test the full chain against at least one
target VM: `01 --debug-symbols` → reboot → `02` → `03` → `04` → F5 in
VS Code → set breakpoints in both your module and a kernel function,
confirm both bind.
