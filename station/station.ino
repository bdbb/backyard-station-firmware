// Backyard Environmental Station v8: sensors, wind, rain -> envi + Adafruit IO
// Board: ESP32-S3 Dev Module, USB CDC On Boot enabled
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
#include <WiFiClientSecure.h>
#include <time.h>
#include <WebServer.h>
#include <ESPmDNS.h>

WebServer web(80);
char lastFields[480] = "{}";

#define ENVI_HOST    "envi.myrondomain.com"
#define ENVI_PATH    "/api/ingest"
#define ENVI_SECRET  "YOUR_INGEST_SECRET"
unsigned long lastEnviPublish = 0;

#define PIN_ANEMOMETER 15
#define PIN_VANE        9   // 10k pull-up to 3V3
#define PIN_RAIN       16
volatile uint32_t windPulses = 0, rainTips = 0;
volatile unsigned long lastWindUs = 0, lastRainMs = 0;
uint32_t gustPulses = 0; unsigned long gustWindowStart = 0; float gustKmh = 0;

void IRAM_ATTR onWindPulse() {
  unsigned long now = micros();
  if (now - lastWindUs > 5000) { windPulses++; lastWindUs = now; }   // 5 ms debounce
}
void IRAM_ATTR onRainTip() {
  unsigned long now = millis();
  if (now - lastRainMs > 100) { rainTips++; lastRainMs = now; }
}

// vane resistance (ohms, SparkFun datasheet) -> degrees
const float VANE_R[16]   = {33000,6570,8200,891,1000,688,2200,1410,3900,3140,16000,14120,120000,42120,64900,21880};
const float VANE_DEG[16] = {0,22.5,45,67.5,90,112.5,135,157.5,180,202.5,225,247.5,270,292.5,315,337.5};
float readWindDir() {
  float v = analogReadMilliVolts(PIN_VANE) / 1000.0;
  if (v >= 3.25 || v <= 0.02) return -1;          // unplugged / shorted
  float r = 10000.0 * v / (3.3 - v);
  int best = 0; float bestErr = 1e9;
  for (int i = 0; i < 16; i++) {
    float e = fabs(log(r) - log(VANE_R[i]));
    if (e < bestErr) { bestErr = e; best = i; }
  }
  return VANE_DEG[best];
}
const unsigned long ENVI_PUBLISH_MS = 10000;

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

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1"><title>Station live</title>
<style>body{margin:0;background:#0b0f14;color:#dbe7f3;font:15px system-ui,sans-serif;padding:16px}
h1{font-size:18px;margin:0 0 12px}.g{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}
.c{background:#111820;border:1px solid #1e2a36;border-radius:12px;padding:12px}.l{color:#7d93a8;font-size:12px}
.v{font-size:26px;font-weight:600;margin-top:4px}.u{font-size:13px;color:#7d93a8}
#cmp{width:150px;height:150px;border-radius:50%;border:2px solid #2dd4bf;position:relative;margin:auto}
#ndl{position:absolute;left:73px;top:10px;width:4px;height:65px;background:#2dd4bf;transform-origin:2px 65px;border-radius:2px;transition:transform .3s}
.n{position:absolute;font-size:12px;color:#7d93a8}#s{color:#7d93a8;font-size:12px;margin-top:12px}</style></head><body>
<h1>Backyard station &middot; live test view</h1>
<div class=g>
<div class=c style="grid-row:span 2;text-align:center"><div class=l>Wind direction (live)</div><br>
<div id=cmp><span class=n style="left:70px;top:2px">N</span><span class=n style="right:4px;top:66px">E</span><span class=n style="left:71px;bottom:2px">S</span><span class=n style="left:4px;top:66px">W</span><div id=ndl></div></div>
<div class=v id=dir>-</div></div>
<div class=c><div class=l>Anemometer clicks (live)</div><div class=v id=pul>-</div></div>
<div class=c><div class=l>Wind speed (10 s avg)</div><div class=v><span id=wk>-</span> <span class=u>km/h</span></div></div>
<div class=c><div class=l>Gust</div><div class=v><span id=wg>-</span> <span class=u>km/h</span></div></div>
<div class=c><div class=l>Rain tips (live)</div><div class=v id=rt>-</div></div>
<div class=c><div class=l>Temperature</div><div class=v><span id=t>-</span> <span class=u>&deg;C</span></div></div>
<div class=c><div class=l>Humidity</div><div class=v><span id=h>-</span> <span class=u>%</span></div></div>
<div class=c><div class=l>CO&#8322;</div><div class=v><span id=co>-</span> <span class=u>ppm</span></div></div>
<div class=c><div class=l>PM2.5</div><div class=v><span id=pm>-</span> <span class=u>&micro;g/m&sup3;</span></div></div>
<div class=c><div class=l>Pressure</div><div class=v><span id=p>-</span> <span class=u>hPa</span></div></div>
<div class=c><div class=l>Light</div><div class=v><span id=lx>-</span> <span class=u>lux</span></div></div>
</div><div id=s>connecting...</div>
<script>const $=i=>document.getElementById(i),f=(v,d=1)=>v==null?'-':(+v).toFixed(d);
async function tick(){try{const r=await(await fetch('/data')).json(),L=r.live,A=r.last;
const d=L.wind_dir_deg;$('dir').textContent=d<0?'vane not connected':d+'°';if(d>=0)$('ndl').style.transform='rotate('+d+'deg)';
$('pul').textContent=L.wind_pulses;$('rt').textContent=L.rain_tips;
$('wk').textContent=f(A.wind_kmh);$('wg').textContent=f(A.wind_gust_kmh);$('t').textContent=f(A.tempC);$('h').textContent=f(A.humidity);
$('co').textContent=A.co2_ppm?A.co2_ppm:'-';$('pm').textContent=f(A.pm25,0);$('p').textContent=f(A.pressure_hpa);$('lx').textContent=f(A.lux,0);
$('s').textContent='updated '+new Date().toLocaleTimeString()}catch(e){$('s').textContent='lost connection to station'}}
setInterval(tick,1000);tick();</script></body></html>)HTML";

void handleData() {
  noInterrupts(); uint32_t p = windPulses, r = rainTips; interrupts();
  char out[700];
  snprintf(out, sizeof(out), "{\"live\":{\"wind_dir_deg\":%.1f,\"wind_pulses\":%u,\"rain_tips\":%u},\"last\":%s}",
           readWindDir(), p, r, lastFields);
  web.send(200, "application/json", out);
}

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

  MDNS.begin("station");
  web.on("/", [](){ web.send_P(200, "text/html; charset=utf-8", PAGE); });
  web.on("/data", handleData);
  web.begin();
  Serial.print("Live test page: http://station.local  or  http://");
  Serial.println(WiFi.localIP());

  configTime(0, 0, "pool.ntp.org", "time.google.com");

  pinMode(PIN_ANEMOMETER, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ANEMOMETER), onWindPulse, FALLING);
  pinMode(PIN_RAIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_RAIN), onRainTip, FALLING);
  analogSetPinAttenuation(PIN_VANE, ADC_11db);
  gustWindowStart = millis();
}

