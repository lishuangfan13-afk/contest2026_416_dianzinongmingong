#ifndef ZHAOXI_EMOTION_FACE_H
#define ZHAOXI_EMOTION_FACE_H
#include "emotion_protocol.h"

// Exact 32x8 raster shared by target renderer and host preview/tests.
void emotionRaster(EmotionMood mood, uint32_t phase_ms, uint8_t pixels[256]);
const char *emotionFaceText(EmotionMood mood, uint32_t phase_ms);
#endif
