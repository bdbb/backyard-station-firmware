// Backyard Environmental Station v2: all I2C sensors
// Board: ESP32-S3 Dev Module, USB CDC On Boot enabled

#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <BH1750.h>
#include <Adafruit_BMP3XX.h>
#include <Adafruit_LTR390.h>
#include <SensirionI2CScd4x.h>

Adafruit_SHT31 sht = Adafruit_SHT31();
BH1750 lightMeter;
Adafruit_BMP3XX bmp;
Adafruit_LTR390 ltr = Adafruit_LTR390();
SensirionI2CScd4x scd4x;

bool haveSht, haveBmp, haveLtr, haveScd;
uint16_t co2 = 0;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Wire.begin(4, 5);

  haveSht = sht.begin(0x44);
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);
  haveBmp = bmp.begin_I2C(0x77, &Wire);
  if (haveBmp) {
    bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
    bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
  }
  haveLtr = ltr.begin(&Wire);
  if (haveLtr) {
    ltr.setMode(LTR390_MODE_UVS);
    ltr.setGain(LTR390_GAIN_3);
    ltr.setResolution(LTR390_RESOLUTION_18BIT);
  }
  scd4x.begin(Wire);
  scd4x.stopPeriodicMeasurement();
  delay(500);
  haveScd = (scd4x.startPeriodicMeasurement() == 0);

  Serial.println("=== Station v2 ===");
  Serial.print("SHT31:");  Serial.println(haveSht ? "OK" : "MISSING");
  Serial.print("BMP388:"); Serial.println(haveBmp ? "OK" : "MISSING");
  Serial.print("LTR390:"); Serial.println(haveLtr ? "OK" : "MISSING");
  Serial.print("SCD41:");  Serial.println(haveScd ? "OK" : "MISSING");
}

void loop() {
  float t = haveSht ? sht.readTemperature() : 0;
  float h = haveSht ? sht.readHumidity()    : 0;
  float lux = lightMeter.readLightLevel();

  float pres = 0;
  if (haveBmp && bmp.performReading()) pres = bmp.pressure / 100.0;

  uint32_t uv = 0;
  if (haveLtr && ltr.newDataAvailable()) uv = ltr.readUVS();

  if (haveScd) {
    bool ready = false;
    if (scd4x.getDataReadyFlag(ready) == 0 && ready) {
      uint16_t c; float st, sh;
      if (scd4x.readMeasurement(c, st, sh) == 0 && c > 0) co2 = c;
    }
  }

  Serial.print("TempC:");     Serial.print(t);
  Serial.print(",Humidity:"); Serial.print(h);
  Serial.print(",Lux/100:");  Serial.print(lux / 100.0);
  Serial.print(",hPa-950:");  Serial.print(pres > 0 ? pres - 950.0 : 0);
  Serial.print(",UVraw/50:"); Serial.print(uv / 50.0);
  Serial.print(",CO2/50:");   Serial.println(co2 / 50.0);

  delay(1000);
}
