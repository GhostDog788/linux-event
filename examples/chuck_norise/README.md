# chuck_norise — example module

Worked example for the lab. Builds as `chuck_norise.ko` and exposes
`/dev/chuck_norise`. Reads return `chuck norise!` repeated, preserving
the file offset.

## Run it through the lab

From the repo root:

```bash
make use-example NAME=chuck_norise
```

Then follow the [top-level Quickstart](../../README.md#quickstart). After
the module loads on the target VM:

```bash
head -c 10 /dev/chuck_norise           # chuck nori

# offset-preserving reads from a single open fd
exec 9</dev/chuck_norise
dd bs=1 count=3 <&9 2>/dev/null        # chu
dd bs=1 count=5 <&9 2>/dev/null        # ck no
dd bs=1 count=7 <&9 2>/dev/null        # rise!ch
exec 9<&-
```

## What this example shows

- The conventional out-of-tree layout (`src/`, `include/`, `Makefile`).
- A `Makefile` that doubles as a Kbuild fragment AND a standalone
  wrapper (`ifndef KERNELRELEASE` clause).
- A `debug_delay_ms` module parameter so the host has time to attach
  GDB before init runs.
- A `LINUX_VERSION_CODE` shim for the 6.4 `class_create()` signature
  change — a real-world example of the compat carrying out-of-tree
  modules have to do.
