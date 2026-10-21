/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1-harbor: one firmware for the FM-1, installed once; the synths are modules in flash slots
 * (firmware/harbor/hb_module.h). This file is the host, on Dinghy's platform (platform.c):
 *
 *   slots      the slot table (flash 0x34000) lists the modules; each is checked (header, ABI,
 *              CRC, sizes) before anything of it runs. The built-in sine is always there.
 *   switching  HOME opens the picker: SELECT / PRESETS move, HOME / SEL / PLAY load. A module is
 *              muted at a block boundary, released, its .data copied, .bss cleared, init().
 *   safety     OCT+ held at power-on starts the sine only. A module that crashes (or hangs into
 *              the watchdog) twice is skipped at boot until it is picked again. A module's crash
 *              does not count towards Dinghy's boot guard (the host is fine).
 *   debug      a log ring, the crash record and the render stats in NOINIT (they survive the
 *              reset): tools/harbor.py log reads them from a RAM dump.
 *   playing    the keys (OCT- / OCT+ the octave), USB-MIDI and the DIN MIDI jack go to the module;
 *              the other buttons and the encoders go to its ui().
 *   settings   one flash sector: the host's (the module, the octave) and a record per module; each
 *              module's bank (8 KiB, by its name) for its user presets.
 *   deploys    a module sent over SysEx (tools/harbor.py deploy) goes into flash of its own, never
 *              the host's: received whole and checked first, the table written last (see below).
 *   keys       the bottom two rows' force is sensed (Velo's sense.c, by way of CS-1, in the key
 *              scan): they play with the velocity they land at, report their pressure while held.
 *   overload   a module that renders slower than real time for 64 blocks in a row (0.4 s or more)
 *              is stopped (the sine plays): the main loop, so USB and the updater, keep running. */
#define APP_NAME "HARBOR"
#define CV_MAX (240u * 96u)
#define APP_FAULT_HOOK hb_fault
#define APP_EARLY_INIT hb_early_init
#define APP_FL_LO 0x93000u                      /* what the host may erase / program besides its settings: the */
#define APP_FL_HI 0xFC000u                      /* deployed modules, their table, the banks; never the host (below) */
#define APP_SYSEX hb_sysex
#include <stdint.h>
static void hb_analog(uint32_t col, uint32_t rowbit, uint32_t v, uint32_t driven);
static int hb_analog_drive(uint32_t col, uint32_t rowbit);
#define FM1_INPUT_ANALOG(col, rowbit, v, driven) hb_analog(col, rowbit, v, driven)
#define FM1_INPUT_DRIVE(col, rowbit) hb_analog_drive(col, rowbit)
#include "platform.c"
#include "fm1_uart.h"
#include "hb_module.h"
#include "sense.c"

#define C_ACC RGB(180, 140, 255)                /* lilac, as the logo */
#define C_OFF RGB(90, 90, 90)
#define C_BAD RGB(255, 90, 70)
#define KEY0 53u                                /* key 0 = F3 */

#define HB_TABLE_AT 0x34000u                    /* flash offset of the image's slot table (XIP 0x02030000) */
#define HB_AREA_LO  0x35000u                    /* the image's modules, packed by tools/build.py */
#define HB_AREA_HI  0x93000u                    /* (the end of what an update writes) */
#define DT_A        0x93000u                    /* the deployed table, two copies (deploys, below) */
#define DT_B        0x94000u
#define DP_LO       0x95000u                    /* the deployed modules, in the plain window */
#define DP_HI       0xE0000u
#define BANK_LO     0xEA000u                    /* the modules' banks, 8 KiB each (Felucca's USR area) */
#define BANK_HI     0xFC000u
#define FL_OF(xip)  ((uint32_t)(xip) - FM1_XIP(0u))             /* XIP address -> flash offset */
#define BLOCK_TICKS (HALF_FRAMES * 1000000u / FS * FM1_TICKS_PER_US)   /* one block's time: 139 296 */

/* ---- what survives a reset: the log, the last crash, which module ran, its strikes */
#define NV_MAGIC 0x564E4248u                    /* "HBNV" */
#define LOG_SIZE 2048u
typedef struct {
    uint32_t magic;
    uint32_t log_w;                             /* bytes written: the ring position is log_w % LOG_SIZE */
    char log[LOG_SIZE];
    struct { uint32_t magic, vec, pc, rets, module, ms; } crash;
    uint32_t active;                            /* 1 + the module running, 0: none */
    struct { uint32_t module, last, worst, blocks, overruns; } stats;   /* render ticks per block */
    uint8_t strikes[HB_SLOTS + 1];
} hb_nv_t;
hb_nv_t hb_nv __attribute__((section(".noinit")));
#define CRASH_MAGIC 0x48535243u                 /* "CRSH" */

static void nv_init(void)
{
    if (hb_nv.magic == NV_MAGIC)
        return;
    memset(&hb_nv, 0, sizeof hb_nv);
    hb_nv.magic = NV_MAGIC;
}

static void log_put(const char *s)
{
    while (*s)
        hb_nv.log[hb_nv.log_w++ % LOG_SIZE] = *s++;
}
static void hb_log_u32(const char *s, uint32_t v)   /* "<ms> s v\n"; v in hex unless ~0 */
{
    char b[12];
    uint32_t i, t = fm1_ms, n = 0;
    do { b[n++] = (char)('0' + t % 10u); t /= 10u; } while (t);
    for (i = 0; i < n / 2u; i++) { char c = b[i]; b[i] = b[n - 1u - i]; b[n - 1u - i] = c; }
    b[n++] = ' ';
    b[n] = 0;
    log_put(b);
    log_put(s);
    if (v != ~0u) {
        for (i = 0; i < 8u; i++) b[1u + i] = "0123456789abcdef"[(v >> (28u - 4u * i)) & 15u];
        b[0] = ' ';
        b[9] = 0;
        log_put(b);
    }
    log_put("\n");
}
static void hb_log(const char *s) { hb_log_u32(s, ~0u); }

