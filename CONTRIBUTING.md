# Contributing

Thanks for taking a look. This is a small project; the bar is "builds clean,
tests pass, stays readable."

## Building and testing

Everything below runs locally, no remote lab required.

```bash
# kernel module (needs linux-headers-$(uname -r))
cd code/module && make
sudo insmod event.ko          # creates /dev/event

# functional tests (need the module loaded) + the ABI drift guard
cd ../test && make
./selftest
./broadcast-storm

# example and benchmark
cd ../example && make && ./demo 5
cd ../bench   && make && ./bench

sudo rmmod event
```

`make` in `code/test` also builds `abi-check`, a `-Werror` compile that fails if
`lib/event.h` and `module/event_uapi.h` drift on the ioctl numbers. Run it
(or `make` in `code/test`) after changing the ABI in either header.

CI build-checks the module against `linux-headers-generic` and runs the full
userspace path on every push and PR (`.github/workflows/ci.yml`). The functional
tests need root and a loaded module, so run those locally before sending a
change.

## Expectations for a change

- The module loads and `selftest` + `broadcast-storm` pass, with a clean
  `dmesg` (no WARN/oops) across load/run/`rmmod`.
- If you touched the ioctl ABI, update both headers; `abi-check` must still
  compile.
- If you touched `module/event.c`, the bench should show no surprising
  regression versus the futex control (`compare.py`).

## Style

- C is formatted with the kernel style in `.clang-format`; `.editorconfig`
  covers the rest. Match the surrounding code.
- Keep code comments short; deeper design rationale belongs in `code/README.md`
  or `code/MECHANISM.md`.

## The optional lab

`scripts/`, the top-level `Makefile`, and `.vscode/` are a personal harness for
developing the module against a separate VM. It is optional and unnecessary for
the workflow above; see the lab section in `code/README.md` if you want it.
