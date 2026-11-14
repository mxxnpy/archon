// Archon — per-tab volume, page side (runs in the page's own world).
//
// Applies a tab gain on top of whatever volume the site sets, without the site
// noticing: <audio>/<video> keep reporting the site's own value (its slider
// does not move), and Web Audio graphs that play to the speakers go through
// one gain node per context. The gain arrives from the extension through
// window.postMessage. Everything lives in this closure: nothing is added to
// the page's globals.
(() => {
  "use strict";
  const proto = HTMLMediaElement.prototype;
  const desc = Object.getOwnPropertyDescriptor(proto, "volume");
  // Already hooked (script injected twice): the native accessor is no longer there.
  if (!desc || !desc.get || !Function.prototype.toString.call(desc.get).includes("[native code]")) return;

  let gain = 1; // 0..1, the tab's volume set from the phone
  const clamp = (v) => (Number.isFinite(v) ? (v < 0 ? 0 : v > 1 ? 1 : v) : 1);

  // ---- <audio> / <video> ------------------------------------------------------
  const siteVolume = new WeakMap(); // element -> the volume the site asked for
  const tracked = new WeakSet(); // elements that played (O(1) membership)
  let refs = []; // WeakRefs of tracked elements, to apply a new gain to them
  // Elements whose sound goes into Web Audio (createMediaElementSource): the
  // context's master gain already scales them, so the element keeps the site's value.
  const routed = new WeakSet();

  function track(el) {
    if (tracked.has(el)) return;
    tracked.add(el);
    if (!siteVolume.has(el)) siteVolume.set(el, desc.get.call(el));
    refs.push(new WeakRef(el));
    if (refs.length > 256 && refs.length % 256 === 0) refs = refs.filter((r) => r.deref()); // drop collected ones
  }

  function apply(el) {
    const site = siteVolume.has(el) ? siteVolume.get(el) : desc.get.call(el);
    desc.set.call(el, routed.has(el) ? site : clamp(site * gain));
  }

  // Same name and shape as the native accessor ("get volume" / "set volume").
  const hooked = {
    get volume() {
      return siteVolume.has(this) ? siteVolume.get(this) : desc.get.call(this);
    },
    set volume(v) {
      const n = Number(v);
      if (!(n >= 0 && n <= 1)) return desc.set.call(this, v); // let the browser throw as usual
      siteVolume.set(this, n);
      desc.set.call(this, routed.has(this) ? n : clamp(n * gain));
    },
  };
  const acc = Object.getOwnPropertyDescriptor(hooked, "volume");
  Object.defineProperty(proto, "volume", { configurable: true, enumerable: desc.enumerable, get: acc.get, set: acc.set });

  const realPlay = proto.play;
  proto.play = function play() {
    track(this);
    apply(this);
    return realPlay.apply(this, arguments);
  };
  // Elements that autoplay without calling play().
  document.addEventListener(
    "playing",
    (e) => {
      if (e.target instanceof HTMLMediaElement) {
        track(e.target);
        apply(e.target);
      }
    },
    true,
  );

  // ---- Web Audio --------------------------------------------------------------
  const masters = new WeakMap(); // AudioContext -> GainNode in front of destination
  const realConnect = AudioNode.prototype.connect;
  const realDisconnect = AudioNode.prototype.disconnect;
  let contexts = []; // WeakRefs of contexts with a master gain
  const Offline = window.OfflineAudioContext;

  function masterFor(ctx) {
    let m = masters.get(ctx);
    if (!m) {
      m = ctx.createGain();
      m.gain.value = gain;
      realConnect.call(m, ctx.destination);
      masters.set(ctx, m);
      contexts.push(new WeakRef(ctx));
    }
    return m;
  }

  // Rendering to a file (OfflineAudioContext) is not playback: never scaled.
  const live = (ctx) => !(Offline && ctx instanceof Offline);

  AudioNode.prototype.connect = function connect(target, ...rest) {
    if (target instanceof AudioDestinationNode && target.context === this.context && live(this.context) &&
        masters.get(this.context) !== this) {
      realConnect.call(this, masterFor(this.context), ...rest);
      return target; // what connect() returns natively, so chained calls keep working
    }
    return realConnect.call(this, target, ...rest);
  };
  AudioNode.prototype.disconnect = function disconnect(target, ...rest) {
    if (target instanceof AudioDestinationNode && masters.has(target.context)) {
      return realDisconnect.call(this, masters.get(target.context), ...rest);
    }
    return realDisconnect.call(this, ...(target === undefined ? [] : [target, ...rest]));
  };

  // A media element routed into Web Audio is scaled once, by the master.
  const Ctx = window.AudioContext; // the method lives here (offline contexts have none)
  const realMES = Ctx && Ctx.prototype.createMediaElementSource;
  if (realMES) {
    Ctx.prototype.createMediaElementSource = function createMediaElementSource(el) {
      const node = realMES.call(this, el);
      if (el instanceof HTMLMediaElement && live(this)) {
        routed.add(el);
        track(el);
        apply(el);
      }
      return node;
    };
  }

  // ---- gain updates from the extension -------------------------------------------
  function setGain(g) {
    const n = Number(g);
    if (!Number.isFinite(n)) return;
    gain = clamp(n);
    refs = refs.filter((ref) => {
      const el = ref.deref();
      if (el) apply(el);
      return !!el;
    });
    contexts = contexts.filter((ref) => {
      const ctx = ref.deref();
      const m = ctx && masters.get(ctx);
      if (m) m.gain.setTargetAtTime(gain, ctx.currentTime, 0.015); // no clicks
      return !!ctx;
    });
  }

  window.addEventListener("message", (e) => {
    const d = e.data;
    if (e.source === window && d && d.__archon === "gain" && typeof d.value === "number") setGain(d.value);
  });
})();
