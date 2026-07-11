# Siri Shortcuts

The web UI talks to the ESP32 over plain REST/JSON (`/api/*`), so iOS's
Shortcuts app can call it directly — no firmware or app changes needed. Every
shortcut below is just a "Get Contents of URL" action pointed at the DHCP
reservation IP (`http://192.168.0.100/...` — set once in the router, see the
DHCP Reservations screenshot from earlier in this project).

Because your iPhone and the ESP32 are on the same Wi-Fi/LAN, this works from
the Shortcuts app or "Hey Siri" — it does **not** work over cellular unless
you port-forward and expose the device to the internet, which is not
recommended for a device with no auth on its API.

## How a shortcut is built

1. Open **Shortcuts** → **+** → **Add Action** → search "Get Contents of URL".
2. Set the URL, e.g. `http://192.168.0.100/api/power`.
3. Tap **Method** → for control endpoints use `POST`; status endpoints (`GET
   /api/status`) can stay `GET` and you don't need a body.
4. For `POST`, tap **Request Body** → **JSON**, and add the fields shown per
   recipe below.
5. Name the shortcut (e.g. "AC Sleep Mode") — this becomes the Siri phrase:
   "Hey Siri, AC Sleep Mode."
6. Optional: add to Home Screen, or trigger via an Automation (time of day,
   arriving/leaving a location, etc.) instead of voice.

## Recipes

**Turn on, Cool 24°, fan low** ("Hey Siri, cool the room")
```
POST http://192.168.0.100/api/power   body: {"on": true}
POST http://192.168.0.100/api/mode    body: {"mode": "cool"}
POST http://192.168.0.100/api/temp    body: {"value": 24}
POST http://192.168.0.100/api/fan     body: {"speed": "low"}
```
Chain all four "Get Contents of URL" actions in one shortcut, in order.

**Turn off** ("Hey Siri, AC off")
```
POST http://192.168.0.100/api/power   body: {"on": false}
```

**Run a preset** ("Hey Siri, Turbo mode") — presets are already bundled
server-side (Night/Turbo/Eco, or whatever you've saved in the web UI):
```
POST http://192.168.0.100/api/presets/apply   body: {"name": "Turbo"}
```

**Start a sleep program** ("Hey Siri, sleep mode") — uses the program IDs
from the Automation tab (`sleep8h`, `sleep4h`, etc.):
```
POST http://192.168.0.100/api/program/start   body: {"id": "sleep8h"}
```

**Stop the running program**
```
POST http://192.168.0.100/api/program/stop
```

**Countdown timer** ("Hey Siri, AC off in an hour"):
```
POST http://192.168.0.100/api/timers   body: {"minutes": 60, "action": {"power": false}}
```

**Speak back the current state** — pair "Get Contents of URL" (`GET
/api/status`) with "Get Dictionary Value" actions to pull `power`/`temp`/
`mode`, then "Speak Text": "Hey Siri, is the AC on?" → "The AC is on, cooling
to 24 degrees."

**Geofencing auto-off** (from the earlier feature list) — this is the one
that needs no firmware work at all: create a Shortcuts **Automation** (not a
shortcut) with trigger "I Leave" a location (home), action "Get Contents of
URL" → `POST /api/power {"on": false}`. Turn off "Ask Before Running" so it
fires silently. This is a full geofenced auto-off with zero ESP32 changes.

## Notes

- The API has no authentication — anyone on your Wi-Fi can hit these
  endpoints. Fine for a home LAN; don't port-forward it to the internet.
- All of this already works today against the existing firmware — nothing in
  this repo needed to change for Shortcuts specifically.
