# Wiring

> ⚠️ **Mains voltage is dangerous.** Do the low-voltage wiring first. Never touch mains wiring while the system is powered. If you are not sure, ask a qualified electrician.

---

## Parts

| Part | Notes |
|---|---|
| ESP32 DevKit | 2.4 GHz Wi‑Fi only |
| Relay module | Low-level (LOW = on) or high-level (HIGH = on). Check which one you have. |
| Push button | 2 legs, normally open |
| LED (optional) | Most ESP32 boards already have one on GPIO 2 |
| Separate 5 V supply | For the relay module, if it has several channels |
| Jumper wires | |

---

## Relay module (up to 8 channels shown)

| Relay module pin | ESP32 pin | Default relay name |
|---|---|---|
| IN1 | GPIO 13 | Relay 1 |
| IN2 | GPIO 14 | Relay 2 |
| IN3 | GPIO 18 | Relay 3 |
| IN4 | GPIO 19 | Relay 4 |
| IN5 | GPIO 21 | Relay 5 |
| IN6 | GPIO 22 | Relay 6 |
| IN7 | GPIO 23 | Relay 7 |
| IN8 | GPIO 25 | Relay 8 |
| VCC | 5 V supply (not the ESP32 5 V pin, if the module draws a lot) | |
| GND | GND (shared with the ESP32) | |

**Important:** the relay module's **GND must be connected to the ESP32 GND**, otherwise the signals do not work.

### Relay logic

Set the logic per relay in the panel:

- **LOW (active-low):** the relay turns on when the input is LOW. Most common modules are like this.
- **HIGH (active-high):** the relay turns on when the input is HIGH.

If a relay turns on when you expect it to turn off, change its logic in *Settings → relay → LOW/HIGH*.

---

## Button (hotspot)

| Button leg | Connect to |
|---|---|
| Leg 1 | GPIO 4 |
| Leg 2 | GND |

No resistor is needed. The firmware enables the internal pull-up.

Pressing the button turns on the **Nexus** Wi‑Fi hotspot.

---

## Status LED

- **Slow blink:** no internet or no server connection
- **Solid:** connected and talking to the server

Most boards have an LED on GPIO 2. No extra wiring is needed. If your board has no LED, connect one through a 220 Ω resistor: anode to GPIO 2, cathode to GND.

---

## Power

- Power the ESP32 from USB or a 5 V supply.
- If the relay module draws more current than the ESP32 can supply, power the relay module from a **separate 5 V supply**. Connect the grounds together.

---

## Mains loads (only if you know what you are doing)

Each relay has three terminals: **COM**, **NO** (normally open) and **NC** (normally closed).

- Connect the live wire of the load to **COM**.
- Connect **NO** to the live wire going to the load's supply.
- The load turns on when the relay is on.

Before you touch mains wiring:
- Turn off the breaker.
- Use an enclosure that is rated for mains voltage.
- Put a fuse or breaker in front of the relay.
- Keep low-voltage and mains wires separated.
- Ground the enclosure and the appliance properly.

This project does not replace a certified installation. The author is not responsible for any damage or injury.
