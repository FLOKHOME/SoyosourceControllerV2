# Soyosource Controller V2 — ESP8266 Nulleinspeisungs-Regler

**Sprachen:** [Deutsch](README.md) | [English](README_EN.md)

---

> ⚠️ **STATUS: PROTOTYP — ECHT-TEST AUSSTEHEND**
>
> Diese Firmware ist **noch nicht an einem echten Soyosource-Wechselrichter getestet!**
> Die RS485-Kommunikation basiert auf dem Protokoll, das in referenz-Implementierungen
> ([macaron-software], [syssi]) dokumentiert und erprobt ist. Kompilierung, Web-Interface,
> JSON-API, Config-GUI, MQTT-Struktur und OTA-Update wurden am realen D1 mini Pro
> verifiziert — **die RS485-Strecke zum Wechselrichter nicht**.
>
> Erster Einsatz unbedingt mit Beobachtung von `frameCount`/`errorCount` über die
> Web-Oberfläche (siehe [Ausstehender Echt-Test](#-ausstehender-echt-test)).

---

Der ESP8266 ist über ein RS485-Modul mit einem (oder mehreren) **Soyosource 1200**
Solar-/Batterie-Wechselrichtern verbunden und teilt ihm die Leistungswerte mit, die er
im Stromnetz ausgleichen bzw. einspeisen soll.

- **ManualMode:** Leistungswert auf der ESP-Webseite eingeben und senden
- **AutoMode:** Der ESP empfängt den Sollwert über MQTT-Topic oder Web-Interface
  (z. B. der gemessene Netzwert eines Shelly 3EM Pro über 3 Phasen) und regelt
  den Soyosource automatisch auf Nulleinspeisung

Kurz gesagt: Die Firmware ersetzt den originalen Limiter — als Neuauflage des
[Originalprojekts von BavarianSuperGuy/KlausLi], dessen Quellcode verloren gegangen war.
Diese V2 wurde aus dem kompilierten BIN (String-Extraktion) **und** den offenen
Referenz-Implementierungen neu aufgebaut (siehe [Quellen & Inspirationen](#-quellen--inspirationen)).

## ✨ Funktionen

| Funktion | Beschreibung |
|---|---|
| **Nulleinspeisung** | Regelung in Toleranzbereich −20 W / +5 W (Netzbezug ≈ 0) |
| **Multi-Soyo** | `SoyoCount` (1–8): Gesamtsollwert wird durch Anzahl geteilt, identischer Befehl an alle am RS485-Bus (abrundend — nie Mehr-Einspeisung) |
| **Batterie-Modus-Limit** | Hartes Limit **900 W pro Soyo** (Batterie-CP-Modus), MaxPower wird beim Speichern automatisch auf `900 × SoyoCount` begrenzt |
| **5 Betriebsmodi** | AutoMqtt, AutoMqtt2, AutoShellyHttpClient, AutoHttpInterface, StaticHttpInterface |
| **Shelly 3EM Pro** | Gen2-RPC-API (`/rpc/EM.GetStatus?id=0`) und Tasmota-API (`/cm?cmnd=status+10`), Phasen-Faktoren L1/L2/L3 |
| **Web-Oberfläche** | Dunkles Theme, Live-Werte (1,5 s), „ESP Herzschlag"-Titel, DC-Werte, RSSI farbcodiert |
| **Config-GUI** | Alle Einstellungen per Browser (`/config`): Modus, Shelly, MQTT, Topics, WLAN — Speichern mit Auto-Reboot |
| **MQTT** | Sollwert/NotAus/MaxPower/WaitSekunden per Topic, Status-JSON, Home-Assistant-Auto-Discovery |
| **WLAN-Konfiguration** | Captive-Portal-AP (`SOYOSOURCE-Controller`) + WLAN-Felder in der Config-GUI + Serial-Befehl |
| **OTA-Update** | ElegantOTA v4 (`/update`) — Firmware per Browser oder `curl` übers Netz |
| **mDNS** | `http://soyosource.local`, Hostname `soyosource` im Router sichtbar |
| **Stromausfall-sicher** | Konfig + NotAus-Zustand in LittleFS; nach Boot sauberer 0-W-Start, bevor der Modus neu regelt |
| **Safety** | Keine Soyosource-Signale → Wechselrichter hört von selbst auf einzuspeisen; NotAus sendet aktiv 0 W + Limiter-Off |

## ⚠️ Sicherheitshinweise

- Der Soyosource stoppt die Einspeisung **von selbst**, wenn keine RS485-Signale mehr
  ankommen (Hersteller-Safety). Darauf baut dieses Projekt bewusst auf.
- **NotAus** bleibt nach Stromausfall **aktiv** (persistent gespeichert) — Freigabe nur
  explizit über Web/MQTT (`/NotAusAus` bzw. `0`/`OFF` auf dem NotAus-Topic).
- Der Autor (und die Quellprojekte) übernehmen keine Haftung. Jeder ist für sein Tun
  selbst verantwortlich.

## 🔧 Hardware

Getestet mit **Wemos D1 mini Pro** (ESP-12, 16 MB Flash, CP2102). Jeder 4-MB-ESP8266
(ESP-12e/f/s, NodeMCU, D1 mini) funktioniert ebenfalls.

```
ESP8266 (D1 mini Pro) ── RS485 (MAX485 @ 3,3V) ══ A/B ══ Soyosource 1
                                    │                    ═╋═ (parallel)
                                    │                    ══ Soyosource 2
                                    └── WiFi ── MQTT Broker / Shelly 3EM Pro
```

### Pin-Belegung D1 mini

| D1 mini | MAX485 | Funktion |
|---------|--------|----------|
| D1 (GPIO5) | DI | TX — Leistungsbefehle senden |
| D2 (GPIO4) | RO | RX — Status-Frames empfangen |
| D5 (GPIO14) | DE + RE (brückt) | RS485-Richtungssteuerung |
| 3V3 | VCC | ⚠️ **3,3 V — nicht 5 V/VIN** (ESP8266 ist nicht 5V-tolerant) |
| GND | GND | |

> **D3 (GPIO0) nicht verwenden** — Boot-Strap-Pin (FLASH-Button) und DTR-Leitung
> des USB-Serial-Adapters. Ursprüngliche Pin-Pläne mit D3 sind obsolet.

### RS485-Bus

- **A/B als Bus parallel** zu allen Soyos (2 Drähte) — identisch zum Original-Controller.
- Kein RJ45 nötig, **keine 120-Ω-Abschlusswiderstände** (9600 Baud, kurze Buslängen).
- GND-Verbindung ESP ↔ Soyo optional; erster Hebel bei Störungen.
- Empfohlenes Modul: **MAX3485 (3,3 V)** oder klassisches MAX485-Modul aus der 3V3-Leitung
  gespeist.

## 📦 Installation

### PlatformIO (empfohlen)

```bash
git clone https://github.com/FLOKHOME/SoyosourceControllerV2.git
cd SoyosourceControllerV2
pio run --target upload        # Firmware
pio run --target uploadfs      # Web-UI (data/index.html) auf LittleFS
```

Board-Profile: `d1_mini_pro` (16 MB, **DIO-Flash-Modus**, ldscript `eagle.flash.4m1m.ld`).
Für 4-MB-Boards läuft es unverändert.

### FlashESP8266.exe / Tasmotizer

Alternative zur ersten Inbetriebnahme: fertige `.bin` mit **FlashESP8266.exe** (aus dem
`espflasher`-Ordner des Original-Repos) oder **Tasmotizer** flashen.
⚠️ **Beim ersten Flash „Erase" anhaken** — danach Web-UI per OTA (`/update`) oder
`pio run --target uploadfs` nachziehen.

## 🚀 Inbetriebnahme

1. **Erster Boot ohne WLAN:** AP **`SOYOSOURCE-Controller`** (offen) startet nach ca. 45 s
   Verbindungsversuch automatisch. Pixel-/neuere Android-Geräte assoziieren teils nicht
   mit ESP8266-SoftAPs — **Laptop/PC verwenden**.
2. **Config-GUI:** `http://192.168.4.1/config` — WLAN unten eintragen → Speichern → Reboot.
3. Danach erreichbar über **http://soyosource.local** oder die DHCP-IP (Router-Liste,
   Hostname `soyosource`).
4. **Modus + Messquelle** in der Config-GUI wählen (z. B. `AutoShellyHttpClient` + Shelly-IP).
5. **SoyoCount** setzen (Anzahl Soyos am Bus!) — der Sollwert wird geteilt.

**Serial-Befehle** (115200 Baud, USB):

| Befehl | Wirkung |
|---|---|
| `wifi:SSID,PASSWORT` | WLAN speichern + Reboot (ohne Browser) |
| `creds` | gespeicherte WLAN-SSID anzeigen |
| `fs` | LittleFS-/config.json-Diagnose |
| `ip` | aktuelle IP |

## 🌐 Web-Interface

| Route | Funktion |
|---|---|
| `/` | Hauptseite: Herzschlag, Gauge, DC-Werte, NotAus, manuelle Eingabe |
| `/data` | JSON-Live-Daten (alle Felder, inkl. `perSoyoPower`) |
| `/config` | **Config-GUI** — alle Einstellungen, Speichern mit Reboot |
| `/config.json` | Rohe Konfiguration (API) |
| `/dataonce?value=600` | Manuellen Sollwert setzen (Gesamtleistung, wird geteilt) |
| `/NotAus` | Sofort 0 W + Limiter-Off (persistent!) |
| `/NotAusAus` | Freigabe |
| `/L1L2L3Auto?Value=…` / `/L1L2L3Static?Value=…` | Phasen-Faktoren setzen |
| `/MaxPower?Value=…` | MaxPower setzen (auto-gecapped auf 900 × SoyoCount) |
| `/wifi`, `/wifisave?s=…&p=…`, `/scan` | WLAN-Konfiguration |
| `/update` | **ElegantOTA** — Firmware-Upload per Browser |
| `/ota/upload` | OTA-Endpunkt (multipart, Feld `file`) |
| `/ReSeT` | Neustart |

## 🔄 Betriebsmodi

| Modus | Beschreibung |
|---|---|
| `AutoMqtt` | Sollwert kommt per MQTT-Topic `…/L1L2L3` (z. B. vom Smart-Meter-Publisher) |
| `AutoMqtt2` | alternativer MQTT-Pfad (gleicher Mechanismus) |
| `AutoShellyHttpClient` | holt die Netzleistung vom Shelly 3EM Pro (Gen2-RPC oder Tasmota) |
| `AutoHttpInterface` | holt den Sollwert per HTTP-GET (`HttpGetUrl` + optionaler `JsonValuePath`) |
| `StaticHttpInterface` | statischer Wert (über `/dataonce` oder Config-GUI gesetzt) |

Alle Interfaces laufen parallel weiter — **ausgewertet wird nur der gewählte Modus**
(so auch im Original).

## ⚙️ Regelverhalten

```
Sollwert (gesamt, z. B. 600 W)
  → Toleranz:   −20…+5 W              → 0 W (Nulleinspeisung)
  → Cap:        min(MaxPower, 900 W × SoyoCount)   [Batterie-Modus-Limit]
  → Verteilung: Sollwert ÷ SoyoCount  (abrunden)
  → Sendung:    identischer 0x13-Befehl an alle Soyos am Bus
  → Intervall:  3 s (WaitSekunden, einstellbar) zwischen Sendungen
  → Änderung:   sendet nur bei ≥ 5 W Differenz
```

Beispiel: Shelly meldet **600 W** Verbrauch, `SoyoCount = 2` → **jeder Soyo bekommt 300 W**.

Boot-Verhalten: nach Stromausfall erst sauber **0 W senden** (definierter Startzustand),
dann regelt der gewählte Modus neu hoch. NotAus-Zustand überlebt den Ausfall.

## 📡 RS485-Protokoll (9600 Baud, 8N1)

> Referenz: [macaron-software] & [syssi] — am echten Soyo noch **nicht** verifiziert!

### SOYO → ESP (Status-Frame, 15 Bytes)

```
A6 | PWR_H | PWR_L | OPMODE | ERR | V_H | V_L | A_H | A_L | ACV_H | ACV_L | HZ | TEMP_H | TEMP_L | CK
```

| Byte | Inhalt |
|------|--------|
| 0 | Header `0xA6` |
| 1–2 | Output Power (big-endian, W) |
| 3 | Opmode: High-Nibble 1=CP, 2=PV, 4=Standby, 8=Limiter |
| 4 | Status-Bits (DC low/high, AC high/low, Overheat, Limiter) |
| 5–6 | Batteriespannung × 10 |
| 7–8 | Batteriestrom × 10 |
| 9–10 | Netzspannung |
| 11 | Netzfrequenz × 2 |
| 12–13 | Temperatur × 10 (Offset −30 °C) |
| 14 | Checksumme `0xFF − sum(Byte 1..13)` |

### ESP → SOYO (Kommando-Frame, 6 Bytes)

```
55 | CMD | D0 | D1 | D2 | CK      (CK = 0xFF − sum(Byte 1..4))
```

| CMD | Funktion | Beispiel |
|-----|----------|----------|
| `0x01` | Status anfordern | `55 01 00 00 00 FE` |
| `0x02` | Modus setzen | `55 02 02 00 12 XX` (Bat-CP + Limiter) |
| `0x03` | Limiter | `55 03 23 64 01 74` (Off) |
| `0x13` | **Power setzen** | `55 13 1E 64 00 XX` (300 W, in 10-W-Schritten) |
| `0x83` | Start-Delay | `55 83 03 00 00 XX` (3 s) |

## 📝 MQTT

| Topic | Richtung | Payload |
|-------|----------|---------|
| `soyosource/L1L2L3` | → ESP | Sollwert **gesamt** in W (z. B. `600`) — wird durch SoyoCount geteilt |
| `soyosource/NotAus` | → ESP | `1`/`ON` = NotAus, `0`/`OFF` = frei (persistent) |
| `soyosource/WaitSekunden` | → ESP | Sendeintervall 1–10 s |
| `soyosource/MaxPower` | → ESP | MaxPower gesamt, W (auto-gecapped) |
| `soyosource/info` + `…/data` | ← ESP | Status-JSON (15 s) |

QoS 0 (fire & forget). Home-Assistant-Auto-Discovery wird beim Connect gepublisht
(Power, DCVolts, DCAmps, WattsOut, targetPower, Temperature).

## 🛡️ Stromausfall-Verhalten

| Daten | Überlebt Ausfall? |
|---|---|
| Konfig (Modus, Shelly, MQTT, Topics, MaxPower, WaitSekunden, L1/L2/L3, SOYO-ID, WLAN) | ✅ (LittleFS, schreibt bei jeder Änderung) |
| NotAus-Zustand | ✅ persistent — Sperre bleibt bis expliziter Freigabe |
| Laufzeitwerte (DC, Target) | ❌ resetten — **bewusst sicher**: Boot sendet 0 W, danach regelt der Modus neu |

## 🔍 Bekannte Verhaltensweisen

- **Band-Steering-Router** (Enterprise/Mesh, z. B. neben `SSID-8021X`-Netzen): 2,4-GHz-Clients
  werden teils 15–30 s abgewiesen. Die Firmware retryt 45 s + dauerversucht im AP-Fallback
  im Hintergrund und findet von selbst zurück.
- **Same-SSID-Kollisionen** (gleichnamiges offenes Gäste-Netz): BSSID-Lock-Scan wählt das
  verschlüsselte AP mit dem höchsten RSSI.
- **Pixel 8 Pro & ESP8266-SoftAP**: bekannte Android-14/15-Assoziationsprobleme →
  WLAN-Konfig am Laptop machen. Captive Portal + `/generate_204`-Redirect sind eingebaut.
- RSSI-Anzeige nur im STA-Modus aussagekräftig (AP-Modus zeigt 0).

## 📜 Quellen & Inspirationen

Dieses Projekt wäre ohne folgende Arbeiten nicht entstanden:

| Quelle | Beitrag |
|---|---|
| **[KlausLi/Esp-Soyosource-Controller]** (BavarianSuperGuy) | Das Original — Firmware-BIN `v1.0.3.3`, Funktionsumfang, Web-Interface-Konzept, MQTT-Topic-Struktur, ConfigMQTT.json-Layout, „NotAus"-Konzept, Videos & Bastelpläne. Der Quellcode war nie öffentlich; die V2 ist die Rekonstruktion + Modernisierung. |
| **[macaron-software/soyosource-esp32-grid-limiter]** | **RS485-Frame-Referenz** (0xA6/0x55-Frames, Checksummen `0xFF − sum`, Kommandos 0x01/0x02/0x03/0x13/0x83), Limiter-Protokoll auf 4800 Baud |
| **[syssi/esphome-soyosource-gtn-virtual-meter]** | Protokoll-Grundlage & Community-Wissen zu Soyosource GTN / GTN-48V |
| **[ayushsharma82/ElegantOTA]** v4 | OTA-Update-Mechanik |
| **[ESPAsyncWebServer]/[ESPAsyncTCP]** (esp32async) | Async-Webserver/TCP |
| **[ArduinoJson]** v7, **[PubSubClient]** | JSON + MQTT |
| **khoih-prog/ESPAsync_WiFiManager** | Inspiration fürs Portal-Design (in V2 durch eigene schlanke Lösung ersetzt) |

**Rekonstruktionsmethode:** 12.989 lesbare Strings (~85 KB) aus dem Original-BIN extrahiert
(siehe [`docs/firmware_v1.0.3.3_strings.txt`](docs/firmware_v1.0.3.3_strings.txt)) — daraus
Web-Routen, MQTT-Topics, JSON-Konfigurationslayout und Modus-Namen identifiziert; das
RS485-Protokoll aus den offenen Referenzen oben. Der originale C++-Quellcode gilt als
verloren.

## 🧪 Ausstehender Echt-Test

Vor dem Produktivbetrieb mit echten Soyos zu verifizieren (MAX485 an D1/D2/D5 + 3V3/GND,
A/B parallel auf die Soyos):

- [ ] `frameCount` steigt in `/data` → Status-Frames (`0xA6`) kommen rein
- [ ] `errorCount` bleibt 0 → Checksummen stimmen (sonst: GND-Brücke, A/B tauschen)
- [ ] `DCVolts`/`DCAmps`/`DCWatts` zeigen plausible Werte der Soyos
- [ ] Opmode-Bits der echten Geräte gegenprüfen (Bat-CP = 1?)
- [ ] Power-Befehl `0x13` am echten Soyo prüfen (Reaktion auf 100/300/600 W)
- [ ] Safety-Verhalten: Signal-Stopp → Soyo schaltet ab (Hersteller-Safety)
- [ ] NotAus am echten System: 0 W + Limiter-Off greift sofort
- [ ] Shelly 3EM Pro (Gen2-RPC **und** Tasmota) liefert plausible Werte
- [ ] MQTT-Kette inkl. HA-Discovery in Home Assistant
- [ ] Langzeittest (≥ 1 Tag Auto-Modus, Stabilität, Wiedereinhängen nach WLAN-Ausfall)

**Rückmeldungen willkommen** — besonders Testergebnisse am echten Soyosource
(GTN 1200 / GTN-48V / HHJ60), damit die Protokoll-Tabelle bestätigt oder korrigiert wird.

## 📜 Lizenz

MIT — siehe [LICENSE](LICENSE). Basierend auf der Arbeit von BavarianSuperGuy/KlausLi (MIT)
und den zitierten Open-Source-Projekten.

[KlausLi/Esp-Soyosource-Controller]: https://github.com/KlausLi/Esp-Soyosource-Controller
[macaron-software/soyosource-esp32-grid-limiter]: https://github.com/macaron-software/soyosource-esp32-grid-limiter
[syssi/esphome-soyosource-gtn-virtual-meter]: https://github.com/syssi/esphome-soyosource-gtn-virtual-meter
[ayushsharma82/ElegantOTA]: https://github.com/ayushsharma82/ElegantOTA
[ESPAsyncWebServer]: https://github.com/ESP32Async/ESPAsyncWebServer
[ESPAsyncTCP]: https://github.com/ESP32Async/ESPAsyncTCP
[ArduinoJson]: https://github.com/bblanchon/ArduinoJson
[PubSubClient]: https://github.com/knolleary/pubsubclient
[Originalprojekts von BavarianSuperGuy/KlausLi]: https://github.com/KlausLi/Esp-Soyosource-Controller