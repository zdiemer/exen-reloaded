/* Registration and shared helpers of the natives declared in extnatives.h. */
#include <stdarg.h>
#include <stdio.h>

#include "extnatives.h"

static const uint32_t ext_hash[EC_COUNT] = {
    [EC_VECTOR3D] = 0xe36f9667u,  [EC_MATRIX3D] = 0x8f9e8280u, [EC_RAYCAST] = 0xd0b8e4acu,
    [EC_DIALOGBOX] = 0xb6ee3b2au, [EC_LIST] = 0xdf774e57u,     [EC_SMS] = 0x6bddc5b7u,
};

int ext_class(ExenVM *vm, int which)
{
    if (!vm->ext.cls[which])
        vm->ext.cls[which] = find_class(vm, ext_hash[which]) + 1; /* -1 (missing) stores 0 */
    return vm->ext.cls[which] - 1;
}

void ext_warn_once(ExenVM *vm, int bit, const char *fmt, ...)
{
    if (vm->ext.warned & (1u << bit))
        return;
    vm->ext.warned |= 1u << bit;
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vm_log(vm, "%s", buf);
}

void extnatives_register(void)
{
    fx_register();
    raycast_register();
    vec3d_register();
    ui_register();
    sms_register();
}
