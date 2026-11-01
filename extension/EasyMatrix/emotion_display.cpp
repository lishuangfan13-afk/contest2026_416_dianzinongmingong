#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "emotion_display.h"
#include "emotion_protocol.h"
#include "emotion_face.h"
#include "light.h"

static SemaphoreHandle_t display_mutex = nullptr;
static volatile bool overlay_active = false;
static EmotionMood current_mood = MOOD_CLOCK;
static uint32_t applied_at = 0, duration_ms = 0, last_render = 0, frame_number = 0;
static const char *const firmware = "zhaoxi-display-1.0.0";

static void lockDisplay() {
  if (display_mutex) xSemaphoreTake(display_mutex, portMAX_DELAY);
}
static void unlockDisplay() {
  if (display_mutex) xSemaphoreGive(display_mutex);
}
void extensionDisplayBegin() {
  if (!display_mutex) display_mutex = xSemaphoreCreateMutex();
}
bool extensionDisplayActive() { return overlay_active; }

bool extensionLegacyDisplayBegin() {
  lockDisplay();
  if (overlay_active) { unlockDisplay(); return false; }
  return true;
}
void extensionLegacyDisplayEnd() { unlockDisplay(); }

void extensionDisplayDeleteTask(void *handle) {
  // Never delete a legacy renderer while it owns the display mutex.
  // All callers are the loop/setup task stopping a separate worker.
  if (!handle || (TaskHandle_t)handle == xTaskGetCurrentTaskHandle()) return;
  lockDisplay();
  vTaskDelete((TaskHandle_t)handle);
  unlockDisplay();
}

static uint16_t colorForMood() {
  switch (current_mood) {
    case MOOD_LISTENING: return matrix.Color(0, 210, 255);
    case MOOD_THINKING: return matrix.Color(255, 150, 20);
    case MOOD_SPEAKING: return matrix.Color(120, 210, 255);
    case MOOD_SUCCESS: return matrix.Color(50, 255, 100);
    case MOOD_ERROR: return matrix.Color(255, 50, 55);
    case MOOD_SLEEP: return matrix.Color(90, 90, 170);
    default: return matrix.Color(170, 100, 255);
  }
}

// Caller owns the mutex. A frame is acknowledged only after matrix.show().
static void renderLocked(uint32_t now) {
  uint8_t pixels[256];
  emotionRaster(current_mood, now - applied_at, pixels);
  matrix.fillScreen(0);
  // Runtime-only brightness: visible but bounded. The clock's NVS value is untouched.
  uint8_t level = (uint8_t)constrain(brightness, 12, 32);
  matrix.setBrightness(level);
  uint16_t color = colorForMood();
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 32; ++x)
      if (pixels[y * 32 + x]) matrix.drawPixel(x, y, color);
  matrix.show();
  ++frame_number;
  last_render = now;
}

static void releaseLocked() {
  overlay_active = false;
  current_mood = MOOD_CLOCK;
  duration_ms = 0;
  drawTimeFirstTime = true;
  matrix.fillScreen(0);
  matrix.setBrightness(brightness);
  matrix.show();
  ++frame_number;
}

void extensionDisplayRelease() {
  lockDisplay();
  if (overlay_active) releaseLocked();
  unlockDisplay();
}

bool extensionDisplayTick() {
  uint32_t now = millis();
  lockDisplay();
  if (overlay_active && duration_ms && (uint32_t)(now - applied_at) >= duration_ms) releaseLocked();
  if (overlay_active && (uint32_t)(now - last_render) >= 120U) renderLocked(now);
  bool active = overlay_active;
  unlockDisplay();
  return active;
}

void extensionDisplayProtocolError(const char *error) {
  Serial.printf("{\"type\":\"zhaoxi.display\",\"v\":1,\"id\":null,\"ok\":false,\"error\":\"%s\"}\n", error);
}

static void respond(const EmotionRequest &request) {
  Serial.printf("{\"type\":\"zhaoxi.display\",\"v\":1,\"id\":%lu,\"ok\":true,\"cmd\":\"%s\","
                "\"device\":\"EasyMatrix\",\"fw\":\"%s\",\"width\":32,\"height\":8,"
                "\"state\":\"%s\",\"active\":%s,\"frame\":%lu,\"uptime_ms\":%lu,\"base_page\":%d,\"ttl_ms\":%lu}\n",
                (unsigned long)request.id, emotionCommandName(request.command), firmware,
                emotionMoodName(current_mood), overlay_active ? "true" : "false",
                (unsigned long)frame_number, (unsigned long)millis(), extensionDisplayBasePage(), (unsigned long)duration_ms);
}

bool extensionDisplayHandleLine(const char *line) {
  while (*line == ' ' || *line == '\t') ++line;
  if (*line != '{') return false;
  EmotionRequest request;
  const char *error = emotionParse(line, request);
  if (error) { extensionDisplayProtocolError(error); return true; }
  if (request.command == EMOTION_SET) {
    if (!display_mutex) { extensionDisplayProtocolError("display_not_ready"); return true; }
    lockDisplay();
    if (request.mood == MOOD_CLOCK) {
      releaseLocked();
    } else {
      current_mood = request.mood;
      applied_at = millis();
      duration_ms = request.ttl_ms;
      overlay_active = true;
      renderLocked(applied_at);
    }
    unlockDisplay();
  }
  respond(request);
  return true;
}
