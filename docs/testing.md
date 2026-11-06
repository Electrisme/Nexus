# Test plan

Run these tests before publishing the repo or recording the video. Mark each one **PASS** or **FAIL** in the results table at the end, and keep the Serial output for any FAIL.

Use a fresh Cloudflare account setup and a freshly flashed ESP32 for test 1.

---

## 1. Quick Start from scratch

Follow the README exactly, step by step, on a clean setup. Do not skip any step.

**Expected:** the bot works, relays can be toggled, and the device shows online.

---

## 2. Automatic webhook

Open the Worker URL once in a browser. Then send `/start` to the bot.

**Expected:** the bot replies. If it does not reply right away, wait up to 5 minutes (the cron check) and try again.

---

## 3. Ownership by the first `/start`

- Account A sends `/start` first.
- Account B sends `/start` second.

**Expected:** A becomes the owner. B gets “Request sent” and the owner receives an approval request. B does not become the owner.

---

## 4. Approve and decline, and message cleanup

- Approve account B. Then decline a third account C.

**Expected:** the owner's request message is deleted after each decision. B receives “approved”. C receives “declined”. The owner's chat is not cluttered.

---

## 5. Partial access

Give account B access to only two relays.

**Expected:** B sees only those two relays. B cannot toggle any other relay, and the panel does not show them.

---

## 6. Relay commands from Telegram

- Toggle a relay ON, then OFF.
- Toggle a relay on a slow connection (or while the device's internet is limited).

**Expected:** a confirmation appears (“is now ON” or “is now OFF”). On a slow connection, the message “No confirmation yet…” may appear, but the panel must **not** switch to Offline for a single timeout.

---

## 7. Safe boot

- Turn several relays ON.
- Power-cycle the ESP32.

**Expected:** all relays are OFF after boot, and Serial shows `boot: relays set OFF`.

---

## 8. Wi‑Fi drop and recovery

- Turn off the router's Wi‑Fi (or move the device out of range), then bring it back.

**Expected:** Serial shows `wifi: link lost, reconnecting now`, then `WiFi OK`. The `drops=` counter increases. The device returns to online on its own.

---

## 9. Hotspot behavior

- Press the GPIO 4 button. Connect to **Nexus** and open `192.168.4.1`.
- Leave the page (or disconnect the phone from the hotspot).
- Press the button 5 times in a row, repeating the same cycle.

**Expected:** the hotspot turns off as soon as the client disconnects. The name stays `Nexus` on every cycle.

---

## 10. Hotspot password and Wi‑Fi list

- Change the hotspot password on the device page. Reconnect with the new password.
- Scan for networks, add a new Wi‑Fi from the list, and confirm the device connects.

**Expected:** the new password works. The new network is saved and stays in the list after a reboot.

---

## Results

| # | Test | Result | Notes / Serial lines (if FAIL) |
|---|---|---|---|
| 1 | Quick Start from scratch | | |
| 2 | Automatic webhook | | |
| 3 | Ownership by first `/start` | | |
| 4 | Approve/decline and message cleanup | | |
| 5 | Partial access | | |
| 6 | Relay commands from Telegram | | |
| 7 | Safe boot | | |
| 8 | Wi‑Fi drop and recovery | | |
| 9 | Hotspot behavior | | |
| 10 | Hotspot password and Wi‑Fi list | | |

---

## Known issues found in testing

- **LED blinking on a slow internet link (fixed in v1.0.0-beta):** the LED used to blink after 15 seconds without a successful server reply (`NET_STALE_MS` was 15000). It is now 45000 ms, which is longer than the 30-second HTTP timeout. Re-run test 6 on a slow connection to confirm the panel stays Online.
