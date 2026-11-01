#include "emotion_protocol.h"
#include "emotion_face.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void good(const char *line, EmotionCommand command, uint32_t id,
                 EmotionMood mood = MOOD_CLOCK, uint32_t ttl = EMOTION_TTL_DEFAULT) {
  EmotionRequest request;
  const char *error = emotionParse(line, request);
  if (error) fprintf(stderr, "Unexpected parse failure: %s\n", error);
  assert(!error && request.command == command && request.id == id);
  assert(request.mood == mood && request.ttl_ms == ttl);
}

int main(int argc, char **argv) {
  good("{\"id\":1,\"cmd\":\"ping\"}", EMOTION_PING, 1);
  good("  { \"cmd\": \"status\", \"id\": 4294967295 } \r\n", EMOTION_STATUS, UINT32_MAX);
  good("{\"state\":\"listening\",\"ttl_ms\":0,\"id\":0,\"cmd\":\"emotion\"}", EMOTION_SET, 0, MOOD_LISTENING, 0);
  good("{\"id\":9,\"cmd\":\"emotion\",\"state\":\"clock\",\"ttl_ms\":300000}", EMOTION_SET, 9, MOOD_CLOCK, 300000);
  const char *bad[] = {
    "{}", "[]", "{\"cmd\":\"ping\"}", "{\"id\":1}",
    "{\"id\":-1,\"cmd\":\"ping\"}", "{\"id\":1.0,\"cmd\":\"ping\"}",
    "{\"id\":01,\"cmd\":\"ping\"}", "{\"id\":4294967296,\"cmd\":\"ping\"}",
    "{\"id\":1,\"cmd\":\"ping\",\"id\":2}", "{\"id\":1,\"cmd\":\"ping\",\"cmd\":\"status\"}",
    "{\"id\":1,\"cmd\":\"reboot\"}", "{\"id\":1,\"cmd\":\"ping\",\"other\":3}",
    "{\"id\":1,\"cmd\":\"ping\",\"state\":\"idle\"}",
    "{\"id\":1,\"cmd\":\"emotion\"}",
    "{\"id\":1,\"cmd\":\"emotion\",\"state\":\"bad\"}",
    "{\"id\":1,\"cmd\":\"emotion\",\"state\":\"idle\",\"ttl_ms\":300001}",
    "{\"id\":1,\"cmd\":\"emotion\",\"state\":\"idle\",\"ttl_ms\":0.5}",
    "{\"id\":1,\"cmd\":\"ping\"} extra", "{\"id\":1,\"cmd\":\"ping\",}",
    "{\"id\":1,\"cmd\":\"p\\u0069ng\"}", "{\"id\":1,\"cmd\":true}"
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    EmotionRequest request;
    if (!emotionParse(bad[i], request)) {
      fprintf(stderr, "Accepted invalid test %zu\n", i);
      assert(false);
    }
  }
  unsigned lit_counts[MOOD_COUNT] = {0};
  for (int mood = MOOD_IDLE; mood < MOOD_COUNT; ++mood) {
    char line[160];
    snprintf(line, sizeof(line), "{\"id\":4,\"cmd\":\"emotion\",\"state\":\"%s\"}", emotionMoodName((EmotionMood)mood));
    good(line, EMOTION_SET, 4, (EmotionMood)mood);
    struct { uint8_t before[16]; uint8_t pixels[256]; uint8_t after[16]; } image;
    memset(&image, 0xa5, sizeof(image));
    emotionRaster((EmotionMood)mood, 500, image.pixels);
    for (int index = 0; index < 16; ++index) assert(image.before[index] == 0xa5 && image.after[index] == 0xa5);
    for (int index = 0; index < 256; ++index) { assert(image.pixels[index] <= 1); lit_counts[mood] += image.pixels[index]; }
    if (mood != MOOD_CLOCK) assert(lit_counts[mood] > 15 && lit_counts[mood] < 130);
    else assert(lit_counts[mood] == 0);
  }
  // Deterministic malformed-input smoke: parser never accepts non-JSON noise.
  srand(7);
  for (unsigned iteration = 0; iteration < 6000; ++iteration) {
    char line[220];
    size_t length = (size_t)(rand() % 219);
    for (size_t index = 0; index < length; ++index) line[index] = (char)('A' + rand() % 26);
    line[length] = 0;
    EmotionRequest request;
    assert(emotionParse(line, request));
  }
  if (argc == 3 && !strcmp(argv[1], "--preview")) {
    FILE *file = fopen(argv[2], "wb");
    assert(file);
    fputs("[", file);
    for (int mood = MOOD_IDLE; mood < MOOD_CLOCK; ++mood) {
      uint8_t pixels[256];
      emotionRaster((EmotionMood)mood, 500, pixels);
      fprintf(file, "%s{\"state\":\"%s\",\"face\":\"%s\",\"pixels\":[", mood ? "," : "",
              emotionMoodName((EmotionMood)mood), emotionFaceText((EmotionMood)mood, 500));
      for (int index = 0; index < 256; ++index) fprintf(file, "%s%u", index ? "," : "", pixels[index]);
      fputs("]}", file);
    }
    fputs("]\n", file);
    fclose(file);
  }
  puts("PASS: parser valid/error/bounds cases, all seven 32x8 faces, guard bytes, 6000 malformed frames");
  return 0;
}
