/* Platform state: display, EEPROM, timer. Grows with the graphics/Gamelet natives. */
#include <stdlib.h>
#include <string.h>

#include "natives.h"

struct ExenPlatform {
    int width, height;
    uint8_t eeprom[2048];
};

int platform_init(ExenVM *vm, const ExenConfig *cfg)
{
    vm->plat = calloc(1, sizeof *vm->plat);
    if (!vm->plat)
        return -1;
    vm->plat->width = cfg && cfg->screen_width ? cfg->screen_width : 101;
    vm->plat->height = cfg && cfg->screen_height ? cfg->screen_height : 80;
    return 0;
}

void platform_free(ExenVM *vm)
{
    free(vm->plat);
    vm->plat = NULL;
}

uint8_t *exen_eeprom(ExenVM *vm) { return vm->plat->eeprom; }
