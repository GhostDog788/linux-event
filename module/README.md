# module/ — your kernel module goes here

The lab's only slot for the module being built and debugged. The repo is
otherwise generic — it doesn't know your module's name, source layout,
or what it does.

**Contract:** see the [top-level README](../README.md#the-module-contract).
TL;DR: a standard out-of-tree `Makefile` (or `Kbuild`) with one `obj-m`
entry; sources anywhere you like; an optional `debug_delay_ms` module
parameter so the host has time to attach GDB.

## Try the example

```bash
make use-example NAME=chuck_norise   # copy examples/chuck_norise/ into module/
make clean-module                    # reset module/ to just this README
```

## One caveat: no symlinks inside `module/`

The lab stages your sources into `build/intermediate/<target>/<kernel>/`
as a tree of absolute symlinks (`cp -as`) and runs Kbuild there. If
`module/` contains its OWN symlinks, the staged copy ends up with
symlinks-to-symlinks; the DWARF prefix-map remap (which assumes sources
live under `module/`) won't produce the paths your IDE expects. Keep
`module/` symlink-free, or copy in real files for external sources you
need.
