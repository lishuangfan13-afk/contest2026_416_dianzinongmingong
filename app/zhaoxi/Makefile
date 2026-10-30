include $(APPDIR)/Make.defs
ifneq ($(CONFIG_EXAMPLES_ZHAOXI),)
PROGNAME = zhaoxi
PRIORITY = 100
STACKSIZE = 4096
MAINSRC = zhaoxi_main.c
CFLAGS += -I$(APPDIR)/packages/ai_agent/src -I$(APPDIR)/packages/ai_agent/include
include $(APPDIR)/Application.mk
endif
