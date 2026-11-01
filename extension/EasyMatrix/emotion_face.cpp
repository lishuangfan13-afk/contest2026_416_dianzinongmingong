#include "emotion_face.h"
#include <string.h>

// Five columns, seven rows. All faces fit in 29x7 pixels without a font library.
struct Glyph { char key; uint8_t rows[7]; };
static const Glyph glyphs[] = {
  {'(', {2,4,8,8,8,4,2}}, {')', {8,4,2,2,2,4,8}},
  {'^', {0,4,10,17,0,0,0}}, {'_', {0,0,0,0,0,0,31}},
  {'o', {0,0,14,17,17,17,14}}, {'-', {0,0,0,31,0,0,0}},
  {'v', {0,0,17,17,17,10,4}}, {'x', {0,0,17,10,4,10,17}},
  {'z', {0,0,31,2,4,8,31}}, {'.', {0,0,0,0,0,4,4}}
};

const char *emotionFaceText(EmotionMood mood, uint32_t phase_ms) {
  switch (mood) {
    case MOOD_IDLE: return phase_ms % 4000U < 180U ? "(-_-)" : "(^_^)";
    case MOOD_LISTENING: return "(o_o)";
    case MOOD_THINKING: return "(-_-)";
    case MOOD_SPEAKING: return phase_ms % 600U < 300U ? "(^o^)" : "(^-^)";
    case MOOD_SUCCESS: return "(^v^)";
    case MOOD_ERROR: return "(x_x)";
    case MOOD_SLEEP: return "(z_z)";
    default: return "";
  }
}

void emotionRaster(EmotionMood mood, uint32_t phase_ms, uint8_t pixels[256]) {
  memset(pixels, 0, 256);
  const char *text = emotionFaceText(mood, phase_ms);
  for (size_t letter = 0; text[letter]; ++letter) {
    const Glyph *glyph = nullptr;
    for (size_t index = 0; index < sizeof(glyphs) / sizeof(glyphs[0]); ++index)
      if (glyphs[index].key == text[letter]) { glyph = &glyphs[index]; break; }
    if (!glyph) continue;
    for (int row = 0; row < 7; ++row)
      for (int column = 0; column < 5; ++column)
        if (glyph->rows[row] & (1U << (4 - column)))
          pixels[row * 32 + 1 + letter * 6 + column] = 1;
  }
  if (mood == MOOD_THINKING) {
    int dots = (phase_ms / 350U) % 3U + 1;
    for (int index = 0; index < dots; ++index) pixels[7 * 32 + 12 + index * 3] = 1;
  } else if (mood == MOOD_LISTENING) {
    pixels[7 * 32 + 15] = 1;
    if (phase_ms % 600U < 300U) { pixels[7 * 32 + 12] = 1; pixels[7 * 32 + 18] = 1; }
  }
}
