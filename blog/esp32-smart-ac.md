---
title: "I Made My Dumb AC Smart for ₹600 (and Learned to Respect Hardware)"
description: "How I turned a basic Samsung split AC into a phone-controlled, scheduling, energy-tracking smart AC with a single ESP32 and an IR LED — including the MOSFET-vs-transistor bug that nearly ended the project."
date: 2026-07-11
tags: [ESP32, Hardware, IoT, Firmware, Maker]
cover: media/in-action.jpg
---

![The finished ESP32 controller mounted and pointing at the Samsung AC](media/in-action.jpg)

My AC is not smart. It's a perfectly good Samsung split unit, but its entire intelligence lives in a plastic remote that I lose behind the couch at least twice a week. Every summer I have the same thought walking home in 40°C heat: *why can't I just tell it to start cooling before I get there?*

Flagship smart ACs can do that. Mine cost a fraction of theirs and it's not going anywhere for years. So this year I stopped wishing and decided to bolt the intelligence on myself — for the price of a couple of pizzas.

This is the story of how a ₹600 pile of parts turned into a genuinely flagship-grade smart AC, including the evening I spent convinced I'd bricked something, when really I'd just grabbed the wrong three-legged component.

## The itch, and a naive plan

The whole idea rests on one boring fact: my AC already has a wireless interface. It's called the infrared remote. If I could get a microcontroller to *speak IR*, I could impersonate the remote and command the AC from anything on my Wi-Fi.

So I bought the cheapest parts that could possibly work:

- an **ESP32 DevKit v1** (Wi-Fi + Bluetooth microcontroller)
- one **IR transmitter** (a 940 nm LED)
- a handful of **jumper wires, resistors, and transistors**

![The parts laid out — ESP32, IR transmitter, jumpers, resistors, transistors](media/parts.jpg)

The plan in my head was textbook. A GPIO pin can only push a few milliamps, and I wanted range, so I'd drive the IR LED through a transistor: pin toggles the transistor, transistor switches the higher current through the LED, LED blasts IR at the AC. Clean. Standard. What could go wrong?

## The failure I want to warn you about

I wired it all up on the breadboard. Flashed firmware that sent a Samsung "power on" frame. Pointed it at the AC.

Nothing.

![The transistor-driver wiring that refused to transmit](media/wiring-transistor.jpg)

No beep. No cold air. No blinking anything — IR is invisible, so I couldn't even *see* whether the LED was firing. I did what everyone does: checked the code, checked the pin number, checked the resistor, re-flashed, swapped jumper wires, phone-camera'd the LED (phone cameras can see IR as a faint purple flicker — mine stayed dark). Hours went by. I was genuinely starting to wonder if the ESP32's pin was fried.

The culprit turned out to be sitting in my hand the whole time. The little three-legged part I'd grabbed as my "transistor" was actually a **MOSFET**, not the **NPN transistor** my circuit assumed. They look nearly identical and both have three legs, but they switch on completely different principles — a bipolar transistor is current-driven through its *base*, a MOSFET is voltage-driven through its *gate*, and their pinouts don't line up. My circuit was feeding the right signal to the wrong kind of pin. The LED never stood a chance.

That's the emotional low point of every hardware project, and if you build anything physical you *will* meet it: the bug isn't in your code, it's in your assumptions about a component you never thought to question.

## The breakthrough: stop being clever

Once I understood the problem, I could have sourced the correct NPN transistor and rebuilt the driver stage. Instead I asked a simpler question: *do I even need the driver?*

The AC sits in the same room as the ESP32. I didn't need whole-house range — I needed to reliably hit a receiver window ten feet away. So I ripped out the transistor entirely and wired the IR LED **straight to the ESP32**, through a single current-limiting resistor:

```
ESP32 GPIO4 ──[ 220Ω ]──►|── IR LED ──── GND
                       (anode)  (cathode)
```

![The direct wiring that finally worked — IR LED straight off GPIO4](media/wiring-direct.jpg)

I rewrote the firmware around this direct connection, flashed it, and pointed it at the AC one more time.

*Beep.* The AC turned on.

I have rarely been that happy about a single beep. The whole system — Wi-Fi, IR protocol, the AC's decoder — worked end to end. Fewer parts, less to go wrong, and plenty of range for a same-room mount. Sometimes the senior-engineer move is to delete the sophisticated thing you were proud of.

## From blinking an LED to an actual product

