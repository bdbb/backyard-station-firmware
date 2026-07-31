// I2C scanner
// expect 0x23 BH1750, 0x44 SHT31, 0x53 LTR390, 0x62 SCD41, 0x77 BMP3xx
// Board: ESP32-S3 Dev Module, USB CDC On Boot enabled
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  delay(2000);
  Wire.begin(4, 5);
  Serial.println("Scanning I2C... v3 pins 4/5");
  for (byte a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print("Found device at 0x");
      Serial.println(a, HEX);
    }
  }
  Serial.println("Scan done.");
}

void loop() { delay(5000); ESP.restart(); }
