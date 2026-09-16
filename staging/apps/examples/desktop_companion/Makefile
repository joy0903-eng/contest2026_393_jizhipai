##############################################################################
# apps/examples/desktop_companion/Makefile
#
# NuttX/OpenVela application Makefile for the AI-VOX3 desktop companion.
# Registers the app as a builtin (see Kconfig + CONFIG_EXAMPLES_DESKTOP_COMPANION).
##############################################################################

include $(APPDIR)/Make.defs

# Application registration — NuttX apps uses PROGNAME (NOT APPNAME).
# Application.mk only reads PROGNAME (no default): with APPNAME set instead,
# PROGNAME_$(target) is never mapped, the MAINCOBJ rule still injects
# "-Dmain=$(addsuffix _main,$(PROGNAME_$@))" and addsuffix on an empty list
# yields EMPTY -> -Dmain= makes main() an empty macro -> "expected identifier
# or '(' before 'int'" at main.c:151 (run#23). PROGNAME also drives builtin
# registration (desktop_companion_main), PRIORITY/STACKSIZE fall back to the
# last list entry but the official examples set them explicitly.
PROGNAME  = desktop_companion
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = CONFIG_DEFAULT_TASK_STACKSIZE

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
