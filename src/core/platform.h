/* Platform state owned by the native layer (display, timer, EEPROM, font, palette LUT).
 *
 * Everything here is plain data except the framebuffer pointer, so a save state can copy the
 * struct alongside the VM arena. Per-image state lives in the arena (see graphics.c).
 */
#ifndef EXEN_PLATFORM_H
#define EXEN_PLATFORM_H

#include "melody.h"
#include "natives.h"

/* The built-in 5x8 font: a 1-bit strip of 256 glyphs, 8 rows of 160 bytes (gensimu.exe VA
 * 0x454dd0, docs/re/natives/graphics_font.c.txt). */
#define EXEN_FONT_VA     0x454dd0u
#define EXEN_FONT_STRIDE 160
#define EXEN_FONT_BYTES  (EXEN_FONT_STRIDE * 8)

/* Framework class indices the natives need, resolved by class hash on first use. */
enum {
    PC_GRAPHICS, PC_IMAGE, PC_PALETTE, PC_RESOURCE, PC_TEXTUREMAP, PC_GAMELET,
    PC_ANIMBITMAP, PC_PLAYFIELD, PC_SPRITE, PC_ANIMFLASH, PC_COUNT
};

struct ExenPlatform {
    int width, height;              /* display (device profile) */
    uint8_t eeprom[2048];
    uint8_t font[EXEN_FONT_BYTES];
    int have_font;

    /* The single global palette -> RGB332 LUT of the original (ctx20 +0x0c/+0x10). */
    uint8_t lut[256];
    uint32_t lut_version;
    int lut_valid;

    int normal_video;               /* setNormalVideo / setInverseVideo flag (display only) */
    int timer_ms;                   /* current timer period, -1 = stopped */

    uint8_t *fb;                    /* width*height RGB332, what the "LCD" shows */

    Melody melody;                  /* melody.c */

    int cls[PC_COUNT];              /* -2 = unresolved, -1 = missing */
    uint32_t warned;                /* log-once bits */
};

int plat_class(ExenVM *vm, int which);
/* Log `msg` the first time `bit` is used. */
void plat_warn_once(ExenVM *vm, int bit, const char *fmt, ...);

/* graphics.c */
void graphics_register(void);
/* resource.c */
void resource_register(void);
/* playfield.c (AnimBitmap, PlayField, AnimFlash, Displayable) */
void playfield_register(void);
/* platform.c */
void gamelet_register(void);
void platform_set_font(ExenVM *vm, const uint8_t *bits);

/* EXimg view, shared by graphics.c and the other natives that draw (AnimBitmap, PlayField, FX).
 * See graphics.c for the arena layout. */
typedef struct EXimg {
    ref_t self;        /* the image_imgT byte[] holding this struct */
    ref_t data_ref;    /* image_Data byte[] */
    int32_t w, h, size, depth, pitch;
    int32_t clipX, clipY, clipW, clipH;
    int32_t ops;       /* 4 or 8 when the depth has an ops table, else 0 */
    uint32_t blitMode;
    uint32_t transpIdx;
    uint32_t pal;      /* int[] ref, EXIMG_PAL_NATIVE, or 0 */
    uint32_t palCount;
    uint32_t palVersion; /* Palette object ref (its paletteVersion is the LUT key), or 0 */
    uint32_t alphaMode;
    uint32_t f54;
} EXimg;

#define EXIMG_PAL_NATIVE 1u

/* Load the EXimg of an exen.Image object. Returns 0 if image_imgT or image_Data is null. */
int eximg_get(ExenVM *vm, ref_t image, EXimg *e);
void eximg_put(ExenVM *vm, const EXimg *e);
uint8_t *eximg_data(ExenVM *vm, const EXimg *e, uint32_t *len);
/* Copy a Graphics object's clip into e (syncClip, 0x425650). */
void eximg_sync_clip(ExenVM *vm, ref_t graphics, EXimg *e);
/* drawImageCore (0x418008). */
int eximg_draw_image(ExenVM *vm, EXimg *d, int dx, int dy, int dw, int dh, EXimg *s, int sx,
                     int sy, int sw, int sh, int tr);
/* Built-in font drawText (0x413b4f): `len` bytes of `s` at (x, y) in colour c, clipped to e's
 * clip rect. 0xFF ends the text. */
void eximg_draw_text(ExenVM *vm, EXimg *e, int32_t x, int32_t y, const uint8_t *s, int32_t len,
                     uint8_t c);

#endif
