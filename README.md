# NEXUS

Control your relays from **Telegram** and from a **local hotspot panel** with an **ESP32** and a **Cloudflare Worker**.

- 📱 Telegram bot with a live panel and per-user relay access
- 📶 ESP32 hotspot with a web panel for when the internet is down
- 🔌 Up to 12 relays, each with its own pin and logic (active-low or active-high)
- ☁️ Runs on Cloudflare's free tier, no port forwarding, no home router changes

> **Status: v1.0.0-beta.** Tested on real hardware. Read [Known limitations](docs/known-limitations.md) before you use it.

Created by **Electrisme**. Video guide: https://www.youtube.com/@Electrisme

---

## ⚠️ Safety first

Relays can switch mains voltage (110/220 V). A wrong wiring can cause **fire, electric shock, or death**.

- Only switch low-voltage loads, or let a qualified electrician handle mains wiring.
- Use a sealed enclosure, a fuse or breaker, and proper grounding.
- Check the relay's rated current and voltage.

The author is not responsible for damage or injury caused by this project. See [LICENSE](LICENSE).

---

## What you need

- An ESP32 development board (ESP32 DevKit or similar, **2.4 GHz Wi‑Fi only**)
- A relay module (1 to 12 channels, low-level or high-level trigger)
- A push button and a few jumper wires
- A free [Cloudflare](https://dash.cloudflare.com) account
- A Telegram account and a bot from [@BotFather](https://t.me/BotFather)
- Arduino IDE 2.x with the ESP32 core and ArduinoJson 7

---

## Quick Start (about 6 minutes)

### 1. Get the code
Fork or clone this repository.

### 2. Create the Telegram bot
1. In Telegram, open [@BotFather](https://t.me/BotFather) and send `/newbot`.
2. Copy the **token** it gives you. You will need it in step 4.

### 3. Deploy the Worker
Click **Deploy to Cloudflare** (or connect the repo under *Workers & Pages → Create → Import a repository*).

Then add these three **Secrets** (*Settings → Variables and Secrets*):

| Name | Value |
|---|---|
| `DEVICE_TOKEN` | a long random string (for example, 64 random characters) |
| `TELEGRAM_TOKEN` | the token from BotFather |
| `TG_WEBHOOK_SECRET` | another long random string |

Save and deploy.

### 4. Connect Telegram
Open your Worker's address once in a browser (it looks like `https://nexus-hub.<you>.workers.dev`). You should see **“NEXUS Hub is running.”** This registers the Telegram webhook automatically.

### 5. Claim ownership
Open your bot in Telegram and send **`/start` right away**. The first user to send `/start` becomes the **owner**. Only the owner can manage relays and approve other users.

Pick the default logic (LOW or HIGH). Eight relays are created.

### 6. Flash the ESP32
1. Open `esp32/nexus/nexus.ino` in Arduino IDE.
2. Fill in the **SETTINGS** block at the top:
   - `WIFI_SSID` and `WIFI_PASS`: your Wi‑Fi (2.4 GHz)
   - `DEVICE_TOKEN`: the same value you set in Cloudflare
   - `POLL_URL`: `https://nexus-hub.<you>.workers.dev/poll`
   - `AP_PASS_DEFAULT`: the default hotspot password (change it later)
3. Select **Tools → Board → ESP32 Dev Module**, choose the port, and upload.

### 7. Use it
- **From Telegram:** tap the relay buttons.
- **Without internet:** press the **button wired to GPIO 4** (see [wiring](docs/wiring.md)). Connect to the **Nexus** Wi‑Fi, then open `http://192.168.4.1`. The BOOT button on the board is not the hotspot button, so do not use it.

> **Before you push your own copy to GitHub:** put the placeholder values back into the SETTINGS block. Never commit real Wi‑Fi passwords or tokens.

---

## Wiring

See [docs/wiring.md](docs/wiring.md) for the full diagram, the button, the status LED and the default pins.

Default relay pins: `13, 14, 18, 19, 21, 22, 23, 25`. Allowed pins: `13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33`.

---

## How it works

```
Phone ──Telegram──► Cloudflare Worker (Durable Object) ◄──HTTPS long-poll── ESP32 ──► Relays
                                                                              ▲
Phone ──Wi‑Fi "Nexus" (hotspot, button)──► ESP32 web panel ───────────────────┘
```

- The ESP32 keeps a long-poll request open to the Worker. Commands reach the device as soon as it polls.
- Every command is acknowledged. The panel shows what the device actually reports.
- If the device loses Wi‑Fi, it reconnects immediately, then backs off up to 30 seconds.
- The hotspot turns off when you leave the panel, or after 2 minutes if nobody connects.

---

## Troubleshooting

See [docs/troubleshooting.md](docs/troubleshooting.md). For the manual test plan, see [docs/testing.md](docs/testing.md).

---

## Known limitations

See [docs/known-limitations.md](docs/known-limitations.md). The most important ones:

- The device needs **Wi‑Fi** (2.4 GHz). It has no SIM card.
- TLS does **not** verify the server certificate yet (`setInsecure()`).
- The default hotspot password is weak. Change it.
- Commands take a few seconds on slow internet.

---

## Contributing

Issues and pull requests are welcome. For big changes, open an issue first.

---

## License

[MIT](LICENSE) © 2026 Electrisme

---

## Author

**Electrisme**
🎬 YouTube: https://www.youtube.com/@Electrisme
