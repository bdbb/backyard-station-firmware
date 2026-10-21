// Backyard Environmental Station v8: sensors, wind, rain -> envi
// Board: ESP32-S3 Dev Module, USB CDC On Boot enabled
#include "secrets.h"

#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <BH1750.h>
#include <Adafruit_BMP3XX.h>
#include <Adafruit_LTR390.h>
#include <SensirionI2CScd4x.h>
#include <Adafruit_PM25AQI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>

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

// rain today: from local midnight, kept in flash across restarts
#define LOCAL_TZ "PST8PDT,M3.2.0,M11.1.0"   // US Pacific with daylight saving
Preferences prefs;
int rainDayKey = -1;
uint32_t rainCarry = 0, rainBase = 0;
uint32_t rainTodayTips() { noInterrupts(); uint32_t t = rainTips; interrupts(); return rainCarry + (t - rainBase); }

void updateRainDay() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;      // clock not synced yet
  struct tm lt; localtime_r(&now, &lt);
  int key = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
  if (key != rainDayKey) {
    noInterrupts(); uint32_t t = rainTips; interrupts();
    if (rainDayKey == -1 && prefs.getInt("day", 0) == key) { rainCarry = prefs.getUInt("tips", 0); rainBase = 0; }
    else if (rainDayKey == -1) { rainCarry = 0; rainBase = 0; }
    else { rainCarry = 0; rainBase = t; }
    rainDayKey = key;
    prefs.putInt("day", key); prefs.putUInt("tips", rainTodayTips());
  }
  static uint32_t saved = 0; static unsigned long lastSave = 0;
  uint32_t today = rainTodayTips();
  if (today != saved && millis() - lastSave > 60000) { prefs.putUInt("tips", today); saved = today; lastSave = millis(); }
}

