# Top-level wrapper for the event.ko project under code/module/.
#
# Builds out-of-tree against a target-specific kernel header tree under
# kernel-cache/<target>/. Sources are staged into
# build/intermediate/<target>/<kernel>/ before Kbuild runs from there,
# and `-ffile-prefix-map=$(INTERMEDIATE_DIR)=$(MODULE_DIR)` is injected
# via KCFLAGS so DWARF paths and `__FILE__` resolve back to code/module/ —
# IDE breakpoints keyed by absolute path bind to the real sources.
#
# Per-target staging also keeps each (target, kernel) pair's object
# files isolated. The code/module/Makefile stays a normal Kbuild file
# with no knowledge of the staging.

HOST_KERNEL    := $(shell uname -r)
CURRENT_CACHE  := $(CURDIR)/kernel-cache/current
CURRENT_TARGET := $(shell if [ -e "$(CURRENT_CACHE)" ]; then basename "$$(readlink -f "$(CURRENT_CACHE)")"; else printf local; fi)
CURRENT_KERNEL := $(shell if [ -f "$(CURRENT_CACHE)/kernel.release" ]; then cat "$(CURRENT_CACHE)/kernel.release"; else printf '$(HOST_KERNEL)'; fi)

KDIR             ?= $(if $(wildcard $(CURRENT_CACHE)/build),$(CURRENT_CACHE)/build,/lib/modules/$(HOST_KERNEL)/build)
BUILD_ID         ?= $(CURRENT_TARGET)/$(CURRENT_KERNEL)
BUILD_ROOT       ?= $(CURDIR)/build
INTERMEDIATE_DIR ?= $(BUILD_ROOT)/intermediate/$(BUILD_ID)
ARTIFACT_DIR     ?= $(BUILD_ROOT)/artifacts/$(BUILD_ID)
MODULE_DIR       ?= $(CURDIR)/code/module

EXTRA_CCFLAGS  ?= -g -DDEBUG
KCFLAGS_INJECT := -ffile-prefix-map=$(INTERMEDIATE_DIR)=$(MODULE_DIR) $(EXTRA_CCFLAGS)

.PHONY: all modules prepare-build clean help

all: modules

help:
	@echo "Top-level lab Makefile."
	@echo ""
	@echo "Normally invoked by scripts/03-build-module.sh <target>, which sets"
	@echo "KDIR, BUILD_ID, INTERMEDIATE_DIR, and ARTIFACT_DIR per-target."
	@echo ""
	@echo "Build targets:"
	@echo "  modules                          build code/module against KDIR (default)"
	@echo "  clean                            remove BUILD_ID's intermediate + artifact dirs"
	@echo "  prepare-build                    stage code/module into INTERMEDIATE_DIR (internal)"
	@echo ""
	@echo "Vars (auto-resolved when kernel-cache/current points at a target):"
	@echo "  MODULE_DIR        $(MODULE_DIR)"
	@echo "  KDIR              $(KDIR)"
	@echo "  BUILD_ID          $(BUILD_ID)"
	@echo "  INTERMEDIATE_DIR  $(INTERMEDIATE_DIR)"
	@echo "  ARTIFACT_DIR      $(ARTIFACT_DIR)"
	@echo "  EXTRA_CCFLAGS     $(EXTRA_CCFLAGS)  (appended to KCFLAGS)"


modules: prepare-build
	$(MAKE) -C "$(KDIR)" M="$(INTERMEDIATE_DIR)" KCFLAGS="$(KCFLAGS_INJECT)" modules
	@mkdir -p "$(ARTIFACT_DIR)"
	@set -e; \
	kos=$$(find "$(INTERMEDIATE_DIR)" -maxdepth 2 -name '*.ko' -type f); \
	if [ -z "$$kos" ]; then \
		echo "Makefile: no .ko produced under $(INTERMEDIATE_DIR);" >&2; \
		echo "  check the Kbuild output above for compile errors." >&2; \
		exit 1; \
	fi; \
	for ko in $$kos; do \
		name=$$(basename "$$ko"); \
		mv "$$ko" "$(ARTIFACT_DIR)/$$name"; \
		echo "built $(ARTIFACT_DIR)/$$name"; \
	done

# `cp -as` recursively creates dirs and absolute symlinks for each file.
# Re-runs are idempotent: we wipe the symlink scaffolding first so a
# removed source file doesn't linger as a stale symlink.
prepare-build:
	@if [ ! -e "$(MODULE_DIR)/Makefile" ] && [ ! -e "$(MODULE_DIR)/Kbuild" ]; then \
		echo "Makefile: no Makefile or Kbuild under $(MODULE_DIR);" >&2; \
		echo "  this lab builds the event.ko project at code/module (see code/README.md)" >&2; \
		exit 1; \
	fi
	@cp --help 2>&1 | grep -q -- '--symbolic-link' || { \
		echo "Makefile: GNU 'cp' (with --symbolic-link, --archive) is required;" >&2; \
		echo "  the lab targets Debian-based hosts. On macOS: brew install coreutils, then re-run with CP=gcp." >&2; \
		exit 1; \
	}
	@mkdir -p "$(INTERMEDIATE_DIR)"
	@find "$(INTERMEDIATE_DIR)" -depth -type l -delete
	@find "$(INTERMEDIATE_DIR)" -depth -type d -empty -not -path "$(INTERMEDIATE_DIR)" -delete
	@cp -as "$(MODULE_DIR)/." "$(INTERMEDIATE_DIR)/"

clean:
	@if [ -d "$(INTERMEDIATE_DIR)" ] && { [ -e "$(INTERMEDIATE_DIR)/Makefile" ] || [ -e "$(INTERMEDIATE_DIR)/Kbuild" ]; }; then \
		$(MAKE) -C "$(KDIR)" M="$(INTERMEDIATE_DIR)" clean || true; \
	fi
	rm -rf "$(INTERMEDIATE_DIR)" "$(ARTIFACT_DIR)"
