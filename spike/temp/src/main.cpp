// Spike 1: does Wokwi's MPU6050 report temperature through TEMP_OUT_H/L?
#include <Arduino.h>
#include <Wire.h>

static const uint8_t ADDR = 0x68;

static bool readRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(ADDR, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22, 100000);
  uint8_t who = 0;
  bool ok = readRegs(0x75, &who, 1);
  Serial.printf("SPIKE temp WHO_AM_I ok=%d value=0x%02X\n", ok, who);
  Serial.printf("SPIKE wake ok=%d\n", writeReg(0x6B, 0x00));
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  uint8_t b[2];
  if (!readRegs(0x41, b, 2)) { Serial.println("SPIKE read FAIL"); return; }
  int16_t raw = (int16_t)((b[0] << 8) | b[1]);
  int32_t centi = (int32_t)raw * 10 / 34 + 3653;   // firmware's integer formula
  float exact = raw / 340.0f + 36.53f;              // datasheet formula
  Serial.printf("SPIKE t=%lu raw=%d centi=%ld exact=%.3f\n",
                (unsigned long)last, raw, (long)centi, exact);
}
