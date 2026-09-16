############################################################################
# vendor/espressif/boards/esp32s3/common/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# Board-common build shim for the OpenVela vendor Make flow.
#
# Why this directory must be REAL and not a stub (run#25/#26 root cause):
#   OpenVela's tools/Config.mk assigns BOARD_COMMON_DIR = $(BOARD_DIR)/../common
#   whenever that directory EXISTS -- the else-branch has NO CONFIG_ARCH_BOARD_COMMON
#   check.  The stub "common/configs" created for configure's process_config.sh
#   therefore made BOARD_COMMON_DIR resolve to the stub, so
#     - scripts/Make.defs' ARCHSCRIPT ($(BOARD_COMMON_DIR)/scripts/*.ld) pointed
#       at non-existent linker scripts, and
#     - Unix.mk linked arch/xtensa/src/board -> here, so "make -C board
#       libboard.a" had no rule.
#   run#26 died with BOTH "No rule to make target" errors.
#
# This Makefile is the target of "make -C board libboard.a" (the board symlink
# points here).  It mirrors the in-tree common delegate structure:
#   - "board/Make.defs" resolves through the board/board symlink (created by
#     Unix.mk inside this directory) to the per-board ai_vox3/src/Make.defs,
#     which declares the conditional per-board CSRCS.
#   - $(TOPDIR)/boards/Board.mk owns the libboard$(LIBEXT) rule and the
#     all/depend/clean/distclean targets.
#
# scripts/*.ld are copied at CI time from the in-tree nuttx
# boards/xtensa/esp32s3/common/scripts (see .github/workflows/build.yml), so
# they always match the nuttx being built.
############################################################################

include $(TOPDIR)/Make.defs
include board/Make.defs
include src/Make.defs

DEPPATH += --dep-path board
DEPPATH += --dep-path src

include $(TOPDIR)/boards/Board.mk

ARCHSRCDIR = $(TOPDIR)$(DELIM)arch$(DELIM)$(CONFIG_ARCH)$(DELIM)src
BOARDDIR = $(ARCHSRCDIR)$(DELIM)board
CFLAGS += ${INCDIR_PREFIX}$(BOARDDIR)$(DELIM)include
