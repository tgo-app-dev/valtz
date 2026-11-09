// The page a PROGRAM runs in (DESIGN §4t): what valtz-procedural hands
// WebKit -- the document, the script run before anything of the
// page's, the module that loads the program -- as text, filled in with
// a render's values.

#ifndef VALTZ_PROCEDURAL_HARNESS_H
#define VALTZ_PROCEDURAL_HARNESS_H

#include <string_view>

namespace valtz::procedural {

// The scheme the page and everything it may load are served under: the
// only one its content rules, its CSP and its navigations allow.
inline constexpr std::string_view kScheme = "valtz-proc";
inline constexpr std::string_view kOrigin = "valtz-proc://local";

// What the page may load, and from where: its own scheme's -- the
// harness, the program, Valtz's copies of three.js and Babylon.js, the
// project's assets it was given -- and data in data: and blob: URLs the
// program makes itself. Nothing else: no network, no file, no frame, no
// worker.
inline constexpr std::string_view kContentSecurityPolicy =
    "default-src 'none'; "
    "script-src valtz-proc: 'unsafe-inline' 'unsafe-eval' "
    "'wasm-unsafe-eval' blob: data:; "
    "style-src valtz-proc: 'unsafe-inline' data: blob:; "
    "img-src valtz-proc: data: blob:; "
    "font-src data: blob:; "
    "media-src valtz-proc: data: blob:; "
    "connect-src valtz-proc: data: blob:; "
    "worker-src 'none'; child-src 'none'; frame-src 'none'; "
    "object-src 'none'; form-action 'none'; base-uri 'none'; "
    "manifest-src 'none'";

// WebKit's content rules, a layer under the CSP: every load blocked but
// the page's own scheme's and the data a program makes.
inline constexpr std::string_view kContentRules = R"JSON([
  {"trigger": {"url-filter": ".*"}, "action": {"type": "block"}},
  {"trigger": {"url-filter": "^valtz-proc:"},
   "action": {"type": "ignore-previous-rules"}},
  {"trigger": {"url-filter": "^data:"},
   "action": {"type": "ignore-previous-rules"}},
  {"trigger": {"url-filter": "^blob:"},
   "action": {"type": "ignore-previous-rules"}},
  {"trigger": {"url-filter": "^about:"},
   "action": {"type": "ignore-previous-rules"}}
])JSON";

// The document: @CSP@, @BABYLON@ (a script tag, or nothing).
inline constexpr std::string_view kPage = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="@CSP@">
<style>
html, body { margin: 0; padding: 0; width: 100%; height: 100%;
             overflow: hidden; background: transparent; }
</style>
<script type="importmap">
{"imports": {
  "three": "valtz-proc://local/lib/three/three.module.js",
  "three/webgpu": "valtz-proc://local/lib/three/three.webgpu.js",
  "three/tsl": "valtz-proc://local/lib/three/three.tsl.js",
  "three/addons/": "valtz-proc://local/lib/three/addons/",
  "three/examples/jsm/": "valtz-proc://local/lib/three/addons/"
}}
</script>
@BABYLON@<script type="module" src="valtz-proc://local/valtz.js"></script>
</head><body></body></html>
)HTML";

inline constexpr std::string_view kBabylonTag =
    "<script src=\"valtz-proc://local/lib/babylon.js\"></script>\n";

