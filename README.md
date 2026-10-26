# Backyard station firmware

Here is the ESP32-S3 code for my backyard weather station

It sends readings to [Envi](https://envi.myrondomain.com/) every 10s, has its own debug dashboard on the ESP's local IP (ex. http://192.168.1.205/ for me)

## History

I have tagged every iteration, use `git checkout [version]` to look at one

Versions:
- v0-scanner: i2c scanner, turned out the headers weren't soldered
- v2: all five i2c sensors
- v3: wifi and an adafruit io dashboard
- v4: pms5003 air quality sensor
- v5: mqtt to a raspberry pi
- v6: wind and rain
- v7: removed the pi, posts straight to envi
- v8: live page on the station
- v8.1: second i2c bus, ota updates, daily rain (never flashed)
- v8.2: fixed fake rain tips, this is what's running
- v8.3: v8.2 plus skipping missing readings (not flashed yet)

## Build

Copy `secrets.h.example` to `secrets.h` and fill it in

You will need these sensors: Adafruit SHT31, BH1750, Adafruit BMP3XX, Adafruit LTR390, Sensirion I2C SCD4X 0.4.0 and Adafruit PM25 AQI.

To download/compile:
```
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi station
```
