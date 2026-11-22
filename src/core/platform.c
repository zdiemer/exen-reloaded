/* Platform state (platform.h) and the exen.Gamelet natives (67-88).
 * docs/re/NATIVES.md §2.2, §3 (EXManuf layer), §4.2-4.3 (timer, keys, display update).
 */
#include <stdlib.h>
#include <string.h>

#include "extnatives.h"
#include "platform.h"

enum { GL_GBUFF, GL_IMAGE }; /* exen.Gamelet instance slots */

/* Per-game context storage. The original copies min(len, 300) bytes into the current gamelet's
 * record inside the 2048-byte EEPROM config (0x4153f8). The record layout is not decoded yet,
 * so we keep the 300 bytes at a fixed offset of the EEPROM image. */
#define CTX_OFFSET 0x500
#define CTX_SIZE 300

int platform_init(ExenVM *vm, const ExenConfig *cfg)
{
    vm->plat = calloc(1, sizeof *vm->plat);
    if (!vm->plat)
        return -1;
    struct ExenPlatform *p = vm->plat;
    p->width = cfg && cfg->screen_width ? cfg->screen_width : 101;
    p->height = cfg && cfg->screen_height ? cfg->screen_height : 80;
    p->fb = calloc((size_t)p->width * (size_t)p->height, 1);
    if (!p->fb)
        return -1;
    p->normal_video = 1;
    p->timer_ms = -1;
    melody_init(&p->melody, cfg ? cfg->audio_rate : 0);
    for (int i = 0; i < PC_COUNT; i++)
        p->cls[i] = -2;
    graphics_register();
    resource_register();
    gamelet_register();
    playfield_register();
    extnatives_register(); /* also Gamelet 78 sendSms, 84-85 nickname (sms.c) */
    return 0;
}

void platform_free(ExenVM *vm)
{
    if (vm->plat)
        free(vm->plat->fb);
    free(vm->plat);
    vm->plat = NULL;
}

void platform_set_font(ExenVM *vm, const uint8_t *bits)
{
    memcpy(vm->plat->font, bits, EXEN_FONT_BYTES);
    vm->plat->have_font = 1;
}

uint8_t *exen_eeprom(ExenVM *vm) { return vm->plat->eeprom; }

/* ------------------------------------------------------------ Gamelet natives */

static void timer_set(ExenVM *vm, int ms)
{
    vm->plat->timer_ms = ms;
    if (vm->host.set_timer)
        vm->host.set_timer(vm->host.user, ms);
}

static int n_isColor(ExenVM *vm, uint32_t *a) { (void)vm; a[0] = 1; return 1; }       /* depth 8 != 1 */
static int n_numColors(ExenVM *vm, uint32_t *a) { (void)vm; a[0] = 256; return 1; }
static int n_getBitmapDepth(ExenVM *vm, uint32_t *a) { (void)vm; a[0] = 8; return 1; }
static int n_getScreenWidth(ExenVM *vm, uint32_t *a) { a[0] = (uint32_t)vm->plat->width; return 1; }
static int n_getScreenHeight(ExenVM *vm, uint32_t *a) { a[0] = (uint32_t)vm->plat->height; return 1; }

/* screenUpdate(II)V 0x424ff2 -> 0x402304 -> EXManufDisplayUpdate. The ints are ignored. */
static int n_screenUpdate(ExenVM *vm, uint32_t *a)
{
    struct ExenPlatform *p = vm->plat;
    int cg = plat_class(vm, PC_GAMELET);
    EXimg e;
    if (cg < 0 || !a[0] || !eximg_get(vm, inst(vm, a[0], cg)[GL_IMAGE], &e)) {
        vm_halt(vm, "Gamelet.screenUpdate: no screen image");
        return 0;
    }
    if (e.w > p->width || e.h > p->height || e.w <= 0 || e.h <= 0)
        return 0; /* -5: not displayed */
    uint32_t len;
    const uint8_t *d = eximg_data(vm, &e, &len);
    int ox = (p->width - e.w) / 2, oy = (p->height - e.h) / 2;
    if (e.depth == 8) {
        for (int y = 0; y < e.h; y++) {
            uint64_t si = (uint64_t)y * (uint32_t)e.pitch;
            if (si + (uint32_t)e.w > len)
                break;
            memcpy(p->fb + (size_t)(y + oy) * (size_t)p->width + (size_t)ox, d + si, (size_t)e.w);
        }
    } else if (e.depth == 1) {
        /* fmt 0: 1bpp, MSB = leftmost pixel; unreachable with the colour profiles */
        int rowb = (e.w + 7) / 8;
        for (int y = 0; y < e.h; y++)
            for (int x = 0; x < e.w; x++) {
                uint32_t i = (uint32_t)(y * rowb + x / 8);
                int bit = i < len ? (d[i] >> (7 - (x & 7))) & 1 : 0;
                p->fb[(size_t)(y + oy) * (size_t)p->width + (size_t)(x + ox)] = bit ? 0xdb : 0x24;
            }
    } else {
        return 0;
    }
    if (vm->host.present)
        vm->host.present(vm->host.user, p->fb, p->width, p->height);
    return 0;
}