// The module that loads the program, and says when it has: its errors
// reported, then whatever it asked to be waited for (valtz.ready).
inline constexpr std::string_view kLoader = R"JS(
const told = (m) => {
  try { window.webkit.messageHandlers.valtz.postMessage(m); } catch (e) {}
};
let ok = true;
try {
  await import('valtz-proc://local/program.js');
} catch (e) {
  ok = false;
  window.__valtz_report(e, 'error');
}
try {
  await window.__valtz_waited();
} catch (e) {
  window.__valtz_report(e, 'error');
}
// Babylon.js compiles its shaders in parallel and draws a mesh only once
// its material is ready -- frames on the render's clock come faster than
// that, and the first ones would show nothing: each scene waited for.
const B = window.BABYLON;
if (B && B.EngineStore && B.EngineStore.Instances) {
  const late = new Promise((r) => setTimeout(r, 20000, 'late'));
  for (const engine of B.EngineStore.Instances) {
    for (const scene of engine.scenes || []) {
      try {
        if (await Promise.race([scene.whenReadyAsync(), late]) === 'late') {
          window.__valtz_report(
              new Error('a Babylon.js scene was not ready in 20 s'), 'warn');
        }
      } catch (e) {
        window.__valtz_report(e, 'error');
      }
    }
  }
}
told({event: 'loaded', ok});
)JS";

// Run before anything of the page's, in every frame: what could reach
// a network, a device or the person's data taken away; the CLOCK made
// the render's -- performance.now, Date, requestAnimationFrame -- and
// Math.random seeded; the console and every error told to Valtz; the
// program's `valtz` object. @PARAMS@: {"width", "height", "fps",
// "frames", "seed", "epoch", "rate" (the soundtrack's sample rate),
// "assets": [{"name", "id", "kind", "type", "width", "height", "seconds",
// "url", "data" (small ones)}]}.
inline constexpr std::string_view kBootstrap = R"JS((() => {
'use strict';
const P = @PARAMS@;
const W = window;
const handler = W.webkit && W.webkit.messageHandlers &&
                W.webkit.messageHandlers.valtz;
let told = 0;
const post = (m) => {
  if (m.event === 'log' && ++told > 400) return;
  try { handler.postMessage(m); } catch (e) {}
};
const fixed = (o, k, v) => {
  try {
    Object.defineProperty(o, k, {value: v, writable: false,
                                 configurable: false});
  } catch (e) {}
};
const kept = (o, k, v) => {
  try {
    Object.defineProperty(o, k, {value: v, writable: true,
                                 configurable: false});
  } catch (e) {}
};
const getter = (o, k, g) => {
  try { Object.defineProperty(o, k, {get: g, configurable: false}); }
  catch (e) {}
};

// ---- nothing leaves the page --------------------------------------------
for (const k of [
    'RTCPeerConnection', 'webkitRTCPeerConnection', 'RTCDataChannel',
    'RTCRtpSender', 'RTCRtpReceiver', 'RTCRtpTransceiver',
    'RTCSessionDescription', 'RTCIceCandidate', 'RTCIceTransport',
    'RTCDtlsTransport', 'RTCSctpTransport', 'RTCCertificate',
    'RTCDTMFSender', 'RTCRtpScriptTransform', 'WebSocket', 'WebTransport',
    'EventSource', 'Worker', 'SharedWorker', 'ServiceWorker',
    'ServiceWorkerContainer', 'ServiceWorkerRegistration',
    'BroadcastChannel', 'Notification', 'PaymentRequest',
    'ApplePaySession', 'PushManager', 'MediaRecorder', 'MediaStream',
    'SpeechRecognition', 'webkitSpeechRecognition', 'speechSynthesis',
    'SpeechSynthesisUtterance', 'PublicKeyCredential',
    'IdentityCredential', 'showOpenFilePicker', 'showSaveFilePicker',
    'showDirectoryPicker', 'cookieStore']) {
  fixed(W, k, undefined);
}
fixed(W, 'open', () => null);
for (const k of [
    'mediaDevices', 'geolocation', 'clipboard', 'serviceWorker',
    'credentials', 'share', 'canShare', 'sendBeacon', 'bluetooth', 'usb',
    'hid', 'serial', 'xr', 'wakeLock', 'mediaSession', 'contacts',
    'getUserMedia', 'webkitGetUserMedia', 'requestMediaKeySystemAccess']) {
  fixed(navigator, k, undefined);
}