void publishToEnvi() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;   // clock not synced yet
  float t = haveSht ? sht.readTemperature() : NAN;
  float h = haveSht ? sht.readHumidity()    : NAN;
  float lux = lightMeter.readLightLevel();
  float pres = NAN;
  if (haveBmp && bmp.performReading()) pres = bmp.pressure / 100.0;
  uint32_t uv = 0;
  if (haveLtr && ltr.newDataAvailable()) uv = ltr.readUVS();

  static uint32_t lastPulses = 0; static unsigned long lastWindMs = 0;
  noInterrupts(); uint32_t pulses = windPulses; uint32_t rainTotal = rainTips; interrupts();
  float secs = lastWindMs ? (millis() - lastWindMs) / 1000.0 : 0;
  float windKmh = secs > 0 ? (pulses - lastPulses) / secs * 2.4 : 0;
  lastPulses = pulses; lastWindMs = millis();

  char fields[480];
  snprintf(fields, sizeof(fields),
    "{\"tempC\":%.2f,\"humidity\":%.2f,\"lux\":%.1f,\"pressure_hpa\":%.2f,"
    "\"uv_index\":%.2f,\"co2_ppm\":%u,\"pm1\":%u,\"pm25\":%u,\"pm10\":%u,"
    "\"wind_kmh\":%.1f,\"wind_gust_kmh\":%.1f,\"wind_dir_deg\":%.1f,\"rain_tips\":%u,\"rain_mm\":%.2f}",
    t, h, lux, pres, uv / 96.0, co2,
    pmFresh ? lastPm.pm10_standard : 0,
    pmFresh ? lastPm.pm25_standard : 0,
    pmFresh ? lastPm.pm100_standard : 0,
    windKmh, gustKmh, readWindDir(), rainTotal, rainTotal * 0.2794);
  Serial.println(fields);
  strncpy(lastFields, fields, sizeof(lastFields) - 1);
  char body[640];
  snprintf(body, sizeof(body),
    "{\"metrics\":[{\"name\":\"env\",\"fields\":%s,\"tags\":{\"host\":\"esp32\"},\"timestamp\":%ld}]}",
    fields, (long)now);

  // raw HTTPS POST: HTTPClient clashes with ArduinoHttpClient on macOS
  WiFiClientSecure tls;
  tls.setInsecure();
  if (tls.connect(ENVI_HOST, 443)) {
    size_t len = strlen(body);
    tls.print("POST " ENVI_PATH " HTTP/1.1\r\n");
    tls.print("Host: " ENVI_HOST "\r\n");
    tls.print("Authorization: Bearer " ENVI_SECRET "\r\n");
    tls.print("Content-Type: application/json\r\n");
    tls.print("Content-Length: "); tls.print(len); tls.print("\r\n");
    tls.print("Connection: close\r\n\r\n");
    tls.write((const uint8_t*)body, len);
    String status = tls.readStringUntil('\n');
    Serial.print("envi POST -> "); Serial.println(status);
    tls.stop();
  } else {
    Serial.println("envi POST -> connect failed");
  }
  gustKmh = 0;
}

void loop() {
  web.handleClient();
  io.run();

  PM25_AQI_Data d;
  if (havePm && aqi.read(&d)) { lastPm = d; pmFresh = true; }

  if (millis() - gustWindowStart >= 3000) {      // gust = max 3 s average (WMO)
    noInterrupts(); uint32_t p = windPulses; interrupts();
    float k = (p - gustPulses) / 3.0 * 2.4;      // 1 pulse/s = 2.4 km/h
    if (k > gustKmh) gustKmh = k;
    gustPulses = p; gustWindowStart = millis();
  }

  if (millis() - lastEnviPublish >= ENVI_PUBLISH_MS) {
    lastEnviPublish = millis();
    publishToEnvi();
  }

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
    fUv->save((int)uv);
    if (co2 > 0)      fCo2->save(co2);

    if (pmFresh) {
      fPm1->save(lastPm.pm10_standard);    // PM1.0
      fPm25->save(lastPm.pm25_standard);   // PM2.5
      fPm10->save(lastPm.pm100_standard);  // PM10
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