void IRAM_ATTR onWindPulse() {
  unsigned long now = micros();
  if (now - lastWindUs > 5000) { windPulses++; lastWindUs = now; }   // 5 ms debounce
}
void IRAM_ATTR onRainTip() {
  unsigned long now = millis();
  if (now - lastRainMs > 1000) { rainTips++; lastRainMs = now; }   // 1 s debounce
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

void readSensors() {
  if (haveSht) sht.readBoth(&tempC, &humidity);
  lux = lightMeter.readLightLevel();
  if (haveBmp && bmp.performReading()) presHpa = bmp.pressure / 100.0;
  if (haveLtr && ltr.newDataAvailable()) { uvRaw = ltr.readUVS(); uvSeen = true; }
}

// layout after Dribbble #27576358 and #27187165
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><meta name=color-scheme content=dark><title>Backyard Environmental Station</title>
<style>
*,*::before,*::after{box-sizing:border-box;min-width:0}
:root{color-scheme:dark;--bg:oklch(14% .008 260);--card:oklch(20% .012 260);--line:oklch(29% .015 260);--fg:oklch(97% 0 none);--mute:oklch(70% .015 260);--accent:oklch(80% .13 185);--bad:oklch(68% .19 25);--g:oklch(72% .17 145);--y:oklch(85% .16 95);--o:oklch(75% .17 60);--r:oklch(65% .2 25);--p:oklch(60% .2 320);--ease-out:cubic-bezier(.23,1,.32,1);font:16px/1.3 system-ui,-apple-system,sans-serif;-webkit-font-smoothing:antialiased}
body{margin:0;min-height:100svh;background:var(--bg);color:var(--fg);padding:clamp(12px,3vw,36px)}
header{display:flex;flex-wrap:wrap;align-items:end;justify-content:space-between;gap:.5rem 1.5rem;margin-block-end:clamp(14px,2vw,28px)}
h1{margin:0;font-size:clamp(1.4rem,1rem + 1.6vw,2.2rem);font-weight:650;text-wrap:balance}
.sub{color:var(--mute);font-size:.9rem;margin-block-start:.3rem}
#st{color:var(--mute);font-variant-numeric:tabular-nums;white-space:nowrap}
#st::before{content:"";display:inline-block;inline-size:.6em;block-size:.6em;border-radius:50%;background:var(--accent);margin-inline-end:.45em}
#st.off::before{background:var(--bad)}
#g{display:grid;gap:clamp(8px,1.2vw,14px);grid-template-columns:repeat(auto-fit,minmax(min(100%,15rem),1fr))}
.c{container-type:inline-size;display:flex;flex-direction:column;background:var(--card);border:1px solid var(--line);border-radius:18px;padding:clamp(14px,1.4vw,20px);transition:background-color .3s var(--ease-out),border-color .3s var(--ease-out)}
.c.hit{background:color-mix(in oklch,var(--accent) 18%,var(--card));border-color:var(--accent);transition:none}
.k{display:flex;justify-content:space-between;gap:.5rem;color:var(--mute);font-size:.72rem;font-weight:600;letter-spacing:.08em;text-transform:uppercase}
.rt{color:var(--fg);letter-spacing:0;text-transform:none;font-size:.8rem;text-align:end}
.v{font-size:clamp(1.8rem,18cqi,3.2rem);font-weight:650;font-variant-numeric:tabular-nums;line-height:1.05;margin-block:.3em .35em}
.u{font-size:.42em;font-weight:500;color:var(--mute);margin-inline-start:.25em}
.bar{position:relative;block-size:6px;border-radius:3px;margin-block:0 .7rem}
.bar i{position:absolute;inset-block-start:50%;inline-size:12px;block-size:12px;margin:-6px 0 0 -6px;border-radius:50%;background:var(--fg);border:2px solid var(--card)}
.sp{display:block;inline-size:100%;block-size:34px;overflow:visible;margin-block-start:auto}
.sp polyline{fill:none;stroke:var(--accent);stroke-width:2;stroke-linejoin:round;vector-effect:non-scaling-stroke}
.x{color:var(--mute);font-size:.82rem;font-variant-numeric:tabular-nums;border-block-start:1px solid var(--line);padding-block-start:.55rem;margin-block-start:.6rem}
.x:empty{display:none}
.wind{grid-row:span 2}
.wind .v{text-align:center}
#dial{display:block;inline-size:min(100%,250px);margin:.5rem auto 0}
#dial .ring{fill:none;stroke:var(--line);stroke-width:7}
#dial .spd{fill:none;stroke:var(--accent);stroke-width:7;stroke-linecap:round;transform:rotate(-90deg);transform-origin:60px 60px}
#dial text{fill:var(--mute);font-size:7px;font-weight:600;text-anchor:middle;dominant-baseline:central}
#dial .big{fill:var(--fg);font-size:22px;font-weight:650}
#arw{fill:var(--fg);transform-origin:60px 60px}
#arw.off{opacity:.15}
footer{margin-block-start:1rem;color:var(--mute);font-size:.8rem;font-variant-numeric:tabular-nums}
@media (prefers-reduced-motion:no-preference){#arw{transition:transform .3s var(--ease-out)}#dial .spd{transition:stroke-dasharray .3s var(--ease-out)}}
</style></head><body>
<header><div><h1>Backyard Environmental Station</h1><div class=sub id=src>Loading weather service…</div></div><div id=st>Connecting…</div></header>
<main id=g><div class="c wind" id=c_wd><div class=k>Wind</div>
<svg id=dial viewBox="0 0 120 120" role=img aria-label="Wind speed and direction"><circle class=ring cx="60" cy="60" r="50"/><circle class=spd id=spd cx="60" cy="60" r="50" stroke-dasharray="0 400"/><text x="60" y="21">N</text><text x="99" y="60">E</text><text x="60" y="99">S</text><text x="21" y="60">W</text><path id=arw class=off d="M54 2 66 2 60 13Z"/><text class=big id=wkv x="60" y="57">–</text><text x="60" y="73">km/h</text></svg>
<div class=v id=v_wd>–</div><svg class=sp viewBox="0 0 100 36" preserveAspectRatio="none"><polyline id="s_wk"></polyline></svg><div class=x id=x_wd></div></div></main>
<footer id=ft></footer>
<script>
const LAT=34.16,LON=-118.64,PLACE='Calabasas',ELEV=null,MAXW=50;
const $=i=>document.getElementById(i),N=300,H={wk:[]},S={},st=$('st'),g=$('g'),arw=$('arw');
const UV=[0,11,'linear-gradient(90deg,var(--g),var(--y) 27%,var(--o) 55%,var(--r) 73%,var(--p))'],
PM=[0,75,'linear-gradient(90deg,var(--g) 0 12%,var(--y) 12% 47%,var(--o) 47% 74%,var(--r) 74%)'],
CO=[400,2000,'linear-gradient(90deg,var(--g) 0 25%,var(--y) 25% 50%,var(--o) 50% 69%,var(--r) 69%)'];
const M=[['t','Temperature',' °C',1,1],['h','Humidity',' %',0,2],['pm25','Air quality · PM2.5',' µg/m³',0,5,PM],['co2','CO₂',' ppm',0,50,CO],['gust','Gust · 5 min max',' km/h',1,2],['rain','Rain today',' mm',1,1],['uv','UV index','',1,.5,UV],['lux','Light',' lux',0,20],['p','Pressure',' hPa',1,1]];
const RT={pm25:[[9,'Good'],[35.4,'Moderate'],[55.4,'Unhealthy for sensitive groups'],[125.4,'Unhealthy'],[1e9,'Very unhealthy']],uv:[[2.5,'Low'],[5.5,'Moderate'],[7.5,'High'],[10.5,'Very high'],[1e9,'Extreme']],co2:[[800,'Fresh'],[1200,'Moderate'],[1500,'Stuffy'],[1e9,'Poor']]};
for(const[k,l,u,,,sc]of M){H[k]=[];g.insertAdjacentHTML('beforeend',`<div class=c id=c_${k}><div class=k>${l}<span class=rt id=r_${k}></span></div><div class=v><span id=v_${k}>–</span><span class=u>${u}</span></div>${sc?`<div class=bar style="background:${sc[2]}"><i id=b_${k} hidden></i></div>`:''}<svg class=sp viewBox="0 0 100 36" preserveAspectRatio="none"><polyline id="s_${k}"></polyline></svg><div class=x id=x_${k}></div></div>`)}
const PT=['N','NNE','NE','ENE','E','ESE','SE','SSE','S','SSW','SW','WSW','W','WNW','NW','NNW'],pt=d=>PT[Math.round(d/22.5)%16];
let ang=null;
function show(k,txt){const e=$('v_'+k);if(e.textContent===txt)return;const first=e.textContent==='–';e.textContent=txt;if(first)return;const c=$('c_'+k);c.classList.add('hit');setTimeout(()=>c.classList.remove('hit'),250)}
function push(k,v){H[k].push(v);if(H[k].length>N)H[k].shift()}
function spark(k,span){const a=H[k],v=a.filter(x=>x!=null);if(!v.length)return;let lo=Math.min(...v),hi=Math.max(...v);if(hi-lo<span){const m=(hi+lo)/2;lo=m-span/2;hi=m+span/2}
const w=Math.max(a.length-1,1);$('s_'+k).setAttribute('points',a.map((x,i)=>x==null?'':(i*100/w).toFixed(1)+','+(33-(x-lo)/(hi-lo)*30).toFixed(1)).filter(Boolean).join(' '))}
function cmp(k,v,dp,u){const e=$('x_'+k),s=S[k];if(!e)return;if(s==null){e.textContent='';return}
if(k=='rain'){e.textContent=`Weather service today ${s.toFixed(1)}${u}`;return}
e.textContent=`Weather service ${s.toFixed(dp)}${u}`+(v==null?'':` · you ${v-s>0?'+':''}${(v-s).toFixed(dp)}`)}
const up=s=>Math.floor(s/3600)+'h '+Math.floor(s/60)%60+'m';
function render(d){
push('wk',d.wk);spark('wk',2);$('wkv').textContent=d.wk==null?'–':d.wk.toFixed(1);$('spd').style.opacity=d.wk>0?1:0;$('spd').setAttribute('stroke-dasharray',`${Math.min((d.wk||0)/MAXW,1)*314.2} 400`);
if(d.wd==null){arw.classList.add('off');show('wd','no vane')}else{ang=ang==null?d.wd:ang+((d.wd-ang)%360+540)%360-180;arw.style.transform=`rotate(${ang}deg)`;arw.classList.remove('off');show('wd',Math.round(d.wd)+'° '+pt(d.wd))}
$('x_wd').textContent=S.wk==null?'':`Weather service ${S.wk.toFixed(1)} km/h from ${Math.round(S.wd)}° ${pt(S.wd)}`+(d.wk==null?'':` · you ${d.wk-S.wk>0?'+':''}${(d.wk-S.wk).toFixed(1)}`);
for(const[k,,u,dp,span,sc]of M){let v=d[k];if(k=='gust'){const w=H.wk.filter(x=>x!=null);v=w.length?Math.max(...w):null}
push(k,v);show(k,v==null?'–':(+v).toFixed(dp));spark(k,span);cmp(k,v,dp,u);
if(sc){const b=$('b_'+k);b.hidden=v==null;if(v!=null)b.style.insetInlineStart=Math.max(0,Math.min(100,(v-sc[0])/(sc[1]-sc[0])*100))+'%'}
if(RT[k])$('r_'+k).textContent=v==null?'':RT[k].find(([lim])=>v<=lim)[1]}
$('ft').textContent=`Vane ${d.vane_mv} mV · cups switch ${d.cups_open?'open':'closed'} · ${d.clicks} wind clicks · ${d.tips} rain tips · Wi-Fi ${d.rssi} dBm · up ${up(d.up)}`}
async function svc(){try{const e=ELEV==null?'':'&elevation='+ELEV,q=`latitude=${LAT}&longitude=${LON}&timezone=auto`;
const[w,a]=await Promise.all([fetch(`https://api.open-meteo.com/v1/forecast?${q}${e}&current=temperature_2m,relative_humidity_2m,surface_pressure,wind_speed_10m,wind_direction_10m,wind_gusts_10m,uv_index&daily=precipitation_sum&forecast_days=1`).then(r=>r.json()),fetch(`https://air-quality-api.open-meteo.com/v1/air-quality?${q}&current=pm2_5,carbon_dioxide`).then(r=>r.json())]);
const c=w.current,x=a.current;Object.assign(S,{t:c.temperature_2m,h:c.relative_humidity_2m,p:c.surface_pressure,wk:c.wind_speed_10m,wd:c.wind_direction_10m,gust:c.wind_gusts_10m,uv:c.uv_index,rain:w.daily.precipitation_sum[0],pm25:x.pm2_5,co2:x.carbon_dioxide});
$('src').textContent=`Compared with the Open-Meteo weather model for ${PLACE} (grid height ${Math.round(w.elevation)} m), as of ${c.time.slice(11)}. Its wind is for 10 m above ground, so expect lower wind readings here.`}
catch(e){$('src').textContent='Weather service unavailable; showing station readings only.'}}
async function tick(){const t0=Date.now();try{const c=new AbortController(),to=setTimeout(()=>c.abort(),2500);const r=await fetch('/data',{signal:c.signal,cache:'no-store'});render(await r.json());clearTimeout(to);st.className='';st.textContent='Live · '+new Date().toLocaleTimeString()}catch(e){st.className='off';st.textContent='Reconnecting…'}setTimeout(tick,Math.max(200,1000-(Date.now()-t0)))}
svc();setInterval(svc,300000);tick();
</script></body></html>)HTML";

static int jnum(char *b, size_t n, const char *k, float v, int d, bool omitNan = false) {
  if (isnan(v)) return omitNan ? 0 : snprintf(b, n, "\"%s\":null,", k);
  return snprintf(b, n, "\"%s\":%.*f,", k, d, v);
}

void handleData() {
  noInterrupts(); uint32_t clicks = windPulses, tips = rainTips; interrupts();
  float wd = readWindDir();
  char out[512];
  int o = snprintf(out, sizeof(out), "{\"clicks\":%u,\"tips\":%u,\"vane_mv\":%u,\"cups_open\":%d,\"rssi\":%d,\"up\":%lu,",
                   clicks, tips, analogReadMilliVolts(PIN_VANE), digitalRead(PIN_ANEMOMETER), WiFi.RSSI(), millis() / 1000);
  o += jnum(out + o, sizeof(out) - o, "t", tempC, 2);
  o += jnum(out + o, sizeof(out) - o, "h", humidity, 1);
  o += jnum(out + o, sizeof(out) - o, "p", presHpa, 1);
  o += jnum(out + o, sizeof(out) - o, "lux", lux < 0 ? NAN : lux, 1);
  o += jnum(out + o, sizeof(out) - o, "uv", uvSeen ? uvRaw / 96.0 : NAN, 2);
  o += jnum(out + o, sizeof(out) - o, "co2", co2 ? co2 : NAN, 0);
  o += jnum(out + o, sizeof(out) - o, "pm25", pmFresh ? lastPm.pm25_standard : NAN, 0);
  o += jnum(out + o, sizeof(out) - o, "wk", windKmh, 1);
  o += jnum(out + o, sizeof(out) - o, "wd", wd < 0 ? NAN : wd, 1);
  o += jnum(out + o, sizeof(out) - o, "rain", rainTodayTips() * 0.2794, 2);
  out[o - 1] = '}';
  web.send(200, "application/json", out);
}

void setup() {
  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);       // don't stall when no serial monitor is open
  delay(2000);
  Wire.begin(4, 5);       // bus 0: SCD41 first, its lamp pulses need the shortest power path
  Wire1.begin(10, 11);    // bus 1: light + UV

  haveSht = sht.begin(0x44);
  bool haveLux = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire1);
  haveBmp = bmp.begin_I2C(0x77, &Wire);
  if (haveBmp) {
    bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
    bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_2X);
    bmp.performReading();   // first reading after config is garbage
    Wire.beginTransmission(0x77); Wire.write(0x00); Wire.endTransmission(false);
    if (Wire.requestFrom((uint16_t)0x77, (uint8_t)1) == 1) {
      uint8_t id = Wire.read();
      Serial.printf("Pressure chip ID 0x%02X = %s\n", id, id == 0x50 ? "BMP388" : id == 0x60 ? "BMP390" : "unknown");
    }
  }
  haveLtr = ltr.begin(&Wire1);
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
  Serial.printf("Sensors: sht=%d bmp=%d scd=%d | bus 1: light=%d uv=%d\n", haveSht, haveBmp, haveScd, haveLux, haveLtr);

  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Serial.printf("Wi-Fi disconnected: %s\n", WiFi.STA.disconnectReasonName((wifi_err_reason_t)info.wifi_sta_disconnected.reason));
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
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
  prefs.begin("rainday");
}

