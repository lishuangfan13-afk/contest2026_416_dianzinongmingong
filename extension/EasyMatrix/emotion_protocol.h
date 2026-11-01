#ifndef ZHAOXI_EMOTION_PROTOCOL_H
#define ZHAOXI_EMOTION_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

// A deliberately small, flat JSON protocol: ASCII keys/values, uint32 numbers.
// No heap allocation, string escapes, nested values or credentials are needed.
static const size_t EMOTION_LINE_MAX = 192;
static const uint32_t EMOTION_TTL_DEFAULT = 30000;
static const uint32_t EMOTION_TTL_MAX = 300000;

enum EmotionCommand { EMOTION_PING, EMOTION_STATUS, EMOTION_SET };
enum EmotionMood { MOOD_IDLE, MOOD_LISTENING, MOOD_THINKING, MOOD_SPEAKING,
                   MOOD_SUCCESS, MOOD_ERROR, MOOD_SLEEP, MOOD_CLOCK, MOOD_COUNT };

struct EmotionRequest {
  uint32_t id;
  uint32_t ttl_ms;
  EmotionCommand command;
  EmotionMood mood;
};

const char *emotionMoodName(EmotionMood mood);
const char *emotionCommandName(EmotionCommand command);
// Returns nullptr on success, or a fixed ASCII error code on invalid input.
const char *emotionParse(const char *line, EmotionRequest &result);

#endif
