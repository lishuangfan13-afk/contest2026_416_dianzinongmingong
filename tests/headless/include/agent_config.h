#pragma once
#define AGENT_BUS_QUEUE_LEN 16
#define AGENT_BUS_PUSH_TIMEOUT_MS 25
#ifndef AGENT_DATA_DIR
#define AGENT_DATA_DIR "/tmp/zhaoxi-headless-test"
#endif
#define AGENT_MEMORY_DIR AGENT_DATA_DIR "/memory"
#define AGENT_SKILLS_DIR AGENT_DATA_DIR "/skills/"
#define AGENT_CRON_FILE AGENT_DATA_DIR "/cron.json"
#define AGENT_CRON_FILE_MAX_SIZE (64 * 1024)
#define AGENT_CRON_MAX_JOBS 16
#define AGENT_CRON_STACK 8192
#define AGENT_CRON_PRIO 40
#define AGENT_CRON_CHECK_INTERVAL_MS 10000
#define AGENT_CRON_ID_LEN 9
#define AGENT_CHAN_SYSTEM "system"
#define AGENT_CHAN_FEISHU "feishu"
#define AGENT_CHAN_VOICE "voice"
