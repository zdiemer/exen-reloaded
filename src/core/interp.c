/* The bytecode interpreter (docs/re/OPCODES.md).
 *
 * Differences from the original that are deliberate, each also noted at the opcode:
 *  - lreturn is implemented correctly (the original's is broken but never used).
 *  - instanceof on an array type pushes a result (the original pushes nothing; never used).
 *  - array allocation sizes are not truncated to 16 bits (the original corrupts its heap).
 * Everything else, including the non-JVM edge cases (MIN_INT / -1 throws, unmasked long shift
 * counts, zero-extended byte fields, 8-bit chars, inclusive exception-range ends), is kept.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vm.h"

static uint32_t a2(uint32_t pc) { return (pc + 1u) & ~1u; }
static uint32_t a4(uint32_t pc) { return (pc + 3u) & ~3u; }

static int64_t getl(const uint32_t *p) { return (int64_t)((uint64_t)p[0] | (uint64_t)p[1] << 32); }
static void putl(uint32_t *p, int64_t v)
{
    p[0] = (uint32_t)(uint64_t)v;
    p[1] = (uint32_t)((uint64_t)v >> 32);
}

void vm_halt(ExenVM *vm, const char *fmt, ...)
{
    if (vm->halted)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->halt_reason, sizeof vm->halt_reason, fmt, ap);
    va_end(ap);
    vm->halted = 1;
    if (vm->host.log) {
        char buf[300];
        snprintf(buf, sizeof buf, "VM halted: %s", vm->halt_reason);
        vm->host.log(vm->host.user, buf);
    }
}

uint32_t *vm_stack_top(ExenVM *vm)
{
    uint32_t *top = vm->fp >= 0 ? vm->frames[vm->fp].sp : vm->stack;
    if (vm->native_top && vm->native_top > top)
        top = vm->native_top;
    return top;
}

/* ------------------------------------------------------------ objects */

ref_t vm_new_object(ExenVM *vm, int cls)
{
    const ExClass *c = &vm->classes[cls];
    return heap_alloc(vm, 4u * c->inst_total, (uint16_t)cls, EXEN_KIND_OBJECT);
}

ref_t vm_new_array(ExenVM *vm, uint16_t tw, uint16_t elem_cls, int32_t n)
{
    if (n < 0 || n > 0xffff)
        return 0;
    ref_t r = heap_alloc(vm, 4u + (uint32_t)n * (uint32_t)elem_size(tw), elem_cls,
                         (uint16_t)(EXEN_KIND_ARRAY | (tw & 0x7fff)));
    if (r)
        wr16(P(vm, r + 10), (uint16_t)n);
    return r;
}

/* ------------------------------------------------------------ classes */