/* ---- CRC-32 (zlib's), in pieces */
static uint32_t crc_upd(uint32_t c, const uint8_t *p, uint32_t n)
{
    uint32_t k;
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

/* ---- the modules: 0 is the built-in sine, then the slot table's */
extern const hb_module_t hb_builtin_sine;       /* modules/sine/sine.c, linked into the host */
enum { M_OK, M_BAD_CRC, M_BAD_HEAD, M_TOO_BIG, M_STRUCK, M_INIT_FAILED, M_SLOW };
static const char *const M_STATE[] = {"", "BAD CRC", "BAD HEADER", "TOO BIG", "CRASHED", "INIT FAILED", "TOO SLOW"};
enum { K_BUILTIN, K_IMAGE, K_DEPLOYED };
typedef struct {
    const hb_file_t *file;                      /* 0: built in */
    const hb_module_t *desc;
    uint32_t key;                               /* crc of the name: its settings record, its bank */
    uint8_t state, kind;
    char name[24];
} hb_entry_t;
static hb_entry_t mods[HB_SLOTS + 1];
static uint32_t n_mods, cur;
static const hb_module_t *volatile hb_active;  /* what the audio ISR renders (0: silence) */
static volatile uint32_t hb_blocks, hb_over, hb_slow;   /* blocks; overruns in a row; stopped for them */

static uint32_t name_key(const char *s)
{
    uint32_t n = 0;
    while (n < 23u && s[n]) n++;
    return crc_upd(0, (const uint8_t *)s, n);
}

static uint32_t check_module(const hb_slot_t *sl, uint32_t lo, uint32_t hi)   /* in flash [lo, hi) */
{
    const hb_file_t *f = (const hb_file_t *)sl->at;
    uint32_t io, img, c, head = (uint32_t)((const uint8_t *)&f->crc - (const uint8_t *)f);
    if (sl->at < FM1_XIP(lo) || sl->at >= FM1_XIP(hi) || sl->size < sizeof *f || sl->size > FM1_XIP(hi) - sl->at)
        return M_BAD_HEAD;
    if (f->magic != HB_MAGIC || f->abi != HB_ABI || f->crc != sl->crc || f->n_relocs > 65536u)
        return M_BAD_HEAD;
    io = (sizeof *f + 4u * f->n_relocs + 255u) & ~255u;
    img = f->code_size + f->data_size + f->rt_size;
    if (f->base != sl->at + io || f->code_size > sl->size || img > sl->size - io ||
        f->desc + sizeof(hb_module_t) > f->code_size || (f->desc & 3u))
        return M_BAD_HEAD;
    if (f->data_size + f->bss_size > HB_MOD_RAM_SIZE || f->rt_size > HB_MOD_RT_SIZE)
        return M_TOO_BIG;
    c = crc_upd(0, (const uint8_t *)f, head);
    c = crc_upd(c, (const uint8_t *)(f + 1), 4u * f->n_relocs);
    c = crc_upd(c, (const uint8_t *)f->base, img);
    if (c != f->crc)
        return M_BAD_CRC;
    if (((const hb_module_t *)(f->base + f->desc))->abi != HB_ABI)
        return M_BAD_HEAD;
    return M_OK;
}

/* one slot into the list; a deployed module that is fine stands in for the image's of the same name */
static void add_slot(const hb_slot_t *sl, uint32_t kind, uint32_t lo, uint32_t hi)
{
    hb_entry_t e;
    const hb_file_t *f = (const hb_file_t *)sl->at;
    uint32_t i, at = n_mods;
    e.state = (uint8_t)check_module(sl, lo, hi);
    e.kind = (uint8_t)kind;
    e.file = f;
    e.desc = e.state == M_OK ? (const hb_module_t *)(f->base + f->desc) : 0;
    str_cpy(e.name, e.state != M_BAD_HEAD ? f->name : "SLOT ?", sizeof e.name);
    e.key = name_key(e.name);
    if (kind == K_DEPLOYED && e.state == M_OK)
        for (i = 1; i < n_mods; i++)
            if (mods[i].kind == K_IMAGE && mods[i].key == e.key) { at = i; break; }
    if (at == n_mods) {
        if (n_mods > HB_SLOTS)
            return;
        n_mods++;
    }
    mods[at] = e;
    if (e.state == M_OK && hb_nv.strikes[at] >= 2u)
        mods[at].state = M_STRUCK;
    hb_log_u32(e.state == M_OK ? "slot ok" : "slot refused", kind << 16 | at << 8 | e.state);
    hb_log(e.name);
}

static void dt_load(void);
static struct { uint32_t magic, seq, n, crc; hb_slot_t s[HB_SLOTS]; } dt;   /* the deployed table in force */

static void scan_slots(void)
{
    const hb_slot_t *t = (const hb_slot_t *)FM1_XIP(HB_TABLE_AT);
    uint32_t i;
    mods[0].file = 0;
    mods[0].desc = &hb_builtin_sine;
    mods[0].state = M_OK;
    mods[0].kind = K_BUILTIN;
    str_cpy(mods[0].name, hb_builtin_sine.name, sizeof mods[0].name);
    mods[0].key = name_key("(built in)");       /* its own settings, apart from a slot of the same name */
    n_mods = 1;
    for (i = 0; i < HB_SLOTS && t[i].magic == HB_SLOT_MAGIC; i++)
        add_slot(&t[i], K_IMAGE, HB_AREA_LO, HB_AREA_HI);
    dt_load();
    for (i = 0; i < dt.n; i++)
        add_slot(&dt.s[i], K_DEPLOYED, DP_LO, DP_HI);
}

/* ---- settings: the host's record and one per module, in one sector; flushed when nothing plays */
#define ST_MAGIC 0x53425248u                    /* "HRBS" */
#define ST_RECS 8u
typedef struct { uint32_t key, len; uint8_t d[240]; } st_rec_t;
typedef struct {
    uint32_t magic, len, crc;                   /* crc over what follows the header */
    uint32_t sel_key;                           /* the module to start */
    int32_t oct;
    uint32_t spare[3];
    st_rec_t rec[ST_RECS];
} st_t;
static st_t st;
static uint32_t st_dirty, st_changed;

static void store_load(void)
{
    uint32_t h[3];
    memset(&st, 0, sizeof st);
    if (flash_ok && !st_read(SETTINGS_AT, h, sizeof h) && h[0] == ST_MAGIC && h[1] == sizeof st &&
        !st_read(SETTINGS_AT, &st, sizeof st) &&
        crc_upd(0, (const uint8_t *)&st + 12, sizeof st - 12u) == st.crc)
        return;
    memset(&st, 0, sizeof st);
}
static void store_flush(void)
{
    uint32_t took;
    st.magic = ST_MAGIC;
    st.len = sizeof st;
    st.crc = crc_upd(0, (const uint8_t *)&st + 12, sizeof st - 12u);
    st_dirty = 0;
    if (!flash_ok || fl_erase4k(SETTINGS_AT, &took))
        return;
    if (!fl_write(SETTINGS_AT + 12u, (const uint8_t *)&st + 12, sizeof st - 12u))
        fl_write(SETTINGS_AT, (const uint8_t *)&st, 12u);   /* the header last: a cut save stays invalid */
    hb_log("settings saved");
}
static void store_touch(void) { st_dirty = 1; st_changed = fm1_ms; }

static int mod_load(void *p, uint32_t n)
{
    uint32_t i, k = mods[cur].key;
    for (i = 0; i < ST_RECS; i++)
        if (st.rec[i].key == k && st.rec[i].len == n && n <= sizeof st.rec[i].d) {
            memcpy(p, st.rec[i].d, n);
            return 0;
        }
    return -1;
}
static void mod_save(const void *p, uint32_t n)
{
    uint32_t i, k = mods[cur].key, pick = ST_RECS;
    if (n > sizeof st.rec[0].d)
        return;
    for (i = 0; i < ST_RECS; i++)
        if (st.rec[i].key == k) { pick = i; break; }
    if (pick == ST_RECS)                        /* a free record, else the first that is not a module's here */
        for (i = 0; i < ST_RECS && pick == ST_RECS; i++) {
            uint32_t m, used = 0;
            for (m = 0; m < n_mods; m++)
                used |= st.rec[i].key == mods[m].key;
            if (!st.rec[i].key || !used) pick = i;
        }
    if (pick == ST_RECS)
        return;
    if (st.rec[pick].key == k && st.rec[pick].len == n && !memcmp(st.rec[pick].d, p, n))
        return;
    st.rec[pick].key = k;
    st.rec[pick].len = n;
    memcpy(st.rec[pick].d, p, n);
    store_touch();
}

/* ---- the keys' force: Velo's sensing (charlesvestal/fm1-velocity; sense.c as CS-1 carries it) on the
 * key scan's analog rows (hal/fm1_input.h).
 * The host's settings are CS-1's defaults at DRIVE 2; a module may set its own while it runs. */
#define SENSE_DRIVE 2u
static uint32_t sense_mask;                     /* the keys on the analog rows (PA7 / PA8): 0..19 */
static void hb_analog(uint32_t col, uint32_t rowbit, uint32_t v, uint32_t driven)   /* (the scan's ISR) */
{
    int id = FM1_KEYMAP[rowbit][col];
    if (id >= 14)
        sense_sample((uint32_t)id - 14u, v, fm1_ticks(), (int)driven);
}
static int hb_analog_drive(uint32_t col, uint32_t rowbit)
{
    int id = FM1_KEYMAP[rowbit][col];
    return id >= 14 && sense_want_drive((uint32_t)id - 14u);
}
static void sense_cfg_set(uint32_t on, uint32_t drive, uint32_t curve, uint32_t ceil)   /* main loop */
{
    drive = drive > 4u ? 4u : drive;
    sn_cfg.d_curve = (uint16_t)curve;
    sn_cfg.d_ceil = (uint16_t)ceil;
    if (drive != sn_cfg.drive) {
        sn_cfg.drive = (uint8_t)drive;
        sense_cal_reset();                      /* (the driven floor depends on it) */
    }
    fm1_an.drive = (uint8_t)drive;
    fm1_an.con = on ? (uint16_t)FM1_ADC_CON(7, 2) : 0u;   /* (CS-1's: divider 7, delay 2, as measured) */
}
static void sense_defaults(void) { sense_cfg_set(1, SENSE_DRIVE, 70, 1000); }
static uint32_t h_sense_keys(void) { return fm1_an.con ? sense_mask : 0; }
static int h_key_sense(uint32_t k, hb_key_sense_t *o)
{
    if (k >= 27u || !(h_sense_keys() >> k & 1u))
        return -1;
    o->state = sn_key[k].st;
    o->vel = sn_key[k].vel;
    o->v = sn_key[k].v;
    o->d = sn_key[k].d;
    o->dland = sn_key[k].dland;
    return 0;
}

/* ---- the service table */
static uint32_t h_ms(void) { return fm1_ms; }
static uint32_t h_ticks(void) { return fm1_ticks(); }
static void h_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 240) w = 240 - x;
    if (y + h > 240) h = 240 - y;
    if (w > 0 && h > 0)
        lcd_fill((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, c);
}
static void h_text(int32_t x, int32_t y, int32_t w, uint32_t big, const char *s, uint16_t fg, uint32_t center)
{
    const felucca_font_t *f = big ? &FONT_L : &FONT_S;
    if (x < 0 || y < 0 || w <= 0 || x + w > 240 || y + (int32_t)f->h > 240)
        return;
    draw_text_box((uint32_t)x, (uint32_t)y, (uint32_t)w, f, s, fg, center ? 1 : 0);
}
static void h_cv_begin(int32_t w, int32_t h, uint16_t bg)
{
    if (w < 1) w = 1;
    if (w > 240) w = 240;
    if (h < 1) h = 1;
    if ((uint32_t)(w * h) > CV_MAX) h = (int32_t)(CV_MAX / (uint32_t)w);
    cv_begin((uint32_t)w, (uint32_t)h, bg);
}
static void h_cv_pixel(int32_t x, int32_t y, uint16_t c) { cv_pset(x, y, c); }
static void h_cv_text(int32_t x, int32_t y, uint32_t big, const char *s, uint16_t fg) { cv_text(x, y, big ? &FONT_L : &FONT_S, s, fg); }
static void h_cv_blit(int32_t x, int32_t y)
{
    if (x >= 0 && y >= 0 && x + (int32_t)cv_w <= 240 && y + (int32_t)cv_h <= 240)
        cv_blit((uint32_t)x, (uint32_t)y);
}
static void h_led(uint32_t id, uint32_t on) { if (id < 41u) fm1_led_key(id, (int)on); }
static uint32_t h_rand(void) { return rng(); }
static void h_midi_send(uint32_t st_, uint32_t d1, uint32_t d2) { midi_send(st_, d1, d2); }
static uint32_t h_master(void) { return (uint32_t)master_q12; }
static void h_blit(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *px)
{
    int32_t r, k;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > 240 || y + h > 240)
        return;
    for (r = 0; r < h; r += k) {                /* in strips of at most 11 520 bytes a DMA, as FM-1 SID does */
        k = h - r < 5760 / w ? h - r : 5760 / w;
        lcd_blit((uint32_t)x, (uint32_t)(y + r), (uint32_t)w, (uint32_t)k, px + r * w);
        lcd_sync();
    }
}
static int32_t h_text_w(uint32_t big, const char *s) { return text_w(big ? &FONT_L : &FONT_S, s); }
/* a module's bank: 8 KiB of the bank area, found by the module's name: [magic][key][length][crc] then
 * the bytes. A save takes the module's own bank, else a free one, else one no module here owns. */