void publishToEnvi() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;   // clock not synced yet

  // rain: tips in this 10 s window
  static uint32_t lastRainTotal = 0;
  noInterrupts(); uint32_t rainTotal = rainTips; interrupts();
  uint32_t rainNow = rainTotal - lastRainTotal; lastRainTotal = rainTotal;

  // missing readings are left out, never sent as 0
  float wd = readWindDir();
  char fields[480]; fields[0] = '{'; int o = 1;
  o += jnum(fields + o, sizeof(fields) - o, "tempC", tempC, 2, true);
  o += jnum(fields + o, sizeof(fields) - o, "humidity", humidity, 2, true);
  o += jnum(fields + o, sizeof(fields) - o, "lux", lux < 0 ? NAN : lux, 1, true);
  o += jnum(fields + o, sizeof(fields) - o, "pressure_hpa", presHpa, 2, true);
  o += jnum(fields + o, sizeof(fields) - o, "uv_index", uvSeen ? uvRaw / 96.0 : NAN, 2, true);
  o += jnum(fields + o, sizeof(fields) - o, "co2_ppm", co2 ? co2 : NAN, 0, true);
  o += jnum(fields + o, sizeof(fields) - o, "pm1", pmFresh ? lastPm.pm10_standard : NAN, 0, true);
  o += jnum(fields + o, sizeof(fields) - o, "pm25", pmFresh ? lastPm.pm25_standard : NAN, 0, true);
  o += jnum(fields + o, sizeof(fields) - o, "pm10", pmFresh ? lastPm.pm100_standard : NAN, 0, true);
  o += jnum(fields + o, sizeof(fields) - o, "wind_kmh", windKmh, 1, true);
  o += jnum(fields + o, sizeof(fields) - o, "wind_gust_kmh", gustKmh, 1, true);
  o += jnum(fields + o, sizeof(fields) - o, "wind_dir_deg", wd < 0 ? NAN : wd, 1, true);
  o += jnum(fields + o, sizeof(fields) - o, "rain_tips", rainNow, 0, true);
  o += jnum(fields + o, sizeof(fields) - o, "rain_mm", rainNow * 0.2794, 2, true);
  o += jnum(fields + o, sizeof(fields) - o, "rain_today_mm", rainDayKey == -1 ? NAN : rainTodayTips() * 0.2794, 2, true);
  if (o > 1) fields[o - 1] = '}'; else { fields[1] = '}'; fields[2] = 0; }
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
      configTzTime(LOCAL_TZ, "pool.ntp.org", "time.google.com");
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

  static unsigned long lastRead = 0;
  if (millis() - lastRead >= 1000) { lastRead = millis(); readSensors(); updateRainDay(); }

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

}
