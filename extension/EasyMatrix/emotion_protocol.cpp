#include "emotion_protocol.h"
#include <string.h>
#include <limits.h>

static const char *const moods[] = {
  "idle", "listening", "thinking", "speaking", "success", "error", "sleep", "clock"
};

const char *emotionMoodName(EmotionMood mood) {
  return mood >= MOOD_IDLE && mood < MOOD_COUNT ? moods[mood] : "clock";
}

const char *emotionCommandName(EmotionCommand command) {
  return command == EMOTION_PING ? "ping" : command == EMOTION_STATUS ? "status" : "emotion";
}

namespace {
struct Reader {
  const char *cursor;
  void spaces() {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
  }
  bool take(char expected) {
    spaces();
    if (*cursor != expected) return false;
    ++cursor;
    return true;
  }
  bool text(char *out, size_t capacity) {
    if (!take('"')) return false;
    size_t used = 0;
    while (*cursor && *cursor != '"') {
      unsigned char ch = (unsigned char)*cursor++;
      if (ch < 32 || ch > 126 || ch == '\\' || used + 1 >= capacity) return false;
      out[used++] = (char)ch;
    }
    if (*cursor != '"') return false;
    ++cursor;
    out[used] = 0;
    return true;
  }
  bool number(uint32_t &value) {
    spaces();
    if (*cursor < '0' || *cursor > '9') return false;
    if (*cursor == '0' && cursor[1] >= '0' && cursor[1] <= '9') return false;
    value = 0;
    do {
      unsigned digit = (unsigned)(*cursor++ - '0');
      if (value > (UINT32_MAX - digit) / 10U) return false;
      value = value * 10U + digit;
    } while (*cursor >= '0' && *cursor <= '9');
    return true;
  }
};
}

const char *emotionParse(const char *line, EmotionRequest &result) {
  if (!line || strlen(line) > EMOTION_LINE_MAX) return "frame_too_long";
  result = EmotionRequest{0, EMOTION_TTL_DEFAULT, EMOTION_PING, MOOD_CLOCK};
  Reader reader{line};
  if (!reader.take('{')) return "invalid_json";
  bool have_id = false, have_cmd = false, have_state = false, have_ttl = false;
  if (reader.take('}')) return "missing_id";
  while (true) {
    char key[16];
    if (!reader.text(key, sizeof(key)) || !reader.take(':')) return "invalid_json";
    if (!strcmp(key, "id")) {
      if (have_id) return "duplicate_field";
      have_id = true;
      if (!reader.number(result.id)) return "invalid_id";
    } else if (!strcmp(key, "cmd")) {
      if (have_cmd) return "duplicate_field";
      have_cmd = true;
      char command[12];
      if (!reader.text(command, sizeof(command))) return "invalid_cmd";
      if (!strcmp(command, "ping")) result.command = EMOTION_PING;
      else if (!strcmp(command, "status")) result.command = EMOTION_STATUS;
      else if (!strcmp(command, "emotion")) result.command = EMOTION_SET;
      else return "unknown_cmd";
    } else if (!strcmp(key, "state")) {
      if (have_state) return "duplicate_field";
      have_state = true;
      char state[16];
      if (!reader.text(state, sizeof(state))) return "invalid_state";
      bool found = false;
      for (int index = 0; index < MOOD_COUNT; ++index) {
        if (!strcmp(state, moods[index])) {
          result.mood = (EmotionMood)index;
          found = true;
          break;
        }
      }
      if (!found) return "unknown_state";
    } else if (!strcmp(key, "ttl_ms")) {
      if (have_ttl) return "duplicate_field";
      have_ttl = true;
      if (!reader.number(result.ttl_ms) || result.ttl_ms > EMOTION_TTL_MAX) return "invalid_ttl";
    } else {
      return "unknown_field";
    }
    if (reader.take('}')) break;
    if (!reader.take(',')) return "invalid_json";
  }
  reader.spaces();
  if (*reader.cursor) return "trailing_data";
  if (!have_id) return "missing_id";
  if (!have_cmd) return "missing_cmd";
  if (result.command == EMOTION_SET) {
    if (!have_state) return "missing_state";
  } else if (have_state || have_ttl) {
    return "unexpected_field";
  }
  return nullptr;
}
