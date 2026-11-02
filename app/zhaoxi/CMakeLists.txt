if(CONFIG_EXAMPLES_ZHAOXI)
  set(ZHAOXI_SRCS zhaoxi_main.c todo_store.c journal.c local_intent.c)
  if(CONFIG_ZHAOXI_BES_AUDIO)
    list(APPEND ZHAOXI_SRCS bes_audio.c bes_clock.c reminder.c drink.c dns_diag.c
         morning.c morning_store.c morning_net.c
         notifications/feishu_notify.c notifications/feishu_notify_sdk.c)
  endif()
  if(CONFIG_ZHAOXI_MIMO_VOICE)
    list(APPEND ZHAOXI_SRCS mimo_voice.c mimo_sse.c)
  endif()
  if(CONFIG_NETUTILS_PPPD)
    list(APPEND ZHAOXI_SRCS pc_link.c pc_control.c)
  endif()
  nuttx_add_application(
    NAME zhaoxi
    PRIORITY 100
    STACKSIZE 32768
    MODULE ${CONFIG_EXAMPLES_ZHAOXI}
    INCLUDE_DIRECTORIES
      ${NUTTX_APPS_DIR}/packages/ai_agent/src
      ${NUTTX_APPS_DIR}/packages/ai_agent/include
      ${NUTTX_APPS_DIR}/netutils/cjson/cJSON
      ${NUTTX_APPS_DIR}/crypto/mbedtls/mbedtls/include
    SRCS ${ZHAOXI_SRCS})
endif()