#define BANK_MAGIC 0x324B4248u                  /* "HBK2" */
#define BANK_SIZE  0x2000u
static uint32_t bank_find(uint32_t key, uint32_t h[4])   /* its flash offset (h: its header), 0: none */
{
    uint32_t at;
    for (at = BANK_LO; at + BANK_SIZE <= BANK_HI; at += BANK_SIZE)
        if (!st_read(at, h, 16u) && h[0] == BANK_MAGIC && h[1] == key)
            return at;
    return 0;
}
static int in_mod_ram(const void *p, uint32_t n)   /* (the flash driver reads its source with the flash off) */
{
    uint32_t a = (uint32_t)p;
    return a >= HB_MOD_RAM && n <= HB_MOD_RAM_SIZE && a - HB_MOD_RAM <= HB_MOD_RAM_SIZE - n;
}
static int h_bank_load(void *p, uint32_t n)
{
    uint32_t h[4], at;
    if (!flash_ok || n > HB_BANK_MAX || !in_mod_ram(p, n) || !(at = bank_find(mods[cur].key, h)) || h[2] != n ||
        st_read(at + 16u, p, n))
        return -1;
    if (crc_upd(0, (const uint8_t *)p, n) != h[3]) {
        memset(p, 0, n);
        return -1;
    }
    hb_log_u32("bank loaded", cur);
    return 0;
}
static int h_bank_save(const void *p, uint32_t n)
{
    uint32_t h[4] = {BANK_MAGIC, mods[cur].key, n, 0}, g[4], at, b, m, spare = 0, took;
    int rc;
    if (!flash_ok || n > HB_BANK_MAX || !in_mod_ram(p, n))
        return -1;
    at = bank_find(h[1], g);
    for (b = BANK_LO; !at && b + BANK_SIZE <= BANK_HI; b += BANK_SIZE) {
        uint32_t owned = 0;
        if (st_read(b, g, 16u))
            continue;
        if (g[0] != BANK_MAGIC) {
            at = b;
            break;
        }
        for (m = 0; m < n_mods; m++)
            owned |= g[1] == mods[m].key;
        if (!owned && !spare)
            spare = b;
    }
    if (!at && !(at = spare))
        return -1;
    h[3] = crc_upd(0, (const uint8_t *)p, n);
    if ((rc = fl_erase4k(at, &took)) != 0 || (rc = fl_erase4k(at + 0x1000u, &took)) != 0 ||
        (rc = fl_write(at + 16u, (const uint8_t *)p, n)) != 0)
        return rc;
    hb_log_u32("bank saved", cur);
    return fl_write(at, (const uint8_t *)h, sizeof h);   /* the header last */
}

static const hb_host_t HOST = {
    HB_ABI, sizeof(hb_host_t), h_ms, h_ticks, hb_log, hb_log_u32,
    h_fill, h_text, h_cv_begin, cv_rect, cv_line, h_cv_pixel, h_cv_text, h_cv_blit,
    h_led, h_rand, mod_load, mod_save, h_midi_send, h_master, h_blit, h_text_w, h_bank_load, h_bank_save,
    h_sense_keys, h_key_sense, sense_cfg_set,
};

/* ---- the sound: the module renders, the host measures */
static void app_audio(int32_t *out, uint32_t n)
{
    const hb_module_t *m = hb_active;
    if (m) {
        uint32_t t0 = fm1_ticks(), dt;
        m->render(out, n);
        dt = fm1_ticks() - t0;
        hb_nv.stats.last = dt;
        if (dt > hb_nv.stats.worst) hb_nv.stats.worst = dt;
        if (dt <= BLOCK_TICKS)
            hb_over = 0;
        else if (hb_nv.stats.overruns++, ++hb_over >= 64u) {
            hb_active = 0;                      /* slower than real time 64 blocks in a row: stopped, the main loop runs again */
            hb_slow = 1;
        }
    }
    hb_nv.stats.blocks++;
    hb_blocks++;
}

/* ---- switching */
static void stop_module(void)
{
    const hb_module_t *m = hb_active;
    uint32_t b = hb_blocks, t0 = fm1_ms;
    hb_active = 0;                              /* the ISR renders silence from the next block */
    while (hb_blocks == b && fm1_ms - t0 < 50u) /* the block that may be running ends */
        fm1_wdt_feed();
    if (m && m->release)
        m->release();
    hb_nv.active = 0;
}

