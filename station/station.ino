// Backyard Environmental Station v4: PMS5003
// Board: ESP32-S3 Dev Module, USB CDC On Boot enabled, PSRAM OPI
// read every pass: the PMS5003 streams, slow reads go stale
#define IO_USERNAME "YOUR_AIO_USERNAME"
#define IO_KEY      "YOUR_AIO_KEY"
#define WIFI_SSID   "YOUR_WIFI_NAME"
#define WIFI_PASS   "YOUR_WIFI_PASSWORD"

#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <BH1750.h>
#include <Adafruit_BMP3XX.h>
#include <Adafruit_LTR390.h>
#include <SensirionI2CScd4x.h>
#include <Adafruit_PM25AQI.h>
#include "AdafruitIO_WiFi.h"

AdafruitIO_WiFi io(IO_USERNAME, IO_KEY, WIFI_SSID, WIFI_PASS);

AdafruitIO_Feed *fTemp = io.feed("temperature");
AdafruitIO_Feed *fHum  = io.feed("humidity");
AdafruitIO_Feed *fLux  = io.feed("light");
AdafruitIO_Feed *fPres = io.feed("pressure");
AdafruitIO_Feed *fUv   = io.feed("uv");
AdafruitIO_Feed *fCo2  = io.feed("co2");
AdafruitIO_Feed *fPm1  = io.feed("pm1");
AdafruitIO_Feed *fPm25 = io.feed("pm25");
AdafruitIO_Feed *fPm10 = io.feed("pm10");

Adafruit_SHT31 sht = Adafruit_SHT31();
BH1750 lightMeter;
Adafruit_BMP3XX bmp;
Adafruit_LTR390 ltr = Adafruit_LTR390();
SensirionI2CScd4x scd4x;
Adafruit_PM25AQI aqi = Adafruit_PM25AQI();
bool havePm = false;
PM25_AQI_Data lastPm;
bool pmFresh = false;

bool haveSht, haveBmp, haveLtr, haveScd;
uint16_t co2 = 0;
unsigned long lastPublish = 0;
const unsigned long PUBLISH_MS = 30000;  // free tier: 30 points/min

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
  // PMS5003 on UART1: sensor TX -> GPIO18, sensor RX -> GPIO17
  Serial1.begin(9600, SERIAL_8N1, 18, 17);
  delay(1000);
  havePm = aqi.begin_UART(&Serial1);
  Serial.print("PMS5003:"); Serial.println(havePm ? "OK" : "MISSING");

  scd4x.begin(Wire);
  scd4x.stopPeriodicMeasurement();
  delay(500);
  haveScd = (scd4x.startPeriodicMeasurement() == 0);

  Serial.print("Connecting to Adafruit IO");
  io.connect();
  while (io.status() < AIO_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println(io.statusText());
}

void loop() {
  io.run();
  PM25_AQI_Data d;
  if (havePm && aqi.read(&d)) { lastPm = d; pmFresh = true; }
  if (haveScd) {
    bool ready = false;
    if (scd4x.getDataReadyFlag(ready) == 0 && ready) {
      uint16_t c; float st, shm;
      if (scd4x.readMeasurement(c, st, shm) == 0 && c > 0) co2 = c;
    }
  }

  if (millis() - lastPublish >= PUBLISH_MS) {
    lastPublish = millis();

    float t = haveSht ? sht.readTemperature() : NAN;
    float h = haveSht ? sht.readHumidity()    : NAN;
    float lux = lightMeter.readLightLevel();
    float pres = NAN;
    if (haveBmp && bmp.performReading()) pres = bmp.pressure / 100.0;
    uint32_t uv = 0;
    if (haveLtr && ltr.newDataAvailable()) uv = ltr.readUVS();

    if (!isnan(t))    fTemp->save(t);
    if (!isnan(h))    fHum->save(h);
    fLux->save(lux);
    if (!isnan(pres)) fPres->save(pres);
    fUv->save(uv / 96.0);   // ≈ UV index at gain 3, 18-bit
    if (co2 > 0)      fCo2->save(co2);

    if (pmFresh) {
      fPm1->save(lastPm.pm10_standard);
      fPm25->save(lastPm.pm25_standard);
      fPm10->save(lastPm.pm100_standard);
      Serial.print("PM2.5="); Serial.println(lastPm.pm25_standard);
    }

    Serial.print("Published: ");
    Serial.print(t); Serial.print("C ");
    Serial.print(h); Serial.print("% ");
    Serial.print(lux); Serial.print("lx ");
    Serial.print(pres); Serial.print("hPa uv=");
    Serial.print(uv); Serial.print(" co2=");
    Serial.println(co2);
  }
}
