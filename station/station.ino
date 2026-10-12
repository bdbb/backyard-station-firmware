// Backyard Environmental Station v8: sensors, wind, rain -> envi + Adafruit IO
// Board: ESP32-S3 Dev Module, USB CDC On Boot enabled
#include "secrets.h"
#define OTA_PASS    IO_KEY

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
#include <ArduinoOTA.h>

WebServer web(80);

#define ENVI_HOST    "envi.myrondomain.com"
#define ENVI_PATH    "/api/ingest"
unsigned long lastEnviPublish = 0;

#define PIN_ANEMOMETER 15
#define PIN_VANE        9   // 10k pull-up to 3V3
#define PIN_RAIN       16
volatile uint32_t windPulses = 0, rainTips = 0;
volatile unsigned long lastWindUs = 0, lastRainMs = 0;
uint32_t gustPulses = 0; unsigned long gustWindowStart = 0; float gustKmh = 0, windKmh = 0;

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
// true north minus shown direction, set after mounting
#define VANE_OFFSET_DEG 0
float readWindDir() {
  float v = analogReadMilliVolts(PIN_VANE) / 1000.0;
  // S3 ADC tops out near 3.12 V, so unplugged reads ~3.12 V
  if (v >= 3.09 || v <= 0.02) return -1;          // unplugged / shorted
  float r = 10000.0 * v / (3.3 - v);
  int best = 0; float bestErr = 1e9;
  for (int i = 0; i < 16; i++) {
    float e = fabs(log(r) - log(VANE_R[i]));
    if (e < bestErr) { bestErr = e; best = i; }
  }
  return fmod(VANE_DEG[best] + VANE_OFFSET_DEG + 360.0, 360.0);
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
float tempC = NAN, humidity = NAN, lux = NAN, presHpa = NAN;
uint32_t uvRaw = 0; bool uvSeen = false;
bool aioUp = false;
unsigned long lastPublish = 0;
const unsigned long PUBLISH_MS = 30000;  // free tier: 30 points/min

void readSensors() {
  if (haveSht) sht.readBoth(&tempC, &humidity);
  lux = lightMeter.readLightLevel();
  if (haveBmp && bmp.performReading()) presHpa = bmp.pressure / 100.0;
  if (haveLtr && ltr.newDataAvailable()) { uvRaw = ltr.readUVS(); uvSeen = true; }
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><meta name=color-scheme content=dark><title>Backyard Environmental Station</title>
<style>
*,*::before,*::after{box-sizing:border-box;min-width:0}
:root{color-scheme:dark;--bg:oklch(17% .012 250);--card:oklch(22% .016 250);--line:oklch(32% .02 250);--fg:oklch(97% 0 none);--mute:oklch(72% .02 250);--accent:oklch(80% .13 185);--bad:oklch(68% .19 25);--ease-out:cubic-bezier(.23,1,.32,1);font:16px/1.3 system-ui,-apple-system,sans-serif;-webkit-font-smoothing:antialiased}
body{margin:0;min-height:100svh;background:var(--bg);color:var(--fg);padding:clamp(12px,3vw,36px)}
header{display:flex;flex-wrap:wrap;align-items:baseline;justify-content:space-between;gap:.25rem 1rem;margin-block-end:clamp(12px,2vw,24px)}
h1{margin:0;font-size:clamp(1.3rem,1rem + 1.6vw,2.2rem);font-weight:650;text-wrap:balance}
#st{color:var(--mute);font-variant-numeric:tabular-nums}
#st::before{content:"";display:inline-block;inline-size:.6em;block-size:.6em;border-radius:50%;background:var(--accent);margin-inline-end:.45em}
#st.off::before{background:var(--bad)}
#g{display:grid;gap:clamp(8px,1.4vw,16px);grid-template-columns:repeat(auto-fit,minmax(min(100%,14rem),1fr))}
.c{container-type:inline-size;background:var(--card);border:1px solid var(--line);border-radius:16px;padding:clamp(12px,1.4vw,20px);transition:background-color .3s var(--ease-out),border-color .3s var(--ease-out)}
.c.hit{background:color-mix(in oklch,var(--accent) 20%,var(--card));border-color:var(--accent);transition:none}
.l{color:var(--mute);font-size:.95rem}
.v{font-size:clamp(1.8rem,19cqi,3.4rem);font-weight:700;font-variant-numeric:tabular-nums;line-height:1.1;margin-block:.15em .25em}
.u{font-size:.42em;font-weight:500;color:var(--mute);margin-inline-start:.25em}
.sp{display:block;inline-size:100%;block-size:36px;overflow:visible}
.sp polyline{fill:none;stroke:var(--accent);stroke-width:2;stroke-linejoin:round;vector-effect:non-scaling-stroke}
.wind{grid-row:span 2;display:grid;justify-items:center;align-content:start;text-align:center}
.wind .l{justify-self:start}
#cmp{inline-size:min(100%,230px);margin-block-start:.5rem}
#cmp circle{fill:none;stroke:var(--line);stroke-width:2}
#cmp text{fill:var(--mute);font-size:9px;text-anchor:middle;dominant-baseline:central}
#ndl{fill:var(--accent);transform-origin:50px 50px}
#ndl.off{opacity:.2}
footer{margin-block-start:1rem;color:var(--mute);font-size:.8rem;font-variant-numeric:tabular-nums}
@media (prefers-reduced-motion:no-preference){#ndl{transition:transform .3s var(--ease-out)}}
</style></head><body>
<header><h1>Backyard Environmental Station</h1><div id=st>Connecting…</div></header>
<main id=g><div class="c wind" id=c_wd><div class=l>Wind direction</div>
<svg id=cmp viewBox="0 0 100 100" role=img aria-label="Wind direction compass"><circle cx="50" cy="50" r="45"/><text x="50" y="12">N</text><text x="89" y="50">E</text><text x="50" y="89">S</text><text x="11" y="50">W</text><path id=ndl class=off d="M50 18 57 54 50 48 43 54Z"/></svg>
<div class=v id=v_wd>–</div></div></main>
<footer id=ft></footer>
<script>
const $=i=>document.getElementById(i),N=300,H={},st=$('st'),g=$('g'),ndl=$('ndl');
const M=[['t','Temperature','°C',1,1],['h','Humidity','%',0,2],['co2','CO₂','ppm',0,50],['pm25','PM2.5','µg/m³',0,5],['wk','Wind speed · 3 s avg','km/h',1,2],['gust','Gust · 5 min max','km/h',1,2],['rain','Rain since restart','mm',1,1],['lux','Light','lux',0,20],['uv','UV index','',1,.5],['p','Pressure','hPa',1,1]];
for(const[k,l,u]of M){H[k]=[];g.insertAdjacentHTML('beforeend',`<div class=c id=c_${k}><div class=l>${l}</div><div class=v><span id=v_${k}>–</span><span class=u>${u}</span></div><svg class=sp viewBox="0 0 100 36" preserveAspectRatio="none"><polyline id="s_${k}"></polyline></svg></div>`)}
const PT=['N','NNE','NE','ENE','E','ESE','SE','SSE','S','SSW','SW','WSW','W','WNW','NW','NNW'];
let ang=null;
function show(k,txt){const e=$('v_'+k);if(e.textContent===txt)return;const first=e.textContent==='–';e.textContent=txt;if(first)return;const c=$('c_'+k);c.classList.add('hit');setTimeout(()=>c.classList.remove('hit'),250)}
function spark(k,span){const a=H[k],v=a.filter(x=>x!=null);if(!v.length)return;let lo=Math.min(...v),hi=Math.max(...v);if(hi-lo<span){const m=(hi+lo)/2;lo=m-span/2;hi=m+span/2}
const w=Math.max(a.length-1,1);$('s_'+k).setAttribute('points',a.map((x,i)=>x==null?'':(i*100/w).toFixed(1)+','+(33-(x-lo)/(hi-lo)*30).toFixed(1)).filter(Boolean).join(' '))}
const up=s=>Math.floor(s/3600)+'h '+Math.floor(s/60)%60+'m';
function render(d){for(const[k,,,dp,span]of M){let v=d[k];if(k=='gust'){const w=H.wk.filter(x=>x!=null);v=w.length?Math.max(...w):null}
H[k].push(v);if(H[k].length>N)H[k].shift();show(k,v==null?'–':(+v).toFixed(dp));spark(k,span)}
if(d.wd==null){ndl.classList.add('off');show('wd','no vane')}else{ang=ang==null?d.wd:ang+((d.wd-ang)%360+540)%360-180;ndl.style.transform=`rotate(${ang}deg)`;ndl.classList.remove('off');show('wd',Math.round(d.wd)+'° '+PT[Math.round(d.wd/22.5)%16])}
$('ft').textContent=`Vane ${d.vane_mv} mV · cups switch ${d.cups_open?'open':'closed'} · ${d.clicks} wind clicks · ${d.tips} rain tips · Wi-Fi ${d.rssi} dBm · Adafruit IO ${d.aio?'connected':'offline'} · up ${up(d.up)}`}
async function tick(){const t0=Date.now();try{const c=new AbortController(),to=setTimeout(()=>c.abort(),2500);const r=await fetch('/data',{signal:c.signal,cache:'no-store'});render(await r.json());clearTimeout(to);st.className='';st.textContent='Live · '+new Date().toLocaleTimeString()}catch(e){st.className='off';st.textContent='Reconnecting…'}setTimeout(tick,Math.max(200,1000-(Date.now()-t0)))}
tick();
</script></body></html>)HTML";

static int jnum(char *b, size_t n, const char *k, float v, int d, bool omitNan = false) {
  if (isnan(v)) return omitNan ? 0 : snprintf(b, n, "\"%s\":null,", k);
  return snprintf(b, n, "\"%s\":%.*f,", k, d, v);
}

void handleData() {
  noInterrupts(); uint32_t clicks = windPulses, tips = rainTips; interrupts();
  float wd = readWindDir();
  char out[512];
  int o = snprintf(out, sizeof(out), "{\"clicks\":%u,\"tips\":%u,\"vane_mv\":%u,\"cups_open\":%d,\"rssi\":%d,\"aio\":%d,\"up\":%lu,",
                   clicks, tips, analogReadMilliVolts(PIN_VANE), digitalRead(PIN_ANEMOMETER), WiFi.RSSI(), aioUp, millis() / 1000);
  o += jnum(out + o, sizeof(out) - o, "t", tempC, 2);
  o += jnum(out + o, sizeof(out) - o, "h", humidity, 1);
  o += jnum(out + o, sizeof(out) - o, "p", presHpa, 1);
  o += jnum(out + o, sizeof(out) - o, "lux", lux < 0 ? NAN : lux, 1);
  o += jnum(out + o, sizeof(out) - o, "uv", uvSeen ? uvRaw / 96.0 : NAN, 2);
  o += jnum(out + o, sizeof(out) - o, "co2", co2 ? co2 : NAN, 0);
  o += jnum(out + o, sizeof(out) - o, "pm25", pmFresh ? lastPm.pm25_standard : NAN, 0);
  o += jnum(out + o, sizeof(out) - o, "wk", windKmh, 1);
  o += jnum(out + o, sizeof(out) - o, "wd", wd < 0 ? NAN : wd, 1);
  o += jnum(out + o, sizeof(out) - o, "rain", tips * 0.2794, 2);
  out[o - 1] = '}';
  web.send(200, "application/json", out);
}

void setup() {
  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);       // don't stall when no serial monitor is open
  delay(2000);
  Wire.begin(4, 5);

  haveSht = sht.begin(0x44);
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);
  haveBmp = bmp.begin_I2C(0x77, &Wire);
  if (haveBmp) {
    bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
    bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
    bmp.performReading();   // first reading after config is garbage
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
  Serial.println("PMS5003: waiting for data");   // begin_UART can't detect the sensor

  scd4x.begin(Wire);
  scd4x.stopPeriodicMeasurement();
  delay(500);
  haveScd = (scd4x.startPeriodicMeasurement() == 0);
  Serial.printf("Sensors: sht=%d bmp=%d ltr=%d scd=%d\n", haveSht, haveBmp, haveLtr, haveScd);

  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Serial.printf("Wi-Fi disconnected: %s\n", WiFi.STA.disconnectReasonName((wifi_err_reason_t)info.wifi_sta_disconnected.reason));
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  io.connect();
  ArduinoOTA.setHostname("station");
  ArduinoOTA.setPassword(OTA_PASS);
  web.on("/", [](){ web.send_P(200, "text/html; charset=utf-8", PAGE); });
  web.on("/data", handleData);
  web.begin();

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

  noInterrupts(); uint32_t rainTotal = rainTips; interrupts();

  // leave out missing readings: nan isn't valid JSON
  char fields[480]; fields[0] = '{'; int o = 1;
  o += jnum(fields + o, sizeof(fields) - o, "tempC", tempC, 2, true);
  o += jnum(fields + o, sizeof(fields) - o, "humidity", humidity, 2, true);
  o += jnum(fields + o, sizeof(fields) - o, "lux", lux, 1, true);
  o += jnum(fields + o, sizeof(fields) - o, "pressure_hpa", presHpa, 2, true);
  snprintf(fields + o, sizeof(fields) - o,
    "\"uv_index\":%.2f,\"co2_ppm\":%u,\"pm1\":%u,\"pm25\":%u,\"pm10\":%u,"
    "\"wind_kmh\":%.1f,\"wind_gust_kmh\":%.1f,\"wind_dir_deg\":%.1f,\"rain_tips\":%u,\"rain_mm\":%.2f}",
    uvRaw / 96.0, co2,
    pmFresh ? lastPm.pm10_standard : 0,
    pmFresh ? lastPm.pm25_standard : 0,
    pmFresh ? lastPm.pm100_standard : 0,
    windKmh, gustKmh, readWindDir(), rainTotal, rainTotal * 0.2794);
  Serial.println(fields);
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
  ArduinoOTA.handle();

  static bool wifiWasUp = false;
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp && !wifiWasUp) {
    static bool started = false;
    if (!started) {
      started = true;
      ArduinoOTA.begin();
      // core bug: first DNS lookup races SNTP and crashes, so look up once before starting the clock
      IPAddress ip; WiFi.hostByName(ENVI_HOST, ip);
      configTime(0, 0, "pool.ntp.org", "time.google.com");
    }
    Serial.print("Wi-Fi up. Live page: http://"); Serial.println(WiFi.localIP());
  }
  wifiWasUp = wifiUp;

  // the core gives up on some Wi-Fi failures, so retry every 30 s
  static unsigned long wifiDownSince = 0;
  if (wifiUp) wifiDownSince = 0;
  else if (!wifiDownSince) wifiDownSince = millis();
  else if (millis() - wifiDownSince > 30000) {
    Serial.printf("Wi-Fi down for 30 s (status %d), retrying\n", WiFi.status());
    WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiDownSince = millis();
  }

  // io.run() blocks up to 60 s while Adafruit IO is down
  bool aioNow = wifiUp && io.mqttStatus(true) == AIO_CONNECTED;
  if (aioNow != aioUp) Serial.println(aioNow ? "Adafruit IO connected" : "Adafruit IO offline");
  aioUp = aioNow;
  if (aioUp) io.run(0, true);

  static unsigned long lastRead = 0;
  if (millis() - lastRead >= 1000) { lastRead = millis(); readSensors(); }

  PM25_AQI_Data d;
  if (havePm && aqi.read(&d)) {
    if (!pmFresh) Serial.println("PMS5003: data OK");
    lastPm = d; pmFresh = true;
  }

  unsigned long windowMs = millis() - gustWindowStart;
  if (windowMs >= 3000) {                         // 3 s average = speed, max of them = gust (WMO)
    noInterrupts(); uint32_t p = windPulses; interrupts();
    windKmh = (p - gustPulses) * 2400.0 / windowMs;  // 1 pulse/s = 2.4 km/h
    if (windKmh > gustKmh) gustKmh = windKmh;
    gustPulses = p; gustWindowStart = millis();
  }

  if (wifiUp && millis() - lastEnviPublish >= ENVI_PUBLISH_MS) {
    lastEnviPublish = millis();
    publishToEnvi();
  }

  static unsigned long lastCo2Ms = 0;
  if (haveScd) {
    bool ready = false;
    if (scd4x.getDataReadyFlag(ready) == 0 && ready) {
      uint16_t c; float st, shm;
      if (scd4x.readMeasurement(c, st, shm) == 0 && c > 0) { co2 = c; lastCo2Ms = millis(); }
    }
    // no sample for 30 s: a power dip reset it, restart
    if (millis() - lastCo2Ms > 30000) {
      lastCo2Ms = millis(); co2 = 0;
      bool wasIdle = scd4x.startPeriodicMeasurement() == 0;
      Serial.println(wasIdle ? "CO2 sensor had reset to idle (power dip?); restarted it"
                             : "CO2 sensor is measuring but gave no data for 30 s");
    }
  }

  if (millis() - lastPublish >= PUBLISH_MS) {
    lastPublish = millis();
    if (aioUp) {
      if (!isnan(tempC))    fTemp->save(tempC);
      if (!isnan(humidity)) fHum->save(humidity);
      fLux->save(lux);
      if (!isnan(presHpa))  fPres->save(presHpa);
      fUv->save((int)uvRaw);
      if (co2 > 0)          fCo2->save(co2);
      if (pmFresh) {
        fPm1->save(lastPm.pm10_standard);    // PM1.0
        fPm25->save(lastPm.pm25_standard);   // PM2.5
        fPm10->save(lastPm.pm100_standard);  // PM10
      }
      Serial.printf("Published to Adafruit IO: %.2fC %.2f%% %.1flx %.2fhPa uv=%u co2=%u\n", tempC, humidity, lux, presHpa, uvRaw, co2);
    }
  }
}
