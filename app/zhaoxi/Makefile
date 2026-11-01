include $(APPDIR)/Make.defs
ifneq ($(CONFIG_EXAMPLES_ZHAOXI),)
PROGNAME = zhaoxi
PRIORITY = 100
STACKSIZE = 32768
MAINSRC = zhaoxi_main.c
ifeq ($(CONFIG_ZHAOXI_BES_AUDIO),y)
CSRCS += bes_audio.c bes_clock.c reminder.c dns_diag.c
endif
ifeq ($(CONFIG_ZHAOXI_MIMO_VOICE),y)
CSRCS += mimo_voice.c
endif
ifeq ($(CONFIG_NETUTILS_PPPD),y)
CSRCS += pc_link.c pc_control.c
endif
CFLAGS += -I$(APPDIR)/packages/ai_agent/src -I$(APPDIR)/packages/ai_agent/include
CFLAGS += -I$(APPDIR)/netutils/cjson/cJSON -I$(APPDIR)/crypto/mbedtls/mbedtls/include
include $(APPDIR)/Application.mk
endif
