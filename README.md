# ESP32 AC Controller

Wi-Fi remote control for a Samsung split AC, built on an ESP32 with an IR LED.
Serves a mobile-friendly web UI from on-board flash, integrates with
Alexa/Google Home through Sinric Pro, and runs clock-based automations
(weekly schedules, countdown timers, and multi-step programs such as sleep
curves) entirely on the device — no cloud required.

## Features

- **Web UI** (`http://ac-controller/` or `http://ac-controller.local/`) —
  power/temperature/mode/fan control with one-tap presets.
- **Automations** — weekly schedules, countdown timers, and step programs
  ("on 60 m at 24°, then 25°, then 26°, then off"), with a live 24-hour
  timeline showing exactly when the AC will switch and what it will cost.
- **Stats** — daily on-time history with kWh/cost estimates for today, the
  last 7 days, and the month; filter-cleaning reminder; event log.
- **Manual-wins policy** — manual or Alexa commands pause schedules for a
  configurable hold window; timers and the safety auto-off bypass it.
- **Resilience** — state, schedules, and stats persist on LittleFS; programs
  resume after a reboot; optional restore-after-power-cut.

## Hardware

| Part | Notes |
|------|-------|
| ESP32 dev board | `esp32dev` PlatformIO target |
| IR LED (+ transistor driver) | data on GPIO 4, 38 kHz carrier |
| Samsung AC | protocol via IRremoteESP8266's `IRSamsungAc` |

## Getting started

1. Copy `include/secrets.h.example` to `include/secrets.h` and fill in your
   Wi-Fi credentials (and Sinric Pro keys, or leave them empty to disable
   cloud control).
2. Build and flash the firmware, then the web UI filesystem image:

   ```sh
   pio run -t upload      # firmware
   pio run -t uploadfs    # web UI (data/ → LittleFS)
   ```

3. Open the serial monitor (`pio device monitor`) to see the device's IP.

The UI is then reachable at:

- `http://ac-controller/` — Windows (NetBIOS)
- `http://ac-controller.local/` — Android/iOS/macOS/Linux (mDNS)
- `http://<device-ip>/` — always works

## Architecture

Single-owner modules wired together in `src/main.cpp`; all cross-module state
changes flow through `AcController::apply()`.

```
main.cpp ─┬─ WiFiManager        reconnect loop
          ├─ TimeManager        SNTP (IST); automations wait for first sync
          ├─ AcController       single source of truth for AC state; the only
          │                     module that touches the IR transmitter (sends
          │                     only from loop(); async tasks just raise flags)
          ├─ AutomationEngine   timers + weekly schedules + step programs
          ├─ StatsManager       per-day on-time (30 days), filter counter,
          │                     safety auto-off
          ├─ SinricManager      Alexa / Google Home bridge (optional)
          ├─ WebServerManager   REST API + static UI (ESPAsyncWebServer)
          ├─ EventLog           in-RAM ring buffer, exposed at /api/log
          └─ ConfigStore        atomic JSON persistence under /cfg/ (LittleFS)
```

**Command sources & precedence** (`CmdSource`): `MANUAL` and `SINRIC` start a
hold that blocks `AUTOMATION` for `holdMinutes`; `TIMER` and `SAFETY` bypass
the hold; any external command cancels a running program.

**Threading model**: HTTP handlers run in the async TCP task. They never write
to flash or the IR pin — they mutate state under mutexes and raise dirty
flags; the Arduino `loop()` performs IR sends and persistence.

## REST API

| Endpoint | Method | Body / notes |
|----------|--------|--------------|
| `/api/status` | GET | state + time + hold + program + next schedule |
| `/api/power` | POST | `{"on": true}` |
| `/api/temp` | POST | `{"value": 16-30}` |
| `/api/mode` | POST | `{"mode": "cool\|dry\|fan\|auto\|heat"}` |
| `/api/fan` | POST | `{"speed": "auto\|low\|medium\|high"}` |
| `/api/presets` | GET/POST | named one-tap states; `/api/presets/apply` |
| `/api/timers` | GET/POST | `{"minutes": n, "action": {...}}`; `/api/timers/cancel` |
| `/api/schedules` | GET/POST | `{"slots": [{name, enabled, days[0-6], time "HH:MM", action}]}` |
| `/api/programs` | GET/POST | `{"programs": [{id, name, repeat, endTime, steps[]}]}` |
| `/api/program/start` | POST | `{"id": "...", "endTime": "HH:MM"?}` |
| `/api/program/stop` | POST | — |
| `/api/stats` | GET | today/week/month cost + 30-day history + filter |
| `/api/settings` | GET/POST | hold, watts, tariff, filter limit, safety |
| `/api/log` | GET | recent events, newest first |
| `/api/override/clear` | POST | end the manual hold early |

An `action` object is a partial state: any of `power`, `temp`, `mode`, `fan`.

## Cost estimates

The device tracks *commanded* on-time (IR is one-way, so actual compressor
duty cycle is unknown). Energy = on-time × **AC power draw (watts)**; cost =
energy × **tariff (₹/kWh)** — both set in Settings. The automation timeline
projects the same estimate 24 h forward from the configured schedules,
timers, and the running program.

## Troubleshooting

**Can't open the UI from a Windows PC (works on phone)**
Windows resolves `.local` (mDNS) names unreliably. In order of preference:

1. Use `http://ac-controller/` — served via NetBIOS, which Windows always
   understands.
2. Use the raw IP printed on the serial console (e.g. `http://192.168.0.121/`).
   Consider a DHCP reservation for the ESP32 in your router so it never moves.
3. If the IP doesn't load either: make sure the PC is on the same network
   (not a guest SSID — those often have client isolation), and that the
   network profile is *Private*, since Windows firewalls treat *Public*
   networks more aggressively.

**`vfs_api.cpp: ... does not exist, no permits for creation` at boot**
Harmless first-boot noise from an older firmware: it tried to `open()` config
files that don't exist yet. Current firmware checks existence first, so these
disappear after flashing. Configs are created on first save.

**Automations don't fire**
Check the status strip in the UI: the clock may not be synced yet, a manual
hold may be active (manual commands pause schedules for `holdMinutes`), or
schedules may be disabled in Settings.

## Development notes

- `tools/gzip_data.py` pre-compresses `data/*` at build time; the server
  prefers the `.gz` siblings (`data/*.gz` are build artifacts, gitignored).
- `board_build.partitions = min_spiffs.csv` — the app needs ~1.9 MB, leaving
  ~190 KB of LittleFS for the UI and configs.
- The web UI is dependency-free vanilla JS (`data/script.js`); the 24 h
  timeline mirrors the firmware's automation rules client-side.
