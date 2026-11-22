/* Class image loading (docs/re/CVP.md §1-§4, §8). */
#include <stdlib.h>
#include <string.h>

#include "vm.h"

#define MAGIC_4CVP 0x50564334u /* "4CVP" little-endian */

static uint32_t align4(uint32_t x) { return (x + 3u) & ~3u; }

static int add_class(ExenVM *vm, uint32_t block, uint32_t size)
{
    ExClass *nc = realloc(vm->classes, sizeof *nc * (size_t)(vm->nclasses + 1));
    if (!nc)
        return -1;
    vm->classes = nc;
    ExClass *c = &vm->classes[vm->nclasses++];
    memset(c, 0, sizeof *c);
    const uint8_t *b = P(vm, block);
    c->block = block;
    c->size = size;
    c->hash = rd32(b + 0x0c);
    c->flags = rd16(b + 0x14);
    c->nstatics = rd16(b + 0x18);
    uint16_t name_off = rd16(b + 0x16);
    if (name_off && name_off + 2u <= size) {
        uint16_t n = rd16(b + name_off);
        char *s = malloc(n + 1u);
        if (s) {
            memcpy(s, b + name_off + 2, n);
            s[n] = 0;
        }
        c->name = s;
    }
    c->super = -1;
    if (!(c->flags & ACC_INTERFACE)) {
        uint16_t sref = rd16(b + 0x28);
        if (sref)
            c->super = (int16_t)rd16(b + sref);
        c->ninst = rd16(b + 0x2a);
    }
    return 0;
}

/* Walk a run of 4CVP blocks exactly like gensimu's loaders (0x406fc8 / 0x40713f). */
int load_image(ExenVM *vm, const uint8_t *img, size_t len, int is_framework)
{
    uint32_t base = align4(vm->code_end);
    if (base + len > vm->mem_size)
        return -1;
    memcpy(P(vm, base), img, len);
    vm->code_end = base + (uint32_t)len;
    size_t p = 0;
    int count = 0;
    while (p + 8 <= len) {
        if (rd32(img + p) != MAGIC_4CVP)
            return -1;
        uint32_t size = rd16(img + p + 4);
        if (size < 0x28 || p + size > len)
            return -1;
        if (add_class(vm, base + (uint32_t)p, size) < 0)
            return -1;
        count++;
        p = align4((uint32_t)(p + size));
        while (p + 4 <= len && rd32(img + p) != MAGIC_4CVP)
            p += 4;
    }
    if (is_framework)
        vm->nfw = count;
    return count;
}

static void setbit(uint8_t *m, int i) { m[i >> 3] |= (uint8_t)(1u << (i & 7)); }

static int is_ref_type(uint16_t t) { return (t >> 8) != 0 || (t & 0xff) == T_OBJECT; }

/* Compute flat instance layout and GC reference maps. Supers are laid out first. */
static int layout_class(ExenVM *vm, int i, int depth)
{
    ExClass *c = &vm->classes[i];
    if (c->inst_refmap || depth > 64)
        return 0;
    int base = 0;
    const uint8_t *super_map = NULL;
    if (c->super >= 0 && c->super < vm->nclasses && c->super != i) {
        layout_class(vm, c->super, depth + 1);
        base = vm->classes[c->super].inst_total;
        super_map = vm->classes[c->super].inst_refmap;
    }
    c->inst_base = (uint16_t)base;
    c->inst_total = (uint16_t)(base + c->ninst);
    c->inst_refmap = calloc((size_t)c->inst_total / 8 + 1, 1);
    c->static_refmap = calloc((size_t)c->nstatics / 8 + 1, 1);
    if (!c->inst_refmap || !c->static_refmap)
        return -1;
    if (super_map)
        memcpy(c->inst_refmap, super_map, (size_t)base / 8 + 1);
    const uint8_t *b = BLK(vm, i);
    uint16_t ft = rd16(b + 0x1e);
    if (ft) {
        uint16_t n = rd16(b + ft);
        uint32_t r = align4(ft + 2u);
        for (uint16_t k = 0; k < n; k++, r += 12) {
            uint16_t fl = rd16(b + r + 4), ty = rd16(b + r + 6), slot = rd16(b + r + 8);
            if (!is_ref_type(ty))
                continue;
            if (fl & ACC_STATIC) {
                if (slot < c->nstatics)
                    setbit(c->static_refmap, slot);
            } else if (base + slot < c->inst_total) {
                setbit(c->inst_refmap, base + slot);
            }
        }
    }
    return 0;
}