Getting the AC to respond was the hard 20%. The fun 80% was turning "I can send one command" into something I'd actually want to use every day.

I paired with **[Claude Code](https://claude.com/claude-code)** for the build — I made the accounts, designed the UI, and decided which features mattered; it did a lot of the heavy lifting on the firmware, the automation logic, and wiring up the cloud integration. Working that way let me spend my attention on *product* decisions instead of boilerplate.

What came out the other side is a web app that runs **on the ESP32 itself**, served over my local Wi-Fi with no cloud in the middle:

![The web app running on my phone — control, schedules, and energy stats](media/webapp.jpg)

- **Full control** — power, temperature, mode, and fan, plus one-tap presets
- **Countdown timers** — "off in 45 minutes"
- **Weekly schedules** — "on at 24° cool, 10 PM, weeknights"
- **Multi-step programs** — sleep curves like *60 min @ 24° → 25° → 26° → off*
- **A live 24-hour cost timeline** that projects exactly when the AC will switch and what it'll cost
- **30 days of energy & cost stats** in kWh and ₹
- **Filter-clean reminders** and a **safety auto-off**
- **Voice control** via Alexa and Google Home (optional, through Sinric Pro)

It even installs to your phone's home screen as a PWA and works offline. That's a feature list I'd expect on an AC costing many times mine.

Two engineering ideas from this build are worth sharing, because they surprised me:

**IR is write-only.** Infrared is a one-way street — the ESP32 can *tell* the AC what to do, but it can never *read* the AC's real state. So the device only ever knows what it *last commanded*. That single fact shapes everything downstream: the energy stats, for example, honestly track *commanded* on-time, not real compressor duty. I found it freeing to design around "here's what we asked for" instead of pretending to know a truth the hardware simply can't observe.

**Defer hardware work to the main loop.** The 38 kHz IR signal is bit-banged in software with microsecond-precise timing. If a web request or a cloud callback tried to fire IR directly — from a different task, mid-transmission — the waveform would shred and the AC would miss the command. The rule that fixed it is simple: web and cloud handlers only ever *mutate state and raise a flag*; the actual IR send and flash writes happen exclusively in the Arduino `loop()`, on one cooperative task. A clean boundary between "decide what to do" and "touch the hardware" made the whole thing reliable.

One more detail I'm quietly proud of, because it's product thinking rather than code: a **"manual wins" policy**. If you reach for your phone or ask Alexa, the device pauses scheduled automations for a hold window — because if you took a deliberate action, you *meant* it, and a schedule shouldn't stomp you five minutes later. But timers and the safety auto-off *bypass* that hold, because you set those on purpose too, and safety is non-negotiable. Little rules like that are the difference between a demo and something you trust running your bedroom overnight.

## What it cost, and how you can build one

The best part: this is genuinely cheap and genuinely clonable.

| Part | Approx. cost (₹) |
|------|------------------|
| ESP32 DevKit v1 | 350–450 |
| IR transmitter (940 nm LED) | 30–80 |
| Jumper wires | 40–60 |
| Resistors (220 Ω) | 10–20 |
| Transistors (optional, for range) | 10–20 |
| Breadboard (optional) | 60–80 |
| **Total** | **~₹600–700** |

You almost certainly already own the other two ingredients: a phone and a Wi-Fi router. No hub, no subscription, no vendor app, no account required (the voice integration is the only optional cloud piece).

**Build your own:** the full firmware, web app, wiring diagrams, and parts list are open source. Clone [the repo](https://github.com/Marshmellow31/Smart-AC-Dashboard), flash it, point the LED at your AC, and you'll have a smart AC by the end of an afternoon. The README walks through both wiring options and every API endpoint.

## What hardware taught me

Software fails politely. A wrong value throws a stack trace that points at the line. Hardware fails *silently* — a MOSFET where you wanted a transistor gives you no error, no log, just an LED that stays dark and an evening of doubting yourself. That humility is the whole lesson: **verify your physical parts before you debug your code**, because the universe will happily let you chase a firmware bug that was never there.

The other lesson is to **ship in layers**. I didn't set out to build weekly schedules and sleep curves and energy analytics. I set out to make one LED blink an AC awake. Everything else was earned one working beep at a time.

Would I recommend making your own smart AC? Absolutely — but bring a phone camera to check the IR LED, double-check your three-legged components, and be ready to delete your cleverest idea the moment a simpler one works.

Turns out the smartest thing in my smart AC was knowing when to keep the circuit dumb.

*— Harshil Patel*
