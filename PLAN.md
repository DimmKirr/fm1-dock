# fm1-harbor: plan

One firmware for the M-VAVE FM-1 that is installed once. Synths are modules sent over
USB-MIDI into flash slots; one runs at a time, as many are stored as fit. A broken module can
never take down the host or the USB rescue, and logs, CPU load and crashes are visible from the
computer.

Built on [Dinghy](https://github.com/zvenson/dinghy) 0.3 (GPL-3.0, its history kept, remote
`upstream`). Developed and tested first in [fm1-emu](https://github.com/joakimeriksson/fm1-emu),
then on hardware.

## What exists already (2026-10-20)

- **Dinghy 0.3** runs on the FM-1: USB-MIDI in/out, USB audio, the updater (M-UPGRADE over
  SysEx, staged at flash 0xE0000), USB rescue (OCT- at power-on, and by itself after two crashes
  at boot), a crash screen with the PC, an 8 s watchdog, settings in their own sector (0xFC000),
  host mode (`tests/run_host.sh`). Its loader (`firmware/loader`) writes only the app area.
- **Dinghy's flash driver** (`firmware/hal/fm1_flash.h`) already has what the slots need:
  - a configurable **plain (unencrypted) XIP window**, `fl_plain_window_init`: from flash
    0x93000 to the end, data and code are read as stored, no chip-key scrambling;
  - **cache invalidation** after a write, `fl_inval` (`flushinv` per 32-byte line);
  - a range guard, `FL_RANGE_OK`, for every erase and program; RAM-resident erase/program
    (`.ram_text`), interrupts off.
- **Memory** (`firmware/app.ld`): code from XIP 0x02000120 (up to 0x8DFBC bytes); RAM text
  0x01C00000 (24 KiB); RAM 96 KiB at 0x01C08000; pool 336 KiB at 0x01C20000; NOINIT
  0x01C7C000 (survives resets). Audio: `app_audio(int32_t *out, n = 256)` at 44.1 kHz.
- **Barge** ([tradiuz/fm1-barge](https://github.com/tradiuz/fm1-barge), GPL-3.0, one commit,
  not maintained): a Dinghy fork with statically linked instruments and a
  [plugin plan](https://github.com/tradiuz/fm1-barge/blob/main/docs/PLUGIN-ARCHITECTURE-PLAN.md)
  that leaves runtime loading open. Ideas reused with credit: the descriptor, the bounded event
  queue, switching at block boundaries, a fallback instrument, core 0 first.
- Nobody loads native code after the build on a real FM-1 yet.

## Design

| Topic | Decision | Why |
|---|---|---|
| Base | Dinghy 0.3; its loader, updater and rescue untouched in v1 | the rescue path is hardware-tested; our bugs must not reach it |
| Running | one module at a time; as many stored as fit | one real synth uses most of the CPU and the pool |
| Where modules run | in place from flash, in the plain window | no RAM for code, no scrambling |
| Placement | anywhere in the slot area: relocated when written (below) | slots of any size, gaps can be closed |
| Module RAM | one fixed region in the pool, shared by all modules | only one runs, so RAM addresses never need relocating |
| Host <-> module | tables only: the module's descriptor, the host's service table passed to `init` | no link-time dependency on the host build; the ABI version is checked |
| Hot code | an optional `.ram_text` in the module, copied to RAM on load | inner loops away from XIP cache misses |
| Audio | `app_audio` calls `module->render` | Dinghy's timing unchanged |
| Cores | core 0 only in v1 | some units' core 1 misreads RAM (fm1-emu `--flaky-core1`) |
| Commit of a deploy | the slot-table entry is written last; no valid entry = no module | a cut at any moment leaves a bootable host |
| Debug | a log ring and crash record in NOINIT, sent as SysEx | survives the reset; fm1-emu reads it directly |

### Relocation

Calls and branches inside a module are PC-relative (pi32v2's 23-bit call range). Absolute
addresses are pointers to the module's own constants and tables, each a 32-bit word (`c0 ff
<u32>` loads a whole word). The build links every module twice, at two flash bases, and compares:
each word that differs by exactly the distance between the bases is a relocation. The list ships
in the module file; whoever writes the module (the host, or the deploy tool) adds the slot's
offset to those words. Words that change by any other amount (an address split over two
instructions) fail the build until they are handled. No reliance on `-fPIC` in JieLi's
clang 4.0.1.

### Module file (v1, little endian)

    magic "HRBR" | abi | flags | name[24] | version
    code_size | ram_size (.data + .bss) | data_size | ram_text_size
    worst_cycles_per_block (measured in fm1-emu) | entry offsets (descriptor)
    n_relocs | relocs[] (offsets of words to add the slot base to)
    CRC32 over all of the above and the image
    image: .text + .rodata, then .data's initial values, then .ram_text

### Interface v1

- Descriptor: `abi`, name, `init(const host_t *)`, `release()`, `note(ch, key, vel)`,
  `cc(ch, num, val)`, `render(int32_t *out, uint32_t n)`, `ui(event)` (keys, buttons,
  encoders), `draw(canvas)`, parameter names and ranges for a generic host page.
- Service table (`host_t`): time, log, settings record per module, canvas drawing, LEDs,
  random, and the helpers a compiler may call (memcpy, memset, division) so a module links
  against nothing.

### Switching modules

Mute at a block boundary, `release()` the old one, copy the new one's `.data` and `.ram_text`,
clear `.bss`, `init()`, unmute. Notes are cut; flash is not touched. Each module's settings
record survives switching away and back.

### Flash

    0x00000-0x04000  boot chain (uboot.boot, isd_config.ini)   never written
    0x04000-...      host (Dinghy + harbor)                    written by updates only
    ...-0xE0000      slot table + module slot area             written by deploys only
    0xE0000-0xE5000  update staging (Dinghy's ota.c)           as Dinghy
    0xFC000          settings                                  as Dinghy

The plain window moves down from 0x93000 to just past the host (measured in P0). The deploy
path's `FL_RANGE_OK` allows the slot area and nothing else.

## Status

2026-10-21, tested in fm1-emu only (`tests/emu_test.py`, `tests/deploy_test.py`, headless fm1-live;
nothing on hardware):

- [x] **P0**: Docker Desktop (hung since Oct 9) restarted; the three SDK files fetched (gitee, hashes
      match); the package builds reproducibly from a clean `build/`; boots in fm1-emu (`--bin` and the
      `.fwsc`). The host is 59.8 KB of the 191.7 KB it may take (the slot table is at XIP 0x02030000).
- [~] **P1**: the log ring, the crash record, the strikes and the render stats are in NOINIT;
      `tools/harbor.py log` reads them from a RAM dump (fm1-live `FM1_DUMPRAM`), `harbor.py logs` /
      `modules` over USB-MIDI SysEx (LOG, HELLO). Not yet: the crash test (fm1-emu has no fault /
      reset model: it halts on a wild jump).
- [x] **P2**: interface v1 (`firmware/harbor/hb_module.h`), the double-link relocation (`tools/hbmod.py`;
      also C++ and libm / libc, static constructors refused), modules packed at build time into
      the app slot (scrambled with the host: no plain window needed yet). The sine from its slot is
      bit-identical to the built-in one, at two slot addresses.
- [~] **P3**: slot table, header / ABI / CRC / size checks, the picker (HOME), switching, the built-in
      sine as the fallback, per-module settings, OCT+ safe boot, two strikes skip a module. Tested:
      a bad CRC is refused. Not testable in fm1-emu yet: a crash or watchdog reset and the reboot after it.
- [~] **P4 deploy** (`firmware/src/harbor.c` "deploys", `tools/hbdeploy.py`, `harbor.py deploy`):
  - SysEx `F0 7D 48 42 cmd seq pack7(payload, CRC-32) F7`: HELLO, LIST, BEGIN / DATA (256 B) / COMMIT,
    ABORT, DELETE, SELECT, SAFE (the sine), LOG. Over USB each frame is answered; over the DIN jack
    (and a `.syx`) nothing comes back, the sender paces itself.
  - Flash: the deployed table at 0x93000 / 0x94000 (two copies, sequence number, header written last),
    modules at 0x95000-0xE0000 (300 KiB, 4 KiB aligned, in Dinghy's plain window, first fit). An update
    writes only below 0x93000, so deployed modules survive host updates (refused if the ABI changes).
  - The file is received whole into the module RAM (the running module stopped) and checked (CRC,
    header, relocations) before any erase; relocated to where it goes, written, read back through XIP
    and checked as at boot; then the table. A deployed module of the same name stands in for the
    image's. No room beside the old copy: the table without it goes first (a cut then loses that
    module, never the host). Not yet: compaction, the WebMIDI page.
  - Tested over DIN in fm1-emu (the package booted, `--flash`): a new module in, running, after a
    reboot, DELETE; a damaged chunk refused at COMMIT with no flash touched; a power cut half way
    through each of the 12 erases / programs of a first deploy and of a replacing one: the host
    boots and plays every time, the table lists the old module or the new, never a broken one;
    CS-1 (50 KB, 619 relocations) deployed over DIN in 20 s and playing. Not tested: USB (fm1-emu
    has no USB device model; the frames and replies go through Dinghy's SysEx path, as the updater's).
- **Brick safety** (beyond Dinghy's loader, rescue and boot guard, all unchanged):
  - the host's flash writes: 0x93000 up only (deploys, banks, Dinghy's staging) and its settings;
  - a crash or watchdog in a slot module does not count towards the boot guard (so it never lands in
    the USB rescue by itself), a crash of the built-in sine does (it is the host's own);
  - a module slower than real time for 64 blocks is stopped: the main loop, USB and the updater keep
    running; HELLO / SAFE / SELECT / DELETE reach a running harbor from the computer;
  - banks are 8 KiB by module name (`HB_BANK_MAX` 8176; CS-1's 4 KiB bank did not fit the old 4 KiB
    sector with its header, so its user presets were never saved).
- **The keys' force** (velocity and polyphonic pressure): Velo's sensing (Charles Vestal,
  `charlesvestal/fm1-velocity`, GPL-3.0; `firmware/src/sense.c` as CS-1 carries it) in the host's
  key scan, with the analog rows (PA7 / PA8, 20 keys; the top row has no converter) and the row
  drive taken onto Dinghy's scanner. Sensed keys play with their landing's velocity (KEY_DOWN's arg,
  note() for the others), held ones report HB_EV_KEY_PRESSURE; CS-1 gets the readings
  (`key_sense`) for its own pressure and sets the sensing from its KEYS pages (`sense_set`).
  The MASTER pot is converted in the scan too. fm1-emu models the pills (branch `fm1-key-pressure`,
  on `fm1-live-flash-file`): `ms:ID@VEL`, `ms:ID~VEL`, the mouse's height on a key. Fitted to CS-1's
  measurements: CS-1's own firmware in the emulator senses 8 / 19 / 50 / 80 / 101 for 1 / 20 / 64 /
  100 / 127. Not on hardware.
- Identity: CS-1's own package is also `FM-1_950`: harbor needs another before any release.
- [ ] P5 hardware: not started.
- **Interface v2** (ABI 2): a module may take the whole screen (HB_F_SCREEN) and the whole panel
  (HB_F_PANEL: every key and button as events, OCT and HOME too; HOME held 1.5 s alone opens the
  module list), raw MIDI (midi()), blit, text_w, and an 8 KiB flash bank of its own (by module
  name, 0xEA000-0xFC000, Felucca's USR area). `modules/lib/hb_fm1.h` gives a ported firmware its
  platform calls (fm1_in, fm1_input_edges, fm1_enc_take, midi_get, ...) on top of them.
- **Modules, with their own UIs**:
  - SID: FM-1 SID's synth mode as it is (`modules/sid/fm1sid/take.py` takes its main.c by line
    ranges, each change checked): its C64 screen, the step sequencer, the FX page; reSID 0.16
    only (one chip, as FM-1 SID), the own model gone; 15 % of a block. Not here: the PSID player.
  - CS-1: CS-1's own app (cs1.c unchanged but for its platform include, `cs1_platform.h`): its
    pages, sounds, user presets (in the bank), MIDI learn and clock; 8 voices on core 0, worst
    61 % of a block; the keys' velocity and pressure from the host's sensing. Not here: the second core.
  - SINE: Dinghy's, and the built-in fallback.
- **fm1-emu**: test with its current master. The binaries in `~/work/fm1-emu/target/release` were
  from 2026-10-12 and cannot run a pair move (`r5_r4 = r11_r10 #`) that master can;
  `tests/emu_test.py` checks every instruction with the emulator's `--check-decode` first.

## Phases

Each phase ends with a test that has to pass before the next starts.

**P0: Setup.** Repo from Dinghy's history; JieLi toolchain (`tools/get_toolchain.sh`, Docker
amd64 on macOS); build Dinghy; run it in host mode and in fm1-emu; measure the host's real size.
*Pass:* the build is reproducible (same bytes twice) and boots with sound in fm1-emu.

**P1: Debug channel.** `hlog()` into a NOINIT ring; a crash record (PC, registers, cause) from
the exception handler; stats (render cycles per block, worst, missed blocks); SysEx requests for
log / stats / crash; `harbor log` CLI. fm1-emu: `--log-ring` prints the ring live.
*Pass:* in fm1-emu, logs show, and a deliberate crash's record is there after the reboot.

**P2: Interface v1 and relocation, no loader yet.** Descriptor and service table; Dinghy's sine
synth moved into a separately linked module, placed in the image at package time; the
double-link relocation tool; a build check for undefined symbols and hidden helper calls.
*Pass:* audio bit-identical to stock Dinghy in an fm1-emu WAV, from two different slot addresses.

**P3: Slot table, loader, picker.** Slot table; header and CRC checks before any call; safe boot
(OCT+ at power-on skips modules; two crashes in a module disable it); the built-in sine as the
fallback; a module picker on the device; switching.
*Pass:* in fm1-emu, a valid module plays, a bad CRC falls back to the sine, a crashing module is
skipped on the next boot, switching between two modules works.

**P4: Deploy.** SysEx framing (7-bit packing, sequence numbers, CRC per chunk, ack/nak,
resume); erase and program inside the slot area only; verify through XIP; invalidate the cache;
table entry last; first-fit placement and compaction (moving modules down, relocated). The same
protocol on USB-MIDI and the DIN jack (fm1-emu has DIN, not USB). `harbor deploy mod.hrbr`
(Python) and a WebMIDI page.
*Pass:* end-to-end deploy in fm1-emu over DIN; a power-cut test, stopped at 200 random points,
always boots the host.

**P5: Hardware.** H1 logs over USB-MIDI; H2 code runs from the plain window; H3 a replaced module
runs the new code without a reboot; H4 erase/program timing and the audio pause; H5 USB rescue
and OCT- rescue still work. Have the RP2040 rescue dongle at hand.
*Pass:* all five on one unit. If H2 fails: the writer scrambles modules with the chip key.

**P6: Real synths.** CS-1 first (77 KiB, single core, GPL), then OberVave and FM-1 SID: the
engine lifted out of its platform onto the service table.
*Pass:* the CS-1 module and the CS-1 firmware give the same WAV from the same MIDI script in
fm1-emu.

**P7: Budgets and SDK.** fm1-emu measures each module's worst render cycles and size; the host
enforces the declared budget (after N overruns: mute and log); a module template repo, docs,
release 0.1.
*Pass:* someone outside the project builds and deploys a module from the docs alone.

**P8: fm1-emu integration.** Catalog entry; "Deploy module…" on the page and in the plugin
writes the emulated slot area.

## fm1-emu work (separate PRs there)

1. The plain-window registers (`SFCENC_CON`, `SFCENC_UNENC_L/H`): today the emulator assumes
   the flash past the image reads plain.
2. `flushinv`, and dropping JIT-compiled blocks of flash that was rewritten (a deploy into flash
   that ran code earlier in the same run would run the old code).
3. `--log-ring`; cycle counts per function (module budgets).
4. Later: a USB-MIDI device model (Dinghy's `usb.c` as the reference), which would also let it
   test updaters (and harbor's deploys over USB, with their replies).
5. Done on a branch (`fm1-live-flash-file`, a worktree, not pushed): `fm1-live --flash FILE` (the
   flash from FILE, back to it at the end) and `FM1_FLASH_CUT=N` (a power cut half way through the
   Nth erase / program), for `tests/deploy_test.py`. Until it is merged the tests need that build.

## Risks

| Risk | Mitigation |
|---|---|
| Code does not run from the plain window | H2 early; fallback: scramble with the chip key when writing |
| The compiler splits addresses or calls hidden helpers | the P2 relocation check and symbol check; helpers in the service table |
| Installing the host bricks a unit | Dinghy's loader and rescue unchanged; H5; the RP2040 dongle |
| Less flash than hoped | measured in P0; one 160 KiB module is enough for a CS-1-sized synth |
| Licence | the host is GPL-3.0, so modules are too in practice; said in the README |
