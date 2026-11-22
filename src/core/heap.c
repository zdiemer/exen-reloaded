/* Heap allocator and garbage collector.
 *
 * The original combines 8-bit reference counts with per-object "stack levels" and only collects
 * when an allocation fails (docs/re/OPCODES.md, Garbage collection). That scheme leaks cycles and
 * can corrupt its counts. We use a mark-sweep collector instead: precise for heap objects (field
 * types come from the class tables) and conservative for the VM operand stacks and locals.
 * Anything the original keeps alive while reachable, this keeps alive too.
 */
#include <stdlib.h>
#include <string.h>

#include "vm.h"

#define OBJMAP_BIT(vm, off) (((off) - (vm)->heap_start) >> 3)

static void objmap_set(ExenVM *vm, uint32_t off, int on)
{
    uint32_t b = OBJMAP_BIT(vm, off);
    if (on)
        vm->objmap[b >> 3] |= (uint8_t)(1u << (b & 7));
    else
        vm->objmap[b >> 3] &= (uint8_t)~(1u << (b & 7));
}

int heap_is_object(ExenVM *vm, uint32_t off)
{
    if (off < vm->heap_start || off >= vm->heap_end || (off & 7))
        return 0;
    uint32_t b = OBJMAP_BIT(vm, off);
    return (vm->objmap[b >> 3] >> (b & 7)) & 1;
}

int heap_init(ExenVM *vm)
{
    vm->heap_start = (vm->code_end + 7u) & ~7u;
    vm->heap_end = vm->mem_size & ~7u;
    if (vm->heap_end <= vm->heap_start + 64)
        return -1;
    size_t bits = (vm->heap_end - vm->heap_start) >> 3;
    free(vm->objmap);
    vm->objmap = calloc((bits + 7) / 8, 1);
    if (!vm->objmap)
        return -1;
    /* one free block spanning the heap; free blocks chain through +4 */
    uint32_t sz = vm->heap_end - vm->heap_start;
    wr32(P(vm, vm->heap_start), sz | HDR_FREE);
    wr32(P(vm, vm->heap_start + 4), 0);
    vm->free_list = vm->heap_start;
    vm->heap_used = 0;
    return 0;
}

int elem_size(uint16_t tw)
{
    if (tw >> 8)
        return 4; /* array of arrays */
    switch (tw & 0xff) {
    case T_BYTE: case T_CHAR: case T_CHAR2: case T_BOOL: case T_BOOL_E:
        return 1; /* chars are 8-bit; booleans are accessed as bytes (caload/baload use stride 1) */
    case T_SHORT:
        return 2;
    case T_LONG:
        return 8;
    case T_INT: case T_OBJECT:
        return 4;
    default:
        return 1 << (((tw & 0xff) >> 2) & 3); /* the original's generic formula */
    }
}

int elem_is_ref(uint16_t tw)
{
    return (tw >> 8) != 0 || (tw & 0xff) == T_OBJECT;
}

static ref_t try_alloc(ExenVM *vm, uint32_t need)
{
    uint32_t prev = 0, cur = vm->free_list;
    while (cur) {
        uint32_t hdr = rd32(P(vm, cur));
        uint32_t sz = hdr & HDR_SIZE_MASK;
        uint32_t next = rd32(P(vm, cur + 4));
        if (sz >= need) {
            uint32_t rest = sz - need;
            uint32_t repl = next;
            if (rest >= 16) {
                uint32_t tail = cur + need;
                wr32(P(vm, tail), rest | HDR_FREE);
                wr32(P(vm, tail + 4), next);
                repl = tail;
            } else {
                need = sz; /* absorb the sliver */
            }
            if (prev)
                wr32(P(vm, prev + 4), repl);
            else
                vm->free_list = repl;
            memset(P(vm, cur), 0, need);
            wr32(P(vm, cur), need);
            objmap_set(vm, cur, 1);
            vm->heap_used += need;
            return cur;
        }
        prev = cur;
        cur = next;
    }
    return 0;
}

ref_t heap_alloc(ExenVM *vm, uint32_t payload, uint16_t cls, uint16_t kind)
{
    uint32_t need = (8 + payload + 7) & ~7u;
    if (need < 16)
        need = 16;
    ref_t r = try_alloc(vm, need);
    if (!r) {
        heap_gc(vm);
        r = try_alloc(vm, need);
        if (!r)
            return 0;
    }
    wr16(P(vm, r + 4), cls);
    wr16(P(vm, r + 6), kind);
    return r;
}

