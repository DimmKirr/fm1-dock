/* ----------------------------------------------------------------- harbor: the module --- */
static void block_wait(void)                /* the audio block that may be running ends */
{
    uint32_t b = blocks, t0 = H->ms();
    while (blocks == b && H->ms() - t0 < 20u)
        ;
}

static void render(int32_t *o, uint32_t n)  /* FM-1 SID's audio interrupt */
{
    uint32_t t0 = fm1_ticks(), i;
    static short s[HALF_FRAMES];
    if (n != HALF_FRAMES) return;
    if (running) {
        syn_step();
        render_resid(s);
    } else {
        for (i = 0; i < HALF_FRAMES; i++)
            s[i] = 0;
    }
    fx_block(s, o, HALF_FRAMES);
    for (i = 1; i < HALF_FRAMES; i += 2u)
        scope[scope_w++ & 511u] = s[i];
    isr_ticks += fm1_ticks() - t0;
    isr_halves++;
    blocks++;
}

static uint32_t last_draw;
static void draw_step(uint32_t full)        /* FM-1 SID's main loop, once */
{
    uint32_t rel, e, now = fm1_ms;
    e = fm1_input_edges(&rel);
    e &= ~(1u << 8);                        /* harbor: HOME (the player) is not here */
    if (e & (1u << 2))                      /* FX: the effects page */
        fx_view = !fx_view;
    if (fx_view && fx_keys()) {             /* (its encoders are the page's) PRESETS: the chip */
        model = !model;
        start_synth();
    }
    synth_keys(e);
    if (full || now - last_draw >= 40u) {
        last_draw = now;
        draw();
        H->blit(0, 0, 240, 240, fb);
    }
}

static void ui(uint32_t ev, int32_t arg) { hbf_event(ev, arg); }

static void note(uint32_t ch, uint32_t key, uint32_t vel)   /* MIDI: as the keys, on the same target */
{
    (void)ch;
    if (key < 128u) syn_cmd(vel ? CMD_ON : CMD_OFF, key, parts_req ? sel : POLY);
}

static void cc(uint32_t ch, uint32_t num, uint32_t val)
{
    (void)ch; (void)val;
    if (num == 123u) syn_cmd(CMD_ALLOFF, 0, 0);
}

static int init(const hb_host_t *h)
{
    static short pre[HALF_FRAMES];
    uint32_t i;
    H = h;
    start_synth();
    for (i = 0; i < 32u; i++)               /* harbor: 0.19 s of the chip unheard (the ISR does not run it yet): */
        render_resid(pre);                  /* its DC settles behind reSID's output filter, no click at the start */
    play_cyc = 0;
    H->log("sid: FM-1 SID's synth, reSID 0.16");
    return 0;
}

static void release(void) { running = 0; }

HB_DESC = {HB_ABI, "SID", init, release, note, cc, render, ui, draw_step, 0, 0, HB_F_SCREEN | HB_F_PANEL, 0};