// ---- the render's clock -------------------------------------------------
let now = 0;
const RealDate = Date;
function VDate(...a) {
  if (!new.target) return new RealDate(P.epoch + now).toString();
  return a.length ? new RealDate(...a) : new RealDate(P.epoch + now);
}
VDate.prototype = RealDate.prototype;
VDate.now = () => P.epoch + now;
VDate.parse = RealDate.parse;
VDate.UTC = RealDate.UTC;
kept(W, 'Date', VDate);
kept(performance, 'now', () => now);
const frames = new Map();
let frameId = 0;
const raf = (cb) => { frames.set(++frameId, cb); return frameId; };
const caf = (id) => { frames.delete(id); };
kept(W, 'requestAnimationFrame', raf);
kept(W, 'cancelAnimationFrame', caf);
kept(W, 'webkitRequestAnimationFrame', raf);
kept(W, 'webkitCancelAnimationFrame', caf);

// Math.random, seeded: the same seed draws the same picture.
let a = P.seed >>> 0, b = Math.floor(P.seed / 4294967296) >>> 0;
let c = 0x9e3779b9, d = 0x243f6a88;
const random = () => {
  a |= 0; b |= 0; c |= 0; d |= 0;
  const t = (((a + b) | 0) + d) | 0;
  d = (d + 1) | 0;
  a = b ^ (b >>> 9);
  b = (c + (c << 3)) | 0;
  c = (c << 21) | (c >>> 11);
  c = (c + t) | 0;
  return (t >>> 0) / 4294967296;
};
for (let i = 0; i < 16; i++) random();
kept(Math, 'random', random);

// Drawn as nobody sees it, one pixel a CSS pixel, the page in view.
getter(W, 'devicePixelRatio', () => 1);
getter(document, 'hidden', () => false);
getter(document, 'visibilityState', () => 'visible');
getter(screen, 'width', () => P.width);
getter(screen, 'height', () => P.height);

// ---- told to Valtz ------------------------------------------------------
const text = (x) => {
  if (x instanceof Error) return `${x.name}: ${x.message}`;
  if (x && typeof x === 'object') {
    try { return JSON.stringify(x).slice(0, 400); } catch (e) {}
  }
  return String(x);
};
const where = (e) => {
  const at = {};
  if (e && typeof e === 'object') {
    if (e.sourceURL) at.source = String(e.sourceURL);
    if (e.line) at.line = e.line;
    if (e.column) at.column = e.column;
  }
  return at;
};
const report = (e, level) => {
  post(Object.assign({event: 'log', level: level || 'error',
                      text: text(e).slice(0, 2000)}, where(e)));
};
fixed(W, '__valtz_report', report);
for (const [k, level] of [['error', 'error'], ['warn', 'warn'],
                          ['log', 'log'], ['info', 'log'],
                          ['debug', 'log']]) {
  kept(console, k, (...args) => {
    post({event: 'log', level,
          text: args.map(text).join(' ').slice(0, 2000)});
  });
}
W.addEventListener('error', (e) => {
  const t = e.target;
  if (t && t !== W && (t.src || t.href)) {
    post({event: 'log', level: 'error',
          text: `could not load ${t.src || t.href}`});
    return;
  }
  post({event: 'log', level: 'error',
        text: e.message || text(e.error), source: e.filename,
        line: e.lineno, column: e.colno});
}, true);
W.addEventListener('unhandledrejection', (e) => {
  const r = e.reason;
  post(Object.assign({event: 'log', level: 'error',
                      text: 'unhandled rejection: ' + text(r)}, where(r)));
});
document.addEventListener('securitypolicyviolation', (e) => {
  post({event: 'log', level: 'warn',
        text: `refused: ${e.blockedURI || 'an inline resource'} ` +
              `(${e.effectiveDirective}): the page reaches nothing ` +
              `outside itself`});
});