static int start_module(uint32_t i)
{
    hb_entry_t *e = &mods[i];
    const hb_file_t *f = e->file;
    stop_module();
    if (e->state != M_OK)
        return -1;
    if (f) {
        const uint8_t *img = (const uint8_t *)f->base;
        memcpy((void *)HB_MOD_RAM, img + f->code_size, f->data_size);
        memset((uint8_t *)HB_MOD_RAM + f->data_size, 0, f->bss_size);
        memcpy((void *)HB_MOD_RT, img + f->code_size + f->data_size, f->rt_size);
    }
    cur = i;
    hb_over = 0;
    sense_defaults();                           /* (what the module before set is gone) */
    hb_nv.active = i + 1u;                      /* a crash in init counts as the module's */
    hb_nv.stats.module = i;
    hb_nv.stats.worst = hb_nv.stats.overruns = 0;
    if (e->desc->init(&HOST)) {
        hb_log_u32("init failed", i);
        hb_nv.active = 0;
        e->state = M_INIT_FAILED;
        return -1;                              /* (the caller starts the sine) */
    }
    hb_log_u32("started", i);
    hb_log(e->name);
    hb_active = e->desc;
    return 0;
}

/* ---- faults: the record, a strike for the module, and the boot guard left alone */
static void hb_fault(const fm1_crash_t *c)
{
    nv_init();
    hb_nv.crash.magic = CRASH_MAGIC;
    hb_nv.crash.vec = c->vec;
    hb_nv.crash.pc = c->pc;
    hb_nv.crash.rets = c->rets;
    hb_nv.crash.module = hb_nv.active;
    hb_nv.crash.ms = fm1_ms;
    hb_log_u32("FAULT pc", c->pc);
    if (hb_nv.active && hb_nv.active <= HB_SLOTS + 1u) {
        if (hb_nv.strikes[hb_nv.active - 1u] < 255u) hb_nv.strikes[hb_nv.active - 1u]++;
        hb_log_u32("in module", hb_nv.active - 1u);
        if (hb_nv.active > 1u)
            bootguard_clear(&bootguard);        /* a slot module's crash: the host itself boots (the sine's
                                                 * counts: it is the host's fallback) */
        hb_nv.active = 0;
    }
}

/* ---- MIDI from the DIN jack: UART1 DMAs into a ring filled with 0xFF (a byte MIDI never sends
 * here: 0xFF is a reset), read by content (hal/fm1_uart.h) */
#define DIN_RING 256u
static volatile uint8_t din_ring[DIN_RING] __attribute__((aligned(16)));
static uint32_t din_r;
static void hb_early_init(void)
{
    uint32_t i;
    for (i = 0; i < DIN_RING; i++)
        din_ring[i] = 0xFFu;
    fm1_uart1_midi_init(din_ring, DIN_RING);
}

static struct { uint8_t st, n, d[2]; } din;      /* the parser: running status, data bytes so far */
#define DF_MAX 640u                             /* a SysEx frame (the 7-bit bytes between F0 and F7), as usb.c's */
static uint8_t df_usb[DF_MAX], df_din[DF_MAX];
static uint32_t df_usb_n, df_din_n, din_sx;    /* a frame waiting: its length; din_sx: 1 + bytes so far */
static int din_get(uint8_t *st_, uint8_t *d1, uint8_t *d2)
{
    for (;;) {
        uint32_t b;
        if (df_din_n)
            return 0;                           /* a SysEx frame waits for the main loop (deploys) */
        b = din_ring[din_r];
        if (b == 0xFFu)
            return 0;
        din_ring[din_r] = 0xFFu;
        din_r = (din_r + 1u) % DIN_RING;
        if (b >= 0xF8u)
            continue;                           /* real time: ignored */
        if (b == 0xF7u && din_sx > 1u)
            df_din_n = din_sx - 1u;
        if (din_sx && b < 0x80u) {
            if (din_sx <= DF_MAX)
                df_din[din_sx++ - 1u] = (uint8_t)b;
            else
                din_sx = 0;                     /* too long: not ours */
            continue;
        }
        if (b & 0x80u) {
            din_sx = b == 0xF0u;                /* SysEx: kept for the deploys */
            din.st = b < 0xF0u ? (uint8_t)b : 0;    /* system common / SysEx: end running status */
            din.n = 0;
            continue;
        }
        if (!din.st)
            continue;
        din.d[din.n++] = (uint8_t)b;
        if (din.n == ((din.st & 0xE0u) == 0xC0u ? 1u : 2u)) {   /* Cn / Dn take one data byte */
            *st_ = din.st;
            *d1 = din.d[0];
            *d2 = din.n > 1u ? din.d[1] : 0;
            din.n = 0;
            return 1;
        }
    }
}

/* ---- the screen: the host's line at the top, the picker */
static void draw_top(int32_t oct)
{
    char s[12] = "OCT +0", c[12] = "CPU  0%";
    uint32_t pct = hb_nv.stats.last * 100u / BLOCK_TICKS;
    if (pct > 99u) pct = 99u;
    s[4] = oct < 0 ? '-' : '+';
    s[5] = (char)('0' + (oct < 0 ? -oct : oct));
    c[4] = pct >= 10u ? (char)('0' + pct / 10u) : ' ';
    c[5] = (char)('0' + pct % 10u);
    cv_begin(240, 20, C_BLACK);
    cv_text(4, 2, &FONT_S, mods[cur].name, C_ACC);
    cv_text(236 - text_w(&FONT_S, s) - 12 - text_w(&FONT_S, c), 2, &FONT_S, c, C_OFF);
    cv_text(236 - text_w(&FONT_S, s), 2, &FONT_S, s, C_OFF);
    cv_rect(0, 19, 240, 1, C_OFF);
    cv_blit(0, 0);
}

static void draw_picker(uint32_t sel)
{
    uint32_t i, first = sel > 5u ? sel - 5u : 0;
    lcd_fill(0, HB_TOP, 240, 240 - HB_TOP, C_BLACK);
    draw_text_box(0, 30, 240, &FONT_S, "MODULES", C_OFF, 1);
    for (i = first; i < n_mods && i < first + 6u; i++) {
        hb_entry_t *e = &mods[i];
        char sz[12] = "BUILT IN";
        uint16_t col = e->state != M_OK ? C_BAD : i == sel ? C_ACC : C_WHITE;
        if (e->file) {
            uint32_t kb = (e->file->code_size + e->file->data_size + 1023u) / 1024u, n = 0, t = kb;
            char d[6];
            do { d[n++] = (char)('0' + t % 10u); t /= 10u; } while (t);
            for (t = 0; t < n; t++) sz[t] = d[n - 1u - t];
            sz[n] = ' '; sz[n + 1u] = 'K'; sz[n + 2u] = 'B'; sz[n + 3u] = 0;
        }
        cv_begin(240, 28, i == sel ? RGB(40, 30, 60) : C_BLACK);
        cv_text(12, 6, &FONT_S, e->name, col);
        cv_text(150, 6, &FONT_S, e->state != M_OK ? M_STATE[e->state] : sz, e->state != M_OK ? C_BAD : C_OFF);
        cv_blit(0, 56 + (i - first) * 30u);
    }
}

static uint32_t find_key(uint32_t k)
{
    uint32_t i;
    for (i = 0; i < n_mods; i++)
        if (mods[i].key == k)
            return i;
    return 0;
}

