##############################################################################
# apps/examples/desktop_companion/Makefile
#
# NuttX/OpenVela application Makefile for the AI-VOX3 desktop companion.
# Registers the app as a builtin (see Kconfig + CONFIG_EXAMPLES_DESKTOP_COMPANION).
##############################################################################

include $(APPDIR)/Make.defs

# Application name (binary + builtin registration)
APPNAME  = desktop_companion

# Source files
MAINSRC  = main.c
CSRCS    = ui_lvgl.c net_services.c llm_client.c http_util.c \
           audio_pipeline.c face_follow.c smart_home.c

# Include path (this directory holds the local headers)
CFLAGS  += ${INCDIR}

# Suppress unused warnings during early bring-up; tighten later.
CFLAGS  += -Wno-unused-variable

ifeq ($(CONFIG_EXAMPLES_DESKTOP_COMPANION),y)
  # Build the builtin application
  $(info "Building desktop_companion")
endif

include $(APPDIR)/Application.mk
