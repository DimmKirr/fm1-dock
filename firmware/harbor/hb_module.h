/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1-harbor's module interface, version 1: what a module exports (hb_module_t, the descriptor at
 * the start of its image) and what the host gives it (hb_host_t, passed to init). Tables only: a
 * module links against nothing of the host's, and the host checks `abi` before any call.
 *
 * A module runs in place from flash and has one fixed RAM region (HB_MOD_RAM), shared by all
 * modules: only one runs at a time. The host copies its .data there, clears its .bss and calls
 * init(); switching away calls release(). render() runs in the audio interrupt; everything else
 * runs in the main loop.
 *
 * Rules (as Dinghy's, docs/RULES.md): integers only, no 64-bit division, no hardware registers,
 * no waiting. Draw only below HB_TOP (the host's line at the top). */
#ifndef HB_MODULE_H
#define HB_MODULE_H
#include <stdint.h>

#define HB_ABI 2u                               /* 2: flags, midi(), blit(), text_w(), key events */

/* memory a module may use (modules/module.ld places .data / .bss here) */
#define HB_MOD_RAM      0x01C30000u             /* .data + .bss, 272 KiB */
#define HB_MOD_RAM_SIZE 0x44000u
#define HB_MOD_RT       0x01C04000u             /* .ram_text, 16 KiB: call it through a pointer (XIP -> RAM is */
#define HB_MOD_RT_SIZE  0x4000u                 /* beyond the 23-bit call range), see HB_FAR */

#define HB_TOP 24                               /* screen rows above this are the host's (not with HB_F_SCREEN) */

/* hb_module_t.flags */
#define HB_F_SCREEN 1u                          /* all of the screen is the module's: the host draws only its picker */
#define HB_F_PANEL  2u                          /* all keys and buttons are the module's, OCT- / OCT+ / HOME too, as
                                                 * events; the host plays no note itself. HOME held 1.5 s alone opens
                                                 * the picker (the module gets the HOME release first) */

/* ui() events: kind in the top byte, the id below; arg is the encoder's clicks */
enum {
    HB_EV_BTN_DOWN = 0x01000000u,               /* | button id (BTN_FX ...; not OCT-/OCT+/HOME: the host's) */
    HB_EV_BTN_UP   = 0x02000000u,
    HB_EV_ENC      = 0x03000000u,               /* | encoder id (ENC_SELECT ... ENC_PRESETS), arg = clicks */
    HB_EV_KEY_DOWN = 0x04000000u,               /* | note key k (0 = F3 .. 26 = G5): HB_F_PANEL only */
    HB_EV_KEY_UP   = 0x05000000u,
};
#define HB_EV_KIND(ev) ((ev) & 0xFF000000u)
#define HB_EV_ID(ev)   ((ev) & 0xFFu)

/* panel ids, as platform.c names them */
enum { HB_BTN_OCT_DOWN = 0, HB_BTN_OCT_UP = 1, HB_BTN_FX = 2, HB_BTN_SEL = 3, HB_BTN_ENV = 4, HB_BTN_LFO = 5,
       HB_BTN_EDIT = 6, HB_BTN_GLO = 7, HB_BTN_HOME = 8, HB_BTN_SAVE = 9, HB_BTN_ARP = 10, HB_BTN_SEQ = 11,
       HB_BTN_PLAY = 12, HB_BTN_REC = 13 };
enum { HB_ENC_SELECT = 0, HB_ENC_ALGORITHM = 1, HB_ENC_KNOB1 = 2, HB_ENC_KNOB2 = 3, HB_ENC_KNOB3 = 4,
       HB_ENC_KNOB4 = 5, HB_ENC_PRESETS = 6 };
#define HB_LED_KEY(k) (14u + (k))               /* note key k's LED (0 = F3 .. 26 = G5) */

/* the descriptor: at the start of the module's image (modules/module.ld) */
#ifndef HB_DESC
#define HB_DESC const hb_module_t hb_module __attribute__((section(".hb_desc"), used))
#endif
/* a call from flash into the module's .ram_text: through a pointer the compiler cannot fold */
static inline void *hb_far(void *p) { void *volatile q = p; return q; }
#define HB_FAR(fn) ((__typeof__(&fn))hb_far((void *)&fn))

#define HB_RGB(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

typedef struct hb_host {
    uint32_t abi;                               /* HB_ABI */
    uint32_t size;                              /* sizeof(hb_host_t): later versions only append */
    /* time */
    uint32_t (*ms)(void);                       /* milliseconds since power-on */
    uint32_t (*ticks)(void);                    /* the 24 MHz counter (busy time, not for waiting) */
    /* the log: a ring in RAM that survives a reset (tools/harbor.py log) */
    void (*log)(const char *s);
    void (*log_u32)(const char *s, uint32_t v); /* s then v in hex */
    /* the screen, 240 x 240, RGB565; text sizes 0 (16 px) and 1 (32 px) */
    void (*fill)(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c);
    void (*text)(int32_t x, int32_t y, int32_t w, uint32_t big, const char *s, uint16_t fg, uint32_t center);
    /* a canvas (up to 240 x 96): drawn off screen, then put on it in one go */
    void (*cv_begin)(int32_t w, int32_t h, uint16_t bg);
    void (*cv_rect)(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c);
    void (*cv_line)(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c);
    void (*cv_pixel)(int32_t x, int32_t y, uint16_t c);
    void (*cv_text)(int32_t x, int32_t y, uint32_t big, const char *s, uint16_t fg);
    void (*cv_blit)(int32_t x, int32_t y);
    /* LEDs: buttons by id, note keys HB_LED_KEY(k) */
    void (*led)(uint32_t id, uint32_t on);
    uint32_t (*rand)(void);
    /* this module's own settings record (up to 240 bytes), kept in flash when nothing plays */
    int (*load)(void *p, uint32_t n);           /* 0: loaded */
    void (*save)(const void *p, uint32_t n);
    void (*midi_send)(uint32_t st, uint32_t d1, uint32_t d2);
    uint32_t (*master)(void);                   /* the MASTER pot as a gain, Q12 (the host applies it) */
    /* ABI 2 */
    void (*blit)(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *px);   /* pixels as the panel takes
                                                 * them (RGB565, high byte first); done when it returns */
    int32_t (*text_w)(uint32_t big, const char *s);   /* the width text() / cv_text() give s */
    /* this module's own 8 KiB of flash (found by its name), for what does not fit its settings
     * record (user presets): up to HB_BANK_MAX bytes with a CRC, p in the module's RAM; a save
     * erases the bank (the sound stalls ~100 ms) */
    int (*bank_load)(void *p, uint32_t n);      /* 0: loaded (n as saved) */
    int (*bank_save)(const void *p, uint32_t n);
} hb_host_t;
#define HB_BANK_MAX 8176u

typedef struct hb_param {
    const char *name;
    int32_t lo, hi, def;
} hb_param_t;

typedef struct hb_module {
    uint32_t abi;                               /* HB_ABI */
    const char *name;
    int  (*init)(const hb_host_t *h);           /* 0: ready; anything else and the host falls back */
    void (*release)(void);                      /* notes are cut, the module is switched away */
    void (*note)(uint32_t ch, uint32_t key, uint32_t vel);   /* vel 0: note off */
    void (*cc)(uint32_t ch, uint32_t num, uint32_t val);
    void (*render)(int32_t *out, uint32_t n);   /* audio interrupt: add n stereo frames (L R, Q15) into out */
    void (*ui)(uint32_t ev, int32_t arg);
    void (*draw)(uint32_t full);                /* main loop, often: draw what changed (full: all of it) */
    const hb_param_t *params;                   /* for a generic page (may be 0) */
    uint32_t n_params;
    uint32_t flags;                             /* HB_F_SCREEN, HB_F_PANEL */
    void (*midi)(uint32_t st, uint32_t d1, uint32_t d2);   /* 0: note() / cc() only; else every channel message */
} hb_module_t;

/* A module in flash (and in a .hrbr file): this header, the relocations, then the image.
 *   image: .text + .rodata (code_size), .data's initial values (data_size), .ram_text (rt_size)
 * The image starts at the first 256-byte boundary after the relocations, linked to run there (`base`); relocs[] are offsets of 32-bit words in the image that
 * hold module addresses. Whoever moves it adds (new base - base) to them and sets base. The
 * descriptor is at image offset desc. crc is CRC-32 over the header up to crc, the relocations
 * and the image. */
#define HB_MAGIC 0x52425248u                    /* "HRBR" */
typedef struct {
    uint32_t magic, abi, flags;
    char name[24];
    uint32_t version;
    uint32_t code_size, data_size, bss_size, rt_size;
    uint32_t worst_cycles;                      /* per 256-frame block, measured (0: unknown) */
    uint32_t desc;                              /* the descriptor's offset in the image */
    uint32_t base;                              /* the image's run address */
    uint32_t n_relocs;
    uint32_t reserved;                          /* 0 */
    uint32_t crc;
} hb_file_t;                                    /* 80 bytes; then n_relocs u32, 0xFF to a 256-byte boundary, the image */

/* the slot table: one 4 KiB sector of these; an entry is written after its module, and an
 * erased entry (all 0xFF) ends the table */
#define HB_SLOT_MAGIC 0x544F4C53u               /* "SLOT" */
#define HB_SLOTS 32u
typedef struct {
    uint32_t magic;
    uint32_t at;                                /* XIP address of the hb_file_t */
    uint32_t size;                              /* header + relocs + image */
    uint32_t crc;                               /* the module's hb_file_t.crc */
} hb_slot_t;

#endif
