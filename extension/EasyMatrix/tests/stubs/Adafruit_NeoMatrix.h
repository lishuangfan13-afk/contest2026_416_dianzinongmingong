#pragma once
#include <Arduino.h>
#include <assert.h>
extern int fake_lock;
class Adafruit_NeoMatrix {
 public:
  uint16_t pixels[256] = {0};
  int shows = 0, level = 0;
  uint16_t Color(int r, int g, int b) { return (uint16_t)((r & 248) << 8 | (g & 252) << 3 | b >> 3); }
  void fillScreen(uint16_t value) { for (auto &pixel: pixels) pixel = value; }
  void setBrightness(int value) { level = value; }
  void drawPixel(int x, int y, uint16_t value) { assert(x >= 0 && x < 32 && y >= 0 && y < 8); pixels[y*32+x] = value; }
  void show() { assert(fake_lock == 1); ++shows; fake_events += 'S'; }
};
