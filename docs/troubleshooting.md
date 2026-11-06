# Troubleshooting

Open the Serial Monitor at **115200 baud** while you debug. Most problems show a message there.

---

## The bot does not reply

| Symptom | Fix |
|---|---|
| Nothing happens after `/start` | Open the Worker URL once in a browser. Wait up to 5 minutes, then try again. |
| “Send /start to set up this NEXUS device” | No owner yet. Send `/start` to claim ownership. |
| Bot works, but Worker URL shows an error | Check that `TELEGRAM_TOKEN` and `TG_WEBHOOK_SECRET` are set in Cloudflare. Redeploy. |
| Lost the owner account | Owner is stored in the Durable Object. Advanced users can clear it from the Cloudflare dashboard. |

---

## The device shows “Offline” in Telegram

Look at the Serial output:

| Serial message | Meaning | Fix |
|---|---|---|
| `WiFi failed, retry in …` | No Wi‑Fi | Check `WIFI_SSID` and `WIFI_PASS`. Use 2.4 GHz. |
| `'NAME' failed: network not found` | The network is not visible to the ESP32 | Check the name. It must be 2.4 GHz. Move closer. |
| `connect failed (wrong password?)` | Wrong password | Fix the password in the SETTINGS block, or change it from the panel. |
| `poll FAIL code -11` | Read timeout | Slow internet. The firmware waits 30 s. Wait, or use a better connection. |
| `poll FAIL code 403` | Wrong `DEVICE_TOKEN` | The token on the device must equal `DEVICE_TOKEN` in Cloudflare. |
| `poll FAIL code 404` | Wrong `POLL_URL` | The URL must end with `/poll`. |
| `poll: bad JSON from server` | Server returned an error page | Check the Worker logs in Cloudflare. |

---

## “Timeout” or “No confirmation yet”

The command was sent, but the device did not confirm in time. This happens on slow internet. The panel updates the state when the device reports it. Wait a few seconds and check again.

---

## The LED blinks even though Wi‑Fi works

The LED blinks when no successful server reply has arrived for 45 seconds (`NET_STALE_MS`). On a very slow internet link this can happen while Wi‑Fi is fine. Check the `poll FAIL` lines in Serial. If the LED stays solid when the link is normal, nothing is wrong.

---

## Relay turns on when it should be off

- The relay logic is wrong. Change it to the other value (LOW ↔ HIGH) for that relay.
- The relay module is wired to the wrong pin. Check the table in `docs/wiring.md`.
- The module's GND is not connected to the ESP32 GND.

---

## Relays turn on at boot

- Check that the relay module is powered from a supply that is stable at boot.
- Check the module's logic. The firmware sets each relay OFF before it starts, but a floating input can still glitch on some modules. Add external 10 kΩ pull-up or pull-down resistors if needed.

---

## Hotspot problems

| Symptom | Fix |
|---|---|
| Hotspot does not appear | Press the button once. The Serial shows `AP on: Nexus`. If it shows `AP start FAILED`, the router channel is not allowed; send the Serial lines when you open an issue. |
| Hotspot name changes to `esp-xxxxxx` | Restart the device. If it repeats, open an issue with the Serial output. |
| Hotspot turns off right after connecting | It turns off when no client is connected, or after 2 minutes if nobody connects. Press the button again. |
| Forgot the hotspot password | Connect to Wi‑Fi first and change it in the device page, or reflash with a new `AP_PASS_DEFAULT`. |

---

## Wi‑Fi drops often

- Use the router's 2.4 GHz band. Set the router to “Mixed” or “Auto” mode.
- A phone hotspot is less stable than a home router.
- Check `drops=` in the `hb:` line. A high number means the link is unstable.

---

## Still stuck?

Open an issue and include:
- The Serial lines around the problem (remove any Wi‑Fi password or token first)
- Your board model and the relay module model
- What you expected and what happened
