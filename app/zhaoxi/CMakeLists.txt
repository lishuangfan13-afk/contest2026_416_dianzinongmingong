if(CONFIG_EXAMPLES_ZHAOXI)
  nuttx_add_application(
    NAME zhaoxi
    PRIORITY 100
    STACKSIZE 4096
    MODULE ${CONFIG_EXAMPLES_ZHAOXI}
    INCLUDE_DIRECTORIES
      ${NUTTX_APPS_DIR}/packages/ai_agent/src
      ${NUTTX_APPS_DIR}/packages/ai_agent/include
    SRCS zhaoxi_main.c)
endif()