// What it drew on, for a page left clear: canvases and their contexts.
const made = {};
const gpuCanvases = new Set();
const realContext = HTMLCanvasElement.prototype.getContext;
HTMLCanvasElement.prototype.getContext = function (type, ...rest) {
  const c = realContext.call(this, type, ...rest);
  if (c) made[type] = (made[type] || 0) + 1;
  if (c && type === 'webgpu') gpuCanvases.add(this);
  return c;
};

// WebGPU: WebKit's snapshot leaves a WebGPU canvas out -- so each is
// copied, right after the work that drew it is submitted, into a 2D
// canvas laid over it, which the snapshot takes.
const mirrors = new Map();
const mirror = () => {
  for (const cv of gpuCanvases) {
    if (!cv.isConnected || !cv.width || !cv.height) continue;
    let m = mirrors.get(cv);
    if (!m) {
      m = realCreate.call(document, 'canvas');
      m.dataset.valtzMirror = '';
      m.style.cssText = 'position:fixed;pointer-events:none;margin:0;' +
                        'padding:0;border:0';
      mirrors.set(cv, m);
    }
    if (m.previousSibling !== cv) cv.after(m);
    const r = cv.getBoundingClientRect();
    m.style.left = r.left + 'px';
    m.style.top = r.top + 'px';
    m.style.width = r.width + 'px';
    m.style.height = r.height + 'px';
    if (m.width !== cv.width) m.width = cv.width;
    if (m.height !== cv.height) m.height = cv.height;
    const g = realContext.call(m, '2d');
    g.clearRect(0, 0, m.width, m.height);
    g.drawImage(cv, 0, 0);
  }
};
if (W.GPUQueue) {
  const realSubmit = GPUQueue.prototype.submit;
  GPUQueue.prototype.submit = function (...args) {
    const r = realSubmit.apply(this, args);
    try { mirror(); } catch (e) {}
    return r;
  };
}

// <video>: held to the clip's clock -- never played, sought to each
// frame's time (its loop wrapping it; `data-valtz-start` seconds later)
// before that frame is drawn -- muted. Those made by the program but not
// on the page too (a texture's).
const videos = new Set();
const realCreate = Document.prototype.createElement;
Document.prototype.createElement = function (tag, ...rest) {
  const el = realCreate.call(this, tag, ...rest);
  if (String(tag).toLowerCase() === 'video') videos.add(el);
  return el;
};
HTMLMediaElement.prototype.play = function () {
  this.muted = true;
  return Promise.resolve();
};
// three.js's VideoTexture then updates every frame, not on a callback a
// held video may never call.
try { delete HTMLVideoElement.prototype.requestVideoFrameCallback; }
catch (e) {}
const once = (el, ev, ms) => new Promise((resolve) => {
  const done = () => { el.removeEventListener(ev, done); resolve(); };
  el.addEventListener(ev, done);
  setTimeout(done, ms);
});
const holdVideo = async (v, t) => {
  if (!v.currentSrc && !v.src && !v.querySelector('source')) return;
  v.muted = true;
  if (!v.paused) v.pause();
  if (v.readyState < 1) await once(v, 'loadedmetadata', 10000);
  const d = v.duration;
  if (!(d > 0) || !isFinite(d)) return;
  let at = Math.max(0, t - (Number(v.dataset.valtzStart) || 0));
  at = v.loop ? at % d : Math.min(at, Math.max(0, d - 0.001));
  if (Math.abs(v.currentTime - at) > 0.0005) {
    const sought = once(v, 'seeked', 10000);
    v.currentTime = at;
    await sought;
  }
  if (v.readyState < 2) await once(v, 'loadeddata', 10000);
};