/* ---------------------------------------------------------------- mark */

typedef struct { ref_t *items; size_t n, cap; } MarkStack;

static void mark_push(ExenVM *vm, MarkStack *ms, uint32_t v)
{
    if (!heap_is_object(vm, v))
        return;
    uint8_t *h = P(vm, v);
    uint32_t hdr = rd32(h);
    if (hdr & (HDR_MARK | HDR_FREE))
        return;
    wr32(h, hdr | HDR_MARK);
    if (ms->n == ms->cap) {
        size_t nc = ms->cap ? ms->cap * 2 : 1024;
        ref_t *ni = realloc(ms->items, nc * sizeof *ni);
        if (!ni)
            return; /* out of host memory: object stays marked, children may be missed */
        ms->items = ni;
        ms->cap = nc;
    }
    ms->items[ms->n++] = v;
}

static void scan_slots(ExenVM *vm, MarkStack *ms, const uint32_t *slots, const uint8_t *map, int n)
{
    if (!map)
        return;
    for (int i = 0; i < n; i++)
        if (map[i >> 3] & (1u << (i & 7)))
            mark_push(vm, ms, slots[i]);
}

static void mark_children(ExenVM *vm, MarkStack *ms, ref_t r)
{
    uint16_t kind = obj_kind(vm, r);
    uint16_t cls = obj_cls(vm, r);
    if (kind & EXEN_KIND_ARRAY) {
        if (elem_is_ref(kind & 0x7fff)) {
            uint16_t n = arr_len(vm, r);
            const uint8_t *d = arr_data(vm, r);
            for (uint16_t i = 0; i < n; i++)
                mark_push(vm, ms, rd32(d + 4u * i));
        }
    } else if (kind == EXEN_KIND_OBJECT && cls < vm->nclasses) {
        const ExClass *c = &vm->classes[cls];
        scan_slots(vm, ms, obj_slots(vm, r), c->inst_refmap, c->inst_total);
    } else if (kind == EXEN_KIND_STATICS && cls < vm->nclasses) {
        const ExClass *c = &vm->classes[cls];
        scan_slots(vm, ms, obj_slots(vm, r), c->static_refmap, c->nstatics);
    }
}

void heap_gc(ExenVM *vm)
{
    MarkStack ms = {0};
    /* roots: static areas, host-registered roots, a pending exception, the VM stack */
    for (int i = 0; i < vm->nclasses; i++)
        if (vm->classes[i].statics)
            mark_push(vm, &ms, vm->classes[i].statics);
    for (int i = 0; i < vm->nroots; i++)
        mark_push(vm, &ms, vm->roots[i]);
    mark_push(vm, &ms, vm->pending);
    uint32_t *top = vm_stack_top(vm);
    for (uint32_t *p = vm->stack; p < top; p++)
        mark_push(vm, &ms, *p); /* conservative */
    while (ms.n)
        mark_children(vm, &ms, ms.items[--ms.n]);
    free(ms.items);

    /* sweep: walk every block, clear marks, coalesce free runs into a fresh free list */
    uint32_t off = vm->heap_start, run = 0, tail = 0;
    vm->free_list = 0;
    vm->heap_used = 0;
    while (off < vm->heap_end) {
        uint32_t hdr = rd32(P(vm, off));
        uint32_t sz = hdr & HDR_SIZE_MASK;
        if (sz == 0)
            break; /* corrupt heap; stop rather than loop */
        int live = !(hdr & HDR_FREE) && (hdr & HDR_MARK);
        if (live) {
            wr32(P(vm, off), hdr & ~HDR_MARK);
            vm->heap_used += sz;
            run = 0;
        } else {
            objmap_set(vm, off, 0);
            if (run) {
                uint32_t rs = rd32(P(vm, run)) & HDR_SIZE_MASK;
                wr32(P(vm, run), (rs + sz) | HDR_FREE);
            } else {
                run = off;
                wr32(P(vm, off), sz | HDR_FREE);
                wr32(P(vm, off + 4), 0);
                if (tail)
                    wr32(P(vm, tail + 4), off);
                else
                    vm->free_list = off;
                tail = off;
            }
        }
        off += sz;
    }
}
