include $(APPDIR)/Make.defs
ifneq ($(CONFIG_EXAMPLES_ZHAOXI),)
PROGNAME = zhaoxi
PRIORITY = 100
STACKSIZE = 32768
MAINSRC = zhaoxi_main.c
CSRCS += todo_store.c journal.c local_intent.c
ifeq ($(CONFIG_ZHAOXI_BES_AUDIO),y)
CSRCS += bes_audio.c bes_clock.c reminder.c drink.c dns_diag.c
CSRCS += morning.c morning_store.c morning_net.c
CSRCS += notifications/feishu_notify.c notifications/feishu_notify_sdk.c
endif
ifeq ($(CONFIG_ZHAOXI_MIMO_VOICE),y)
CSRCS += mimo_voice.c mimo_sse.c
endif
ifeq ($(CONFIG_NETUTILS_PPPD),y)
CSRCS += pc_link.c pc_control.c
endif
CFLAGS += -I$(APPDIR)/packages/ai_agent/src -I$(APPDIR)/packages/ai_agent/include
CFLAGS += -I$(APPDIR)/netutils/cjson/cJSON -I$(APPDIR)/crypto/mbedtls/mbedtls/include
include $(APPDIR)/Application.mk
endif