// ---- the project's assets, read-only ------------------------------------
const assets = Object.freeze((P.assets || []).map((a) => Object.freeze(a)));
const find = (key) => {
  if (typeof key === 'number') return assets[key];
  const k = String(key);
  return assets.find((a) => a.name === k) || assets.find((a) => a.id === k) ||
         assets.find((a) => a.name.toLowerCase() === k.toLowerCase());
};
const need = (key, kinds) => {
  const a = find(key);
  if (!a) {
    throw new Error(`no asset "${key}": the program reads ` +
                    (assets.map((x) => `"${x.name}"`).join(', ') || 'none'));
  }
  if (kinds && !kinds.includes(a.kind)) {
    throw new Error(`"${a.name}" is a ${a.kind}, not a ${kinds.join(' or ')}`);
  }
  return a;
};
const picture = (src, what) => new Promise((resolve, reject) => {
  const img = new Image();
  img.onload = () => resolve(img);
  img.onerror = () => reject(new Error(`could not read ${what}`));
  img.src = src;
});
const read = async (a) => {
  const r = await fetch(a.data || a.url);
  if (!r.ok) throw new Error(`could not read "${a.name}"`);
  return r;
};

// ---- sound --------------------------------------------------------------
// A clip's SOUNDTRACK: an OfflineAudioContext the clip's length, rendered
// once its frames are drawn -- what is connected to its destination,
// scheduled on its own clock from 0. A program's `new AudioContext()` is
// it too: nothing plays aloud.
const RealOffline = W.OfflineAudioContext;
let soundtrack = null;
const audio = () => {
  if (!soundtrack) {
    const secs = P.frames > 0 ? P.frames / P.fps : 1;
    soundtrack = new RealOffline(2, Math.max(1, Math.round(secs * P.rate)),
                                 P.rate);
  }
  return soundtrack;
};
const sameContext = function () { return audio(); };
kept(W, 'AudioContext', sameContext);
kept(W, 'webkitAudioContext', sameContext);
fixed(W, '__valtz_sound', async () => {
  if (!soundtrack || P.frames <= 0) return null;
  const buf = await soundtrack.startRendering();
  const ch = buf.numberOfChannels;
  const n = buf.length;
  const chans = [];
  for (let c = 0; c < ch; c++) chans.push(buf.getChannelData(c));
  const pcm = new Int16Array(n * ch);
  let peak = 0;
  for (let i = 0; i < n; i++) {
    for (let c = 0; c < ch; c++) {
      const s = Math.max(-1, Math.min(1, chans[c][i]));
      peak = Math.max(peak, Math.abs(s));
      pcm[i * ch + c] = s < 0 ? s * 32768 : s * 32767;
    }
  }
  if (peak === 0) return {peak: 0};
  const bytes = new Uint8Array(pcm.buffer);
  let bin = '';
  for (let i = 0; i < bytes.length; i += 0x8000) {
    bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  }
  return {peak, rate: buf.sampleRate, channels: ch, frames: n,
          data: btoa(bin)};
});

