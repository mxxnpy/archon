// Builds the Archon browser extension for Chromium (Chrome, Edge, Brave, Opera)
// and Firefox into dist/<browser>/, plus a .zip of each for the stores.
//   node build.mjs            -> dist/chromium, dist/firefox (+ zips)
// No dependencies: icons are drawn here and written as PNG with zlib.
import { mkdirSync, rmSync, copyFileSync, writeFileSync, readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { deflateSync, crc32 } from "node:zlib";

const root = dirname(fileURLToPath(import.meta.url));
const pkg = JSON.parse(readFileSync(join(root, "package.json"), "utf8"));

// Fixed public key: an unpacked install always gets the same extension id
// (bneibifbhplfblkgnoglbifijppilaed), which the Archon native host allows.
// The private key is not kept anywhere; the stores sign their own builds.
const CHROMIUM_KEY =
  "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAwKYZUGwmTd0o+m9RXsPoUZOYJXq30NXeLtOs3MSEFluVtKRF45cDmMIbQNdw6cpfGd6V5HweVXqF42Ld5lj/is/kjyobpzvkVlh/9UmZmZSRVWXx4HcBzJzGvMweOGwQyr7EQl7smFTeSE0fKw+yoVvZU0WJrilbuO3V+/I5pZuIJ1Iq5QfmXvV0NUWyan43rHzPm2Vi7tNE5Db3Y+e1wUi6NkryJTT/CjQgtkLgPT2roeZyKbW8dxmLE9Xh6Vg0hqao3O06TlR65oC45AcjPT7NVM76LmvZ5eG2JdMBUOcFmKzKdbOnAGAhZ6bX5sywDnn8OGj8JRYSf0vsbB4DaQIDAQAB";
export const FIREFOX_ID = "tabs@archon.dev";

const icons = { 16: "icons/16.png", 32: "icons/32.png", 48: "icons/48.png", 128: "icons/128.png" };
const contentScripts = [
  // Page world first, so the volume hooks exist before the site's own scripts.
  { matches: ["<all_urls>"], js: ["page_volume.js"], run_at: "document_start", all_frames: true, match_about_blank: true, world: "MAIN" },
  { matches: ["<all_urls>"], js: ["bridge.js"], run_at: "document_start", all_frames: true, match_about_blank: true },
];
const base = {
  manifest_version: 3,
  name: "Archon — tab volume",
  short_name: "Archon",
  version: pkg.version,
  description: "Control the volume of each browser tab from your phone, with the Archon app.",
  icons,
  permissions: ["nativeMessaging", "tabs", "storage"],
  host_permissions: ["<all_urls>"],
  content_scripts: contentScripts,
  action: { default_title: "Archon", default_icon: icons },
};

const manifests = {
  chromium: { ...base, key: CHROMIUM_KEY, minimum_chrome_version: "111", background: { service_worker: "background.js" } },
  firefox: {
    ...base,
    background: { scripts: ["background.js"] },
    browser_specific_settings: { gecko: { id: FIREFOX_ID, strict_min_version: "128.0" } },
  },
};

// ---- icon: the tray ring (thin flat accent ring) on transparent ------------
function png(size) {
  const px = Buffer.alloc(size * (size * 4 + 1));
  const c = (size - 1) / 2;
  const r = size * 0.36, w = Math.max(1.2, size * 0.11);
  for (let y = 0; y < size; y++) {
    px[y * (size * 4 + 1)] = 0; // filter: none
    for (let x = 0; x < size; x++) {
      const d = Math.hypot(x - c, y - c);
      const ring = Math.max(0, 1 - Math.abs(d - r) / (w / 2)); // soft edge
      const a = Math.min(1, ring * 1.4);
      const o = y * (size * 4 + 1) + 1 + x * 4;
      px[o] = 0xf6; px[o + 1] = 0xe7; px[o + 2] = 0xa1; // accent #F6E7A1 (docs/design.md)
      px[o + 3] = Math.round(a * 255);
    }
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0); ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; ihdr[9] = 6; // 8-bit RGBA
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr), chunk("IDAT", deflateSync(px)), chunk("IEND", Buffer.alloc(0)),
  ]);
}

// ---- zip (stored + deflate, enough for the stores) --------------------------
function zip(files) {
  const local = [], central = [];
  let offset = 0;
  for (const [name, data] of files) {
    const comp = deflateSync(data, { level: 9 });
    const n = Buffer.from(name);
    const h = Buffer.alloc(30);
    h.writeUInt32LE(0x04034b50, 0); h.writeUInt16LE(20, 4); h.writeUInt16LE(0, 6); h.writeUInt16LE(8, 8);
    h.writeUInt32LE(0, 10); h.writeUInt32LE(crc32(data) >>> 0, 14);
    h.writeUInt32LE(comp.length, 18); h.writeUInt32LE(data.length, 22); h.writeUInt16LE(n.length, 26); h.writeUInt16LE(0, 28);
    local.push(h, n, comp);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(20, 4); ch.writeUInt16LE(20, 6); ch.writeUInt16LE(0, 8); ch.writeUInt16LE(8, 10);
    ch.writeUInt32LE(0, 12); ch.writeUInt32LE(crc32(data) >>> 0, 16); ch.writeUInt32LE(comp.length, 20); ch.writeUInt32LE(data.length, 24);
    ch.writeUInt16LE(n.length, 28); ch.writeUInt32LE(offset, 42);
    central.push(ch, n);
    offset += h.length + n.length + comp.length;
  }
  const cd = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(files.length, 8); end.writeUInt16LE(files.length, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(offset, 16);
  return Buffer.concat([...local, cd, end]);
}

const dist = join(root, "dist");
rmSync(dist, { recursive: true, force: true });
for (const [target, manifest] of Object.entries(manifests)) {
  const out = join(dist, target);
  mkdirSync(join(out, "icons"), { recursive: true });
  const files = [["manifest.json", Buffer.from(JSON.stringify(manifest, null, 2) + "\n")]];
  for (const f of ["background.js", "bridge.js", "page_volume.js"]) files.push([f, readFileSync(join(root, "src", f))]);
  for (const [size, path] of Object.entries(icons)) files.push([path, png(Number(size))]);
  for (const [name, data] of files) writeFileSync(join(out, name), data);
  writeFileSync(join(dist, `archon-tabs-${target}-${pkg.version}.zip`), zip(files));
  console.log(`dist/${target} (${files.length} files)`);
}
