// NEXUS Hub - Cloudflare Worker + Durable Object
// Copyright (c) 2026 Electrisme - MIT License
// Created by Electrisme: https://www.youtube.com/@Electrisme

import { DurableObject } from "cloudflare:workers";

const POLL_HOLD_MS = 10000;     // how long the device long-poll waits for a command
const OFFLINE_MS = 60000;       // device is offline after this long without a poll
const CMD_TIMEOUT_MS = 20000;   // how long a command waits for the device's ACK
const REDELIVER_MS = 6000;      // resend an undelivered command after this time
const MAX_BODY = 4096;
const TG_SEEN_MAX = 200;
const MAX_RELAYS = 12;
const PAGE_SIZE = 8;
const HUB_NAME = "home-v2";     // Durable Object instance name (keep it if you redeploy)
const MAX_NAME = 24;
const DEFAULT_RELAY_COUNT = 8;

// Must match the allowed pins in nexus.ino
const ALL_PINS = [13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33];

const BTN = {
  panel: "🏠 Panel",
  settings: "⚙️ Settings",
  users: "👥 Users",
  help: "❓ Help",
};
const MENU_LABELS = new Set(Object.values(BTN));

// Callback operations that only the owner may use
const OWNER_OPS = new Set([
  "st", "pm", "pt", "ap", "apn", "apl", "er", "en", "el", "ep", "eps",
  "ed", "edk", "us", "uu", "up", "ur", "dclr", "al?", "al!", "wz",
]);
// Callback operations that only change the visible page
const NAV_OPS = new Set(["h", "hp", "st", "pm", "ap", "apn", "er", "ep", "ed", "us", "uu"]);

const WELCOME = [
  "👋 Welcome to NEXUS!",
  "━━━━━━━━━━━━",
  "Control your device's relays from here.",
  "",
  "🔐 You have no relay access yet. The owner grants access.",
  "🔘 Tap a relay button to turn it on or off.",
  "❓ Tap Help for details.",
].join("\n");

const COMMANDS_USER = [
  { command: "start", description: "Start and show the panel" },
  { command: "panel", description: "Show the panel" },
  { command: "help", description: "Help" },
];
const COMMANDS_OWNER = [
  ...COMMANDS_USER,
  { command: "settings", description: "Settings" },
  { command: "users", description: "Users and access" },
];

// ---------- small helpers ----------

const json = (o, status = 200) =>
  new Response(JSON.stringify(o), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" },
  });

const clip = (v, n = 64) => String(v ?? "").slice(0, n);
const num = (rid) => Number(String(rid).slice(1));
const stateText = (v) => (v === "ON" ? "ON" : v === "OFF" ? "OFF" : "unknown");
const dot = (v) => (v === "ON" ? "🟢" : v === "OFF" ? "⚪" : "❔");
const secs = (ms) => (ms / 1000).toFixed(1);
const btn = (text, cb) => ({ text, callback_data: cb });
const kb = (rows) => ({ inline_keyboard: rows.filter((r) => r && r.length) });
const chunk = (arr, n) => {
  const out = [];
  for (let i = 0; i < arr.length; i += n) out.push(arr.slice(i, i + n));
  return out;
};
const logicName = (l) => (l === "H" ? "🔺 HIGH (active-high)" : "🔻 LOW (active-low)");

function menuKeyboard(isOwner) {
  const rows = isOwner
    ? [[BTN.panel, BTN.settings], [BTN.users, BTN.help]]
    : [[BTN.panel, BTN.help]];
  return { keyboard: rows, resize_keyboard: true, is_persistent: true };
}

async function safeEqual(a, b) {
  const enc = new TextEncoder();
  const [x, y] = await Promise.all([
    crypto.subtle.digest("SHA-256", enc.encode(a)),
    crypto.subtle.digest("SHA-256", enc.encode(b)),
  ]);
  return crypto.subtle.timingSafeEqual(x, y);
}

async function sha256Hex(s) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function deviceOk(request, env) {
  const auth = request.headers.get("Authorization") || "";
  return !!env.DEVICE_TOKEN && (await safeEqual(auth, "Bearer " + env.DEVICE_TOKEN));
}

// ---------- Worker entry ----------

export default {
  async fetch(request, env, ctx) {
    try {
      return await route(request, env, ctx);
    } catch (e) {
      console.log("worker error", String((e && e.stack) || e));
      return json({ error: "internal" }, 500);
    }
  },

  // Every 5 minutes: re-check the Telegram webhook and bot commands
  async scheduled(event, env, ctx) {
    const hub = env.HUB.get(env.HUB.idFromName(HUB_NAME));
    ctx.waitUntil(hub.ensureWebhook().catch((e) => console.log("ensureWebhook", String(e))));
  },
};

async function route(request, env, ctx) {
  const url = new URL(request.url);
  const hub = env.HUB.get(env.HUB.idFromName(HUB_NAME));

  // Opening the Worker URL once stores the address and registers the Telegram webhook
  if (url.pathname === "/") {
    ctx.waitUntil(hub.setBase(url.origin).catch((e) => console.log("setBase", String(e))));
    return new Response("NEXUS Hub is running.\n", {
      headers: { "content-type": "text/plain; charset=utf-8" },
    });
  }

  if (url.pathname === "/poll") {
    if (request.method !== "POST") return json({ error: "POST only" }, 405);
    if (!(await deviceOk(request, env))) return json({ error: "forbidden" }, 403);
    const text = await request.text();
    if (text.length > MAX_BODY) return json({ error: "too large" }, 413);
    let body;
    try { body = JSON.parse(text); } catch { return json({ error: "bad json" }, 400); }
    return json(await hub.poll(body));
  }

  if (url.pathname === "/tg/webhook") {
    if (request.method !== "POST") return json({ error: "POST only" }, 405);
    const sec = request.headers.get("X-Telegram-Bot-Api-Secret-Token") || "";
    if (!env.TG_WEBHOOK_SECRET || !(await safeEqual(sec, env.TG_WEBHOOK_SECRET))) {
      return json({ error: "forbidden" }, 403);
    }
    let update;
    try { update = await request.json(); } catch { return new Response("ok"); }
    ctx.waitUntil(hub.tg(update).catch((e) => console.log("tg error", String(e))));
    return new Response("ok");
  }

  return new Response("not found", { status: 404 });
}