// ---- the program's `valtz` ----------------------------------------------
const onFrame = [];
const waits = [];
let canvas = null;
const api = {
  width: P.width,
  height: P.height,
  fps: P.fps,
  frames: P.frames,
  duration: P.frames > 0 ? P.frames / P.fps : 0,
  still: P.frames === 0,
  seed: P.seed,
  get time() { return now / 1000; },
  get frame() { return Math.round(now * P.fps / 1000); },
  random,
  onFrame(fn) { if (typeof fn === 'function') onFrame.push(fn); },
  ready(p) { waits.push(Promise.resolve(p)); },
  // The assets it was given: by name (or id, or place), their files at
  // `url`, small ones at `data` too, read at once.
  assets,
  asset(key) { return need(key); },
  // A picture -- an image asset, a clip's frame at `t` seconds -- loaded
  // as an <img>: for drawImage, a texture.
  image(key, t) {
    const a = need(key, ['image', 'video']);
    if (a.kind === 'video') return api.frame(key, t || 0);
    return picture(a.data || a.url, `"${a.name}"`);
  },
  frame(key, t) {
    const a = need(key, ['video', 'image']);
    if (a.kind === 'image') return picture(a.data || a.url, `"${a.name}"`);
    const at = Math.max(0, Math.min(Number(t) || 0, a.seconds || 0));
    return picture(`${a.url}/frame/${at.toFixed(4)}`,
                   `"${a.name}" at ${at.toFixed(2)} s`);
  },
  async text(key) { return (await read(need(key))).text(); },
  async bytes(key) { return (await read(need(key))).arrayBuffer(); },
  // A sound -- or a clip's -- decoded: an AudioBuffer, to play into the
  // soundtrack or to read (valtz.level).
  async sound(key) {
    const a = need(key, ['audio', 'video']);
    const bytes = await (await read(a)).arrayBuffer();
    return new RealOffline(2, 1, P.rate).decodeAudioData(bytes);
  },
  // How loud a sound is about `t` seconds -- the RMS of its channels over
  // `span` seconds (a frame's by default), 0 ... 1: what moves with it.
  level(buffer, t, span) {
    if (!buffer || !buffer.getChannelData) return 0;
    const w = Math.max(1, Math.round((span || 1 / P.fps) *
                                     buffer.sampleRate));
    const from = Math.max(0, Math.round(t * buffer.sampleRate - w / 2));
    const to = Math.min(buffer.length, from + w);
    if (to <= from) return 0;
    let sum = 0;
    for (let c = 0; c < buffer.numberOfChannels; c++) {
      const d = buffer.getChannelData(c);
      for (let i = from; i < to; i++) sum += d[i] * d[i];
    }
    return Math.sqrt(sum / ((to - from) * buffer.numberOfChannels));
  },
  // The clip's soundtrack (an OfflineAudioContext the clip's length):
  // connect to valtz.audio.destination, schedule from 0.
  get audio() { return audio(); },
  get canvas() {
    if (!canvas) {
      canvas = document.createElement('canvas');
      canvas.width = P.width;
      canvas.height = P.height;
      canvas.style.cssText =
          'position:fixed;left:0;top:0;width:100%;height:100%;' +
          'display:block';
      (document.body || document.documentElement).appendChild(canvas);
    }
    return canvas;
  },
};
fixed(W, 'valtz', Object.freeze(api));
fixed(W, '__valtz_waited', () => Promise.all(waits));
// The page as it is: what a clear one held.
fixed(W, '__valtz_inspect', () => ({
  canvases: document.querySelectorAll('canvas').length,
  contexts: Object.keys(made).reduce((n, k) => n + made[k], 0),
  svgs: document.querySelectorAll('svg').length,
  elements: document.body ? document.body.querySelectorAll('*').length : 0,
  on_frame: onFrame.length,
  waiting: frames.size,
  valtz_canvas: !!canvas,
}));

// A frame: the clock at it, what was waiting for it run -- the frame
// callbacks requestAnimationFrame queued, valtz.onFrame's -- and the
// page's own animations held there.
fixed(W, '__valtz_step', async (i) => {
  now = i * 1000 / P.fps;
  try {
    for (const an of document.getAnimations()) {
      an.pause();
      an.currentTime = now;
    }
  } catch (e) {}
  for (const s of document.querySelectorAll('svg')) {
    try { s.pauseAnimations(); s.setCurrentTime(now / 1000); } catch (e) {}
  }
  // Its videos at the frame's time, first: what draws them reads them.
  const held = new Set([...videos, ...document.querySelectorAll('video')]);
  await Promise.all([...held].map((v) => holdVideo(v, now / 1000)
                                         .catch(() => {})));
  const due = [...frames.values()];
  frames.clear();
  for (const cb of due) {
    try { cb(now); } catch (e) { report(e); }
  }
  for (const fn of onFrame) {
    try { await fn(now / 1000, i); } catch (e) { report(e); }
  }
  await new Promise((r) => setTimeout(r, 0));
  return true;
});
})();
)JS";

}  // namespace valtz::procedural

#endif