/* ---- deploys: a module file (tools/hbmod.py's .hrbr) sent over SysEx, USB-MIDI or the DIN jack, into
 * flash of its own. What it may write is APP_FL_LO.. (0x93000 up), never the host or the image's
 * modules below it, which only an update writes:
 *   0x93000, 0x94000   the deployed table, two copies: the valid copy with the higher sequence number
 *                      counts. A new table goes into the other copy, its header last.
 *   0x95000-0xE0000    the deployed modules, each 4 KiB aligned (Dinghy's plain XIP window)
 * The module is received whole into the module RAM (the module that ran is stopped) and checked
 * there (the file's CRC, its header, its relocations); only then is flash touched: free space
 * erased, the module relocated to run there and written, read back through XIP and checked as at
 * boot, then the new table. A cut at any moment leaves the old table or the new one, and the host.
 *
 * Frames: F0 7D 48 42 cmd seq pack7(payload, CRC-32 of it) F7 (7D: non-commercial; 48 42 "HB";
 * pack7 as ota.c's). Over USB each one is answered (cmd | 0x40, the same seq, a status first);
 * over DIN nothing comes back, the sender paces itself (tools/hbdeploy.py). */
enum { D_HELLO = 0x01, D_LIST = 0x02, D_BEGIN = 0x10, D_DATA = 0x11, D_COMMIT = 0x12, D_ABORT = 0x13,
       D_DELETE = 0x14, D_LOG = 0x20, D_SAFE = 0x21, D_SELECT = 0x22 };
enum { D_OK, D_BAD_CRC, D_BAD_HEAD, D_TOO_BIG, D_NO_SPACE, D_FLASH, D_VERIFY, D_MISSING, D_IDLE, D_NOT_FOUND,
       D_BAD_MSG };
static const char *const D_TXT[] = {"DONE", "BAD CRC", "BAD HEADER", "TOO BIG", "NO SPACE", "FLASH ERROR",
                                    "VERIFY FAILED", "DATA MISSING", "NOT STARTED", "NOT FOUND", "BAD MESSAGE"};
#define DT_MAGIC 0x54444248u                    /* "HBDT" */
#define RX_BUF ((uint8_t *)HB_MOD_RAM)
#define RX_MAX HB_MOD_RAM_SIZE                  /* (less than DP_HI - DP_LO) */
static uint32_t dt_at;                          /* the table's copy in force: DT_A / DT_B, 0: none */
static struct {
    uint32_t on, size, crc, got, t_last, t_draw, prev_key;
    char name[24];
    uint8_t have[RX_MAX / 256u / 8u];           /* the 256-byte chunks received */
} rx;

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t dt_crc(uint32_t seq, uint32_t n, const hb_slot_t *e)
{
    uint32_t h[2] = {seq, n};
    return crc_upd(crc_upd(0, (const uint8_t *)h, 8u), (const uint8_t *)e, 16u * n);
}
static void dt_load(void)
{
    static struct { uint32_t magic, seq, n, crc; hb_slot_t s[HB_SLOTS]; } t;
    uint32_t i, at[2] = {DT_A, DT_B};
    memset(&dt, 0, sizeof dt);
    dt_at = 0;
    for (i = 0; i < 2u; i++) {
        if (!flash_ok || st_read(at[i], &t, 16u) || t.magic != DT_MAGIC || t.n > HB_SLOTS ||
            st_read(at[i] + 16u, t.s, 16u * t.n) || dt_crc(t.seq, t.n, t.s) != t.crc)
            continue;
        if (!dt_at || (int32_t)(t.seq - dt.seq) > 0) {
            memcpy(&dt, &t, sizeof dt);
            dt_at = at[i];
        }
    }
}
static uint32_t dt_write(const hb_slot_t *e, uint32_t n)   /* into the other copy, the header last */
{
    static struct { uint32_t magic, seq, n, crc; hb_slot_t s[HB_SLOTS]; } t;
    uint32_t to = dt_at == DT_A ? DT_B : DT_A, took;
    t.magic = DT_MAGIC;
    t.seq = dt.seq + 1u;
    t.n = n;
    memcpy(t.s, e, 16u * n);
    t.crc = dt_crc(t.seq, n, t.s);
    if (fl_erase4k(to, &took) || (n && fl_write(to + 16u, (const uint8_t *)t.s, 16u * n)) ||
        fl_write(to, (const uint8_t *)&t, 16u))
        return D_FLASH;
    dt_load();
    hb_log_u32("deploy: table", t.seq);
    return dt_at == to && dt.seq == t.seq ? D_OK : D_VERIFY;
}
static uint32_t dt_find(uint32_t key)          /* the table's entry of that name, HB_SLOTS: none */
{
    uint32_t i;
    for (i = 0; i < dt.n; i++) {
        const hb_slot_t *e = &dt.s[i];
        if (e->at >= FM1_XIP(DP_LO) && e->at < FM1_XIP(DP_HI) && name_key(((const hb_file_t *)e->at)->name) == key)
            return i;
    }
    return HB_SLOTS;
}
static uint32_t dp_room(uint32_t at)            /* the free bytes from at (4 KiB aligned) to the next module */
{
    uint32_t i, end = DP_HI;
    for (i = 0; i < dt.n; i++) {
        uint32_t lo = FL_OF(dt.s[i].at), hi = lo + ((dt.s[i].size + 0xFFFu) & ~0xFFFu);
        if (at >= lo && at < hi)
            return 0;
        if (lo >= at && lo < end)
            end = lo;
    }
    return end - at;
}
static uint32_t dp_place(uint32_t need, uint32_t *largest, uint32_t *total)   /* first fit; 0: none */
{
    uint32_t i, at = 0, r;
    *largest = *total = 0;
    for (i = 0; i <= dt.n; i++) {               /* the gaps start at DP_LO or where a module ends */
        uint32_t s0 = i < dt.n ? FL_OF(dt.s[i].at) + ((dt.s[i].size + 0xFFFu) & ~0xFFFu) : DP_LO;
        if (s0 < DP_LO || s0 >= DP_HI || !(r = dp_room(s0)))
            continue;
        if (r > *largest) *largest = r;
        *total += r;
        if (r >= need && (!at || s0 < at))
            at = s0;
    }
    return at;
}

static void rx_draw(const char *what, uint16_t col)
{
    uint32_t w = rx.size ? rx.got * 256u * 200u / rx.size : 0;
    if (w > 200u) w = 200u;
    cv_begin(240, 96, C_BLACK);
    cv_text(120 - text_w(&FONT_S, what) / 2, 4, &FONT_S, what, col);
    cv_text(120 - text_w(&FONT_S, rx.name) / 2, 32, &FONT_S, rx.name, C_ACC);
    cv_rect(20, 68, 200, 8, C_OFF);
    cv_rect(20, 68, (int32_t)w, 8, C_ACC);
    cv_blit(0, 80);
}

