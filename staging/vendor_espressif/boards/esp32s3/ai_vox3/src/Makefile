############################################################################
# vendor/espressif/boards/esp32s3/ai_vox3/src/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# Builds libboard.a for the emakefun AI-VOX3 (ESP32-S3-R8) board.
#
# Why this file exists (run#25 root cause):
#   NuttX builds board sources via "make -C board libboard$(LIBEXT)", where
#   the "board" symlink under arch/xtensa/src/ points at whatever
#   tools/Unix.mk resolved.  The vendor tree has no esp32s3/common directory,
#   so with CONFIG_ARCH_BOARD_COMMON=y the symlink pointed at the NON-EXISTENT
#   path ai_vox3/../common and the sub-make died with:
#     make[1]: *** No rule to make target 'libboard.a'.  Stop.
#   Fix (run#26): unset CONFIG_ARCH_BOARD_COMMON in defconfig (Unix.mk then
#   links board directly to this src/ directory) and provide this standard
#   board-level Makefile so the sub-make has a real rule.  The libboard.a
#   target itself (including all/depend/clean/distclean) comes from
#   $(TOPDIR)/boards/Board.mk, which only needs CSRCS + Make.defs.
#
############################################################################

include $(TOPDIR)/Make.defs

# Board source list (kept in sync with Make.defs so this file is
# self-contained regardless of who includes what)
CSRCS = board.c

# nxmutex link shim (run#31/#32): unconditionally compiled, see the file
# header for why it must never be gated by a CONFIG symbol.
CSRCS += ai_vox3_mutex_shim.c

ifeq ($(CONFIG_LCD_ST7789),y)
CSRCS += ai_vox3_lcd.c
endif

ifeq ($(CONFIG_ESP32S3_I2S),y)
CSRCS += ai_vox3_audio.c
endif

ifeq ($(CONFIG_AI_VOX3_SERVO),y)
CSRCS += ai_vox3_servo.c
endif

ifeq ($(CONFIG_AI_VOX3_WS2812),y)
CSRCS += ai_vox3_ws2812.c
endif

ifeq ($(CONFIG_AI_VOX3_BUTTONS),y)
CSRCS += ai_vox3_buttons.c
endif

ifeq ($(CONFIG_MMCSD),y)
CSRCS += ai_vox3_sd.c
endif

DEPPATH += --dep-path .
VPATH += :.

include $(TOPDIR)/boards/Board.mk
