// Archon — per-tab volume, page side (runs in the page's own world).
//
// Applies a tab gain on top of whatever volume the site sets, without the site
// noticing: <audio>/<video> keep reporting the site's own value (its slider
// does not move), and Web Audio graphs go through one gain node per context.
// The gain arrives from the extension through window.postMessage.
(() => {
  "use strict";
  if (window.__archonTabVolume) return;
  window.__archonTabVolume = true;

  let gain = 1; // 0..1, the tab's volume set from the phone
  const clamp = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);

  // ---- <audio> / <video> ------------------------------------------------------
  const proto = HTMLMediaElement.prototype;
  const desc = Object.getOwnPropertyDescriptor(proto, "volume");
  const siteVolume = new WeakMap(); // element -> the volume the site asked for
  const elements = new Set(); // WeakRefs of elements that played

  function track(el) {
    if (!siteVolume.has(el)) siteVolume.set(el, desc.get.call(el));
    for (const ref of elements) if (ref.deref() === el) return;
    elements.add(new WeakRef(el));
  }

  function apply(el) {
    const site = siteVolume.has(el) ? siteVolume.get(el) : desc.get.call(el);
    desc.set.call(el, clamp(site * gain));
  }

  Object.defineProperty(proto, "volume", {
    configurable: true,
    enumerable: desc.enumerable,
    get() {
      return siteVolume.has(this) ? siteVolume.get(this) : desc.get.call(this);
    },
    set(v) {
      const n = Number(v);
      if (!(n >= 0 && n <= 1)) return desc.set.call(this, v); // let the browser throw as usual
      siteVolume.set(this, n);
      desc.set.call(this, clamp(n * gain));
    },
  });

  const play = proto.play;
  proto.play = function (...args) {
    track(this);
    apply(this);
    return play.apply(this, args);
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
  const masters = new WeakMap(); // BaseAudioContext -> GainNode in front of destination
  const realConnect = AudioNode.prototype.connect;
  const realDisconnect = AudioNode.prototype.disconnect;
  const contexts = new Set(); // WeakRefs of contexts with a master gain

  function masterFor(ctx) {
    let m = masters.get(ctx);
    if (!m) {
      m = ctx.createGain();
      m.gain.value = gain;
      realConnect.call(m, ctx.destination);
      masters.set(ctx, m);
      contexts.add(new WeakRef(ctx));
    }
    return m;
  }

  AudioNode.prototype.connect = function (target, ...rest) {
    if (target instanceof AudioDestinationNode && target.context === this.context && !(this instanceof GainNode && masters.get(this.context) === this)) {
      realConnect.call(this, masterFor(this.context), ...rest);
      return target; // what connect() returns natively, so chained calls keep working
    }
    return realConnect.call(this, target, ...rest);
  };
  AudioNode.prototype.disconnect = function (target, ...rest) {
    if (target instanceof AudioDestinationNode && masters.has(target.context)) {
      return realDisconnect.call(this, masters.get(target.context), ...rest);
    }
    return realDisconnect.call(this, ...(target === undefined ? [] : [target, ...rest]));
  };

  // ---- gain updates from the extension -------------------------------------------
  function setGain(g) {
    gain = clamp(Number(g));
    for (const ref of elements) {
      const el = ref.deref();
      if (el) apply(el);
      else elements.delete(ref);
    }
    for (const ref of contexts) {
      const ctx = ref.deref();
      if (!ctx) {
        contexts.delete(ref);
        continue;
      }
      const m = masters.get(ctx);
      if (m) m.gain.setTargetAtTime(gain, ctx.currentTime, 0.015); // no clicks
    }
  }

  window.addEventListener("message", (e) => {
    const d = e.data;
    if (e.source === window && d && d.__archon === "gain" && typeof d.value === "number") setGain(d.value);
  });
})();
