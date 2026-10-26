// SPDX-License-Identifier: GPL-3.0-only
// docs/install/fm1install.js on node: the package checks (on the real packages in DIR, from
// tools/site_packages.py), and the whole update against a fake FM-1 (after Felucca's web/test_web.mjs FakeFM1,
// by Leo Kuroshita, Hügelton Instruments, GPL-3.0).
//   node tests/install_test.mjs [DIR]
import { readFileSync, existsSync } from "node:fs";
import { checkPackage, packageDamage, logicalImage, productOf, installOn } from "../docs/install/fm1install.js";
import { pack7, unpack7 } from "../docs/install/fm1ota.js";

let failed = 0;
const ok = (c, what) => { console.log(`${c ? "ok  " : "FAIL"} ${what}`); if (!c) failed++; };
const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const reject = (p) => p.then(() => null, (e) => e);

// ------------------------------------------------------------------ packages ---
const dir = process.argv[2];
if (dir && existsSync(`${dir}/index.json`)) {
  const index = JSON.parse(readFileSync(`${dir}/index.json`, "utf8"));
  for (const [id, entry] of Object.entries(index.firmwares)) {
    const raw = new Uint8Array(readFileSync(`${dir}/${entry.file}`));
    const pkg = await checkPackage(raw, entry);
    ok(pkg.product === entry.product && pkg.image.length === raw.length - 20, `${id}: ${entry.file} passes, ${pkg.product}`);
    const bad = raw.slice();
    bad[raw.length - 4000] ^= 0x55;                        // a byte near the end (the update loader)
    ok(packageDamage(logicalImage(bad)) !== null, `${id}: a changed byte is caught (${packageDamage(logicalImage(bad))})`);
    const app = raw.slice();
    app[0x20000] ^= 0x01;                                  // one bit in the firmware's own file
    ok(packageDamage(logicalImage(app)) !== null, `${id}: one bit in the firmware is caught (${packageDamage(logicalImage(app))})`);
    const e = await reject(checkPackage(bad, entry));
    ok(e && e.code === "badpkg", `${id}: ... and refused (${e && e.message})`);
    const e2 = await reject(checkPackage(raw, { ...entry, product: "FM-1_999" }));
    ok(e2 && e2.code === "badpkg", `${id}: another identity than the list's is refused`);
  }
} else console.log("(no package dir: the package checks are skipped)");

// ------------------------------------------------------------------ the update ---
const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];

// an FM-1 on WebMIDI: identity, then "device asks, host answers" reads of the image; boots into the loader
// after step 1, into the new firmware (identity `product`) after step 2
class FakeFM1 {
  constructor(image, identity, product) {
    this.image = image; this.product = product; this.served = 0; this.bad = 0; this.sent = 0; this.upgrades = 0;
    this.access = { inputs: new Map(), outputs: new Map() };
    this.boot(identity, identity.startsWith("ota-") ? "Felucca Update" : "Felucca");
  }
  boot(identity, name) {
    this.identity = identity; this.waiting = null; this.queue = [];
    for (const m of [this.access.inputs, this.access.outputs]) { for (const p of m.values()) p.state = "disconnected"; m.clear(); }
    const id = Math.random().toString(36).slice(2);
    this.input = { id: "i" + id, name, state: "connected", onmidimessage: null, open: async () => {} };
    this.output = { id: "o" + id, name, state: "connected", open: async () => {}, send: (d) => {
      if (this.output.state !== "connected") throw new Error("InvalidStateError");
      this.sent++;
      setTimeout(() => this.rx(Array.from(d)), 1);
    } };
    this.access.inputs.set(this.input.id, this.input);
    this.access.outputs.set(this.output.id, this.output);
  }
  tx(bytes) { const i = this.input; setTimeout(() => { if (i.state === "connected" && i.onmidimessage) i.onmidimessage({ data: Uint8Array.from(bytes) }); }, 1); }
  rx(d) {
    if (eq(d, HS)) {
      const t = [...new TextEncoder().encode(this.identity)];
      this.tx([0xF0, ...pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]);
    } else if (eq(d, UPGRADE)) {
      this.upgrades++;
      this.queue = this.identity.startsWith("ota-")
        ? [...Array.from({ length: Math.ceil(this.image.length / 512) }, (_, k) => [k * 512, Math.min(512, this.image.length - k * 512)]), [0xF0000000, 8]]
        : [[0, 64], [0x40, 160], [0x1000, 512], [0xE0000000, 8]];
      this.next();
    } else if (this.waiting) {
      const u = unpack7(d.slice(1, -1));
      const [addr, len] = this.waiting;
      const got = u.slice(14, 14 + (addr >= 0xE0000000 ? 8 : len));
      const want = addr >= 0xE0000000 ? [...new TextEncoder().encode("success"), 0] : Array.from(this.image.subarray(addr, addr + len));
      if (!eq(got, want)) this.bad++;
      this.waiting = null;
      this.served++;
      if (addr === 0xE0000000) setTimeout(() => this.boot("ota-FM-1_900", "Felucca Update"), 300);
      else if (addr === 0xF0000000) setTimeout(() => this.boot(this.product, "Felucca"), 300);
      else this.next();
    }
  }
  next() {
    const r = this.queue.shift();
    if (!r) return;
    this.waiting = r;
    const [addr, len] = r;
    const u = [0, 0x59, 0x30, 0, 0, 0, 0, addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF, len & 0xFF, len >> 8, 0];
    let s = 0;
    for (let i = 6; i < 14; i++) s += u[i];
    u.push(~s & 0xFF);
    this.tx([0xF0, ...pack7(u), 0xF7]);
  }
}