/* the received file into flash; *key: its name's */
static uint32_t d_commit(uint32_t *key)
{
    static hb_slot_t e[HB_SLOTS];
    hb_file_t *f = (hb_file_t *)RX_BUF;
    const uint32_t *rel = (const uint32_t *)(RX_BUF + sizeof *f);
    uint8_t *img;
    uint32_t io, isz, total, need, at, i, n, old, rc, big, all, d, took;
    hb_slot_t sl;
    if (rx.got != (rx.size + 255u) / 256u)
        return D_MISSING;
    if (crc_upd(0, RX_BUF, rx.size) != rx.crc)
        return D_BAD_CRC;
    if (f->magic != HB_MAGIC || f->abi != HB_ABI || f->n_relocs > 65536u || f->code_size > RX_MAX ||
        f->data_size > RX_MAX || f->rt_size > RX_MAX)
        return D_BAD_HEAD;
    io = (sizeof *f + 4u * f->n_relocs + 255u) & ~255u;
    isz = f->code_size + f->data_size + f->rt_size;
    total = io + isz;
    if (total > rx.size || isz < 4u || f->desc + sizeof(hb_module_t) > f->code_size || (f->desc & 3u))
        return D_BAD_HEAD;
    if (f->data_size + f->bss_size > HB_MOD_RAM_SIZE || f->rt_size > HB_MOD_RT_SIZE)
        return D_TOO_BIG;
    img = RX_BUF + io;
    if (crc_upd(crc_upd(crc_upd(0, RX_BUF, (uint32_t)((uint8_t *)&f->crc - RX_BUF)), (const uint8_t *)rel,
                        4u * f->n_relocs), img, isz) != f->crc)
        return D_BAD_CRC;
    for (i = 0; i < f->n_relocs; i++)
        if (rel[i] > isz - 4u)
            return D_BAD_HEAD;
    *key = name_key(f->name);
    old = dt_find(*key);
    if (old == HB_SLOTS && dt.n >= HB_SLOTS)
        return D_NO_SPACE;
    need = (total + 0xFFFu) & ~0xFFFu;
    at = dp_place(need, &big, &all);
    if (!at && old < dt.n) {                    /* no room beside the old copy: the table without it first */
        for (i = n = 0; i < dt.n; i++)
            if (i != old) e[n++] = dt.s[i];
        if ((rc = dt_write(e, n)) != D_OK)
            return rc;
        hb_log("deploy: the old copy dropped first");
        old = HB_SLOTS;
        at = dp_place(need, &big, &all);
    }
    if (!at)
        return D_NO_SPACE;
    rx_draw("WRITING", C_WHITE);
    d = FM1_XIP(at) + io - f->base;             /* relocated to run there (as hbmod.move) */
    for (i = 0; i < f->n_relocs; i++)
        wr32(img + rel[i], rd32(img + rel[i]) + d);
    f->base = FM1_XIP(at) + io;
    f->crc = crc_upd(crc_upd(crc_upd(0, RX_BUF, (uint32_t)((uint8_t *)&f->crc - RX_BUF)), (const uint8_t *)rel,
                             4u * f->n_relocs), img, isz);
    for (i = 0; i < need; i += 0x1000u) {
        if (fl_erase4k(at + i, &took))
            return D_FLASH;
        fm1_wdt_feed();
    }
    for (i = 0; i < total; i += 0x1000u) {
        if (fl_write(at + i, RX_BUF + i, total - i < 0x1000u ? total - i : 0x1000u))
            return D_FLASH;
        fm1_wdt_feed();
    }
    sl.magic = HB_SLOT_MAGIC;
    sl.at = FM1_XIP(at);
    sl.size = total;
    sl.crc = f->crc;
    if (fl_verify_xip(at, RX_BUF, total) || check_module(&sl, DP_LO, DP_HI) != M_OK)
        return D_VERIFY;
    for (i = n = 0; i < dt.n; i++)              /* the new one in the old one's place, else at the end */
        e[n++] = i == old ? sl : dt.s[i];
    if (old == HB_SLOTS)
        e[n++] = sl;
    hb_log_u32("deploy: written at", sl.at);
    return dt_write(e, n);
}

/* after the table changed: the list again, and a module running (the one asked for, else the sine) */
static void d_restart(uint32_t key)
{
    uint32_t k;
    stop_module();
    memset(hb_nv.strikes, 0, sizeof hb_nv.strikes);   /* (by place in the list, which may have moved) */
    scan_slots();
    k = find_key(key);
    if (start_module(k))
        start_module(0);
    if (mods[cur].key != st.sel_key) {
        st.sel_key = mods[cur].key;
        store_touch();
    }
}
static void rx_end(uint32_t status)
{
    rx.on = 0;
    if (status != D_OK) {
        uint32_t t0 = fm1_ms;
        hb_log_u32("deploy failed", status);
        rx_draw(D_TXT[status < 11u ? status : 10u], C_BAD);
        while (fm1_ms - t0 < 1500u)
            fm1_wdt_feed();
    }
}

static int hb_sysex(const uint8_t *p, uint32_t n)   /* platform_poll: a USB frame, kept for the main loop */
{
    if (n < 5u || n > DF_MAX || p[0] != 0x7Du || p[1] != 0x48u || p[2] != 0x42u || df_usb_n)
        return 0;
    memcpy(df_usb, p, n);
    df_usb_n = n;
    return 1;
}

static uint8_t d_msg[DF_MAX], d_out[DF_MAX];
static void d_reply(uint32_t cmd, uint32_t seq, const uint8_t *p, uint32_t n)
{
    static uint8_t b[264];
    uint32_t w;
    if (!usb.config || n > 260u)
        return;
    memcpy(b, p, n);
    wr32(b + n, crc_upd(0, p, n));
    d_out[0] = 0xF0; d_out[1] = 0x7D; d_out[2] = 0x48; d_out[3] = 0x42;
    d_out[4] = (uint8_t)(cmd | 0x40u);
    d_out[5] = (uint8_t)(seq & 0x7Fu);
    w = 6u + ota_pack7(b, n + 4u, d_out + 6);
    d_out[w++] = 0xF7;
    ota_wire_send(d_out, w);
}

