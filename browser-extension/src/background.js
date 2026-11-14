// Archon — per-tab volume, background (service worker on Chromium, event page
// on Firefox). Talks to the Archon agent through native messaging
// ("dev.archon.tabs"), reports the tabs that make sound and applies the
// volume / mute the phone sends.
"use strict";

const api = globalThis.browser ?? globalThis.chrome;
const HOST = "dev.archon.tabs";
const RECENT_MS = 5 * 60 * 1000; // a tab that played in the last 5 minutes stays listed
const MAX = 10000; // protocol volume scale

const volume = new Map(); // tabId -> 0..10000 set from the phone
const lastAudible = new Map(); // tabId -> time it last made sound
const icons = new Map(); // favicon URL -> base64 PNG ("" = none)
let port = null;
let reportTimer = null;
let retry = 1000;

function browserName() {
  const ua = navigator.userAgent;
  if (/Firefox\//.test(ua)) return "firefox";
  if (/Edg\//.test(ua)) return "edge";
  if (/OPR\//.test(ua)) return "opera";
  if (navigator.brave) return "brave";
  return "chrome";
}

function connect() {
  let heard = false;
  try {
    port = api.runtime.connectNative(HOST);
  } catch (e) {
    port = null;
    retry = Math.min(retry * 2, 60000);
    return setTimeout(connect, retry);
  }
  port.onMessage.addListener((msg) => {
    heard = true;
    retry = 1000; // the agent answered: a later drop starts the backoff again from 1 s
    onHostMessage(msg);
  });
  port.onDisconnect.addListener(() => {
    port = null;
    // The agent may be closed: try again later, waiting longer each time (up to 1 min).
    retry = heard ? 1000 : Math.min(retry * 2, 60000);
    setTimeout(connect, retry);
  });
  post({ t: "hello", browser: browserName(), version: api.runtime.getManifest().version });
  scheduleReport();
}

function post(msg) {
  if (!port) return;
  try {
    port.postMessage(msg);
  } catch (e) {
    port = null;
  }
}

function host(url) {
  try {
    return new URL(url).hostname;
  } catch (e) {
    return "";
  }
}

// Cut to `max` characters without splitting one (an emoji is two UTF-16 units).
function cut(s, max) {
  const chars = Array.from(s || "");
  return chars.length > max ? chars.slice(0, max).join("") : s || "";
}

// Favicon as a small PNG (the phone shows it on the tab's card). The page picks
// the URL, so: no cookies, 3 s at most, images only, 256 KB at most.
const MAX_ICON_BYTES = 256 * 1024;
async function iconFor(url) {
  if (!url || !/^https?:|^data:image\//.test(url)) return "";
  if (icons.has(url)) return icons.get(url);
  icons.set(url, ""); // no repeated fetches while this one runs
  const abort = new AbortController();
  const timer = setTimeout(() => abort.abort(), 3000);
  try {
    const res = await fetch(url, { credentials: "omit", redirect: "error", signal: abort.signal });
    const type = res.headers.get("content-type") || "";
    const size = Number(res.headers.get("content-length") || 0);
    if (!res.ok || !/^image\//.test(type) || size > MAX_ICON_BYTES) throw new Error("not an icon");
    const blob = await res.blob();
    if (blob.size > MAX_ICON_BYTES) throw new Error("too big");
    const bmp = await createImageBitmap(blob);
    const canvas = new OffscreenCanvas(32, 32);
    canvas.getContext("2d").drawImage(bmp, 0, 0, 32, 32);
    const png = await canvas.convertToBlob({ type: "image/png" });
    const bytes = new Uint8Array(await png.arrayBuffer());
    let bin = "";
    for (const b of bytes) bin += String.fromCharCode(b);
    const b64 = btoa(bin);
    icons.set(url, b64.length <= 48 * 1024 ? b64 : "");
    if (icons.size > 200) icons.delete(icons.keys().next().value);
    scheduleReport();
  } catch (e) {
    /* no icon */
  } finally {
    clearTimeout(timer);
  }
  return icons.get(url) || "";
}

function scheduleReport() {
  if (reportTimer) return;
  reportTimer = setTimeout(report, 150); // coalesce bursts of tab events
}

async function report() {
  reportTimer = null;
  if (!port) return;
  const now = Date.now();
  const all = await api.tabs.query({});
  const out = [];
  for (const tab of all) {
    if (tab.incognito) continue; // private windows stay private: never reported
    if (tab.audible) lastAudible.set(tab.id, now);
    const v = volume.has(tab.id) ? volume.get(tab.id) : MAX;
    const muted = !!(tab.mutedInfo && tab.mutedInfo.muted);
    const recent = now - (lastAudible.get(tab.id) || 0) < RECENT_MS;
    if (!tab.audible && !recent && !muted && v === MAX) continue;
    out.push({
      id: tab.id,
      title: cut(tab.title || host(tab.url) || "", 200),
      host: host(tab.url),
      audible: !!tab.audible,
      muted,
      volume: v,
      icon: icons.get(tab.favIconUrl) || "",
    });
    if (tab.favIconUrl && !icons.has(tab.favIconUrl)) iconFor(tab.favIconUrl);
  }
  post({ t: "tabs", tabs: out });
}

// The browser may stop this worker when idle: volumes set from the phone are
// kept in session storage (memory only, gone when the browser closes).
const session = api.storage && api.storage.session;
function saveVolumes() {
  if (session) session.set({ volumes: Object.fromEntries(volume) }).catch(() => {});
}
const restored = session
  ? session
      .get("volumes")
      .then((r) => {
        for (const [id, v] of Object.entries((r && r.volumes) || {})) volume.set(Number(id), Number(v));
      })
      .catch(() => {})
  : Promise.resolve();

async function setVolume(tabId, v) {
  v = Math.max(0, Math.min(MAX, Math.round(v)));
  volume.set(tabId, v);
  saveVolumes();
  try {
    await api.tabs.sendMessage(tabId, { t: "gain", value: v / MAX }); // every frame's bridge.js
  } catch (e) {
    /* the page has no content script yet (e.g. a browser page) */
  }
  scheduleReport();
}

function onHostMessage(msg) {
  if (!msg || typeof msg.id !== "number") return;
  if (msg.t === "volume" && typeof msg.volume === "number") setVolume(msg.id, msg.volume);
  else if (msg.t === "mute" && typeof msg.muted === "boolean") api.tabs.update(msg.id, { muted: msg.muted }).catch(() => {});
}

// A page that loads (or reloads) gets its tab's gain back.
// (sendResponse, not a returned Promise: older Chromium ignores promises here.)
api.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (msg && msg.t === "hello" && sender.tab) {
    const v = volume.has(sender.tab.id) ? volume.get(sender.tab.id) : MAX;
    sendResponse({ value: v / MAX });
  }
  return false;
});

api.tabs.onUpdated.addListener((id, change) => {
  if ("audible" in change || "mutedInfo" in change || "title" in change || "favIconUrl" in change) scheduleReport();
});
api.tabs.onRemoved.addListener((id) => {
  if (volume.delete(id)) saveVolumes();
  lastAudible.delete(id);
  scheduleReport();
});
// Recently-audible tabs drop off the list once their 5 minutes pass.
setInterval(scheduleReport, 60 * 1000);

restored.then(connect);
