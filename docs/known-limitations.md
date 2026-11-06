# Known limitations (v1.0.0-beta)

This project is a working beta. These are the limits you should know before you rely on it.

## Security

- **TLS certificate is not verified.** The ESP32 uses `setInsecure()`, so it does not check that the server is really Cloudflare. Use it only on networks you trust. Planned fix: `setCACert` with the Cloudflare root CA.
- **Passwords are stored in plain text in the ESP32 flash.** Anyone with physical access to the device can read the Wi‑Fi and hotspot passwords.
- **The default hotspot password is `12345678`.** It is weak. Change it from the device page right after setup.
- **The first `/start` becomes the owner.** Send `/start` right after deploying, before anyone else does.
- **`DEVICE_TOKEN` is a fixed secret.** There is no automatic rotation.
- **No audit log.** The project does not record who changed what.

## Connectivity

- **The device needs Wi‑Fi on 2.4 GHz.** The ESP32 cannot use 5 GHz, and it has no SIM card or mobile data.
- **Remote control depends on the device's internet.** When the internet is down, you can only control the relays through the local hotspot.
- **Commands take a few seconds.** The device uses HTTPS long-polling, which works through mobile carriers. WebSocket was tested first, but some carriers dropped it.
- **Slow internet causes timeouts.** The panel shows “No confirmation yet” when this happens. The state updates when the device reports it.
- **Hotspot works only with the button.** It turns off when you leave the panel, or after 2 minutes if nobody connects.
- **Hotspot supports up to 4 clients** at the same time.
- **Some Wi‑Fi networks do not work:** enterprise networks (WPA-Enterprise, with a username) and networks with a browser login page (hotels, cafés).

## Features

- **One device per Worker deployment.** Several devices are not supported yet.
- **One owner.** The owner can give other users access to specific relays.
- **Maximum 12 relays**, on the allowed pins only.
- **No scheduler or automation.** Relays only change when someone presses a button.
- **No history or charts** of relay state.
- **No notifications** (for example, when the device goes offline).
- **No OTA updates.** New firmware needs a USB cable.
- **Panel language is English only.**

## Reliability

- **The command queue is in memory.** If the Durable Object restarts, queued commands are lost.
- **Relay state is reported, not measured.** If a relay sticks mechanically, the software does not know.
- **After power loss, all relays turn off.** This is safe, but some loads (pumps, fridges) may not like it.
- **Tested on one setup.** Many edge cases (long outages, crowded networks) are not tested.

## Electrical safety

- The project does not protect against overloads. Use a fuse or breaker.
- Mains wiring needs training and proper enclosures. This project does not replace a certified installation.

## Maintenance

- The ESP32 firmware is a single file.
- There are no automated tests.
- Pinned versions: ESP32 core 2.0.17 and ArduinoJson 7.
