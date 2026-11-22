/* The natives outside the Graphics/Image/Resource/PlayField/Gamelet core: FX, RayCast,
 * Vector3D/Matrix3D, DialogBox/List, Sms and catalog (docs/re/NATIVES.md §2.3-2.4).
 * extnatives_register() installs them all; platform_init calls it.
 */
#ifndef EXEN_EXTNATIVES_H
#define EXEN_EXTNATIVES_H

#include "platform.h"

void extnatives_register(void);

void fx_register(void);       /* fx.c: 103-108 */
void raycast_register(void);  /* raycast.c: 137-146 */
void vec3d_register(void);    /* vec3d.c: 123-136 */
void ui_register(void);       /* ui.c: DialogBox 101-102, List 109 */
void sms_register(void);      /* sms.c: Sms 89-100, Gamelet 78/84/85, catalog 178-184 */

/* Framework classes these natives use, resolved by hash on first use (vm->ext.cls). */
enum {
    EC_VECTOR3D, EC_MATRIX3D, EC_RAYCAST, EC_DIALOGBOX, EC_LIST, EC_SMS, EC_COUNT
};
int ext_class(ExenVM *vm, int which);
/* Log once per VM for each bit. */
void ext_warn_once(ExenVM *vm, int bit, const char *fmt, ...);

/* The global palette -> RGB332 LUT primed for e's palette (graphics.c, 0x42d279). */
const uint8_t *eximg_lut(ExenVM *vm, const EXimg *e);

/* exen.Math helpers (natives.c): 16.16 cos/sin of a 2048-unit angle, integer sqrt */
int exen_cos(ExenVM *vm, int a);
int exen_sin(ExenVM *vm, int a);
int32_t exen_isqrt(int32_t x);

#endif