int vm_init_class(ExenVM *vm, int cls)
{
    ExClass *c = &vm->classes[cls];
    if (c->statics)
        return 0;
    const uint8_t *b = BLK(vm, cls);
    ref_t st = heap_alloc(vm, 4u * c->nstatics, (uint16_t)cls, EXEN_KIND_STATICS);
    if (!st) {
        vm_halt(vm, "out of memory initialising class %d", cls);
        return -1;
    }
    c->statics = st; /* set first: <clinit> may touch its own statics */
    uint16_t il = rd16(b + 0x1c);
    if (il) {
        uint16_t n = rd16(b + il);
        for (uint16_t k = 0; k < n; k++) {
            int ic = rd16(b + rd16(b + il + 2 + 2 * k));
            if (ic < vm->nclasses && vm_init_class(vm, ic) < 0)
                return -1;
        }
    }
    uint16_t clinit = rd16(b + 0x1a);
    if (clinit) {
        if (vm_call(vm, cls, clinit, NULL, 0, NULL) < 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ invoke */

static uint32_t mcode(ExenVM *vm, const ExFrame *f) { (void)vm; return f->code; }

/* Push a frame for a bytecode method, or run a native. args already popped from the caller.
 * Returns 1 if a frame was pushed, 0 if a native ran, -1 on error. */
int vm_invoke(ExenVM *vm, int cls, uint32_t mrec, uint32_t *args, int has_this)
{
    const uint8_t *b = BLK(vm, cls);
    uint16_t flags = rd16(b + mrec + 4);
    int nargs = rd16(b + mrec + 6) + (has_this ? 1 : 0);
    uint16_t code = rd16(b + mrec + 8);
    if (flags & ACC_NATIVE) {
        uint32_t idx = rd32(b + code);
        uint32_t *saved_top = vm->native_top;
        vm->native_top = args + nargs + 2;
        vm->native_exc = 0;
        int n = natives_call(vm, idx, args);
        vm->native_top = saved_top;
        vm->last_ret_words = n;
        if (vm->fp >= 0 && !vm->frames[vm->fp].base_of_run_pending)
            vm->frames[vm->fp].sp = args + n;
        if (vm->native_exc) {
            uint32_t h = vm->native_exc;
            vm->native_exc = 0;
            vm_throw_hash(vm, h);
        } else if (vm->pending) {
            ref_t e = vm->pending;
            vm->pending = 0;
            vm_throw(vm, e);
        }
        return 0;
    }
    if (!code) {
        vm_halt(vm, "call to abstract method %08x in class %d", rd32(b + mrec), cls);
        return -1;
    }
    uint16_t max_stack = rd16(b + code), max_locals = rd16(b + code + 2);
    if (vm->fp + 1 >= EXEN_MAX_FRAMES ||
        args + max_locals + max_stack + 4 >= vm->stack + EXEN_STACK_WORDS) {
        vm_throw_hash(vm, H_STACKOVF);
        return -1;
    }
    for (int i = nargs; i < max_locals; i++)
        args[i] = 0;
    ExFrame *f = &vm->frames[++vm->fp];
    f->cls = (uint16_t)cls;
    f->pc = (uint16_t)(code + 6);
    f->mrec = mrec;
    f->code = code;
    f->loc = args;
    f->sp = args + max_locals;
    f->limit = f->sp + max_stack;
    f->jsr = 0;
    f->run_base = 0;
    f->base_of_run_pending = 0;
    return 1;
}

/* ------------------------------------------------------------ exceptions */

void vm_throw(ExenVM *vm, ref_t exc)
{
    int ecls = obj_cls(vm, exc);
    while (vm->fp >= 0) {
        ExFrame *f = &vm->frames[vm->fp];
        const uint8_t *b = BLK(vm, f->cls);
        uint16_t exo = rd16(b + mcode(vm, f) + 4);
        if (exo) {
            uint16_t n = rd16(b + exo);
            for (uint16_t k = 0; k < n; k++) {
                const uint8_t *e = b + exo + 2 + 8 * k;
                uint16_t start = rd16(e), end = rd16(e + 2), handler = rd16(e + 4), cref = rd16(e + 6);
                if (f->pc < start || f->pc > end) /* end is inclusive */
                    continue;
                if (cref && !is_subclass(vm, ecls, rd16(b + cref)))
                    continue;
                f->pc = handler;
                *f->sp++ = exc; /* the operand stack is not reset */
                return;
            }
        }
        if (f->run_base) {
            /* leaving a nested run: let the C caller propagate it outward */
            vm->pending = exc;
            vm->fp--;
            return;
        }
        vm->fp--;
    }
    vm->pending = exc;
}

void vm_throw_hash(ExenVM *vm, uint32_t hash)
{
    if (vm->in_native) {
        if (!vm->native_exc)
            vm->native_exc = hash;
        return;
    }
    int cls = find_class(vm, hash);
    if (cls < 0) {
        /* The original cannot construct exception classes the framework lacks; it stops
         * the VM (0x40dccb). Only OutOfMemoryError exists in the framework. */
        vm_halt(vm, "uncatchable exception %08x", hash);
        return;
    }
    if (vm_init_class(vm, cls) < 0)
        return;
    ref_t e = vm_new_object(vm, cls);
    if (!e) {
        vm_halt(vm, "out of memory creating exception %08x", hash);
        return;
    }
    uint16_t ctor = rd16(BLK(vm, cls) + 0x2c);
    if (ctor) {
        uint32_t a = e;
        vm->roots[vm->nroots++] = e;
        vm_call(vm, cls, ctor, &a, 1, NULL);
        vm->nroots--;
    }
    vm_throw(vm, e);
}

/* ------------------------------------------------------------ interpreter */

static void run(ExenVM *vm);

/* Call a method from C (host event, <clinit>, a native) and run it to completion.
 * args are copied onto the VM stack. Returns the number of result words, or -1 if the VM
 * halted or an exception escaped (left in vm->pending). */
int vm_call(ExenVM *vm, int cls, uint32_t mrec, uint32_t *args, int nargs, uint32_t *ret)
{
    if (vm->halted)
        return -1;
    uint32_t *base = vm_stack_top(vm);
    if (base + nargs + 8 >= vm->stack + EXEN_STACK_WORDS) {
        vm_halt(vm, "VM stack exhausted");
        return -1;
    }
    for (int i = 0; i < nargs; i++)
        base[i] = args[i];
    const uint8_t *b = BLK(vm, cls);
    uint16_t flags = rd16(b + mrec + 4);
    int has_this = !(flags & ACC_STATIC);
    int saved_in_native = vm->in_native;
    vm->in_native = 0;
    int outer_fp = vm->fp;
    if (outer_fp >= 0)
        vm->frames[outer_fp].base_of_run_pending = 1;
    int r = vm_invoke(vm, cls, mrec, base, has_this);
    int nret;
    if (r == 1) {
        vm->frames[vm->fp].run_base = 1;
        run(vm);
    }
    if (outer_fp >= 0)
        vm->frames[outer_fp].base_of_run_pending = 0;
    vm->in_native = saved_in_native;
    if (vm->halted)
        return -1;
    if (vm->pending)
        return -1;
    nret = vm->last_ret_words;
    if (r == 0)
        nret = vm->last_ret_words;
    if (ret)
        for (int i = 0; i < nret; i++)
            ret[i] = base[i];
    return nret;
}

static int do_return(ExenVM *vm, int words)
{
    ExFrame *f = &vm->frames[vm->fp];
    uint32_t *res = f->sp - words;
    for (int i = 0; i < words; i++)
        f->loc[i] = res[i];
    int was_base = f->run_base;
    uint32_t *dst = f->loc;
    vm->fp--;
    vm->last_ret_words = words;
    if (!was_base && vm->fp >= 0)
        vm->frames[vm->fp].sp = dst + words;
    return was_base;
}

/* Array access helper: pops index and array (and leaves value handling to the caller).
 * Returns a pointer to the element or NULL after throwing. mask == 0: any array. */
static uint8_t *arr_elem(ExenVM *vm, ref_t a, int32_t i, uint8_t mask, int esz)
{
    if (!a) {
        vm_throw_hash(vm, H_NPE);
        return NULL;
    }
    if (!heap_is_object(vm, a) || !is_array(vm, a) || (mask && (arr_type(vm, a) & mask) != mask)) {
        vm_throw_hash(vm, H_EXCEPTION);
        return NULL;
    }
    if (i < 0 || i >= arr_len(vm, a)) {
        vm_throw_hash(vm, H_AIOOBE);
        return NULL;
    }
    return arr_data(vm, a) + (uint32_t)i * (uint32_t)esz;
}

/* Field value transfer keyed on the type word (same switch in all of f3..fa). */
static void field_get(uint32_t **spp, uint32_t *slot, uint16_t ty)
{
    uint32_t *sp = *spp;
    uint8_t t = ty & 0xff;
    if ((ty >> 8) || t == T_OBJECT || t == T_INT) {
        *sp++ = *slot;
    } else if (t == T_SHORT) {
        *sp++ = (uint32_t)(int32_t)(int16_t)(*slot & 0xffff);
    } else if (t == T_BYTE || t == T_BOOL || t == T_CHAR || t == T_CHAR2) {
        *sp++ = *slot & 0xff; /* zero-extended, even for byte (docs/re/OPCODES.md) */
    } else if (t == T_LONG) {
        *sp++ = slot[0];
        *sp++ = slot[1];
    }
    *spp = sp;
}

static void field_put(uint32_t **spp, uint32_t *slot, uint16_t ty)
{
    uint32_t *sp = *spp;
    uint8_t t = ty & 0xff;
    if ((ty >> 8) || t == T_OBJECT || t == T_INT || t == T_SHORT) {
        *slot = *--sp;
    } else if (t == T_BYTE || t == T_BOOL || t == T_CHAR || t == T_CHAR2) {
        *slot = (*slot & ~0xffu) | (*--sp & 0xff); /* low byte only */
    } else if (t == T_LONG) {
        sp -= 2;
        slot[0] = sp[0];
        slot[1] = sp[1];
    }
    *spp = sp;
}

static int field_words(uint16_t ty)
{
    uint8_t t = ty & 0xff;
    if (ty >> 8)
        return 1;
    if (t == T_LONG)
        return 2;
    if (t == T_OBJECT || t == T_INT || t == T_SHORT || t == T_BYTE || t == T_BOOL ||
        t == T_CHAR || t == T_CHAR2)
        return 1;
    return 0;
}

static ref_t build_multi(ExenVM *vm, uint16_t tw, uint16_t ecls, const int32_t *dims, int nd)
{
    ref_t a = vm_new_array(vm, tw, ecls, dims[0]);
    if (!a || nd == 1)
        return a;
    vm->roots[vm->nroots++] = a;
    for (int32_t i = 0; i < dims[0]; i++) {
        ref_t sub = build_multi(vm, (uint16_t)(tw - 0x100), ecls, dims + 1, nd - 1);
        if (!sub) {
            vm->nroots--;
            return 0;
        }
        wr32(arr_data(vm, a) + 4u * (uint32_t)i, sub);
    }
    vm->nroots--;
    return a;
}

static int switch_target(const uint8_t *b, uint32_t pc, uint8_t op, int32_t v)
{
    uint32_t p = a2(pc);
    uint16_t def = rd16(b + p);
    if (op == 0xaa) { /* tableswitch */
        uint32_t q = a4(p + 2);
        int32_t lo = (int32_t)rd32(b + q), hi = (int32_t)rd32(b + q + 4);
        if (v < lo || v > hi)
            return def;
        return rd16(b + q + 8 + 2u * (uint32_t)(v - lo));
    }
    if (op == 0xab) { /* lookupswitch */
        uint16_t n = rd16(b + p + 2);
        uint32_t k = a4(p + 4);
        for (uint16_t i = 0; i < n; i++)
            if ((int32_t)rd32(b + k + 4u * i) == v)
                return rd16(b + k + 4u * n + 2u * i);
        return def;
    }
    if (op == 0xcd) { /* tableswitch_s16 */
        int16_t lo = (int16_t)rd16(b + p + 2), hi = (int16_t)rd16(b + p + 4);
        if (v < lo || v > hi)
            return def;
        return rd16(b + p + 6 + 2u * (uint32_t)(v - lo));
    }
    /* 0xcc lookupswitch_s16 */
    uint16_t n = rd16(b + p + 2);
    for (uint16_t i = 0; i < n; i++)
        if ((int32_t)(int16_t)rd16(b + p + 4 + 2u * i) == v)
            return rd16(b + p + 4 + 2u * n + 2u * i);
    return def;
}

#define F (&vm->frames[vm->fp])
#define POP() (*--F->sp)
#define PUSH(v) (*F->sp++ = (uint32_t)(v))
#define THROW(h) do { vm_throw_hash(vm, (h)); goto next; } while (0)
#define U16AT(o) rd16(b + (o))

static void run(ExenVM *vm)
{
    int base_fp = vm->fp;
    while (!vm->halted && vm->fp >= base_fp) {
        ExFrame *f = F;
        const uint8_t *b = BLK(vm, f->cls);
        uint32_t pc = f->pc;
        uint8_t op = b[pc++];
        f->pc = (uint16_t)pc;
        switch (op) {
        case 0x00: break;
        case 0x01: PUSH(0); break;
        case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
            PUSH((int32_t)op - 3); break;
        case 0x09: case 0x0a: putl(f->sp, op - 9); f->sp += 2; break;
        case 0x10: PUSH((int32_t)(int8_t)b[pc]); f->pc = (uint16_t)(pc + 1); break;
        case 0x11: pc = a2(pc); PUSH((int32_t)(int16_t)U16AT(pc)); f->pc = (uint16_t)(pc + 2); break;
        case 0x12: pc = a2(pc); PUSH(rd32(b + U16AT(pc))); f->pc = (uint16_t)(pc + 2); break;
        case 0x14: {
            pc = a2(pc);
            uint16_t o = U16AT(pc);
            f->sp[0] = rd32(b + o);
            f->sp[1] = rd32(b + o + 4);
            f->sp += 2;
            f->pc = (uint16_t)(pc + 2);
            break;
        }
        case 0x19: PUSH(f->loc[b[pc]]); f->pc = (uint16_t)(pc + 1); break;
        case 0x2a: case 0x2b: case 0x2c: case 0x2d: PUSH(f->loc[op - 0x2a]); break;
        case 0x3a: f->loc[b[pc]] = POP(); f->pc = (uint16_t)(pc + 1); break;
        case 0x4b: case 0x4c: case 0x4d: case 0x4e: f->loc[op - 0x4b] = POP(); break;

        /* array loads */
        case 0x2e: case 0x2f: case 0x32: case 0x33: case 0x34: case 0x35: {
            int32_t i = (int32_t)POP();
            ref_t a = POP();
            static const uint8_t mask[] = {[0x2e - 0x2e] = T_INT, [0x2f - 0x2e] = T_LONG,
                [0x32 - 0x2e] = 0, [0x33 - 0x2e] = T_BYTE, [0x34 - 0x2e] = T_CHAR2,
                [0x35 - 0x2e] = T_SHORT};
            static const uint8_t esz[] = {4, 8, 0, 0, 4, 1, 1, 2};
            uint8_t *e = arr_elem(vm, a, i, mask[op - 0x2e], esz[op - 0x2e]);
            if (!e)
                break;
            switch (op) {
            case 0x2e: case 0x32: PUSH(rd32(e)); break;
            case 0x2f: PUSH(rd32(e)); PUSH(rd32(e + 4)); break;
            case 0x33: PUSH((int32_t)(int8_t)e[0]); break;
            case 0x34: PUSH(e[0]); break;
            case 0x35: PUSH((int32_t)(int16_t)rd16(e)); break;
            }
            break;
        }
        /* array stores */
        case 0x4f: case 0x50: case 0x53: case 0x54: case 0x55: case 0x56: {
            uint32_t v0 = 0, v1 = 0;
            if (op == 0x50) { v1 = POP(); v0 = POP(); } else { v0 = POP(); }
            int32_t i = (int32_t)POP();
            ref_t a = POP();
            uint8_t mask = op == 0x4f ? T_INT : op == 0x50 ? T_LONG : op == 0x53 ? 0
                         : op == 0x54 ? T_BYTE : op == 0x55 ? T_CHAR2 : T_SHORT;
            int sz = op == 0x4f || op == 0x53 ? 4 : op == 0x50 ? 8 : op == 0x56 ? 2 : 1;
            uint8_t *e = arr_elem(vm, a, i, mask, sz);
            if (!e)
                break;
            if (sz == 4) wr32(e, v0);
            else if (sz == 8) { wr32(e, v0); wr32(e + 4, v1); }
            else if (sz == 2) wr16(e, (uint16_t)v0);
            else e[0] = (uint8_t)v0;
            break;
        }

        /* stack */
        case 0x57: f->sp--; break;
        case 0x58: f->sp -= 2; break;
        case 0x59: f->sp[0] = f->sp[-1]; f->sp++; break;
        case 0x5a: { uint32_t v1 = f->sp[-1], v2 = f->sp[-2];
            f->sp[-2] = v1; f->sp[-1] = v2; f->sp[0] = v1; f->sp++; break; }
        case 0x5b: { uint32_t v1 = f->sp[-1], v2 = f->sp[-2], v3 = f->sp[-3];
            f->sp[-3] = v1; f->sp[-2] = v3; f->sp[-1] = v2; f->sp[0] = v1; f->sp++; break; }
        case 0x5c: f->sp[0] = f->sp[-2]; f->sp[1] = f->sp[-1]; f->sp += 2; break;
        case 0x5d: { uint32_t v1 = f->sp[-1], v2 = f->sp[-2], v3 = f->sp[-3];
            f->sp[-3] = v2; f->sp[-2] = v1; f->sp[-1] = v3; f->sp[0] = v2; f->sp[1] = v1;
            f->sp += 2; break; }
        case 0x5e: { uint32_t v1 = f->sp[-1], v2 = f->sp[-2], v3 = f->sp[-3], v4 = f->sp[-4];
            f->sp[-4] = v2; f->sp[-3] = v1; f->sp[-2] = v4; f->sp[-1] = v3;
            f->sp[0] = v2; f->sp[1] = v1; f->sp += 2; break; }
        case 0x5f: { uint32_t t = f->sp[-1]; f->sp[-1] = f->sp[-2]; f->sp[-2] = t; break; }

        /* int arithmetic (wrapping) */
        case 0x60: f->sp[-2] = f->sp[-2] + f->sp[-1]; f->sp--; break;
        case 0x64: f->sp[-2] = f->sp[-2] - f->sp[-1]; f->sp--; break;
        case 0x68: f->sp[-2] = f->sp[-2] * f->sp[-1]; f->sp--; break;
        case 0x6c: case 0x70: {
            int32_t d = (int32_t)POP(), n = (int32_t)POP();
            if (d == 0 || (n == INT32_MIN && d == -1))
                THROW(H_ARITH); /* MIN_INT / -1 throws too (original behaviour) */
            PUSH(op == 0x6c ? n / d : n % d);
            break;
        }
        case 0x74: f->sp[-1] = (uint32_t)0 - f->sp[-1]; break;
        case 0x78: f->sp[-2] = f->sp[-2] << (f->sp[-1] & 31); f->sp--; break;
        case 0x7a: f->sp[-2] = (uint32_t)((int32_t)f->sp[-2] >> (f->sp[-1] & 31)); f->sp--; break;
        case 0x7c: f->sp[-2] = f->sp[-2] >> (f->sp[-1] & 31); f->sp--; break;
        case 0x7e: f->sp[-2] &= f->sp[-1]; f->sp--; break;
        case 0x80: f->sp[-2] |= f->sp[-1]; f->sp--; break;
        case 0x82: f->sp[-2] ^= f->sp[-1]; f->sp--; break;

        /* long arithmetic */
        case 0x61: case 0x65: case 0x69: case 0x7f: case 0x81: case 0x83: {
            uint64_t y = (uint64_t)getl(f->sp - 2), x = (uint64_t)getl(f->sp - 4), r = 0;
            switch (op) {
            case 0x61: r = x + y; break;
            case 0x65: r = x - y; break;
            case 0x69: r = x * y; break;
            case 0x7f: r = x & y; break;
            case 0x81: r = x | y; break;
            case 0x83: r = x ^ y; break;
            }
            f->sp -= 2;
            putl(f->sp - 2, (int64_t)r);
            break;
        }
        case 0x6d: case 0x71: {
            int64_t y = getl(f->sp - 2), x = getl(f->sp - 4);
            f->sp -= 4;
            /* the original's overflow check compares the divisor against MIN, not -1 */
            if (y == 0 || (x == INT64_MIN && y == INT64_MIN))
                THROW(H_ARITH);
            int64_t r;
            if (x == INT64_MIN && y == -1)
                r = op == 0x6d ? INT64_MIN : 0;
            else
                r = op == 0x6d ? x / y : x % y;
            putl(f->sp, r);
            f->sp += 2;
            break;
        }
        case 0x75: putl(f->sp - 2, (int64_t)(0 - (uint64_t)getl(f->sp - 2))); break;
        case 0x79: case 0x7b: case 0x7d: {
            uint32_t s = POP();
            int64_t x = getl(f->sp - 2);
            int64_t r;
            if (op == 0x79) /* lshl: count not masked; __allshl gives 0 for >= 64 */
                r = s >= 64 ? 0 : (int64_t)((uint64_t)x << s);
            else if (op == 0x7b) /* lshr: count not masked; >= 64 gives the sign fill */
                r = s >= 64 ? (x < 0 ? -1 : 0) : x >> s;
            else
                r = (int64_t)((uint64_t)x >> (s & 63));
            putl(f->sp - 2, r);
            break;
        }

        case 0x84: {
            uint8_t idx = b[pc];
            f->loc[idx] += (uint32_t)(int32_t)(int8_t)b[pc + 1];
            f->pc = (uint16_t)(pc + 2);
            break;
        }
        case 0x85: { int32_t v = (int32_t)POP(); putl(f->sp, v); f->sp += 2; break; }
        case 0x88: { f->sp -= 1; break; } /* l2i: keep the low word */
        case 0x91: f->sp[-1] = (uint32_t)(int32_t)(int8_t)f->sp[-1]; break;
        case 0x92: f->sp[-1] &= 0xff; break; /* i2c: 8-bit chars */
        case 0x93: f->sp[-1] = (uint32_t)(int32_t)(int16_t)f->sp[-1]; break;
        case 0x94: {
            int64_t y = getl(f->sp - 2), x = getl(f->sp - 4);
            f->sp -= 4;
            PUSH(x < y ? -1 : x > y ? 1 : 0);
            break;
        }

        /* branches: absolute u16 targets */
        case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: {
            pc = a2(pc);
            uint16_t t = U16AT(pc);
            int32_t v = (int32_t)POP();
            int c = op == 0x99 ? v == 0 : op == 0x9a ? v != 0 : op == 0x9b ? v < 0
                  : op == 0x9c ? v >= 0 : op == 0x9d ? v > 0 : v <= 0;
            f->pc = c ? t : (uint16_t)(pc + 2);
            break;
        }
        case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: {
            pc = a2(pc);
            uint16_t t = U16AT(pc);
            int32_t y = (int32_t)POP(), x = (int32_t)POP();
            int c = op == 0x9f ? x == y : op == 0xa0 ? x != y : op == 0xa1 ? x < y
                  : op == 0xa2 ? x >= y : op == 0xa3 ? x > y : x <= y;
            f->pc = c ? t : (uint16_t)(pc + 2);
            break;
        }
        case 0xa5: case 0xa6: case 0xc6: case 0xc7: {
            pc = a2(pc);
            uint16_t t = U16AT(pc);
            int c;
            if (op <= 0xa6) {
                uint32_t y = POP(), x = POP();
                c = op == 0xa5 ? x == y : x != y;
            } else {
                uint32_t x = POP();
                c = op == 0xc6 ? x == 0 : x != 0;
            }
            f->pc = c ? t : (uint16_t)(pc + 2);
            break;
        }
        case 0xa7: pc = a2(pc); f->pc = U16AT(pc); break;
        case 0xa8: pc = a2(pc); f->jsr = (uint16_t)(pc + 2); PUSH(0); f->pc = U16AT(pc); break;
        case 0xa9: f->pc = f->jsr; PUSH(0); break;
        case 0xaa: case 0xab: case 0xcc: case 0xcd: {
            int32_t v = (int32_t)POP();
            f->pc = (uint16_t)switch_target(b, pc, op, v);
            break;
        }

        /* returns */
        case 0xb0: case 0xe9: do_return(vm, 1); break;
        case 0xea: do_return(vm, 2); break; /* the original's lreturn is broken; unused */
        case 0xb1: do_return(vm, 0); break;

        /* objects */
        case 0xbb: {
            pc = a2(pc);
            int cls = U16AT(U16AT(pc));
            f->pc = (uint16_t)(pc + 2);
            if (cls >= vm->nclasses)
                THROW(H_CNF);
            if (vm_init_class(vm, cls) < 0)
                break;
            ref_t o = vm_new_object(vm, cls);
            if (!o)
                THROW(H_OOM);
            PUSH(o);
            break;
        }
        case 0xbc: {
            pc = a2(pc);
            uint16_t tw = U16AT(pc);
            pc += 2;
            uint16_t ecls = 0;
            if ((tw & 0xff) == T_OBJECT) {
                pc = a2(pc);
                ecls = U16AT(U16AT(pc));
                pc += 2;
            }
            f->pc = (uint16_t)pc;
            int32_t n = (int32_t)POP();
            if (n < 0)
                THROW(H_NEGSIZE);
            ref_t a = vm_new_array(vm, tw, ecls, n);
            if (!a)
                THROW(H_OOM);
            PUSH(a);
            break;
        }
        case 0xbe: {
            ref_t a = POP();
            if (!a)
                THROW(H_NPE);
            if (!heap_is_object(vm, a) || !is_array(vm, a))
                THROW(H_EXCEPTION);
            PUSH(arr_len(vm, a));
            break;
        }
        case 0xbf: {
            ref_t e = POP();
            if (!e)
                THROW(H_NPE);
            vm_throw(vm, e);
            break;
        }
        case 0xc0: { /* checkcast: no check, skip the operand */
            pc = a2(pc);
            uint16_t tw = U16AT(pc);
            pc += 2;
            if ((tw & 0xff) == T_OBJECT)
                pc += 2;
            f->pc = (uint16_t)pc;
            break;
        }
        case 0xc1: {
            pc = a2(pc);
            uint16_t tw = U16AT(pc);
            pc += 2;
            int target = -1;
            if ((tw & 0xff) == T_OBJECT) {
                target = U16AT(U16AT(pc));
                pc += 2;
            }
            f->pc = (uint16_t)pc;
            ref_t o = POP();
            int res = 0;
            if (o && heap_is_object(vm, o)) {
                if (tw >> 8) /* array type: the original pushes nothing here (never used) */
                    res = is_array(vm, o) && arr_type(vm, o) == (uint16_t)(tw - 0x100);
                else if (target == vm->cls_object)
                    res = 1;
                else if (target >= 0 && !is_array(vm, o))
                    res = is_subclass(vm, obj_cls(vm, o), target);
            }
            PUSH(res);
            break;
        }
        case 0xc2: case 0xc3: break; /* monitorenter/exit: empty, ref stays on the stack */
        case 0xc5: {
            uint8_t nd = b[pc];
            pc = a2(pc + 1);
            uint16_t tw = U16AT(pc);
            pc += 2;
            uint16_t ecls = 0;
            if ((tw & 0xff) == T_OBJECT) {
                ecls = U16AT(U16AT(pc));
                pc += 2;
            }
            f->pc = (uint16_t)pc;
            int32_t dims[16];
            if (nd == 0 || nd > 16) {
                f->sp -= nd;
                THROW(H_EXCEPTION);
            }
            for (int i = nd - 1; i >= 0; i--)
                dims[i] = (int32_t)(int16_t)POP();
            for (int i = 0; i < nd; i++)
                if (dims[i] < 0)
                    THROW(H_NEGSIZE);
            /* tw is the full array type (int[][] = 0x0259), so the outermost array's elements
             * are tw - 0x100 (seen in Reversi and Age of Empires) */
            ref_t a = build_multi(vm, (uint16_t)(tw - 0x100), ecls, dims, nd);
            if (!a)
                THROW(H_OOM);
            PUSH(a);
            break;
        }
        case 0xd0: {
            pc = a2(pc);
            uint16_t o = U16AT(pc);
            f->pc = (uint16_t)(pc + 2);
            ref_t s = vm_new_object(vm, vm->cls_string);
            if (!s)
                THROW(H_OOM);
            obj_slots(vm, s)[vm->classes[vm->cls_string].inst_base] = vm->classes[f->cls].block + o;
            PUSH(s);
            break;
        }

        /* int/long locals at In-Fusio numbers */
        case 0xd5: PUSH(f->loc[b[pc]]); f->pc = (uint16_t)(pc + 1); break;
        case 0xd6: f->loc[b[pc]] = POP(); f->pc = (uint16_t)(pc + 1); break;
        case 0xd7: { uint8_t i = b[pc]; PUSH(f->loc[i]); PUSH(f->loc[i + 1]); f->pc = (uint16_t)(pc + 1); break; }
        case 0xd8: { uint8_t i = b[pc]; f->loc[i + 1] = POP(); f->loc[i] = POP(); f->pc = (uint16_t)(pc + 1); break; }
        case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf: case 0xe0:
        case 0xe1: case 0xe2: case 0xe3: case 0xe4: case 0xe5: case 0xe6: case 0xe7: case 0xe8: {
            int n = (op - 0xd9) >> 2, k = (op - 0xd9) & 3;
            if (k == 0) PUSH(f->loc[n]);
            else if (k == 1) f->loc[n] = POP();
            else if (k == 2) { PUSH(f->loc[n]); PUSH(f->loc[n + 1]); }
            else { f->loc[n + 1] = POP(); f->loc[n] = POP(); }
            break;
        }

        /* invokes */
        case 0xed: case 0xef: case 0xf1: {
            pc = a2(pc);
            uint16_t rec = U16AT(pc);
            f->pc = (uint16_t)(pc + 2);
            int has_this = op != 0xf1;
            int nargs = U16AT(rec + 6) + has_this;
            uint32_t *args = f->sp - nargs;
            /* Initialise the class while the arguments are still on the operand stack: a
             * <clinit> run starts at the stack top and would overwrite them (and the GC would
             * no longer see the references among them). */
            if (!has_this && vm_init_class(vm, f->cls) < 0)
                break;
            f->sp = args;
            if (has_this && !args[0])
                THROW(H_NPE);
            vm_invoke(vm, f->cls, rec, args, has_this);
            break;
        }
        case 0xee: case 0xf0: case 0xf2: {
            pc = a2(pc);
            uint16_t ref = U16AT(pc);
            f->pc = (uint16_t)(pc + 2);
            uint32_t hash = rd32(b + ref);
            int has_this = op != 0xf2;
            int nargs = U16AT(ref + 4) + has_this;
            int rcls = U16AT(U16AT(ref + 6));
            uint32_t *args = f->sp - nargs;
            /* as above: <clinit> must not run over the popped arguments */
            if (op == 0xf2 && rcls < vm->nclasses && vm_init_class(vm, rcls) < 0)
                break;
            f->sp = args;
            int found = -1;
            uint32_t m;
            if (op == 0xee) {
                if (!args[0])
                    THROW(H_NPE);
                int ocls = is_array(vm, args[0]) ? vm->cls_object : obj_cls(vm, args[0]);
                m = find_method(vm, ocls, hash, 1, &found);
            } else if (op == 0xf0) {
                if (!args[0])
                    THROW(H_NPE);
                m = find_method(vm, rcls, hash, 0, &found);
                if (!m)
                    m = find_method(vm, rcls, hash, 1, &found);
            } else {
                if (rcls >= vm->nclasses || vm_init_class(vm, rcls) < 0)
                    THROW(H_CNF);
                m = find_method(vm, rcls, hash, 1, &found);
            }
            if (!m)
                THROW(H_LINKAGE);
            vm_invoke(vm, found, m, args, has_this);
            break;
        }

        /* fields */
        case 0xf3: case 0xf4: case 0xf5: case 0xf6: {
            pc = a2(pc);
            uint16_t rec = U16AT(pc);
            f->pc = (uint16_t)(pc + 2);
            uint16_t ty = U16AT(rec + 6), slot = U16AT(rec + 8);
            uint32_t *slots;
            if (op <= 0xf4) {
                if (vm_init_class(vm, f->cls) < 0)
                    break;
                slots = obj_slots(vm, vm->classes[f->cls].statics) + slot;
                if (op == 0xf3)
                    field_get(&F->sp, slots, ty);
                else
                    field_put(&F->sp, slots, ty);
            } else {
                int fw = field_words(ty);
                ref_t o = op == 0xf5 ? f->sp[-1] : f->sp[-1 - fw];
                if (!o)
                    THROW(H_NPE);
                slots = obj_slots(vm, o) + vm->classes[f->cls].inst_base + slot;
                if (op == 0xf5) {
                    f->sp--;
                    field_get(&f->sp, slots, ty);
                } else {
                    field_put(&f->sp, slots, ty);
                    f->sp--;
                }
            }
            break;
        }
        case 0xf7: case 0xf8: case 0xf9: case 0xfa: {
            pc = a2(pc);
            uint16_t ref = U16AT(pc);
            f->pc = (uint16_t)(pc + 2);
            uint32_t hash = rd32(b + ref);
            uint16_t ty = U16AT(ref + 4);
            int rcls = U16AT(U16AT(ref + 6));
            int found = -1;
            if (rcls >= vm->nclasses)
                THROW(H_LINKAGE);
            uint32_t frec = find_field(vm, rcls, hash, 1, &found);
            if (!frec)
                THROW(H_LINKAGE);
            const uint8_t *fb = BLK(vm, found);
            uint16_t fl = rd16(fb + frec + 4), slot = rd16(fb + frec + 8);
            if (fl & ACC_PRIVATE)
                THROW(H_ILLACCESS);
            if (op <= 0xf8) {
                if (vm_init_class(vm, found) < 0)
                    break;
                uint32_t *slots = obj_slots(vm, vm->classes[found].statics) + slot;
                if (op == 0xf7)
                    field_get(&F->sp, slots, ty);
                else
                    field_put(&F->sp, slots, ty);
            } else {
                if (fl & ACC_STATIC)
                    THROW(H_ERROR);
                int fw = field_words(ty);
                ref_t o = op == 0xf9 ? f->sp[-1] : f->sp[-1 - fw];
                if (!o)
                    THROW(H_NPE);
                uint32_t *slots = obj_slots(vm, o) + vm->classes[found].inst_base + slot;
                if (op == 0xf9) {
                    f->sp--;
                    field_get(&f->sp, slots, ty);
                } else {
                    field_put(&f->sp, slots, ty);
                    f->sp--;
                }
            }
            break;
        }
        default:
            break; /* undefined opcodes are silent 1-byte no-ops in the original */
        }
    next:;
    }
}