static int n_exitVm(ExenVM *vm, uint32_t *a)
{
    (void)a;
    melody_stop(vm);
    vm_halt(vm, "exitVm: the gamelet exited");
    return 0;
}

static int n_throwInternalException(ExenVM *vm, uint32_t *a)
{
    vm_log(vm, "Gamelet.throwInternalException(%d)", (int32_t)a[0]);
    melody_stop(vm);
    vm_halt(vm, "throwInternalException(%d) (non catchable interrupt 0)", (int32_t)a[0]);
    return 0;
}

static int n_getTimerTickCount(ExenVM *vm, uint32_t *a)
{
    a[0] = vm->host.ticks_ms ? vm->host.ticks_ms(vm->host.user) : 0;
    return 1;
}

static int n_startTimer(ExenVM *vm, uint32_t *a)
{
    int32_t ms = (int32_t)a[1];
    if (ms >= 0) {
        if (ms < 10) ms = 10;          /* EXManufSetTimerTick clamps to [10, 60000] */
        if (ms > 60000) ms = 60000;
        timer_set(vm, ms);
    }
    return 0;
}

static int n_stopTimer(ExenVM *vm, uint32_t *a) { (void)a; timer_set(vm, -1); return 0; }

static int n_saveCtx(ExenVM *vm, uint32_t *a)
{
    if (!a[1]) {
        vm_halt(vm, "Gamelet.saveCtx: null array");
        return 0;
    }
    uint32_t n = arr_len(vm, a[1]);
    memcpy(vm->plat->eeprom + CTX_OFFSET, arr_data(vm, a[1]), n < CTX_SIZE ? n : CTX_SIZE);
    return 0;
}

static int n_loadCtx(ExenVM *vm, uint32_t *a)
{
    if (!a[1]) {
        vm_halt(vm, "Gamelet.loadCtx: null array");
        return 0;
    }
    uint32_t n = arr_len(vm, a[1]);
    memcpy(arr_data(vm, a[1]), vm->plat->eeprom + CTX_OFFSET, n < CTX_SIZE ? n : CTX_SIZE);
    return 0;
}

static int n_playVibrator(ExenVM *vm, uint32_t *a) { (void)vm; (void)a; return 0; }

static int n_playMelody(ExenVM *vm, uint32_t *a)
{
    if (!a[1]) {
        vm_halt(vm, "Gamelet.playMelody: null array");
        return 0;
    }
    /* The original only plays when the EEPROM sound byte (cfg+0x15c) is nonzero. The shipped
     * eeprom.dat has 0 there, which would silence every game, so we always play and leave
     * muting to the host (exen_audio_set_volume). The bytes are copied: the array may move or
     * be collected while the melody plays. */
    melody_play(vm, arr_data(vm, a[1]), arr_len(vm, a[1]));
    return 0;
}

static int n_stopMelody(ExenVM *vm, uint32_t *a) { (void)a; melody_stop(vm); return 0; }

static int n_getVersionInfo(ExenVM *vm, uint32_t *a)
{
    const uint8_t *c = vm->plat->eeprom;
    /* movsx: bytes >= 0x80 OR ones into the higher bits */
    a[0] = (uint32_t)((int32_t)(int8_t)c[4] << 24) | (uint32_t)((int32_t)(int8_t)c[5] << 16) |
           (uint32_t)((int32_t)(int8_t)c[6] << 8) | (uint32_t)(int32_t)(int8_t)c[7];
    return 1;
}

static int n_getProperty(ExenVM *vm, uint32_t *a)
{
    if (!a[0]) {
        vm_throw_hash(vm, H_NPE);
        return 1;
    }
    const uint8_t *s;
    int n = vm_string_get(vm, a[0], &s);
    if (n == 9 && !memcmp(s, "FrameWork", 9))
        a[0] = vm_new_cstring(vm, "V2.00");
    else
        a[0] = 0;
    return 1;
}

static int n_platformRequest(ExenVM *vm, uint32_t *a) { (void)vm; a[0] = 0; return 1; }

void gamelet_register(void)
{
    natives_register(67, n_isColor);
    natives_register(68, n_numColors);
    natives_register(69, n_getBitmapDepth);
    natives_register(70, n_getScreenWidth);
    natives_register(71, n_getScreenHeight);
    natives_register(72, n_screenUpdate);
    natives_register(73, n_exitVm);
    natives_register(74, n_throwInternalException);
    natives_register(75, n_getTimerTickCount);
    natives_register(76, n_startTimer);
    natives_register(77, n_stopTimer);
    natives_register(79, n_saveCtx);
    natives_register(80, n_loadCtx);
    natives_register(81, n_playVibrator);
    natives_register(82, n_playMelody);
    natives_register(83, n_stopMelody);
    natives_register(86, n_getVersionInfo);
    natives_register(87, n_getProperty);
    natives_register(88, n_platformRequest);
}
