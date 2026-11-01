#pragma once
#include <stdint.h>
#include <string>
#include <stdarg.h>
#include <stdio.h>
using String = std::string;
using byte = uint8_t;
using TaskHandle_t = void *;
#define PROGMEM
extern uint32_t fake_now;
extern std::string fake_events;
inline uint32_t millis() { return fake_now; }
template <typename T> T constrain(T value, T low, T high) { return value < low ? low : value > high ? high : value; }
struct FakeSerial {
  std::string output;
  void print(const char *text) { fake_events += 'R'; output += text; }
  void println(const char *text) { print(text); output += '\n'; }
  void printf(const char *format, ...) {
    char buffer[1024]; va_list args; va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args); va_end(args); print(buffer);
  }
};
extern FakeSerial Serial;
extern int fake_deletes;
extern int fake_lock;
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return nullptr; }
inline void vTaskDelete(TaskHandle_t handle) { if (handle && fake_lock == 1) ++fake_deletes; }