/* the frame waiting (USB first); 1: the module or the screen changed */
static uint32_t deploy_frame(void)
{
    uint32_t usb_src = df_usb_n != 0, n = usb_src ? df_usb_n : df_din_n, len, cmd, seq, i, k, ch = 0;
    const uint8_t *p = usb_src ? df_usb : df_din;
    uint8_t r[48];
    uint32_t rn = 1;
    char name[24];
    memset(r, 0, sizeof r);
    cmd = p[3];
    seq = p[4];
    len = ota_unpack7(p + 5, n - 5u, d_msg, sizeof d_msg);
    if (len < 4u || len > sizeof d_msg || crc_upd(0, d_msg, len - 4u) != rd32(d_msg + len - 4u)) {
        r[0] = D_BAD_MSG;
        cmd = 0x3Fu;
    } else {
        len -= 4u;
    }
    if (usb_src) df_usb_n = 0; else df_din_n = 0;
    if (cmd == D_BEGIN || cmd == D_DELETE || cmd == D_SELECT) {
        for (i = 0; i < 23u; i++) name[i] = i + (cmd == D_BEGIN ? 8u : 0u) < len ? (char)d_msg[i + (cmd == D_BEGIN ? 8u : 0u)] : 0;
        name[23] = 0;
    }
    switch (cmd) {
    case D_HELLO: {
        uint32_t big, all;
        dp_place(~0u, &big, &all);
        r[1] = 1; r[2] = HB_ABI; r[3] = (uint8_t)n_mods; r[4] = (uint8_t)cur; r[5] = (uint8_t)rx.on;
        wr32(r + 8, big); wr32(r + 12, all); wr32(r + 16, RX_MAX);
        wr32(r + 20, hb_nv.crash.magic == CRASH_MAGIC ? hb_nv.crash.pc : 0); wr32(r + 24, hb_nv.crash.module);
        wr32(r + 28, hb_nv.stats.worst); wr32(r + 32, hb_nv.stats.overruns); wr32(r + 36, hb_nv.log_w);
        wr32(r + 40, BLOCK_TICKS);
        rn = 44;
        break;
    }
    case D_LIST:                                /* one reply per module */
        for (i = 0; i < n_mods; i++) {
            memset(r, 0, sizeof r);
            r[1] = (uint8_t)i; r[2] = mods[i].state; r[3] = mods[i].kind;
            if (mods[i].file) {
                wr32(r + 4, mods[i].file->version);
                wr32(r + 8, mods[i].file->code_size + mods[i].file->data_size);
                wr32(r + 12, (uint32_t)mods[i].file);
            }
            memcpy(r + 16, mods[i].name, 24);
            d_reply(cmd, seq, r, 40);
        }
        r[0] = D_OK; r[1] = 0xFFu;              /* the end */
        rn = 2;
        break;
    case D_BEGIN: {
        uint32_t size = len >= 8u ? rd32(d_msg) : 0;
        if (len < 32u) { r[0] = D_BAD_MSG; break; }
        if (size < sizeof(hb_file_t) + 4u || size > RX_MAX) { r[0] = D_TOO_BIG; break; }
        k = rx.on ? rx.prev_key : mods[cur].key;   /* (a BEGIN again: the same module to go back to) */
        stop_module();
        memset(&rx, 0, sizeof rx);
        rx.prev_key = k;
        rx.on = 1;
        rx.size = size;
        rx.crc = rd32(d_msg + 4);
        rx.t_last = fm1_ms;
        str_cpy(rx.name, name, sizeof rx.name);
        hb_log_u32("deploy: receiving", size);
        hb_log(rx.name);
        lcd_fill(0, 0, 240, 240, C_BLACK);
        rx_draw("RECEIVING", C_WHITE);
        ch = 1;
        break;
    }
    case D_DATA: {
        uint32_t off = len >= 4u ? rd32(d_msg) : ~0u, want;
        wr32(r + 4, off);
        rn = 8;
        if (!rx.on) { r[0] = D_IDLE; break; }
        want = off < rx.size ? (rx.size - off < 256u ? rx.size - off : 256u) : 0;
        if ((off & 255u) || !want || len - 4u != want) { r[0] = D_BAD_MSG; break; }
        memcpy(RX_BUF + off, d_msg + 4, want);
        if (!(rx.have[off >> 11] >> (off >> 8 & 7u) & 1u)) {
            rx.have[off >> 11] |= (uint8_t)(1u << (off >> 8 & 7u));
            rx.got++;
        }
        rx.t_last = fm1_ms;
        break;
    }
    case D_COMMIT: {
        uint32_t key = 0, st_ = rx.on ? d_commit(&key) : D_IDLE;
        r[0] = (uint8_t)st_;
        if (rx.on) {
            uint32_t back = st_ == D_OK ? key : rx.prev_key;
            if (st_ == D_OK) hb_log("deploy: done");
            rx_end(st_);
            d_restart(back);
            wr32(r + 4, mods[cur].file ? (uint32_t)mods[cur].file : 0);
            r[1] = (uint8_t)cur;
            rn = 8;
            ch = 1;
        }
        break;
    }
    case D_ABORT:
        if (rx.on) {
            hb_log("deploy: aborted");
            rx.on = 0;
            d_restart(rx.prev_key);
            ch = 1;
        }
        break;
    case D_DELETE:
        if (rx.on) { r[0] = D_IDLE; break; }
        k = dt_find(name_key(name));
        if (k == HB_SLOTS) { r[0] = D_NOT_FOUND; break; }
        {
            static hb_slot_t e[HB_SLOTS];
            uint32_t m, keep = mods[cur].key;
            for (i = m = 0; i < dt.n; i++)
                if (i != k) e[m++] = dt.s[i];
            stop_module();
            r[0] = (uint8_t)dt_write(e, m);
            hb_log("deploy: deleted");
            hb_log(name);
            d_restart(keep);
            ch = 1;
        }
        break;
    case D_LOG: {                               /* the log ring from a byte count on: [log_w][from] bytes */
        static uint8_t b[200];
        uint32_t from = len >= 4u ? rd32(d_msg) : 0, w = hb_nv.log_w, m = 0;
        if (w - from > LOG_SIZE) from = w > LOG_SIZE ? w - LOG_SIZE : 0;
        if (from > w) from = w;
        b[0] = D_OK;
        wr32(b + 4, w);
        wr32(b + 8, from);
        while (from + m < w && m < 180u) {
            b[12 + m] = (uint8_t)hb_nv.log[(from + m) % LOG_SIZE];
            m++;
        }
        if (usb_src) d_reply(cmd, seq, b, 12u + m);
        return 0;
    }
    case D_SAFE:                                /* the sine, from the computer */
    case D_SELECT:
        if (rx.on) { r[0] = D_IDLE; break; }
        k = cmd == D_SAFE ? 0 : find_key(name_key(name));
        if (cmd == D_SELECT && !k) {
            r[0] = D_NOT_FOUND;
            break;
        }
        if (mods[k].state == M_STRUCK || mods[k].state == M_SLOW) {
            hb_nv.strikes[k] = 0;
            mods[k].state = M_OK;
        }
        if (start_module(k)) start_module(0);
        if (cmd == D_SELECT) { st.sel_key = mods[cur].key; store_touch(); }
        r[1] = (uint8_t)cur;
        rn = 2;
        ch = 1;
        break;
    default:
        if (!r[0]) r[0] = D_BAD_MSG;
        break;
    }
    if (usb_src)
        d_reply(cmd, seq, r, rn);
    return ch;
}

static uint32_t deploy_tick(void)               /* the main loop's: a frame, the progress, the time-out */
{
    uint32_t ch = 0;
    if (df_usb_n || df_din_n)
        ch = deploy_frame();
    if (rx.on && fm1_ms - rx.t_last > 15000u) {
        hb_log("deploy: timed out");
        rx_end(D_MISSING);
        d_restart(rx.prev_key);
        ch = 1;
    } else if (rx.on && fm1_ms - rx.t_draw > 200u) {
        rx.t_draw = fm1_ms;
        rx_draw("RECEIVING", C_WHITE);
    }
    return ch;
}

static void play_note(uint32_t ch, uint32_t key, uint32_t vel)
{
    const hb_module_t *m = hb_active;
    if (m) m->note(ch, key, vel);
}

