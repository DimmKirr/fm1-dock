// SPDX-License-Identifier: GPL-3.0-only
// "Install on my FM-1" for the pages of fm1-dock, FM-1 SID and Scintilla (all on joakimeriksson.github.io, so
// they share this file and the packages next to it). The packages are the latest GitHub releases, copied into
// this site's fw/ by .github/workflows/pages.yml (tools/site_packages.py) with fw/index.json: GitHub does not let
// a page download a release asset itself. The page checks its copy against the SHA-256 GitHub gave for the
// release asset, and the package itself as tools/fm1_install.py does (identity, update loader, every file's CRC).
// The update is Felucca's fm1ota.js (hugelton/Felucca 727f272, Felucca 1.0, unchanged), over WebMIDI:
// Chrome or Edge.
//
//   <div data-fm1-install="fm1-sid"></div>
//   <script type="module">
//     import { installButton } from "/fm1-dock/install/fm1install.js";
//     for (const el of document.querySelectorAll("[data-fm1-install]")) installButton(el, el.dataset.fm1Install);
//   </script>
import { Updater } from "./fm1ota.js";

const SITE = new URL("../", import.meta.url);           // fm1-dock's site: fw/ is there
const BLOCKS = 20, BLK = 0x30, KEEP = 0x2F;
const LOADER_MARK = "FELUCCA-LOADER-1";
const IS_OTA = (id) => /^ota-/i.test(id.model);
const OUR_LOADER = (id) => /^ota-FM-1_9\d\d$/i.test(id.text);   // Felucca's and fm1-dock's (both ota-FM-1_900)

// what the pages call them, and what each keeps over an install
export const FIRMWARES = {
  "fm1-dock": { name: "fm1-dock", keeps: "Your deployed modules, settings and presets stay." },
  "fm1-sid": { name: "FM-1 SID", keeps: "" },
  "scintilla": { name: "Scintilla", keeps: "" },
};

// ------------------------------------------------------------------ the package ---

// the package identity ("FM-1_921"): one marker byte after each of the first 20 blocks
export function productOf(raw) {
  let s = "";
  for (let i = 0; i < BLOCKS; i++) {
    const m = raw[i * BLK + KEEP];
    if (m !== 0x7D) s += String.fromCharCode((m - i - 1) & 0xFF);
  }
  return s;
}

// the image the FM-1 reads during an update: the package without the 20 marker bytes
export function logicalImage(raw) {
  const out = new Uint8Array(raw.length - BLOCKS);
  for (let i = 0; i < BLOCKS; i++) out.set(raw.subarray(i * BLK, i * BLK + KEEP), i * KEEP);
  out.set(raw.subarray(BLOCKS * BLK), BLOCKS * KEEP);
  return out;
}