const image = Uint8Array.from({ length: 0x2400 }, (_, i) => (i * 7) & 0xFF);
const pkg = { product: "FM-1_922", image };
const yes = async () => true, no = async () => false;

{
  const dev = new FakeFM1(image, "FM-1_921", "FM-1_922");
  const steps = [];
  const got = await installOn(dev.access, pkg, "FM-1 SID", yes, (k) => steps.push(k));
  ok(got === "FM-1_922" && dev.bad === 0 && steps.includes("write") && steps.at(-1) === "done",
    `install over a running firmware: check, loader, write, back as FM-1_922 (${dev.served} reads, all correct)`);
}
{
  const dev = new FakeFM1(image, "FM-1_921", "FM-1_922");
  const got = await installOn(dev.access, pkg, "FM-1 SID", no, () => {});
  ok(got === null && dev.upgrades === 0, "Cancel at the question: no update command is sent");
}
{
  const dev = new FakeFM1(image, "ota-FM-1_900", "FM-1_922");
  const got = await installOn(dev.access, pkg, "FM-1 SID", yes, () => {});
  ok(got === "FM-1_922" && dev.bad === 0, "an FM-1 left in update mode (ota-FM-1_900): the write is finished");
}
{
  const dev = new FakeFM1(image, "ota-FM-1_015", "FM-1_922");
  const e = await reject(installOn(dev.access, pkg, "FM-1 SID", yes, () => {}));
  ok(e && e.code === "foreign" && dev.upgrades === 0, "M-VAVE's update mode (ota-FM-1_015): refused, nothing sent");
}
{
  const dev = new FakeFM1(image, "FM-1_921", "FM-1_956");
  const e = await reject(installOn(dev.access, pkg, "FM-1 SID", yes, () => {}));
  ok(e && e.code === "mismatch", "back with another identity than the package's: reported (mismatch)");
}
{
  const e = await reject(installOn({ inputs: new Map(), outputs: new Map() }, pkg, "FM-1 SID", yes, () => {}));
  ok(e && e.code === "notfound", "no FM-1: notfound");
}
{
  const dev = new FakeFM1(image, "XY-9_100", "FM-1_922");
  const e = await reject(installOn(dev.access, pkg, "FM-1 SID", yes, () => {}));
  ok(e && e.code === "model", "another device than an FM-1: refused (model)");
}
ok(productOf(new Uint8Array(0x400).fill(0x7D)) === "", "productOf: no marker bytes, no identity");

console.log(failed ? `INSTALL TESTS FAILED (${failed})` : "install tests passed");
process.exit(failed ? 1 : 0);