static void app_main(void)
{
    uint32_t prev = 0, k, full = 1, picker = 0, sel = 0, t_top = 0, held_any = 0, t_notes = 0, safe = 0;
    uint32_t home_t0 = 0, home_alone = 0, home_eaten = 0;   /* HOME held alone 1.5 s: the picker (HB_F_PANEL) */
    uint32_t sheld = 0, t_press = 0;            /* the sensed keys held (bit k); the pressure's last round */
    int32_t oct_drawn = 99;
    uint8_t held[27] = {0}, press_sent[27] = {0};
    sense_ev_t se;

    nv_init();
    if (hb_nv.active && (fm1_boot.p3_rst & 4u)) {   /* the watchdog, with a module running */
        if (hb_nv.active <= HB_SLOTS + 1u && hb_nv.strikes[hb_nv.active - 1u] < 255u)
            hb_nv.strikes[hb_nv.active - 1u]++;
        hb_log_u32("watchdog in module", hb_nv.active - 1u);
        if (hb_nv.active > 1u && bootguard.failed)
            bootguard.failed--;                 /* a slot module hung, not the host's boot: as hb_fault */
    }
    hb_nv.active = 0;
    hb_log_u32("harbor " APP_VER " boot, reset", fm1_boot.p3_rst);
    if (hb_nv.crash.magic == CRASH_MAGIC)
        hb_log_u32("last crash pc", hb_nv.crash.pc);

    store_load();
    if (st.oct < -2 || st.oct > 2) st.oct = 0;
    for (k = 0; k < FM1_NCOL; k++) {            /* the keys on the analog rows */
        if (FM1_KEYMAP[3][k] >= 14) sense_mask |= 1u << (FM1_KEYMAP[3][k] - 14);
        if (FM1_KEYMAP[4][k] >= 14) sense_mask |= 1u << (FM1_KEYMAP[4][k] - 14);
    }
    sense_init(sense_mask);
    sense_defaults();
    fl_plain_window_init();                     /* flash 0x93000 up reads as stored: the deployed modules */
    scan_slots();

    fm1_delay_ms(60);                           /* the key scan sees what is held at power-on */
    safe = (fm1_in.buttons >> BTN_OCT_UP) & 1u;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    if (safe)
        hb_log("safe boot (OCT+): the sine only");
    k = safe ? 0 : find_key(st.sel_key);
    if (k && mods[k].state != M_OK) {
        hb_log_u32("skipped at boot", k);
        k = 0;
    }
    if (start_module(k))
        start_module(0);

    for (;;) {
        uint32_t keys, down, up, rel, b, e, base = (uint32_t)(KEY0 + 12 * st.oct), panel, screen, sensed;
        uint8_t s, d1, d2;
        const hb_module_t *m;
        platform_poll();
        m = hb_active;
        panel = m && (m->flags & HB_F_PANEL) && m->ui;
        screen = m && (m->flags & HB_F_SCREEN);

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        sensed = h_sense_keys();
        for (k = 0; k < 27u; k++) {
            if (sensed >> k & 1u) {             /* a sensed key plays from its landing (below) */
                if (!panel)
                    fm1_led_key(KEY_LED(k), keys >> k & 1u);
                continue;
            }
            if (panel) {                        /* the module plays its keys */
                if (down >> k & 1u) m->ui(HB_EV_KEY_DOWN | k, 0);
                if (up >> k & 1u) m->ui(HB_EV_KEY_UP | k, 0);
                continue;
            }
            if (down >> k & 1u) { held[k] = (uint8_t)(base + k); play_note(0, held[k], 100); midi_send(0x90, held[k], 100); }
            if (up >> k & 1u) { play_note(0, held[k], 0); midi_send(0x80, held[k], 0); }   /* a key stops the note it started */
            fm1_led_key(KEY_LED(k), keys >> k & 1u);
        }
        while (sense_take(&se)) {               /* the sensed keys: landed (with a velocity), lifted */
            k = se.key;
            if (k >= 27u || !(sensed >> k & 1u))
                continue;
            if (se.type == EV_ON && !(sheld >> k & 1u)) {
                uint32_t v = se.vel ? se.vel : 1u;
                sheld |= 1u << k;
                press_sent[k] = 0;
                if (panel) {
                    m->ui(HB_EV_KEY_DOWN | k, (int32_t)v);
                } else {
                    held[k] = (uint8_t)(base + k);
                    play_note(0, held[k], v);
                    midi_send(0x90, held[k], v);
                }
            } else if (se.type == EV_OFF && sheld >> k & 1u) {
                sheld &= ~(1u << k);
                if (panel) {
                    m->ui(HB_EV_KEY_UP | k, 0);
                } else {
                    play_note(0, held[k], 0);
                    midi_send(0x80, held[k], 0);
                }
            }
        }
        for (k = 0; k < 27u; k++)               /* sensing switched off with keys held: let them go */
            if (sheld >> k & ~sensed >> k & 1u) {
                sheld &= ~(1u << k);
                if (panel) m->ui(HB_EV_KEY_UP | k, 0); else { play_note(0, held[k], 0); midi_send(0x80, held[k], 0); }
            }
        if (sheld && fm1_ms - t_press >= 10u) {  /* their pressure, ~100 times a second */
            t_press = fm1_ms;
            for (k = 0; k < 27u; k++) {
                uint32_t pr;
                if (!(sheld >> k & 1u) || (pr = sense_pressure(k)) == press_sent[k])
                    continue;
                press_sent[k] = (uint8_t)pr;
                if (panel) {
                    m->ui(HB_EV_KEY_PRESSURE | k, (int32_t)pr);
                } else {
                    if (m && m->midi) m->midi(0xA0, held[k], pr);
                    midi_send(0xA0, held[k], pr);
                }
            }
        }
        held_any = keys != 0;
        if (down)
            home_alone = 0;

        while (midi_get(&s, &d1, &d2) || din_get(&s, &d1, &d2)) {   /* USB-MIDI and the DIN jack */
            uint32_t ch = s & 15u;
            if ((s & 0xE0u) == 0x80u) t_notes = fm1_ms;
            if (m && m->midi) { m->midi(s, d1, d2); continue; }
            switch (s & 0xF0u) {
            case 0x90: play_note(ch, d1, d2); break;
            case 0x80: play_note(ch, d1, 0); break;
            case 0xB0: if (m && m->cc) m->cc(ch, d1, d2); break;
            default: break;
            }
        }

        if (deploy_tick()) {                    /* a deploy: the module may have changed */
            picker = 0;
            full = 1;
            if (!rx.on)
                lcd_fill(0, 0, 240, 240, C_BLACK);
            continue;
        }
        if (hb_slow) {
            hb_slow = 0;
            hb_log_u32("too slow: stopped", cur);
            mods[cur].state = M_SLOW;
            start_module(0);
            picker = 0;
            full = 1;
            lcd_fill(0, 0, 240, 240, C_BLACK);
        }

        b = fm1_input_edges(&rel);              /* buttons pressed / released since the last call */
        if (rx.on) {                            /* receiving: the module RAM is the deploy's */
            b = rel = 0;
            for (e = 0; e < 7u; e++)
                fm1_enc_take(e);
        }
        if (!panel || picker) {
            if (b & (1u << BTN_OCT_DOWN) && st.oct > -2) { st.oct--; store_touch(); }
            if (b & (1u << BTN_OCT_UP) && st.oct < 2) { st.oct++; store_touch(); }
        }
        if (picker) {
            int32_t mv = fm1_enc_take(ENC_SELECT) + fm1_enc_take(ENC_PRESETS);
            for (e = 1; e < 6u; e++)
                fm1_enc_take(e);                /* (the module's knobs do nothing meanwhile) */
            if (mv) {
                int32_t ns = (int32_t)sel + mv;
                sel = ns < 0 ? 0 : ns >= (int32_t)n_mods ? n_mods - 1u : (uint32_t)ns;
                draw_picker(sel);
            }
            if (b & (1u << BTN_HOME | 1u << BTN_SEL | 1u << BTN_PLAY)) {
                picker = 0;
                full = 1;
                lcd_fill(0, 0, 240, 240, C_BLACK);
                if (sel != cur || mods[sel].state == M_STRUCK || mods[sel].state == M_SLOW) {
                    if (mods[sel].state == M_STRUCK || mods[sel].state == M_SLOW) {   /* picked again: another chance */
                        hb_nv.strikes[sel] = 0;
                        mods[sel].state = M_OK;
                    }
                    if (start_module(sel))
                        start_module(0);
                    st.sel_key = mods[cur].key;
                    store_touch();
                }
            }
        } else if (panel) {                     /* every button and encoder is the module's */
            if (b >> BTN_HOME & 1u) { home_t0 = fm1_ms; home_alone = 1; home_eaten = 0; }
            if ((b & ~(1u << BTN_HOME)) || (keys && b >> BTN_HOME & 1u))
                home_alone = 0;
            for (k = 0; k < 14u; k++) {
                if (b >> k & 1u) m->ui(HB_EV_BTN_DOWN | k, 0);
                if (rel >> k & 1u && !(k == BTN_HOME && home_eaten)) m->ui(HB_EV_BTN_UP | k, 0);
            }
            for (e = 0; e < 7u; e++) {
                int32_t c = fm1_enc_take(e);
                if (c) { m->ui(HB_EV_ENC | e, c); home_alone = 0; }
            }
            if (home_alone && (fm1_in.buttons >> BTN_HOME & 1u) && fm1_ms - home_t0 >= 1500u) {
                m->ui(HB_EV_BTN_UP | BTN_HOME, 0);   /* HOME held alone: the picker; the module saw it let go */
                home_alone = 0;
                home_eaten = 1;
                picker = 1;
                sel = cur;
                lcd_fill(0, 0, 240, HB_TOP, C_BLACK);
                draw_picker(sel);
                full = 1;
            }
        } else {
            if (b & (1u << BTN_HOME)) {
                picker = 1;
                sel = cur;
                draw_picker(sel);
            } else if (m && m->ui) {
                for (k = 2; k < 14u; k++) {     /* the module's buttons: not OCT- / OCT+ / HOME */
                    if (k == BTN_HOME) continue;
                    if (b >> k & 1u) m->ui(HB_EV_BTN_DOWN | k, 0);
                    if (rel >> k & 1u) m->ui(HB_EV_BTN_UP | k, 0);
                }
                for (e = 0; e < 7u; e++) {
                    int32_t c = fm1_enc_take(e);
                    if (c) m->ui(HB_EV_ENC | e, c);
                }
            }
        }

        if ((!screen || picker) && (st.oct != oct_drawn || fm1_ms - t_top > 500u || full)) {
            oct_drawn = st.oct;
            t_top = fm1_ms;
            draw_top(st.oct);
        }
        if (!picker) {
            m = hb_active;
            if (m && m->draw) m->draw(full);
            full = 0;
        }

        if (st_dirty && !held_any && fm1_ms - st_changed > 2000u && fm1_ms - t_notes > 2000u)
            store_flush();                      /* (interrupts are off while the flash is busy: no module code runs) */
    }
}
