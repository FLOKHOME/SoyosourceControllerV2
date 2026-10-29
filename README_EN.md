# Soyosource Controller V2 — ESP8266 Zero-Export Regulator

**Languages:** [Deutsch](README.md) | [English](README_EN.md)

---

> ⚠️ **STATUS: PROTOTYPE — REAL-WORLD TEST PENDING**
>
> This firmware has **not yet been tested against a real Soyosource inverter!**
> The RS485 communication follows the protocol documented and proven in the reference
> implementations ([macaron-software], [syssi]). Compilation, web UI, JSON API, config
> GUI, MQTT wiring and OTA update have been verified on a real D1 mini Pro —
> **the RS485 link to the inverter has not**.
>
> For first deployment, watch `frameCount`/`errorCount` on the web UI
> (see [Pending real-world test](#-pending-real-world-test)).

---

The ESP8266 is connected via an RS485 module to one (or more) **Soyosource 1200**
solar/battery inverters and tells it how much power to inject into the grid.

- **ManualMode:** enter and send the target power on the ESP web page
- **AutoMode:** the ESP receives the target via MQTT topic or web interface
  (e.g. the grid measurement of a Shelly 3EM Pro across 3 phases) and automatically
  regulates the Soyosource to zero export

In short: this firmware replaces the original limiter — a revival of the
[original project by BavarianSuperGuy/KlausLi], whose source code was lost. This V2
was rebuilt from the compiled binary (string extraction) **and** the open reference
implementations (see [Sources & inspiration](#-sources--inspiration)).

## ✨ Features

| Feature | Description |
|---|---|
| **Zero export** | Regulation within a −20 W / +5 W tolerance band (grid draw ≈ 0) |
| **Multi-inverter** | `SoyoCount` (1–8): total setpoint is divided by the count, identical command to all units on the RS485 bus (rounded down — never over-injects) |
| **Battery-mode limit** | Hard limit of **900 W per Soyo** (battery CP mode); MaxPower is automatically capped at `900 × SoyoCount` on save |
| **5 operating modes** | AutoMqtt, AutoMqtt2, AutoShellyHttpClient, AutoHttpInterface, StaticHttpInterface |
| **Shelly 3EM Pro** | Gen2 RPC API (`/rpc/EM.GetStatus?id=0`) and Tasmota API (`/cm?cmnd=status+10`), per-phase factors L1/L2/L3 |
| **Web UI** | Dark theme, live values (1.5 s), blinking "heartbeat" title, DC values, colour-coded RSSI |
| **Config GUI** | All settings in the browser (`/config`): mode, Shelly, MQTT, topics, Wi-Fi — save with auto-reboot |
| **MQTT** | Setpoint/NotAus/MaxPower/WaitSekunden via topics, status JSON, Home Assistant auto-discovery |
| **Wi-Fi provisioning** | Captive-portal AP (`SOYOSOURCE-Controller`) + Wi-Fi fields in the config GUI + serial command |
| **OTA update** | ElegantOTA v4 (`/update`) — flash via browser or `curl` over the network |
| **mDNS** | `http://soyosource.local`, hostname `soyosource` visible in the router |
| **Power-fail safe** | Config + NotAus state stored in LittleFS; clean 0 W start after boot before the mode takes over |
| **Safety** | No Soyosource signals → the inverter stops feeding by itself; NotAus actively sends 0 W + limiter off |

## ⚠️ Safety notes

- The Soyosource **stops feeding by itself** when no RS485 signals arrive (vendor safety).
  This project deliberately builds on that behaviour.
- **NotAus** stays **active** after a power failure (persisted) — release only explicitly
  via web/MQTT (`/NotAusAus` or `0`/`OFF` on the NotAus topic).
- The author (and the source projects) accept no liability. You are responsible for your
  own actions.

## 🔧 Hardware

Tested with a **Wemos D1 mini Pro** (ESP-12, 16 MB flash, CP2102). Any 4 MB ESP8266
(ESP-12e/f/s, NodeMCU, D1 mini) works as well.

```
ESP8266 (D1 mini Pro) ── RS485 (MAX485 @ 3.3V) ══ A/B ══ Soyosource 1
                                    │                    ═╋═ (parallel)
                                    │                    ══ Soyosource 2
                                    └── WiFi ── MQTT broker / Shelly 3EM Pro
```

### D1 mini pin assignment

| D1 mini | MAX485 | Function |
|---------|--------|----------|
| D1 (GPIO5) | DI | TX — send power commands |
| D2 (GPIO4) | RO | RX — receive status frames |
| D5 (GPIO14) | DE + RE (bridged) | RS485 direction control |
| 3V3 | VCC | ⚠️ **3.3 V — not 5 V/VIN** (ESP8266 is not 5V-tolerant) |
| GND | GND | |

> **Do not use D3 (GPIO0)** — boot-strap pin (FLASH button) and DTR line of the
> USB-serial adapter. Legacy pin plans using D3 are obsolete.

### RS485 bus

- **A/B as a bus wired in parallel** to all Soyos (2 wires) — same as the original controller.
- No RJ45 needed, **no 120 Ω termination** (9600 baud, short bus runs).
- GND between ESP and Soyo is optional; first fix to try when frames get corrupted.
- Recommended module: **MAX3485 (3.3 V)**, or a classic MAX485 module powered from 3V3.

## 📦 Installation

### PlatformIO (recommended)

```bash
git clone https://github.com/FLOKHOME/SoyosourceControllerV2.git
cd SoyosourceControllerV2
pio run --target upload        # firmware
pio run --target uploadfs      # web UI (data/index.html) onto LittleFS
```

Board profile: `d1_mini_pro` (16 MB, **DIO flash mode**, ldscript `eagle.flash.4m1m.ld`).
Works unchanged on 4 MB boards.

### FlashESP8266.exe / Tasmotizer

Alternative for first bring-up: flash a ready-made `.bin` with **FlashESP8266.exe** (from
the `espflasher` folder of the original repo) or **Tasmotizer**.
⚠️ **Check "Erase" on the first flash** — then install the web UI via OTA (`/update`) or
`pio run --target uploadfs`.

## 🚀 First start

1. **First boot without Wi-Fi:** after ~45 s of connection attempts, the AP
   **`SOYOSOURCE-Controller`** (open) starts automatically. Pixel/newer Android devices
   sometimes refuse ESP8266 softAPs — **use a laptop/PC**.
2. **Config GUI:** `http://192.168.4.1/config` — enter Wi-Fi at the bottom → Save → reboot.
3. Then reachable at **http://soyosource.local** or via DHCP IP (router list, hostname
   `soyosource`).
4. Choose **mode + measurement source** in the config GUI (e.g. `AutoShellyHttpClient`
   + Shelly IP).
5. Set **SoyoCount** (number of Soyos on the bus!) — the setpoint gets divided.

**Serial commands** (115200 baud, USB):

| Command | Effect |
|---|---|
| `wifi:SSID,PASSWORD` | store Wi-Fi + reboot (no browser needed) |
| `creds` | show stored Wi-Fi SSID |
| `fs` | LittleFS / config.json diagnostics |
| `ip` | current IP |

## 🌐 Web interface

| Route | Function |
|---|---|
| `/` | Main page: heartbeat, gauge, DC values, NotAus, manual entry |
| `/data` | JSON live data (all fields, incl. `perSoyoPower`) |
| `/config` | **Config GUI** — all settings, save with reboot |
| `/config.json` | Raw configuration (API) |
| `/dataonce?value=600` | Set manual setpoint (total power, gets divided) |
| `/NotAus` | Immediate 0 W + limiter off (persistent!) |
| `/NotAusAus` | Release |
| `/L1L2L3Auto?Value=…` / `/L1L2L3Static?Value=…` | Set phase factors |
| `/MaxPower?Value=…` | Set MaxPower (auto-capped at 900 × SoyoCount) |
| `/wifi`, `/wifisave?s=…&p=…`, `/scan` | Wi-Fi provisioning |
| `/update` | **ElegantOTA** — firmware upload via browser |
| `/ota/upload` | OTA endpoint (multipart, field `file`) |
| `/ReSeT` | Reboot |

## 🔄 Operating modes

| Mode | Description |
|---|---|
| `AutoMqtt` | Setpoint arrives via MQTT topic `…/L1L2L3` (e.g. from a smart-meter publisher) |
| `AutoMqtt2` | alternative MQTT path (same mechanism) |
| `AutoShellyHttpClient` | fetches grid power from the Shelly 3EM Pro (Gen2 RPC or Tasmota) |
| `AutoHttpInterface` | fetches the setpoint via HTTP GET (`HttpGetUrl` + optional `JsonValuePath`) |
| `StaticHttpInterface` | static value (set via `/dataonce` or the config GUI) |

All interfaces keep running — **only the selected mode is evaluated**
(same as the original).

## ⚙️ Control behaviour

```
Setpoint (total, e.g. 600 W)
  → tolerance:  −20…+5 W               → 0 W (zero export)
  → cap:        min(MaxPower, 900 W × SoyoCount)   [battery-mode limit]
  → division:   setpoint ÷ SoyoCount   (rounded down)
  → send:       identical 0x13 command to all Soyos on the bus
  → interval:   3 s (WaitSekunden, configurable) between sends
  → change:     only sends on ≥ 5 W difference
```

Example: Shelly reports **600 W** consumption, `SoyoCount = 2` → **each Soyo gets 300 W**.

Boot behaviour: after a power failure it first cleanly sends **0 W** (defined start
state), then the selected mode regulates back up. The NotAus state survives the outage.

## 📡 RS485 protocol (9600 baud, 8N1)

> Reference: [macaron-software] & [syssi] — **not yet** verified against a real Soyo!

### SOYO → ESP (status frame, 15 bytes)

```
A6 | PWR_H | PWR_L | OPMODE | ERR | V_H | V_L | A_H | A_L | ACV_H | ACV_L | HZ | TEMP_H | TEMP_L | CK
```

| Byte | Content |
|------|---------|
| 0 | Header `0xA6` |
| 1–2 | Output power (big-endian, W) |
| 3 | Opmode: high nibble 1=CP, 2=PV, 4=standby, 8=limiter |
| 4 | Status bits (DC low/high, AC high/low, overheat, limiter) |
| 5–6 | Battery voltage × 10 |
| 7–8 | Battery current × 10 |
| 9–10 | Grid voltage |
| 11 | Grid frequency × 2 |
| 12–13 | Temperature × 10 (offset −30 °C) |
| 14 | Checksum `0xFF − sum(bytes 1..13)` |

### ESP → SOYO (command frame, 6 bytes)

```
55 | CMD | D0 | D1 | D2 | CK      (CK = 0xFF − sum(bytes 1..4))
```

| CMD | Function | Example |
|-----|----------|---------|
| `0x01` | Request status | `55 01 00 00 00 FE` |
| `0x02` | Set mode | `55 02 02 00 12 XX` (bat-CP + limiter) |
| `0x03` | Limiter | `55 03 23 64 01 74` (off) |
| `0x13` | **Set power** | `55 13 1E 64 00 XX` (300 W, in 10 W steps) |
| `0x83` | Start delay | `55 83 03 00 00 XX` (3 s) |

## 📝 MQTT

| Topic | Direction | Payload |
|-------|-----------|---------|
| `soyosource/L1L2L3` | → ESP | setpoint **total** in W (e.g. `600`) — divided by SoyoCount |
| `soyosource/NotAus` | → ESP | `1`/`ON` = NotAus, `0`/`OFF` = release (persistent) |
| `soyosource/WaitSekunden` | → ESP | send interval 1–10 s |
| `soyosource/MaxPower` | → ESP | MaxPower total, W (auto-capped) |
| `soyosource/info` + `…/data` | ← ESP | status JSON (15 s) |

QoS 0 (fire & forget). Home Assistant auto-discovery is published on connect
(Power, DCVolts, DCAmps, WattsOut, targetPower, Temperature).

## 🛡️ Power-failure behaviour

| Data | Survives outage? |
|---|---|
| Config (mode, Shelly, MQTT, topics, MaxPower, WaitSekunden, L1/L2/L3, SOYO ID, Wi-Fi) | ✅ (LittleFS, written on every change) |
| NotAus state | ✅ persistent — lock stays until explicit release |
| Runtime values (DC, target) | ❌ reset — **deliberately safe**: boot sends 0 W, then the mode regulates fresh |

## 🔍 Known behaviours

- **Band-steering routers** (enterprise/mesh, e.g. next to `SSID-8021X` networks): 2.4 GHz
  clients are sometimes rejected for 15–30 s. The firmware retries for 45 s + keeps
  retrying in the background from the AP fallback and recovers on its own.
- **Same-SSID collisions** (equally named open guest network): the BSSID-lock scan picks
  the encrypted AP with the highest RSSI.
- **Pixel 8 Pro & ESP8266 softAP**: known Android 14/15 association problems → do the
  Wi-Fi setup from a laptop. Captive portal + `/generate_204` redirect are built in.
- RSSI display is only meaningful in STA mode (AP mode shows 0).

## 📜 Sources & inspiration

This project would not exist without the following work:

| Source | Contribution |
|---|---|
| **[KlausLi/Esp-Soyosource-Controller]** (BavarianSuperGuy) | The original — firmware BIN `v1.0.3.3`, feature set, web interface concept, MQTT topic structure, ConfigMQTT.json layout, the "NotAus" concept, videos & wiring diagrams. The source code was never public; V2 is the reconstruction + modernisation. |
| **[macaron-software/soyosource-esp32-grid-limiter]** | **RS485 frame reference** (0xA6/0x55 frames, checksums `0xFF − sum`, commands 0x01/0x02/0x03/0x13/0x83), limiter protocol at 4800 baud |
| **[syssi/esphome-soyosource-gtn-virtual-meter]** | Protocol foundation & community knowledge on Soyosource GTN / GTN-48V |
| **[ayushsharma82/ElegantOTA]** v4 | OTA update mechanics |
| **[ESPAsyncWebServer]/[ESPAsyncTCP]** (esp32async) | Async web server / TCP |
| **[ArduinoJson]** v7, **[PubSubClient]** | JSON + MQTT |
| **khoih-prog/ESPAsync_WiFiManager** | Portal design inspiration (V2 replaced it with a lean built-in solution) |

**Reconstruction method:** 12,989 readable strings (~85 KB) extracted from the original
BIN (see [`docs/firmware_v1.0.3.3_strings.txt`](docs/firmware_v1.0.3.3_strings.txt)) —
web routes, MQTT topics, JSON config layout and mode names identified from them; the
RS485 protocol came from the open references above. The original C++ source is considered
lost.

## 🧪 Pending real-world test

To verify before productive use with real Soyos (MAX485 on D1/D2/D5 + 3V3/GND, A/B in
parallel to the Soyos):

- [ ] `frameCount` rises in `/data` → status frames (`0xA6`) arriving
- [ ] `errorCount` stays 0 → checksums match (otherwise: common GND, swap A/B)
- [ ] `DCVolts`/`DCAmps`/`DCWatts` show plausible values from the Soyos
- [ ] Verify opmode bits of the real devices (bat-CP = 1?)
- [ ] Verify power command `0x13` on a real Soyo (reaction to 100/300/600 W)
- [ ] Safety behaviour: signal stop → Soyo switches off (vendor safety)
- [ ] NotAus on the real system: 0 W + limiter-off takes effect immediately
- [ ] Shelly 3EM Pro (Gen2 RPC **and** Tasmota) delivers plausible values
- [ ] MQTT chain incl. HA discovery in Home Assistant
- [ ] Long-term test (≥ 1 day auto mode, stability, re-join after Wi-Fi outage)

**Feedback welcome** — especially test results on a real Soyosource (GTN 1200 / GTN-48V /
HHJ60), so the protocol table can be confirmed or corrected.

## 📜 License

MIT — see [LICENSE](LICENSE). Based on the work of BavarianSuperGuy/KlausLi (MIT) and the
cited open-source projects.

[KlausLi/Esp-Soyosource-Controller]: https://github.com/KlausLi/Esp-Soyosource-Controller
[macaron-software/soyosource-esp32-grid-limiter]: https://github.com/macaron-software/soyosource-esp32-grid-limiter
[syssi/esphome-soyosource-gtn-virtual-meter]: https://github.com/syssi/esphome-soyosource-gtn-virtual-meter
[ayushsharma82/ElegantOTA]: https://github.com/ayushsharma82/ElegantOTA
[ESPAsyncWebServer]: https://github.com/ESP32Async/ESPAsyncWebServer
[ESPAsyncTCP]: https://github.com/ESP32Async/ESPAsyncTCP
[ArduinoJson]: https://github.com/bblanchon/ArduinoJson
[PubSubClient]: https://github.com/knolleary/pubsubclient
[original project by BavarianSuperGuy/KlausLi]: https://github.com/KlausLi/Esp-Soyosource-Controller