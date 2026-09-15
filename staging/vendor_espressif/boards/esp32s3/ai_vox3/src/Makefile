##############################################################################
# src/Makefile — AI-VOX3 board logic
#
# Compiles the board-specific sources into the board library. This is the
# canonical NuttX board src/Makefile; align with the esp32s3-eye board's
# existing src/Makefile if your tree uses a different variable set.
##############################################################################

include $(TOPDIR)/.config
include $(TOPDIR)/tools/Config.mk

ASRCS =
CSRCS = board.c ai_vox3_lcd.c ai_vox3_audio.c ai_vox3_servo.c \
        ai_vox3_ws2812.c ai_vox3_buttons.c ai_vox3_sd.c

AOBJS = $(ASRCS:.S=$(OBJEXT))
COBJS = $(CSRCS:.c=$(OBJEXT))

SRCS = $(ASRCS) $(CSRCS)
OBJS = $(AOBJS) $(COBJS)

ifneq ($(CONFIG_ARCH_BOARD_AI_VOX3),)

$(AOBJS): %$(OBJEXT): %.S
	$(call ASSEMBLE, $<, $@)

$(COBJS): %$(OBJEXT): %.c
	$(call COMPILE, $<, $@)

all: $(OBJS)
	@:

endif

.PHONY: all