// ---------- Durable Object: all state and logic ----------

export class Hub extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.lastSeen = 0;
    this.down = false;
    this.info = null;
    this.devCfg = 0;
    this.queue = [];
    this.pollWaiter = null;
    this.ackWaiters = new Map();
    this.state = {};
    this.cfg = null;
    this.owner = "";
    this.base = "";
    this.loaded = false;
  }

  // ================= storage helpers =================

  async kv(key, def) {
    const v = await this.ctx.storage.get(key);
    return v ?? def;
  }
  kvPut(key, val) { return this.ctx.storage.put(key, val); }

  async load() {
    if (this.loaded) return;
    this.state = (await this.kv("state", {})) || {};
    const c = (await this.kv("cfg", null)) || {};
    this.cfg = {
      setup: !!c.setup,
      ver: Number(c.ver) || 0,
      cfgFail: Number(c.cfgFail) || 0,
      allowed: Array.isArray(c.allowed) ? c.allowed : ALL_PINS.slice(),
      relays: c.relays || {},
    };
    this.cfg.allowed = this.cfg.allowed.filter((p) => ALL_PINS.includes(p));
    this.owner = String(await this.kv("owner", ""));
    this.base = String(await this.kv("base", ""));
    this.loaded = true;
  }

  isOwner(uid) { return !!this.owner && String(uid) === this.owner; }

  // ================= device connection =================

  online() { return !this.down && Date.now() - this.lastSeen < OFFLINE_MS; }

  markSeen() {
    const was = !this.online();
    this.lastSeen = Date.now();
    this.down = false;
    return was;
  }

  // Note: a single command timeout does NOT mark the device offline.
  // Offline is decided only by the alarm (no poll for OFFLINE_MS).
  async markOffline() {
    if (this.down) return;
    this.down = true;
    await this.refreshPanels();
  }

  usedPins() { return new Set(Object.values(this.cfg.relays).map((r) => r.pin)); }

  invalidRelays() {
    return this.relayIds().filter((rid) => !ALL_PINS.includes(this.cfg.relays[rid].pin));
  }

  freeRid() {
    for (let n = 1; n <= MAX_RELAYS; n++) if (!this.cfg.relays[`r${n}`]) return `r${n}`;
    return null;
  }

  relayIds() { return Object.keys(this.cfg.relays).sort((a, b) => num(a) - num(b)); }

  visibleRids(users, uid) {
    const all = this.relayIds();
    if (this.isOwner(uid)) return all;
    const perms = (users[uid] && users[uid].perms) || [];
    return all.filter((r) => perms.includes(r));
  }

  deviceSynced() { return this.devCfg === this.cfg.ver; }
  configRejected() { return this.cfg.ver > 0 && this.cfg.cfgFail === this.cfg.ver; }

  // ================= webhook and bot commands (automatic) =================

  // Called when the Worker URL is opened: remembers the address and registers the webhook
  async setBase(origin) {
    await this.load();
    if (origin && origin !== this.base) {
      this.base = origin;
      await this.kvPut("base", origin);
    }
    return this.ensureWebhook();
  }

  async ensureWebhook() {
    await this.load();
    const token = String(this.env.TELEGRAM_TOKEN || "");
    const secret = String(this.env.TG_WEBHOOK_SECRET || "");
    if (!token || !secret || !this.base) return { ok: false, reason: "missing secret or first visit" };

    const want = `${this.base}/tg/webhook`;
    const secHash = await sha256Hex(secret);
    const info = await this.tgApi("getWebhookInfo", {});
    const current = (info && info.result && info.result.url) || "";
    const storedSec = await this.kv("whSec", "");

    if (current !== want || storedSec !== secHash) {
      const r = await this.tgApi("setWebhook", {
        url: want,
        secret_token: secret,
        allowed_updates: ["message", "callback_query"],
        drop_pending_updates: false,
      });
      if (!r.ok) {
        console.log("setWebhook failed", clip(r.description, 120));
        return { ok: false, reason: clip(r.description, 120) };
      }
      await this.kvPut("whSec", secHash);
    }
    await this.syncCommands(false);
    return { ok: true };
  }

  async syncCommands(force) {
    const key = await sha256Hex(`${this.owner}|commands-v1`);
    if (!force && (await this.kv("cmdKey", "")) === key) return;
    await this.tgApi("setMyCommands", { commands: COMMANDS_USER });
    if (this.owner && /^\d+$/.test(this.owner)) {
      await this.tgApi("setMyCommands", {
        scope: { type: "chat", chat_id: Number(this.owner) },
        commands: COMMANDS_OWNER,
      });
    }
    await this.kvPut("cmdKey", key);
  }

  // The first user who sends /start becomes the owner
  async claimOwner(uid) {
    this.owner = String(uid);
    await this.kvPut("owner", this.owner);
    await this.syncCommands(true);
  }

  // ================= config sync with device =================

  async saveCfg(bump) {
    if (bump) this.cfg.ver += 1;
    await this.kvPut("cfg", this.cfg);
    this.ensureConfigSync();
    this.wake();
  }

  ensureConfigSync() {
    if (!this.cfg.setup || this.deviceSynced() || this.configRejected()) return;
    if (this.invalidRelays().length) return;
    if (this.queue.some((c) => c.msg.type === "CONFIG" && c.msg.ver === this.cfg.ver)) return;
    this.queue = this.queue.filter((c) => c.msg.type !== "CONFIG");
    const id = crypto.randomUUID();
    const relays = this.relayIds().map((rid) => ({
      id: rid,
      name: this.cfg.relays[rid].name,
      pin: this.cfg.relays[rid].pin,
      logic: this.cfg.relays[rid].logic,
    }));
    this.queue.push({
      id,
      msg: { v: 1, type: "CONFIG", id, ver: this.cfg.ver, relays },
      delivered: false,
      deliveredAt: 0,
    });
  }

  async applySt(st) {
    let changed = false;
    for (const rid of Object.keys(this.cfg.relays)) {
      const v = st[rid];
      if ((v === "ON" || v === "OFF") && this.state[rid] !== v) {
        this.state[rid] = v;
        changed = true;
      }
    }
    if (changed) await this.kvPut("state", this.state);
    return changed;
  }

  // ================= command queue and ACKs =================

  nextCommand() {
    const now = Date.now();
    for (const c of this.queue) {
      if (!c.delivered || now - c.deliveredAt > REDELIVER_MS) {
        c.delivered = true;
        c.deliveredAt = now;
        return c.msg;
      }
    }
    return null;
  }

  wake() {
    if (!this.pollWaiter) return;
    const w = this.pollWaiter;
    this.pollWaiter = null;
    w(this.nextCommand() || { v: 1, type: "NOOP" });
  }

  async applyAck(a) {
    if (!a || typeof a.id !== "string") return;
    const item = this.queue.find((c) => c.id === a.id);
    this.queue = this.queue.filter((c) => c.id !== a.id);

    // If the device rejects a config, do not resend it until the next change
    if (item && item.msg.type === "CONFIG" && !a.ok) {
      this.cfg.cfgFail = item.msg.ver;
      await this.kvPut("cfg", this.cfg);
      this.ctx.waitUntil(this.refreshPanels());
    }

    const cb = this.ackWaiters.get(a.id);
    if (cb) {
      this.ackWaiters.delete(a.id);
      cb(a);
    }
  }

  async dispatch(msg) {
    const ackP = new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.ackWaiters.delete(msg.id);
        resolve(null);
      }, CMD_TIMEOUT_MS);
      this.ackWaiters.set(msg.id, (a) => {
        clearTimeout(timer);
        resolve(a);
      });
    });
    this.queue.push({ id: msg.id, msg, delivered: false, deliveredAt: 0 });
    this.wake();
    const ack = await ackP;
    if (!ack) this.queue = this.queue.filter((c) => c.id !== msg.id);
    return ack;
  }

  // ================= HTTP from device =================

  async poll(body) {
    await this.load();
    const wasOffline = this.markSeen();
    await this.ctx.storage.setAlarm(this.lastSeen + OFFLINE_MS + 500);

    if (body && body.info && typeof body.info === "object") {
      const i = body.info;
      this.info = {
        ssid: clip(i.ssid, 32),
        rssi: Number(i.rssi) || 0,
        uptime: Number(i.uptime) || 0,
        heap: Number(i.heap) || 0,
        fw: clip(i.fw, 16),
      };
      this.devCfg = Number(i.cfg) || 0;
    }

    let changed = wasOffline;
    if (body && body.st && typeof body.st === "object") {
      changed = (await this.applySt(body.st)) || changed;
    }

    const acks = Array.isArray(body && body.acks) ? body.acks.slice(0, 16) : [];
    for (const a of acks) await this.applyAck(a);

    this.ensureConfigSync();

    if (changed) this.ctx.waitUntil(this.refreshPanels());

    if (this.pollWaiter) {
      const w = this.pollWaiter;
      this.pollWaiter = null;
      w({ v: 1, type: "NOOP" });
    }

    const cmd = this.nextCommand();
    if (cmd) return cmd;

    return new Promise((resolve) => {
      this.pollWaiter = resolve;
      setTimeout(() => {
        if (this.pollWaiter === resolve) {
          this.pollWaiter = null;
          resolve({ v: 1, type: "NOOP" });
        }
      }, POLL_HOLD_MS);
    });
  }

  async alarm() {
    await this.load();
    if (!this.online()) await this.markOffline();
  }

  // ================= relay commands =================

  async toggleRelay(rid, state) {
    if (!this.cfg.relays[rid]) return { ok: false, error: "bad_relay" };
    if (this.invalidRelays().length) return { ok: false, error: "invalid" };
    if (this.configRejected()) return { ok: false, error: "rejected" };
    if (!this.deviceSynced()) return { ok: false, error: "sync" };
    if (!this.online()) return { ok: false, error: "offline" };
    if (this.state[rid] === state) return { ok: true, source: "cache", ms: 0 };

    const t0 = Date.now();
    const ack = await this.dispatch({
      v: 1, type: "COMMAND", id: crypto.randomUUID(), target: rid, state,
    });
    if (!ack) return { ok: false, error: "timeout" };
    if (ack.ok !== true) return { ok: false, error: "mismatch" };

    this.state[rid] = state;
    await this.kvPut("state", this.state);
    return { ok: true, source: "device", ms: Date.now() - t0 };
  }

  noteFor(rid, next, r) {
    const name = (this.cfg.relays[rid] && this.cfg.relays[rid].name) || rid;
    if (r.ok && r.source === "cache") return `ℹ️ ${name} is already ${stateText(next)}`;
    if (r.ok) return `✅ ${name} is now ${stateText(next)} (${secs(r.ms)} s)`;
    const map = {
      invalid: "⚠️ Some relays use an invalid pin. Fix it in Settings first.",
      rejected: "❌ The device rejected the settings. Check the pins.",
      sync: "⏳ The device has not received the latest settings yet.",
      offline: "🔴 The device is offline. Command not sent.",
      timeout: "⏳ No confirmation yet. The device's internet may be slow. Check again in a moment.",
      bad_relay: "❌ This relay no longer exists.",
      mismatch: "❌ The device reported a different state.",
    };
    return map[r.error] || `❌ Failed: ${clip(r.error, 60)}`;
  }

  async allOff() {
    let fails = 0;
    for (const rid of this.relayIds()) {
      if (this.state[rid] === "ON") {
        const r = await this.toggleRelay(rid, "OFF");
        if (!r.ok) fails++;
      }
    }
    return fails;
  }

  // ================= Telegram API =================

  async tgApi(method, params) {
    const token = String(this.env.TELEGRAM_TOKEN || "");
    if (!token) return { ok: false, description: "no token" };
    try {
      const r = await fetch(`https://api.telegram.org/bot${token}/${method}`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(params),
      });
      return await r.json();
    } catch (e) {
      return { ok: false, description: clip(String(e), 120) };
    }
  }

  tgSend(chat, text) { return this.tgApi("sendMessage", { chat_id: chat, text }); }

  deleteMsg(chat, id) {
    return id ? this.tgApi("deleteMessage", { chat_id: chat, message_id: id }) : null;
  }

  // ================= screens =================

  async render(screen, uid, opts = {}) {
    const isOwner = this.isOwner(uid);
    const [op, a] = String(screen || "h:0").split(":");

    if (!this.cfg.setup && isOwner) return this.renderWizard();
    if (op === "hp") return this.renderHelp(isOwner);
    if (OWNER_OPS.has(op) && !isOwner) return this.renderHome(uid, 0, opts);

    switch (op) {
      case "st": return this.renderSettings(opts);
      case "pm": return this.renderPins(opts);
      case "ap": return this.renderAddPin(opts);
      case "apn": return this.renderAddLogic(a);
      case "er": return this.renderEditRelay(a, opts);
      case "ep": return this.renderPickPin(a);
      case "ed": return this.renderConfirmDelete(a);
      case "us": return this.renderUsers(opts);
      case "uu": return this.renderUser(a, opts);
      default: return this.renderHome(uid, Number(a) || 0, opts);
    }
  }

  renderWizard() {
    return {
      screen: "h:0",
      text: [
        "🛠 NEXUS setup",
        "━━━━━━━━━━━━",
        "Choose the default logic for your relays:",
        "",
        "🔻 LOW (active-low): the relay turns on when the pin is LOW. Most common modules are like this.",
        "🔺 HIGH (active-high): the relay turns on when the pin is HIGH.",
        "",
        `This applies to the ${DEFAULT_RELAY_COUNT} default relays. You can change it later in Settings.`,
      ].join("\n"),
      kb: kb([[btn("🔻 LOW", "wz:L"), btn("🔺 HIGH", "wz:H")]]),
    };
  }

  async renderHome(uid, page, opts = {}) {
    const isOwner = this.isOwner(uid);
    if (!this.cfg.setup) {
      return {
        screen: "h:0",
        text: "⏳ The system is not ready yet. The owner must finish setup.",
        kb: kb([[btn(BTN.help, "hp")]]),
      };
    }

    const users = await this.kv("users", {});
    const rids = this.visibleRids(users, uid);
    const pages = Math.max(1, Math.ceil(rids.length / PAGE_SIZE));
    const p = Math.min(Math.max(0, page), pages - 1);
    const slice = rids.slice(p * PAGE_SIZE, p * PAGE_SIZE + PAGE_SIZE);

    const online = this.online();
    const sig = online && this.info ? `📶 Signal: ${this.info.rssi} dBm` : "📶 Signal: —";

    let warn = null;
    if (isOwner && this.invalidRelays().length) {
      warn = "⚠️ Some relays use an invalid pin. Fix them in Settings.";
    } else if (isOwner && this.configRejected()) {
      warn = "❌ The device rejected the settings.";
    } else if (!this.deviceSynced()) {
      warn = "⏳ Syncing settings with the device…";
    }

    const text = [
      `🏠 NEXUS  ·  ${online ? "🟢 Online" : "🔴 Offline"}`,
      "━━━━━━━━━━━━",
      sig,
      warn,
      rids.length ? null : "🔒 You have no relay access yet.",
      pages > 1 ? `Page ${p + 1} of ${pages}` : null,
      opts.note || null,
      opts.confirmAll
        ? "⚠️ Turn off all relays?"
        : rids.length ? "Tap a button to change a relay." : null,
    ].filter((x) => x !== null).join("\n");

    const relayRows = chunk(
      slice.map((rid) => btn(`${dot(this.state[rid])} ${this.cfg.relays[rid].name}`, `t:${rid}:${p}`)),
      2
    );
    const pageRow = pages > 1
      ? [
          p > 0 ? btn("◀️", `h:${p - 1}`) : null,
          btn(`${p + 1}/${pages}`, "x"),
          p < pages - 1 ? btn("▶️", `h:${p + 1}`) : null,
        ].filter(Boolean)
      : [];

    let action = [];
    if (opts.confirmAll) {
      action = [[btn("✅ Yes, turn all off", "al!"), btn("↩️ Cancel", `h:${p}`)]];
    } else if (isOwner && rids.some((r) => this.state[r] === "ON")) {
      action = [[btn("⛔ Turn all off", `al?:${p}`)]];
    }

    const nav = isOwner
      ? [[btn(BTN.settings, "st"), btn(BTN.users, "us")], [btn(BTN.help, "hp")]]
      : [[btn(BTN.help, "hp")]];

    return { screen: `h:${p}`, text, kb: kb([...relayRows, pageRow, ...action, ...nav]) };
  }

  renderHelp(isOwner) {
    const lines = [
      "📖 Help",
      "━━━━━━━━━━━━",
      "🔘 Tap a relay button to turn it on or off.",
      "🟢 ON   ⚪ OFF   ❔ unknown",
      "🔐 You only see relays the owner gave you.",
      "🔄 The panel updates automatically.",
      "📡 If the device changes state from its hotspot, the panel updates after it reconnects.",
    ];
    if (isOwner) {
      lines.push(
        "",
        "👑 Owner guide",
        "⚙️ Settings: add, delete, rename, change pins and logic.",
        "🔁 Logic: LOW means the relay turns on when its pin is LOW.",
        "📌 Allowed pins: only enabled pins can be used for relays.",
        "👥 Users: approve requests and choose each user's relays.",
        "⛔ Turn all off: switches off every relay that is on.",
        "",
        "📶 Hotspot: press the device button. The Nexus network appears. It turns off when you leave, or after 2 minutes if nobody connects.",
        "🔑 Hotspot password: change it on the device's web page (Device tab)."
      );
    }
    return { screen: "hp", text: lines.join("\n"), kb: kb([[btn("🔙 Back", "h:0")]]) };
  }

  renderSettings(opts = {}) {
    const ids = this.relayIds();
    const relayRows = chunk(
      ids.map((rid) => {
        const r = this.cfg.relays[rid];
        const bad = ALL_PINS.includes(r.pin) ? "" : "⚠️ ";
        return btn(`${bad}✏️ ${r.name} · pin ${r.pin}`, `er:${rid}`);
      }),
      2
    );
    const add = ids.length < MAX_RELAYS ? [btn("➕ Add relay", "ap")] : null;
    const text = [
      "⚙️ Settings",
      "━━━━━━━━━━━━",
      `📶 Wi‑Fi: ${(this.info && this.info.ssid) || "—"}`,
      `🌐 Device: ${this.online() ? "🟢 Online" : "🔴 Offline"}`,
      `🔌 Relays: ${ids.length} of ${MAX_RELAYS}`,
      `📌 Allowed pins: ${this.cfg.allowed.length} of ${ALL_PINS.length}`,
      "",
      "Tap a relay to edit it.",
      opts.note || null,
    ].filter((x) => x !== null).join("\n");
    return {
      screen: "st",
      text,
      kb: kb([...relayRows, add, [btn("📌 Allowed pins", "pm")], [btn("🔙 Back", "h:0")]]),
    };
  }

  renderPins(opts = {}) {
    const used = this.usedPins();
    const rows = chunk(
      ALL_PINS.map((pin) => {
        const on = this.cfg.allowed.includes(pin);
        return btn(`${on ? "✅" : "⬜"}${used.has(pin) ? "🔒" : ""} ${pin}`, `pt:${pin}`);
      }),
      4
    );
    const text = [
      "📌 Allowed pins",
      "━━━━━━━━━━━━",
      "✅ allowed   ⬜ not allowed   🔒 in use",
      "",
      "Only allowed pins can be used for relays.",
      "A pin in use cannot be disallowed.",
      opts.note || null,
    ].filter((x) => x !== null).join("\n");
    return { screen: "pm", text, kb: kb([...rows, [btn("🔙 Back", "st")]]) };
  }

  renderAddPin(opts = {}) {
    const used = this.usedPins();
    const free = this.cfg.allowed.filter((p) => !used.has(p));
    if (!free.length) {
      return {
        screen: "ap",
        text: "⚠️ No free allowed pin.\nAdd a pin in «Allowed pins» or delete a relay.",
        kb: kb([[btn("🔙 Back", "st")]]),
      };
    }
    return {
      screen: "ap",
      text: `➕ Add relay\n━━━━━━━━━━━━\nChoose a pin:${opts.note ? "\n" + opts.note : ""}`,
      kb: kb([
        ...chunk(free.map((p) => btn(`${p}`, `apn:${p}`)), 4),
        [btn("🔙 Back", "st")],
      ]),
    };
  }

  renderAddLogic(pin) {
    return {
      screen: `apn:${pin}`,
      text: `🔁 Logic for the relay on pin ${pin}?`,
      kb: kb([
        [btn("🔻 LOW", `apl:${pin}:L`), btn("🔺 HIGH", `apl:${pin}:H`)],
        [btn("🔙 Back", "ap")],
      ]),
    };
  }

  renderEditRelay(rid, opts = {}) {
    const r = this.cfg.relays[rid];
    if (!r) return this.renderSettings({ note: "❌ This relay does not exist" });
    const badPin = !ALL_PINS.includes(r.pin);
    const isL = r.logic !== "H";
    const text = [
      `✏️ ${r.name}`,
      "━━━━━━━━━━━━",
      `📌 Pin: ${r.pin}${badPin ? "  ⚠️ not allowed, choose a new pin" : ""}`,
      `🔁 Logic: ${logicName(r.logic)}`,
      `💡 State: ${stateText(this.state[rid])}`,
      "",
      opts.note || null,
    ].filter((x) => x !== null).join("\n");
    return {
      screen: `er:${rid}`,
      text,
      kb: kb([
        [btn("✏️ Rename", `en:${rid}`)],
        [
          btn(`${isL ? "✅ " : ""}🔻 LOW`, `el:${rid}:L`),
          btn(`${isL ? "" : "✅ "}🔺 HIGH`, `el:${rid}:H`),
        ],
        [btn("📌 Change pin", `ep:${rid}`), btn("🗑 Delete", `ed:${rid}`)],
        [btn("🔙 Back", "st")],
      ]),
    };
  }

  renderPickPin(rid) {
    const r = this.cfg.relays[rid];
    if (!r) return this.renderSettings();
    const used = this.usedPins();
    const free = this.cfg.allowed.filter((p) => !used.has(p));
    return {
      screen: `ep:${rid}`,
      text: `📌 New pin for ${r.name}? (current: ${r.pin})\nIf the relay is on, it turns off first.`,
      kb: kb([
        ...chunk(free.map((p) => btn(`${p}`, `eps:${rid}:${p}`)), 4),
        [btn("🔙 Back", `er:${rid}`)],
      ]),
    };
  }

  renderConfirmDelete(rid) {
    const r = this.cfg.relays[rid];
    if (!r) return this.renderSettings();
    return {
      screen: `ed:${rid}`,
      text: `🗑 Delete relay «${r.name}»?\nIf it is on, it turns off first, and users lose access to it.`,
      kb: kb([[btn("✅ Yes, delete", `edk:${rid}`), btn("↩️ Cancel", `er:${rid}`)]]),
    };
  }

  async renderUsers(opts = {}) {
    const users = await this.kv("users", {});
    const pending = await this.kv("pending", {});
    const denied = await this.kv("denied", {});
    const pRows = Object.entries(pending).map(([id, p]) => [
      btn(`✅ ${p.name || id}`, `ok:${id}`),
      btn("❌", `no:${id}`),
    ]);
    const uRows = Object.entries(users).map(([id, u]) => btn(`👤 ${u.name || id}`, `uu:${id}`));
    const deniedCount = Object.keys(denied).length;
    const clearRow = deniedCount ? [btn("♻️ Clear declined list", "dclr")] : null;
    const text = [
      "👥 Users",
      "━━━━━━━━━━━━",
      `✅ Approved: ${Object.keys(users).length}`,
      `⏳ Pending: ${Object.keys(pending).length}`,
      `⛔ Declined: ${deniedCount}`,
      "",
      uRows.length ? "Tap a user to choose their relays." : "No approved users yet.",
      opts.note || null,
    ].filter((x) => x !== null).join("\n");
    return {
      screen: "us",
      text,
      kb: kb([...pRows, ...chunk(uRows, 2), clearRow, [btn("🔙 Back", "h:0")]]),
    };
  }

  async renderUser(uid, opts = {}) {
    const users = await this.kv("users", {});
    const u = users[uid];
    if (!u) return this.renderUsers({ note: "❌ User not found" });
    const perms = u.perms || [];
    const rows = chunk(
      this.relayIds().map((rid) =>
        btn(`${perms.includes(rid) ? "✅" : "⬜"} ${this.cfg.relays[rid].name}`, `up:${uid}:${rid}`)
      ),
      2
    );
    const text = [
      `👤 ${u.name || "-"}${u.username ? " @" + u.username : ""}`,
      "━━━━━━━━━━━━",
      `🆔 ${uid}`,
      `🔌 Access to ${perms.length} relay(s)`,
      "",
      opts.note || null,
    ].filter((x) => x !== null).join("\n");
    return {
      screen: `uu:${uid}`,
      text,
      kb: kb([...rows, [btn("⛔ Remove access", `ur:${uid}`)], [btn("🔙 Back", "us")]]),
    };
  }

  // ================= panel updates =================

  async show(chat, uid, screen, opts = {}, msgId = null, fresh = false, noFresh = false) {
    const view = await this.render(screen, uid, opts);
    const panels = await this.kv("panels", {});
    const id = msgId ?? (panels[chat] && panels[chat].id);
    if (!fresh && id) {
      const r = await this.tgApi("editMessageText", {
        chat_id: chat,
        message_id: id,
        text: view.text,
        reply_markup: view.kb,
      });
      if (r.ok || String(r.description || "").includes("not modified")) {
        panels[chat] = { id, screen: view.screen };
        await this.kvPut("panels", panels);
        return r;
      }
      // automatic refreshes never send a new message (avoids spam)
      if (noFresh) return r;
    }
    return this.sendFresh(chat, view);
  }

  async sendFresh(chat, view) {
    const panels = await this.kv("panels", {});
    const old = panels[chat] && panels[chat].id;
    const sent = await this.tgApi("sendMessage", {
      chat_id: chat,
      text: view.text,
      reply_markup: view.kb,
    });
    if (sent.ok && sent.result) {
      panels[chat] = { id: sent.result.message_id, screen: view.screen };
      await this.kvPut("panels", panels);
      if (old && old !== sent.result.message_id) await this.deleteMsg(chat, old);
    }
    return sent;
  }

  async command(chat, userMsgId, uid, screen = "h:0") {
    await this.deleteMsg(chat, userMsgId);
    return this.show(chat, uid, screen, {}, null, true);
  }

  async start(chat, uid, userMsgId) {
    await this.deleteMsg(chat, userMsgId);
    const welcomed = await this.kv("welcomed", {});
    if (!welcomed[uid]) {
      welcomed[uid] = true;
      await this.kvPut("welcomed", welcomed);
      await this.tgApi("sendMessage", {
        chat_id: chat,
        text: WELCOME,
        reply_markup: menuKeyboard(this.isOwner(uid)),
      });
    }
    return this.show(chat, uid, "h:0", {}, null, true);
  }

  async refreshPanels() {
    const panels = await this.kv("panels", {});
    const users = await this.kv("users", {});
    for (const chat of Object.keys(panels)) {
      if (!this.isOwner(chat) && !users[chat]) continue;
      await this.show(chat, chat, (panels[chat] && panels[chat].screen) || "h:0", {}, null, false, true);
    }
  }

  // ================= access requests =================

  async requestAccess(chat, uid, from) {
    const denied = await this.kv("denied", {});
    if (denied[uid]) return this.tgSend(chat, "⛔ Your request was declined earlier.");
    const pending = await this.kv("pending", {});
    if (pending[uid]) return this.tgSend(chat, "⏳ Your request is waiting for the owner.");

    const name = [from && from.first_name, from && from.last_name].filter(Boolean).join(" ") || "-";
    const username = (from && from.username) || "";
    pending[uid] = { name, username, at: Date.now() };
    await this.kvPut("pending", pending);

    await this.tgSend(chat, "⏳ Request sent. Please wait for the owner to approve it.");
    return this.tgApi("sendMessage", {
      chat_id: Number(this.owner),
      text: `👤 New access request\n${name}${username ? " @" + username : ""}\nID: ${uid}`,
      reply_markup: kb([[btn("Approve ✅", `ok:${uid}`), btn("Decline ❌", `no:${uid}`)]]),
    });
  }

  // Approve or decline. The owner's request message is deleted so the panel stays clean.
  async decide(op, id, answer, chat, msgId) {
    if (!/^\d{1,20}$/.test(id || "")) return answer("Invalid");
    const pending = await this.kv("pending", {});
    const p = pending[id];
    delete pending[id];
    await this.kvPut("pending", pending);
    await this.deleteMsg(chat, msgId);

    if (op === "ok") {
      const users = await this.kv("users", {});
      if (!users[id]) {
        users[id] = { name: (p && p.name) || "", username: (p && p.username) || "", at: Date.now(), perms: [] };
        await this.kvPut("users", users);
      }
      await this.tgSend(id, "✅ Your access was approved. Send /start to open the panel.");
      return answer("Approved");
    }

    const denied = await this.kv("denied", {});
    denied[id] = Date.now();
    await this.kvPut("denied", denied);
    await this.tgSend(id, "❌ Your access request was declined.");
    return answer("Declined");
  }

  // ================= buttons =================

  async onToggle(chat, msgId, uid, rid, page, users, answer) {
    const perms = (users[uid] && users[uid].perms) || [];
    if (!this.cfg.relays[rid] || (!this.isOwner(uid) && !perms.includes(rid))) {
      return answer("No access");
    }
    await answer();
    await this.show(chat, uid, `h:${page}`, { note: "⏳ Sending command…" }, msgId);
    const next = this.state[rid] === "ON" ? "OFF" : "ON";
    const r = await this.toggleRelay(rid, next);
    return this.show(chat, uid, `h:${page}`, { note: this.noteFor(rid, next, r) }, msgId);
  }

  async onCallback(cb) {
    const uid = String((cb.from && cb.from.id) ?? "");
    const msg = cb.message;
    const chat = (msg && msg.chat && msg.chat.id) ?? uid;
    const msgId = (msg && msg.message_id) ?? null;
    const data = String(cb.data || "");
    const [op, a, b] = data.split(":");
    const answer = (text = "") => this.tgApi("answerCallbackQuery", { callback_query_id: cb.id, text });
    const owner = this.isOwner(uid);

    if (op === "ok" || op === "no") {
      if (!owner) return answer("Owner only");
      return this.decide(op, a, answer, chat, msgId);
    }

    const users = await this.kv("users", {});
    if (!owner && !users[uid]) return answer("No access");
    if (OWNER_OPS.has(op) && !owner) return answer("Owner only");
    if (op === "x") return answer();

    if (NAV_OPS.has(op)) {
      await answer();
      return this.show(chat, uid, data, {}, msgId);
    }

    switch (op) {
      case "t":
        return this.onToggle(chat, msgId, uid, a, Number(b) || 0, users, answer);

      case "al?":
        await answer();
        return this.show(chat, uid, `h:${Number(a) || 0}`, { confirmAll: true }, msgId);

      case "al!": {
        await answer();
        await this.show(chat, uid, "h:0", { note: "⏳ Turning relays off…" }, msgId);
        const fails = await this.allOff();
        return this.show(
          chat, uid, "h:0",
          { note: fails ? `⚠️ ${fails} relay(s) did not turn off` : "✅ All relays are off" },
          msgId
        );
      }

      case "wz": {
        await answer();
        if (this.cfg.setup) return this.show(chat, uid, "h:0", {}, msgId);
        const logic = a === "H" ? "H" : "L";
        this.cfg.relays = {};
        ALL_PINS.slice(0, DEFAULT_RELAY_COUNT).forEach((pin, i) => {
          this.cfg.relays[`r${i + 1}`] = { name: `Relay ${i + 1}`, pin, logic };
        });
        this.cfg.setup = true;
        await this.saveCfg(true);
        return this.show(chat, uid, "h:0", { note: "✅ Setup complete" }, msgId);
      }

      case "pt": {
        const pin = Number(a);
        if (!ALL_PINS.includes(pin)) return answer("Invalid");
        if (this.cfg.allowed.includes(pin)) {
          if (this.usedPins().has(pin)) return answer("🔒 This pin is in use");
          this.cfg.allowed = this.cfg.allowed.filter((p) => p !== pin);
        } else {
          this.cfg.allowed = [...this.cfg.allowed, pin].sort((x, y) => x - y);
        }
        await this.kvPut("cfg", this.cfg);
        await answer();
        return this.show(chat, uid, "pm", {}, msgId);
      }

      case "apl": {
        const pin = Number(a);
        const rid = this.freeRid();
        if (!rid) return answer(`Maximum ${MAX_RELAYS} relays`);
        if (!this.cfg.allowed.includes(pin) || this.usedPins().has(pin)) {
          return answer("Invalid or in use");
        }
        this.cfg.relays[rid] = { name: `Relay ${num(rid)}`, pin, logic: b === "H" ? "H" : "L" };
        await this.saveCfg(true);
        await answer("✅ Relay added");
        return this.show(chat, uid, `er:${rid}`, {}, msgId);
      }

      case "en": {
        if (!this.cfg.relays[a]) return answer("Invalid");
        await this.kvPut("input", { type: "rename", rid: a });
        await answer();
        return this.show(chat, uid, `er:${a}`, { note: "✏️ Send the new name in this chat (max 24 characters)" }, msgId);
      }

      case "el": {
        const r = this.cfg.relays[a];
        if (!r) return answer("Invalid");
        const logic = b === "H" ? "H" : "L";
        if (r.logic === logic) {
          await answer();
          return this.show(chat, uid, `er:${a}`, {}, msgId);
        }
        r.logic = logic;
        delete this.state[a];   // the real state comes back from the device
        await this.kvPut("state", this.state);
        await this.saveCfg(true);
        await answer("✅ Logic changed");
        return this.show(chat, uid, `er:${a}`, { note: "⏳ Applying to the device…" }, msgId);
      }

      case "eps": {
        const r = this.cfg.relays[a];
        const pin = Number(b);
        if (!r) return answer("Invalid");
        if (!this.cfg.allowed.includes(pin) || this.usedPins().has(pin)) {
          return answer("Invalid or in use");
        }
        r.pin = pin;
        delete this.state[a];
        await this.kvPut("state", this.state);
        await this.saveCfg(true);
        await answer();
        return this.show(chat, uid, `er:${a}`, { note: "⏳ Pin changed; applying…" }, msgId);
      }

      case "edk": {
        const r = this.cfg.relays[a];
        if (!r) return answer("Invalid");
        const name = r.name;
        if (this.state[a] === "ON") await this.toggleRelay(a, "OFF");
        delete this.cfg.relays[a];
        delete this.state[a];
        const all = await this.kv("users", {});
        for (const u of Object.values(all)) {
          if (u.perms) u.perms = u.perms.filter((x) => x !== a);
        }
        await this.kvPut("users", all);
        await this.kvPut("state", this.state);
        await this.saveCfg(true);
        await answer("🗑 Deleted");
        return this.show(chat, uid, "st", { note: `🗑 «${name}» deleted` }, msgId);
      }

      case "up": {
        const all = await this.kv("users", {});
        const u = all[a];
        if (!u) return answer("User not found");
        if (!this.cfg.relays[b]) return answer("Invalid");
        const perms = new Set(u.perms || []);
        if (perms.has(b)) perms.delete(b);
        else perms.add(b);
        u.perms = [...perms];
        await this.kvPut("users", all);
        await answer();
        return this.show(chat, uid, `uu:${a}`, {}, msgId);
      }

      case "ur": {
        const all = await this.kv("users", {});
        if (!all[a]) return answer("User not found");
        delete all[a];
        await this.kvPut("users", all);
        await this.tgSend(a, "⛔ The owner removed your access.");
        await answer("Removed");
        return this.show(chat, uid, "us", {}, msgId);
      }

      case "dclr": {
        await this.kvPut("denied", {});
        await answer("Cleared");
        return this.show(chat, uid, "us", { note: "♻️ Declined list cleared" }, msgId);
      }

      default:
        return answer();
    }
  }

  // ================= incoming Telegram updates =================

  async tg(update) {
    await this.load();

    const uid0 = Number(update && update.update_id);
    if (Number.isFinite(uid0)) {
      const seen = await this.kv("tgSeen", []);
      if (seen.includes(uid0)) return;
      seen.push(uid0);
      if (seen.length > TG_SEEN_MAX) seen.splice(0, seen.length - TG_SEEN_MAX);
      await this.kvPut("tgSeen", seen);
    }

    if (update.callback_query) return this.onCallback(update.callback_query);

    const m = update.message;
    if (!m || typeof m.text !== "string") return;

    const uid = String((m.from && m.from.id) ?? "");
    const chat = m.chat && m.chat.id;
    const text = m.text.trim();
    const cmd = text.split(/\s+/)[0].split("@")[0].toLowerCase();

    // No owner yet: the first /start claims ownership
    if (!this.owner) {
      if (cmd === "/start") {
        await this.claimOwner(uid);
        await this.tgSend(chat, "👑 You are now the owner of this NEXUS device.");
        return this.start(chat, uid, m.message_id);
      }
      return this.tgSend(chat, "Send /start to set up this NEXUS device.");
    }

    const owner = this.isOwner(uid);
    const users = await this.kv("users", {});
    if (!owner && !users[uid]) {
      if (cmd === "/start") return this.requestAccess(chat, uid, m.from);
      return this.tgSend(chat, "🔒 No access. Send /start to request it.");
    }

    if (owner && cmd.startsWith("/")) await this.ctx.storage.delete("input");

    // Owner is renaming a relay: this message is the new name (menu buttons excluded)
    if (owner && !cmd.startsWith("/") && text && !MENU_LABELS.has(text)) {
      const input = await this.kv("input", null);
      if (input && input.type === "rename" && this.cfg.relays[input.rid]) {
        await this.ctx.storage.delete("input");
        this.cfg.relays[input.rid].name = clip(text, MAX_NAME);
        await this.saveCfg(true);
        await this.deleteMsg(chat, m.message_id);
        return this.show(chat, uid, `er:${input.rid}`, { note: "✅ Name saved" });
      }
    }

    if (cmd === "/start") return this.start(chat, uid, m.message_id);
    if (cmd === "/panel" || text === BTN.panel) return this.command(chat, m.message_id, uid, "h:0");
    if (cmd === "/help" || text === BTN.help) return this.command(chat, m.message_id, uid, "hp");
    if (cmd === "/users" || text === BTN.users) {
      return this.command(chat, m.message_id, uid, owner ? "us" : "h:0");
    }
    if (cmd === "/settings" || text === BTN.settings) {
      return this.command(chat, m.message_id, uid, owner ? "st" : "h:0");
    }
    return this.command(chat, m.message_id, uid, "h:0");
  }
}