/* Public API (exen.h): lifecycle, loading, boot and host events. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "natives.h"

int exen_find_framework(const uint8_t *exe, size_t len, size_t *off, size_t *size);
int exen_finalize_classes(ExenVM *vm);
int platform_init(ExenVM *vm, const ExenConfig *cfg);
void platform_free(ExenVM *vm);

#define CODE_CAP (512u * 1024u)
#define DEFAULT_HEAP (4u * 1024u * 1024u)

/* vm.sys.Bootstrap entry points (docs/re/CVP.md §9) */
#define BS_INIT       0x35b0f11eu /* init([C)V */
#define BS_KEYPRESS   0x305a6d35u
#define BS_KEYRELEASE 0x305a1e56u
#define BS_TICK       0x3f522033u
#define BS_EXIT       0x3f523566u

static void set_error(ExenVM *vm, const char *msg)
{
    snprintf(vm->error, sizeof vm->error, "%s", msg);
}

ExenVM *exen_create(const ExenConfig *cfg)
{
    ExenVM *vm = calloc(1, sizeof *vm);
    if (!vm)
        return NULL;
    uint32_t heap = cfg && cfg->heap_bytes ? cfg->heap_bytes : DEFAULT_HEAP;
    vm->mem_size = CODE_CAP + heap;
    vm->mem = calloc(vm->mem_size, 1);
    if (!vm->mem) {
        free(vm);
        return NULL;
    }
    vm->code_end = 16; /* keep offset 0 unused so 0 can mean null */
    vm->fp = -1;
    if (cfg)
        vm->host = cfg->host;
    vm->math.s0 = vm->math.s1 = 0xc800;
    vm->math.period = 1;
    if (platform_init(vm, cfg) < 0) {
        exen_destroy(vm);
        return NULL;
    }
    return vm;
}

void exen_destroy(ExenVM *vm)
{
    if (!vm)
        return;
    platform_free(vm);
    for (int i = 0; i < vm->nclasses; i++) {
        free((void *)vm->classes[i].name);
        free(vm->classes[i].inst_refmap);
        free(vm->classes[i].static_refmap);
    }
    free(vm->classes);
    free(vm->objmap);
    free(vm->exn);
    free(vm->mem);
    free(vm);
}

const char *exen_error(ExenVM *vm)
{
    if (vm->halted)
        return vm->halt_reason;
    return vm->error;
}

static int va_to_file(const uint8_t *exe, size_t len, uint32_t va, size_t *off)
{
    uint32_t pe = rd32(exe + 0x3c);
    uint16_t nsec = rd16(exe + pe + 6), opt = rd16(exe + pe + 20);
    uint32_t base = rd32(exe + pe + 24 + 28);
    for (uint16_t s = 0; s < nsec; s++) {
        const uint8_t *sh = exe + pe + 24 + opt + 40u * s;
        uint32_t rva = rd32(sh + 12), rsize = rd32(sh + 16), roff = rd32(sh + 20);
        if (va >= base + rva && va < base + rva + rsize) {
            *off = roff + (va - base - rva);
            return *off < len ? 0 : -1;
        }
    }
    return -1;
}

int exen_load_framework(ExenVM *vm, const uint8_t *exe, size_t len)
{
    size_t off, size;
    if (exen_find_framework(exe, len, &off, &size) < 0) {
        set_error(vm, "framework not found in simulator executable");
        return -1;
    }
    if (load_image(vm, exe + off, size, 1) < 0) {
        set_error(vm, "framework image is malformed");
        return -1;
    }
    size_t t;
    if (va_to_file(exe, len, 0x4567f8u, &t) < 0 || t + 513 * 2 > len) {
        set_error(vm, "math table not found in simulator executable");
        return -1;
    }
    for (int i = 0; i < 513; i++)
        vm->math.cos_table[i] = rd16(exe + t + 2 * i);
    return 0;
}

int exen_load_game(ExenVM *vm, const uint8_t *exn, size_t len)
{
    if (len < 0x40 || memcmp(exn, "NEXE", 4) != 0) {
        set_error(vm, "not an ExEn game (missing NEXE header)");
        return -1;
    }
    uint32_t n = rd32(exn + 0x34);
    if (0x38 + 4ull * (n + 1) > len) {
        set_error(vm, "truncated resource table");
        return -1;
    }
    uint32_t code = rd32(exn + 0x38 + 4 * n);
    if (code >= len || memcmp(exn + code, "4CVP", 4) != 0) {
        set_error(vm, "code chunk missing");
        return -1;
    }
    if (load_image(vm, exn + code, len - code, 0) < 0) {
        set_error(vm, "game code is malformed");
        return -1;
    }
    free(vm->exn);
    vm->exn = malloc(len);
    if (!vm->exn) {
        set_error(vm, "out of memory");
        return -1;
    }
    memcpy(vm->exn, exn, len);
    vm->exn_len = len;
    memcpy(vm->main_class, exn + 0x14, 32);
    vm->main_class[32] = 0;
    if (exen_finalize_classes(vm) < 0 || heap_init(vm) < 0) {
        set_error(vm, "class setup failed");
        return -1;
    }
    return 0;
}

static int call_bootstrap(ExenVM *vm, uint32_t hash, uint32_t *args, int nargs)
{
    if (vm->halted)
        return -1;
    int found;
    uint32_t m = find_method(vm, vm->cls_bootstrap, hash, 0, &found);
    if (!m) {
        vm_halt(vm, "Bootstrap entry %08x missing", hash);
        return -1;
    }
    if (vm_init_class(vm, vm->cls_bootstrap) < 0)
        return -1;
    int r = vm_call(vm, found, m, args, nargs, NULL);
    if (vm->pending) {
        int c = obj_cls(vm, vm->pending);
        vm->pending = 0;
        vm_halt(vm, "uncaught exception %s", vm->classes[c].name ? vm->classes[c].name : "?");
        return -1;
    }
    /* The original collects after every callback; we collect when the heap is half full. */
    if (vm->heap_used > (vm->heap_end - vm->heap_start) / 2)
        heap_gc(vm);
    return r < 0 ? -1 : 0;
}

int exen_boot(ExenVM *vm)
{
    int n = (int)strlen(vm->main_class);
    ref_t name = new_char_array(vm, (const uint8_t *)vm->main_class, n);
    if (!name) {
        set_error(vm, "out of memory");
        return -1;
    }
    uint32_t arg = name;
    root_push(vm, name);
    int r = call_bootstrap(vm, BS_INIT, &arg, 1);
    root_pop(vm, 1);
    return r;
}

int exen_tick(ExenVM *vm) { return call_bootstrap(vm, BS_TICK, NULL, 0); }

int exen_key(ExenVM *vm, int keycode, int pressed)
{
    uint32_t arg = (uint32_t)keycode;
    return call_bootstrap(vm, pressed ? BS_KEYPRESS : BS_KEYRELEASE, &arg, 1);
}

int exen_halted(ExenVM *vm) { return vm->halted; }
