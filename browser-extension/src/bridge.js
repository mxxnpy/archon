// Archon — per-tab volume, extension side of each page (isolated world).
// Relays the tab's gain from the extension to page_volume.js (page world).
(() => {
  "use strict";
  const api = globalThis.browser ?? globalThis.chrome;
  const send = (value) => window.postMessage({ __archon: "gain", value }, "*");

  api.runtime.onMessage.addListener((msg) => {
    if (msg && msg.t === "gain" && typeof msg.value === "number") send(msg.value);
  });
  // A page (or frame) that loads after the gain was set asks for it once.
  api.runtime
    .sendMessage({ t: "hello" })
    .then((r) => {
      if (r && typeof r.value === "number" && r.value !== 1) send(r.value);
    })
    .catch(() => {});
})();
