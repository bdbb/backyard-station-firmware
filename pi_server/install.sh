#!/usr/bin/env bash
# Raspberry Pi 5 data server (August 6 – September 30, 2026; retired when the Pi was lost).
# Raspberry Pi OS (Debian trixie). Run step by step; fill in tokens afterwards.
set -e
sudo apt update && sudo apt full-upgrade -y
sudo apt install -y locales-all          # fixes locale warnings over SSH

# MQTT broker
sudo apt install -y mosquitto mosquitto-clients
sudo cp mosquitto_station.conf /etc/mosquitto/conf.d/station.conf
sudo systemctl enable --now mosquitto && sudo systemctl restart mosquitto

# Grafana
sudo mkdir -p /etc/apt/keyrings
wget -q -O - https://apt.grafana.com/gpg.key | gpg --dearmor | sudo tee /etc/apt/keyrings/grafana.gpg > /dev/null
echo "deb [signed-by=/etc/apt/keyrings/grafana.gpg] https://apt.grafana.com stable main" | sudo tee /etc/apt/sources.list.d/grafana.list
sudo apt update && sudo apt install -y grafana
sudo cp grafana_datasource_influx.yaml /etc/grafana/provisioning/datasources/influx.yaml
sudo systemctl enable --now grafana-server

# InfluxDB 2 — current signing key (InfluxData rotated subkeys on 2026-01-06; the old _compat key fails).
# Pipe the key into gpg: "gpg --dearmor FILE" writes FILE.gpg and sends nothing down the pipe.
curl -fsSL https://repos.influxdata.com/influxdata-archive.key | gpg --dearmor | sudo tee /etc/apt/keyrings/influxdata.gpg > /dev/null
echo "deb [signed-by=/etc/apt/keyrings/influxdata.gpg] https://repos.influxdata.com/debian stable main" | sudo tee /etc/apt/sources.list.d/influxdata.list
sudo apt update && sudo apt install -y influxdb2   # accept the maintainer's influxdata.list when asked
sudo systemctl enable --now influxdb
# Then: http://<pi>:8086 setup wizard -> org "envstation", bucket "sensors", copy the API token.

# Telegraf (MQTT -> InfluxDB + envi)
sudo apt install -y telegraf
sudo cp telegraf_station.conf /etc/telegraf/telegraf.d/station.conf   # then put the real tokens in
sudo systemctl enable --now telegraf && sudo systemctl restart telegraf

systemctl status mosquitto grafana-server influxdb telegraf --no-pager | grep -E "●|Active"
# Watch live data: mosquitto_sub -t 'station/#' -v