function crc16(d, crc = 0) {                        // CRC-16/XMODEM, as the package and the FM-1 use
  for (const b of d) {
    crc ^= b << 8;
    for (let k = 0; k < 8; k++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
  }
  return crc;
}

function dec(b) {                                   // JieLi's ENC stream cipher (its own inverse)
  const out = Uint8Array.from(b);
  let key = 0xFFFF;
  for (let i = 0; i < out.length; i++) {
    out[i] ^= key & 0xFF;
    key = ((key << 1) ^ (key & 0x8000 ? 0x1021 : 0)) & 0xFFFF;
  }
  return out;
}

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

// what is wrong with the update image (header, file list, each file's CRC), or null: the FM-1 only checks the
// loader before it rewrites the firmware, so a damaged file is caught here (tools/fm1_install.py package_damage)
export function packageDamage(image) {
  if (image.length < 0x40) return "too short";
  const hdr = dec(image.subarray(0, 0x40));
  if (crc16(hdr.subarray(2, 0x40)) !== u16(hdr, 0)) return "header CRC";
  const count = u16(hdr, 8);
  if (count < 1 || count > 8 || image.length < 0x40 + 0x50 * count) return "file list";
  const list = image.subarray(0x40, 0x40 + 0x50 * count);
  if (crc16(list) !== u16(hdr, 2)) return "file list CRC";
  for (let i = 0; i < count; i++) {
    const e = dec(list.subarray(i * 0x50, (i + 1) * 0x50));
    const off = u32(e, 8), size = u32(e, 12);
    const name = new TextDecoder().decode(e.subarray(0x40, 0x50)).split("\0")[0];
    if (off + size > image.length) return `${name}: cut short`;
    if (crc16(image.subarray(off, off + size)) !== u16(e, 4)) return `${name}: CRC`;
  }
  return null;
}

async function sha256hex(bytes) {
  const h = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return [...h].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function hasLoader(raw) {
  const mark = new TextEncoder().encode(LOADER_MARK);
  outer: for (let i = 0; i + mark.length <= raw.length; i++) {
    for (let k = 0; k < mark.length; k++) if (raw[i + k] !== mark[k]) continue outer;
    return true;
  }
  return false;
}

function fail(code, msg) { const e = new Error(msg); e.code = code; return e; }

// a package's bytes, checked: -> { product, image }
export async function checkPackage(raw, entry) {
  if (entry.sha256 && await sha256hex(raw) !== entry.sha256)
    throw fail("badpkg", `${entry.file} is not the release's file (SHA-256 differs)`);
  if (raw.length < BLOCKS * BLK) throw fail("badpkg", `${entry.file}: too short`);
  const product = productOf(raw);
  if (!/^[^_]+_\d+$/.test(product) || (entry.product && product !== entry.product))
    throw fail("badpkg", `${entry.file}: identity ${product}`);
  if (!hasLoader(raw)) throw fail("badpkg", `${entry.file}: no update loader in it`);
  const image = logicalImage(raw);
  const bad = packageDamage(image);
  if (bad) throw fail("badpkg", `${entry.file}: ${bad}`);
  return { product, image };
}

// fw/index.json: { firmwares: { id: { name, version, tag, file, sha256, size, product, release } } }
let catalogue = null;
export function catalog() {
  catalogue ||= fetch(new URL("fw/index.json", SITE), { cache: "no-cache" }).then((r) => {
    if (!r.ok) throw new Error(`fw/index.json: HTTP ${r.status}`);
    return r.json();
  }).catch((e) => { catalogue = null; throw e; });
  return catalogue;
}

export async function loadPackage(entry) {
  const r = await fetch(new URL("fw/" + entry.file, SITE));
  if (!r.ok) throw fail("badpkg", `${entry.file}: HTTP ${r.status}`);
  return checkPackage(new Uint8Array(await r.arrayBuffer()), entry);
}

// ------------------------------------------------------------------ the install ---

// the whole update on the FM-1 found on midi: a running firmware is updated (ask(text) confirms it first), an
// FM-1 left in update mode by an install that was cut off is finished. step(kind, value) as fm1ota.js's.
export async function installOn(midi, pkg, name, ask, step) {
  const up = new Updater(midi);
  const dev = await up.find((id) => !IS_OTA(id));
  if (dev) {
    dev.link.close();
    if (!await ask(`Install ${name} on the FM-1? It runs ${dev.id.text} now.`)) return null;
    return up.install(pkg.image, pkg.product, step);
  }
  const ota = await up.find(IS_OTA);
  if (!ota) throw fail("notfound", "FM-1 not found");
  ota.link.close();
  if (!OUR_LOADER(ota.id))           // another firmware's update loader (M-VAVE's): its image layout may differ
    throw fail("foreign", ota.id.text);
  if (!await ask(`The FM-1 is in update mode: an install was cut off. Finish it with ${name}?`)) return null;
  if (!await up.resume(pkg.image, step, pkg.product)) throw fail("stopped", "");
  return pkg.product;
}

const SAY = {
  nomidi: "This browser cannot reach the FM-1: open the page in Chrome or Edge.",
  denied: "MIDI (SysEx) was not allowed. Allow MIDI in the site settings (the address bar) and press Install again.",
  notfound: "FM-1 not found. Plug it in with a USB data cable, close other apps that use MIDI, and press Install again.",
  model: "The connected device is not an FM-1.",
  foreign: "The FM-1 is in the update mode of another firmware: finish that update with its own installer, then press Install again.",
  stopped: "Stopped. Replug the USB cable and press Install again: the FM-1 stays in update mode until it is finished.",
  lost: "The USB connection was lost. Replug it and press Install again: the FM-1 stays in update mode until it is finished.",
  noloader: "The FM-1 in update mode was not found. Replug the USB cable and press Install again (it stays in update mode until it is finished).",
  noreturn: "The FM-1 did not come back. Switch it off and on.",
  mismatch: "Written, but the FM-1 reports another firmware: ",
  badpkg: "The package on this page failed its check, nothing was written. Reload the page. ",
  badreq: "The FM-1 asked for something outside the package; nothing more was sent. ",
  start: "Starting the update", verify: "The FM-1 checks the package", loader: "Switching to update mode",
  write: "Writing: do not unplug the FM-1", reboot: "Restarting",
};

// a button, a progress bar and a status line in el, for the firmware id (FIRMWARES); opts.buttonClass
export function installButton(el, id, opts = {}) {
  const fw = FIRMWARES[id];
  if (!fw) throw new Error(`fm1install: no firmware ${id}`);
  el.classList.add("fm1-install");
  el.innerHTML = "";
  const btn = Object.assign(document.createElement("button"), { type: "button", className: opts.buttonClass || "", disabled: true });
  btn.textContent = `Install ${fw.name} on my FM-1`;
  const bar = Object.assign(document.createElement("progress"), { max: 100, value: 0, hidden: true });
  const status = document.createElement("p");
  status.className = "fm1-install-status";
  status.setAttribute("role", "status");
  const note = document.createElement("p");
  note.className = "fm1-install-note";
  note.textContent = "Over USB, in Chrome or Edge. Do not unplug the FM-1 while it writes. " +
    (fw.keeps ? fw.keeps + " " : "") + "Installing firmware is at your own risk; OCT− held at power-on is the USB rescue.";
  el.append(btn, bar, status, note);
  const say = (text) => { status.textContent = text; };
  let entry = null, pkg = null, writing = false;

  if (!navigator.requestMIDIAccess) say(SAY.nomidi);
  catalog().then((c) => {
    entry = c.firmwares && c.firmwares[id];
    if (!entry) { say(`No ${fw.name} package on this site yet.`); return; }
    btn.textContent = `Install ${fw.name} ${entry.version} on my FM-1`;
    btn.disabled = !navigator.requestMIDIAccess;
  }).catch((e) => say(`Could not load the list of packages (${e.message}).`));

  addEventListener("beforeunload", (e) => { if (writing) { e.preventDefault(); e.returnValue = ""; } });
  btn.addEventListener("click", async () => {
    if (writing || !entry) return;
    writing = true; btn.disabled = true;
    try {
      pkg ||= await loadPackage(entry);
      let midi;
      try { midi = await navigator.requestMIDIAccess({ sysex: true }); } catch (_) { throw fail("denied", ""); }
      say("Looking for the FM-1 …");
      const reads = Math.ceil(pkg.image.length / 512);
      const step = (k, n) => {
        if (k === "verify") { bar.hidden = false; bar.value = Math.min(10, n / 8); }
        if (k === "write") { bar.hidden = false; bar.value = 10 + Math.min(89, (89 * n) / reads); }
        if (k === "done") { bar.value = 100; return; }
        if (SAY[k]) say(SAY[k] + (k === "write" ? ` (${Math.round(bar.value)} %)` : ""));
      };
      const got = await installOn(midi, pkg, `${fw.name} ${entry.version}`, async (q) => confirm(q), step);
      if (got === null) say("Cancelled: nothing was written.");
      else say(`Done: the FM-1 runs ${fw.name} ${entry.version} (${got}).`);
    } catch (e) {
      say(SAY[e.code] ? SAY[e.code] + (e.code === "mismatch" ? e.detail : e.code === "badpkg" || e.code === "badreq" ? e.message : "")
        : `Error: ${e.message}`);
      console.error(e);
    }
    writing = false; btn.disabled = false;
  });
}
