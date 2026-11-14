// Spike 2: does Wokwi reset the ESP32 when the Task Watchdog fires,
// and does esp_reset_reason() report it as a task-watchdog reset?
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

static const char *reasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXT";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default:               return "OTHER";
  }
}

RTC_NOINIT_ATTR static uint32_t bootCount;

void setup() {
  Serial.begin(115200);
  esp_reset_reason_t r = esp_reset_reason();
  if (r == ESP_RST_POWERON) bootCount = 0;
  bootCount++;
  Serial.printf("SPIKE BOOT n=%lu reason=%s (%d)\r\n",(unsigned long)bootCount, reasonName(r), (int)r);
  if (bootCount >= 2) {
    Serial.println("SPIKE second boot reached; idling (no further hang)");
    return;
  }
  esp_task_wdt_init(3, true);   // 3 s timeout, panic (reboot) on expiry  [core 2.x API]
  esp_task_wdt_add(NULL);       // subscribe the loop task
  Serial.println("SPIKE TWDT armed (3 s, panic)");
}

void loop() {
  if (bootCount >= 2) { delay(1000); return; }
  static uint32_t start = millis();
  if (millis() - start < 5000) {   // healthy for 5 s
    esp_task_wdt_reset();
    delay(10);
    return;
  }
  // Timing check: print simulated time since the last feed every 250 ms.
  // The last line before the task_wdt error is the real timeout, measured in
  // simulated time (a stopwatch would be off if Wokwi runs slower than real time).
  static uint32_t lastFeed = 0;
  static uint32_t lastPrint = 0;
  if (lastFeed == 0) {
    lastFeed = millis();
    lastPrint = lastFeed;
    Serial.printf("SPIKE HANG: stop feeding the watchdog at t=%lu\r\n", (unsigned long)lastFeed);
  }
  if (millis() - lastPrint >= 250) {
    lastPrint = millis();
    Serial.printf("SPIKE +%lu ms without feeding\r\n", (unsigned long)(lastPrint - lastFeed));
  }
  delay(10);                       // keep running, but never feed again
}