int exen_finalize_classes(ExenVM *vm);
int exen_finalize_classes(ExenVM *vm)
{
    for (int i = 0; i < vm->nclasses; i++)
        if (layout_class(vm, i, 0) < 0)
            return -1;
    vm->cls_object = find_class(vm, exen_crc32("java.lang.Object", 16));
    vm->cls_string = find_class(vm, exen_crc32("java.lang.String", 16));
    vm->cls_class = find_class(vm, exen_crc32("java.lang.Class", 15));
    vm->cls_bootstrap = find_class(vm, exen_crc32("vm.sys.Bootstrap", 16));
    vm->cls_gamelet = find_class(vm, exen_crc32("exen.Gamelet", 12));
    vm->cls_stringbuffer = find_class(vm, exen_crc32("java.lang.StringBuffer", 22));
    return (vm->cls_object < 0 || vm->cls_string < 0 || vm->cls_bootstrap < 0) ? -1 : 0;
}

int find_class(ExenVM *vm, uint32_t hash)
{
    for (int i = 0; i < vm->nclasses; i++)
        if (vm->classes[i].hash == hash)
            return i;
    return -1;
}

static uint32_t table_find(ExenVM *vm, int cls, int hdr_off, uint32_t hash)
{
    const uint8_t *b = BLK(vm, cls);
    uint16_t t = rd16(b + hdr_off);
    if (!t)
        return 0;
    uint16_t n = rd16(b + t);
    uint32_t r = align4(t + 2u);
    for (uint16_t k = 0; k < n; k++, r += 12)
        if (rd32(b + r) == hash)
            return r;
    return 0;
}

uint32_t find_method(ExenVM *vm, int cls, uint32_t hash, int walk_super, int *found_cls)
{
    for (int guard = 0; cls >= 0 && cls < vm->nclasses && guard < 64; guard++) {
        uint32_t r = table_find(vm, cls, 0x20, hash);
        if (r) {
            if (found_cls)
                *found_cls = cls;
            return r;
        }
        if (!walk_super)
            break;
        cls = vm->classes[cls].super;
    }
    return 0;
}

uint32_t find_field(ExenVM *vm, int cls, uint32_t hash, int walk_super, int *found_cls)
{
    for (int guard = 0; cls >= 0 && cls < vm->nclasses && guard < 64; guard++) {
        uint32_t r = table_find(vm, cls, 0x1e, hash);
        if (r) {
            if (found_cls)
                *found_cls = cls;
            return r;
        }
        if (!walk_super)
            break;
        cls = vm->classes[cls].super;
    }
    return 0;
}

int is_subclass(ExenVM *vm, int cls, int of)
{
    for (int guard = 0; cls >= 0 && cls < vm->nclasses && guard < 64; guard++) {
        if (cls == of)
            return 1;
        /* interfaces listed at +0x1c */
        const uint8_t *b = BLK(vm, cls);
        uint16_t il = rd16(b + 0x1c);
        if (il) {
            uint16_t n = rd16(b + il);
            for (uint16_t k = 0; k < n; k++) {
                uint16_t ref = rd16(b + il + 2 + 2 * k);
                int ic = rd16(b + ref);
                if (ic != cls && is_subclass(vm, ic, of))
                    return 1;
            }
        }
        cls = vm->classes[cls].super;
    }
    return 0;
}

/* ------------------------------------------------------------ containers */

/* Locate the framework image in gensimu.exe: 0x40713f does `mov [ebp-0xc], len;
 * mov [ebp-0x10], va` (c7 45 f4 / c7 45 f0). Map the VA through the PE section table. */
int exen_find_framework(const uint8_t *exe, size_t len, size_t *off, size_t *size);
int exen_find_framework(const uint8_t *exe, size_t len, size_t *off, size_t *size)
{
    if (len < 0x200 || exe[0] != 'M' || exe[1] != 'Z')
        return -1;
    uint32_t pe = rd32(exe + 0x3c);
    if (pe + 0xf8 > len)
        return -1;
    uint16_t nsec = rd16(exe + pe + 6), opt = rd16(exe + pe + 20);
    uint32_t base = rd32(exe + pe + 24 + 28);
    for (size_t i = 0; i + 14 <= len; i++) {
        if (exe[i] != 0xc7 || exe[i + 1] != 0x45 || exe[i + 2] != 0xf4 ||
            exe[i + 7] != 0xc7 || exe[i + 8] != 0x45 || exe[i + 9] != 0xf0)
            continue;
        uint32_t flen = rd32(exe + i + 3), va = rd32(exe + i + 10);
        for (uint16_t s = 0; s < nsec; s++) {
            const uint8_t *sh = exe + pe + 24 + opt + 40u * s;
            uint32_t vsize = rd32(sh + 8), rva = rd32(sh + 12), rsize = rd32(sh + 16), roff = rd32(sh + 20);
            uint32_t lim = vsize < rsize ? vsize : rsize;
            if (va >= base + rva && va < base + rva + lim) {
                size_t fo = roff + (va - base - rva);
                if (fo + flen <= len && rd32(exe + fo) == MAGIC_4CVP) {
                    *off = fo;
                    *size = flen;
                    return 0;
                }
            }
        }
    }
    return -1;
}
