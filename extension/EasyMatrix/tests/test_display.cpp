#include <Arduino.h>
#include <Adafruit_NeoMatrix.h>
#include "common.h"
#include "emotion_display.h"
#include <assert.h>

uint32_t fake_now = 1000;
std::string fake_events;
int fake_lock = 0;
int fake_deletes = 0;
FakeSerial Serial;
Adafruit_NeoMatrix matrix;
int brightness = 45;
bool drawTimeFirstTime = false;
enum CurrentPage currentPage = TIME;
int extensionDisplayBasePage(){ return (int)currentPage; }

static void resetOutput() { fake_events.clear(); Serial.output.clear(); }
int main() {
  extensionDisplayBegin();
  resetOutput();
  assert(extensionDisplayHandleLine("{\"id\":1,\"cmd\":\"ping\"}"));
  assert(fake_events == "R" && matrix.shows == 0 && !extensionDisplayActive());
  assert(Serial.output.find("\"state\":\"clock\"") != std::string::npos);
  assert(Serial.output.find("\"id\":1") != std::string::npos);
  resetOutput();
  assert(extensionDisplayHandleLine("{\"id\":2,\"cmd\":\"emotion\",\"state\":\"listening\",\"ttl_ms\":20}"));
  assert(fake_events == "SR" && matrix.shows == 1 && extensionDisplayActive());
  assert(matrix.level == 32 && brightness == 45 && currentPage == TIME);
  assert(!extensionLegacyDisplayBegin() && fake_lock == 0);
  int shows = matrix.shows;
  resetOutput();
  assert(extensionDisplayHandleLine("{\"id\":3,\"cmd\":\"emotion\",\"state\":\"wrong\"}"));
  assert(matrix.shows == shows && extensionDisplayActive());
  assert(Serial.output.find("\"ok\":false") != std::string::npos);
  assert(fake_events.find('S') == std::string::npos);
  fake_now += 19;
  assert(extensionDisplayTick());
  ++fake_now;
  assert(!extensionDisplayTick() && matrix.level == brightness && drawTimeFirstTime);
  assert(extensionLegacyDisplayBegin()); extensionLegacyDisplayEnd();
  // Wrap-safe TTL and an explicit clock restore preserve the original page/settings.
  fake_now = 0xfffffff0U;
  resetOutput();
  assert(extensionDisplayHandleLine("{\"id\":4,\"cmd\":\"emotion\",\"state\":\"sleep\",\"ttl_ms\":30}"));
  fake_now += 29;
  assert(extensionDisplayTick());
  ++fake_now;
  assert(!extensionDisplayTick());
  assert(extensionDisplayHandleLine("{\"id\":5,\"cmd\":\"emotion\",\"state\":\"idle\",\"ttl_ms\":0}"));
  fake_now += 400000U;
  assert(extensionDisplayTick());
  resetOutput();
  assert(extensionDisplayHandleLine("{\"id\":6,\"cmd\":\"emotion\",\"state\":\"clock\"}"));
  assert(fake_events == "SR" && !extensionDisplayActive() && currentPage == TIME && brightness == 45);
  assert(!extensionDisplayHandleLine("help"));
  assert(fake_lock == 0);
  int worker = 1;
  extensionDisplayDeleteTask(&worker);
  assert(fake_deletes == 1 && fake_lock == 0);
  extensionDisplayDeleteTask(nullptr);
  assert(fake_deletes == 1 && fake_lock == 0);
  puts("PASS: matrix.show before ACK, read-only status, invalid no-op, TTL/wrap/clock restore, shared lock and guarded task deletion");
}
