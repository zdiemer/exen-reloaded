/* Native dispatch and the java.lang / vm.sys / exen.Math / exen.util.Debug natives
 * (docs/re/NATIVES.md, docs/re/natives/string.c.txt, docs/re/natives/math.c.txt).
 *
 * ABI (as in the original): args[0] is `this` (instance natives) or the first argument;
 * results are written from args[0]; the return value is the number of result words.
 * Natives must not unwind the VM themselves: vm_throw_hash() inside a native only records
 * the exception, and vm_invoke() raises it after the native returns.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "natives.h"

/* ------------------------------------------------------------ helpers */

void root_push(ExenVM *vm, ref_t r)
{
    if (vm->nroots < EXEN_MAX_ROOTS)
        vm->roots[vm->nroots++] = r;
}

void root_pop(ExenVM *vm, int n)
{
    vm->nroots -= n;
    if (vm->nroots < 0)
        vm->nroots = 0;
}

uint32_t *inst(ExenVM *vm, ref_t obj, int cls)
{
    return obj_slots(vm, obj) + vm->classes[cls].inst_base;
}

ref_t new_char_array(ExenVM *vm, const uint8_t *src, int n)
{
    ref_t a = vm_new_array(vm, T_CHAR, 0, n);
    if (a && src && n)
        memcpy(arr_data(vm, a), src, (size_t)n);
    return a;
}

/* String layout: inst slot 0 (m_strBuffer, a long whose low word is used) = arena offset of a
 * {u16 len, u8 chars[]} pair; slot 2 (m_refArrayBuffer) = backing char[] or 0 for literals.
 * For a backing array the pair is the array's own length field followed by its data. */
int vm_string_get(ExenVM *vm, ref_t s, const uint8_t **chars)
{
    if (!s) {
        *chars = NULL;
        return 0;
    }
    uint32_t pstr = inst(vm, s, vm->cls_string)[0];
    if (!pstr) {
        *chars = NULL;
        return 0;
    }
    *chars = P(vm, pstr + 2);
    return rd16(P(vm, pstr));
}

static void string_set_backing(ExenVM *vm, ref_t s, int cls, ref_t arr)
{
    uint32_t *f = inst(vm, s, cls);
    f[0] = arr ? arr + 10 : 0;
    f[1] = 0;
    f[2] = arr;
}

ref_t vm_new_string(ExenVM *vm, const uint8_t *chars, int len)
{
    ref_t a = new_char_array(vm, chars, len);
    if (!a)
        return 0;
    root_push(vm, a);
    ref_t s = vm_new_object(vm, vm->cls_string);
    root_pop(vm, 1);
    if (s)
        string_set_backing(vm, s, vm->cls_string, a);
    return s;
}

ref_t vm_new_cstring(ExenVM *vm, const char *s)
{
    return vm_new_string(vm, (const uint8_t *)s, (int)strlen(s));
}

static ref_t clone_array(ExenVM *vm, ref_t a)
{
    uint16_t n = arr_len(vm, a);
    uint16_t tw = arr_type(vm, a);
    root_push(vm, a);
    ref_t c = vm_new_array(vm, tw, obj_cls(vm, a), n);
    root_pop(vm, 1);
    if (c)
        memcpy(arr_data(vm, c), arr_data(vm, a), (size_t)n * (size_t)elem_size(tw));
    return c;
}

void vm_log(ExenVM *vm, const char *fmt, ...)
{
    if (!vm->host.log)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vm->host.log(vm->host.user, buf);
}

/* ------------------------------------------------------------ java.lang.Object / Class */

static int n_Object_getClass(ExenVM *vm, uint32_t *a)
{
    ref_t o = a[0];
    if (!o) {
        vm_throw_hash(vm, H_NPE);
        return 1;
    }
    int cls = is_array(vm, o) ? -1 : obj_cls(vm, o);
    ref_t c = vm_new_object(vm, vm->cls_class);
    if (!c) {
        vm_throw_hash(vm, H_OOM);
        return 1;
    }
    uint32_t *f = inst(vm, c, vm->cls_class);
    f[0] = (uint32_t)cls;
    f[1] = is_array(vm, o) ? (uint32_t)(arr_type(vm, o) + 0x100) : T_OBJECT;
    a[0] = c;
    return 1;
}

static int n_Object_hashCode(ExenVM *vm, uint32_t *a) { (void)vm; (void)a; return 1; }

static int n_Object_clone(ExenVM *vm, uint32_t *a)
{
    ref_t o = a[0];
    if (!o) {
        vm_throw_hash(vm, H_NPE);
        return 1;
    }
    ref_t c;
    if (is_array(vm, o)) {
        c = clone_array(vm, o);
    } else {
        int cls = obj_cls(vm, o);
        int cloneable = find_class(vm, 0x2b927978u);
        if (cloneable < 0 || !is_subclass(vm, cls, cloneable)) {
            vm_throw_hash(vm, exen_crc32("java.lang.CloneNotSupportedException", 36));
            return 1;
        }
        root_push(vm, o);
        c = vm_new_object(vm, cls);
        root_pop(vm, 1);
        if (c)
            memcpy(obj_slots(vm, c), obj_slots(vm, o), 4u * vm->classes[cls].inst_total);
    }
    if (!c)
        vm_throw_hash(vm, H_OOM);
    a[0] = c;
    return 1;
}

static int n_Object_wait(ExenVM *vm, uint32_t *a) { (void)vm; (void)a; return 0; }

static int n_Class_forName(ExenVM *vm, uint32_t *a)
{
    const uint8_t *s;
    int n = vm_string_get(vm, a[0], &s);
    if (!a[0]) {
        vm_throw_hash(vm, H_NPE);
        return 1;
    }
    int cls = find_class(vm, exen_crc32(s, (size_t)n));
    if (cls < 0) {
        vm_throw_hash(vm, H_CNF);
        a[0] = 0;
        return 1;
    }
    ref_t c = vm_new_object(vm, vm->cls_class);
    if (!c) {
        vm_throw_hash(vm, H_OOM);
        return 1;
    }
    uint32_t *f = inst(vm, c, vm->cls_class);
    f[0] = (uint32_t)cls;
    f[1] = T_OBJECT;
    a[0] = c;
    return 1;
}

static int n_Class_newInstance(ExenVM *vm, uint32_t *a)
{
    uint32_t *f = inst(vm, a[0], vm->cls_class);
    int cls = (int)f[0];
    if ((f[1] & 0xffff) != T_OBJECT || cls < 0 || cls >= vm->nclasses) {
        vm_throw_hash(vm, H_EXCEPTION);
        a[0] = 0;
        return 1;
    }
    if (vm_init_class(vm, cls) < 0)
        return 1;
    ref_t o = vm_new_object(vm, cls);
    if (!o) {
        vm_throw_hash(vm, H_OOM);
        return 1;
    }
    /* Run the default constructor, as VM-created objects do (0x4101c4). */
    uint16_t ctor = rd16(BLK(vm, cls) + 0x2c);
    if (ctor) {
        uint32_t arg = o;
        root_push(vm, o);
        vm_call(vm, cls, ctor, &arg, 1, NULL);
        root_pop(vm, 1);
    }
    a[0] = o;
    return 1;
}

static int n_Class_getName(ExenVM *vm, uint32_t *a)
{
    uint32_t *f = inst(vm, a[0], vm->cls_class);
    int cls = (int)f[0];
    char buf[96];
    if (cls >= 0 && cls < vm->nclasses && vm->classes[cls].name)
        snprintf(buf, sizeof buf, "%s", vm->classes[cls].name);
    else
        snprintf(buf, sizeof buf, "class#%d", cls);
    ref_t s = vm_new_cstring(vm, buf);
    if (!s)
        vm_throw_hash(vm, H_OOM);
    a[0] = s;
    return 1;
}

/* ------------------------------------------------------------ java.lang.String */

static int n_String_length(ExenVM *vm, uint32_t *a)
{
    const uint8_t *s;
    a[0] = (uint32_t)vm_string_get(vm, a[0], &s);
    return 1;
}

static int n_String_toBytes(ExenVM *vm, uint32_t *a, uint16_t tw)
{
    const uint8_t *s;
    int n = vm_string_get(vm, a[0], &s);
    root_push(vm, a[0]);
    ref_t r = vm_new_array(vm, tw, 0, n);
    root_pop(vm, 1);
    if (!r) {
        vm_throw_hash(vm, H_OOM);
        return 1;
    }
    n = vm_string_get(vm, a[0], &s); /* re-read: the allocation may have collected */
    memcpy(arr_data(vm, r), s, (size_t)n);
    a[0] = r;
    return 1;
}
static int n_String_toCharArray(ExenVM *vm, uint32_t *a) { return n_String_toBytes(vm, a, T_CHAR); }
static int n_String_getBytes(ExenVM *vm, uint32_t *a) { return n_String_toBytes(vm, a, T_BYTE); }

static int n_String_compareTo(ExenVM *vm, uint32_t *a)
{
    if (!a[1]) {
        vm_throw_hash(vm, H_NPE);
        a[0] = 1;
        return 1;
    }
    uint32_t pa = inst(vm, a[0], vm->cls_string)[0];
    if (!pa) {
        a[0] = (uint32_t)-1;
        return 1;
    }
    const uint8_t *x, *y;
    int lx = vm_string_get(vm, a[0], &x), ly = vm_string_get(vm, a[1], &y);
    for (int i = 0; i < lx && i < ly; i++) {
        if (x[i] < y[i]) { a[0] = (uint32_t)-1; return 1; }
        if (x[i] > y[i]) { a[0] = 1; return 1; }
    }
    /* the original's length tiebreak is inverted relative to Java */
    a[0] = ly < lx ? (uint32_t)-1 : ly > lx ? 1u : 0u;
    return 1;
}

static int n_String_appendString(ExenVM *vm, uint32_t *a)
{
    if (!a[1]) {
        vm_throw_hash(vm, H_NPE);
        return 0;
    }
    uint32_t *src = inst(vm, a[1], vm->cls_string);
    if (src[2]) {
        ref_t c = clone_array(vm, src[2]);
        if (!c) {
            vm_throw_hash(vm, H_OOM);
            return 0;
        }
        string_set_backing(vm, a[0], vm->cls_string, c);
    } else {
        uint32_t *dst = inst(vm, a[0], vm->cls_string);
        dst[0] = src[0];
        dst[1] = 0;
        dst[2] = 0;
    }
    return 0;
}

static int n_String_appendChars(ExenVM *vm, uint32_t *a)
{
    if (!a[1]) {
        vm_throw_hash(vm, H_NPE);
        return 0;
    }
    if (!arr_len(vm, a[1]))
        return 0;
    ref_t c = clone_array(vm, a[1]);
    if (!c) {
        vm_throw_hash(vm, H_OOM);
        return 0;
    }
    wr16(P(vm, c + 6), EXEN_KIND_ARRAY | T_CHAR);
    string_set_backing(vm, a[0], vm->cls_string, c);
    return 0;
}

static int n_String_appendBytes(ExenVM *vm, uint32_t *a) { return n_String_appendChars(vm, a); }

static int fmt_int(char *buf, int32_t v)
{
    return sprintf(buf, "%ld", (long)v);
}

static int n_String_valueOfInt(ExenVM *vm, uint32_t *a)
{
    char buf[16];
    int n = fmt_int(buf, (int32_t)a[0]);
    ref_t s = vm_new_string(vm, (const uint8_t *)buf, n);
    if (!s)
        vm_throw_hash(vm, H_OOM);
    a[0] = s;
    return 1;
}

/* ------------------------------------------------------------ java.lang.StringBuffer
 * The storage char[]'s length field holds the current string length; its capacity is kept in
 * the array header's spare u16 at +8 (the original derives it from the block size). */

static int cls_sb(ExenVM *vm) { return vm->cls_stringbuffer; }

static int sb_reserve(ExenVM *vm, ref_t sb, int add)
{
    uint32_t *f = inst(vm, sb, cls_sb(vm));
    ref_t arr = f[2];
    int need = add, len = 0;
    if (arr) {
        len = arr_len(vm, arr);
        need += len;
        if (need <= rd16(P(vm, arr + 8)))
            return 1;
    }
    if (need > 0x4000) {
        vm_throw_hash(vm, 0xbea5520fu); /* StringIndexOutOfBoundsException */
        return 0;
    }
    root_push(vm, sb);
    ref_t n = vm_new_array(vm, T_CHAR, 0, need);
    root_pop(vm, 1);
    if (!n) {
        vm_throw_hash(vm, H_OOM);
        return 0;
    }
    f = inst(vm, sb, cls_sb(vm));
    arr = f[2];
    wr16(P(vm, n + 8), (uint16_t)need);
    if (arr)
        memcpy(arr_data(vm, n), arr_data(vm, arr), (size_t)len);
    wr16(P(vm, n + 10), (uint16_t)len);
    string_set_backing(vm, sb, cls_sb(vm), n);
    return 1;
}

static void sb_put(ExenVM *vm, ref_t sb, const uint8_t *s, int n)
{
    ref_t arr = inst(vm, sb, cls_sb(vm))[2];
    uint16_t len = arr_len(vm, arr);
    memcpy(arr_data(vm, arr) + len, s, (size_t)n);
    wr16(P(vm, arr + 10), (uint16_t)(len + n));
}

static int n_SB_init(ExenVM *vm, uint32_t *a)
{
    int32_t n = (int32_t)a[1];
    if (n <= 0) {
        vm_throw_hash(vm, H_NEGSIZE);
        return 0;
    }
    sb_reserve(vm, a[0], n);
    return 0;
}

static int n_SB_length(ExenVM *vm, uint32_t *a)
{
    ref_t arr = inst(vm, a[0], cls_sb(vm))[2];
    a[0] = arr ? arr_len(vm, arr) : 0;
    return 1;
}

static int n_SB_capacity(ExenVM *vm, uint32_t *a)
{
    ref_t arr = inst(vm, a[0], cls_sb(vm))[2];
    a[0] = arr ? rd16(P(vm, arr + 8)) : 0;
    return 1;
}

static int n_SB_appendString(ExenVM *vm, uint32_t *a)
{
    const uint8_t *s;
    int n = vm_string_get(vm, a[1], &s);
    if (!a[1] || !n)
        return 1; /* null is silently ignored */
    root_push(vm, a[1]);
    int ok = sb_reserve(vm, a[0], n);
    root_pop(vm, 1);
    if (ok) {
        n = vm_string_get(vm, a[1], &s);
        sb_put(vm, a[0], s, n);
    }
    return 1;
}

static int n_SB_appendChars(ExenVM *vm, uint32_t *a)
{
    if (!a[1]) {
        vm_throw_hash(vm, H_NPE);
        return 1;
    }
    int n = arr_len(vm, a[1]);
    root_push(vm, a[1]);
    int ok = sb_reserve(vm, a[0], n);
    root_pop(vm, 1);
    if (ok)
        sb_put(vm, a[0], arr_data(vm, a[1]), n);
    return 1;
}

static int n_SB_appendChar(ExenVM *vm, uint32_t *a)
{
    uint8_t c = (uint8_t)a[1];
    if (sb_reserve(vm, a[0], 1))
        sb_put(vm, a[0], &c, 1);
    return 1;
}

static int n_SB_appendInt(ExenVM *vm, uint32_t *a)
{
    char buf[16];
    int n = fmt_int(buf, (int32_t)a[1]);
    if (sb_reserve(vm, a[0], 11))
        sb_put(vm, a[0], (const uint8_t *)buf, n);
    return 1;
}

static int n_SB_appendLong(ExenVM *vm, uint32_t *a)
{
    char buf[24];
    int64_t v = (int64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32);
    int n = sprintf(buf, "%lld", (long long)v);
    if (sb_reserve(vm, a[0], n)) /* the original reserves only 11 and can overrun */
        sb_put(vm, a[0], (const uint8_t *)buf, n);
    return 1;
}

static int n_SB_toString(ExenVM *vm, uint32_t *a)
{
    ref_t arr = inst(vm, a[0], cls_sb(vm))[2];
    ref_t s;
    if (arr) {
        ref_t c = clone_array(vm, arr);
        if (!c) {
            vm_throw_hash(vm, H_OOM);
            return 1;
        }
        root_push(vm, c);
        s = vm_new_object(vm, vm->cls_string);
        root_pop(vm, 1);
        if (s)
            string_set_backing(vm, s, vm->cls_string, c);
    } else {
        s = vm_new_string(vm, NULL, 0);
    }
    if (!s)
        vm_throw_hash(vm, H_OOM);
    a[0] = s;
    return 1;
}

/* ------------------------------------------------------------ vm.sys.Runtime */

static int n_Runtime_gc(ExenVM *vm, uint32_t *a) { (void)a; heap_gc(vm); return 0; }
static int n_Runtime_createTempClass(ExenVM *vm, uint32_t *a) { (void)vm; a[0] = 0; return 1; }
static int n_Runtime_getTickCount(ExenVM *vm, uint32_t *a)
{
    a[0] = vm->host.ticks_ms ? vm->host.ticks_ms(vm->host.user) : 0;
    return 1;
}

/* ------------------------------------------------------------ exen.Math */

static int cos_(ExenVM *vm, int x)
{
    const uint16_t *T = vm->math.cos_table;
    x &= 0x7ff;
    if (x < 0x200) return T[x];
    if (x < 0x400) return -(int)T[0x400 - x];
    if (x < 0x600) return -(int)T[x - 0x400];
    return T[0x800 - x];
}
static int sin_(ExenVM *vm, int x) { return cos_(vm, x - 0x200); }

static int acos_idx(ExenVM *vm, int v)
{
    int lo = 0, hi = 0x200;
    while (hi - lo > 1) {
        int m = (lo + hi) >> 1;
        if ((int)vm->math.cos_table[m] > v) lo = m; else hi = m;
    }
    return lo;
}

static int32_t isqrt_(int32_t x)
{
    if (x < 2)
        return x;
    uint32_t n = (uint32_t)x, t = n;
    int k = 0;
    while ((t >>= 2) != 0)
        k++;
    uint32_t r = 1u << k, bb = r, sq = r << k;
    while (k-- != 0) {
        bb >>= 1;
        uint32_t c = ((r + r + bb) << k) + sq;
        if (c <= n) {
            r += bb;
            sq = c;
        }
    }
    return (int32_t)r;
}

static int n_Math_sin(ExenVM *vm, uint32_t *a) { a[0] = (uint32_t)sin_(vm, (int32_t)a[0]); return 1; }
static int n_Math_cos(ExenVM *vm, uint32_t *a) { a[0] = (uint32_t)cos_(vm, (int32_t)a[0]); return 1; }

static int n_Math_getAngle(ExenVM *vm, uint32_t *a)
{
    int32_t x = (int32_t)a[0], y = (int32_t)a[1];
    int32_t r = isqrt_((int32_t)((uint32_t)x * (uint32_t)x + (uint32_t)y * (uint32_t)y));
    int ang, neg = 0;
    if (r == 0) {
        a[0] = 0;
        return 1;
    }
    if (y < 0) { neg = 1; y = -y; }
    if (x > 0) {
        if (x > y) { ang = acos_idx(vm, (int32_t)((uint32_t)y << 16) / r) - 0x200; ang = (0x800 - ang) & 0x7ff; }
        else ang = acos_idx(vm, (int32_t)((uint32_t)x << 16) / r);
    } else {
        if (-x < y) ang = 0x400 - acos_idx(vm, (int32_t)((uint32_t)(-x) << 16) / r);
        else ang = acos_idx(vm, (int32_t)((uint32_t)y << 16) / r) + 0x200;
    }
    if (neg)
        ang = 0x800 - ang;
    a[0] = (uint32_t)ang;
    return 1;
}

static int n_Math_setSinusPeriod(ExenVM *vm, uint32_t *a)
{
    if ((int32_t)a[0] > 0)
        vm->math.period = (int32_t)a[0];
    return 0;
}
static int n_Math_getSinusPeriod(ExenVM *vm, uint32_t *a) { a[0] = (uint32_t)vm->math.period; return 1; }
static int n_Math_getSinOfPeriod(ExenVM *vm, uint32_t *a)
{
    a[0] = (uint32_t)sin_(vm, (int32_t)((uint32_t)a[0] << 11) / vm->math.period);
    return 1;
}
static int n_Math_getCosOfPeriod(ExenVM *vm, uint32_t *a)
{
    a[0] = (uint32_t)cos_(vm, (int32_t)((uint32_t)a[0] << 11) / vm->math.period);
    return 1;
}
static int precise(ExenVM *vm, int32_t x, int use_sin)
{
    int32_t v = (int32_t)((uint32_t)x << 19) / vm->math.period;
    int32_t i = v / 256;
    int32_t fr = v & 0xff;
    int c0 = use_sin ? sin_(vm, i) : cos_(vm, i);
    int c1 = use_sin ? sin_(vm, i + 1) : cos_(vm, i + 1);
    return (c0 * (255 - fr) + c1 * fr) / 255;
}
static int n_Math_getCosPeriodPrecise(ExenVM *vm, uint32_t *a) { a[0] = (uint32_t)precise(vm, (int32_t)a[0], 0); return 1; }
static int n_Math_getSinPeriodPrecise(ExenVM *vm, uint32_t *a) { a[0] = (uint32_t)precise(vm, (int32_t)a[0], 1); return 1; }
static int n_Math_abs(ExenVM *vm, uint32_t *a)
{
    (void)vm;
    int32_t x = (int32_t)a[0];
    a[0] = x < 0 ? (uint32_t)0 - (uint32_t)x : (uint32_t)x;
    return 1;
}
static int n_Math_setRandSeed(ExenVM *vm, uint32_t *a)
{
    vm->math.s0 = vm->math.s1 = (int32_t)a[0];
    return 0;
}
static int n_Math_random(ExenVM *vm, uint32_t *a)
{
    int32_t v = vm->math.s0;
    int n = vm->math.s1 & 31;
    for (int i = 0; i < n; i++)
        v = (int32_t)(((uint32_t)v >> 1) | (((uint32_t)v & 1u) << 31)) | (v < 0 ? (int32_t)0x80000000 : 0);
    vm->math.s1 = vm->math.s0 ^ 0x05960395;
    vm->math.s0 = (int32_t)((uint32_t)v + (uint32_t)vm->math.s1 + 0x7921u);
    a[0] = (uint32_t)vm->math.s0;
    return 1;
}
static int n_Math_sqrt(ExenVM *vm, uint32_t *a) { (void)vm; a[0] = (uint32_t)isqrt_((int32_t)a[0]); return 1; }

/* ------------------------------------------------------------ exen.util.Debug */

static int n_Debug_DisplayText(ExenVM *vm, uint32_t *a)
{
    if (a[0]) {
        int n = arr_len(vm, a[0]);
        vm_log(vm, "Debug: %.*s", n, (const char *)arr_data(vm, a[0]));
    }
    return 0;
}
static int n_Debug_nop(ExenVM *vm, uint32_t *a) { (void)vm; (void)a; return 0; }
static int n_Debug_DisplayInt(ExenVM *vm, uint32_t *a) { vm_log(vm, "Debug: %d", (int32_t)a[0]); return 0; }

/* ------------------------------------------------------------ dispatch */

typedef struct {
    const char *cls, *name, *desc;
    uint32_t hash;
    int is_static, argw, retw;
} NativeInfo;

static const NativeInfo native_info[EXEN_NATIVE_COUNT] = {
#define X(i, id, c, n, d, h, s, aw, rw) [i] = {c, n, d, h, s, aw, rw},
    EXEN_NATIVES(X)
#undef X
};

static ExNativeFn native_fn[EXEN_NATIVE_COUNT] = {
    [151] = n_Object_getClass, [152] = n_Object_hashCode, [153] = n_Object_clone,
    [154] = n_Object_wait,
    [155] = n_Class_forName, [156] = n_Class_newInstance, [157] = n_Class_getName,
    [158] = n_String_length, [159] = n_String_toCharArray, [160] = n_String_getBytes,
    [161] = n_String_compareTo, [162] = n_String_appendString, [163] = n_String_appendChars,
    [164] = n_String_appendBytes, [165] = n_String_valueOfInt,
    [166] = n_SB_init, [167] = n_SB_length, [168] = n_SB_capacity, [169] = n_SB_appendString,
    [170] = n_SB_appendChars, [171] = n_SB_appendChar, [172] = n_SB_appendInt,
    [173] = n_SB_appendLong, [174] = n_SB_toString,
    [175] = n_Runtime_gc, [176] = n_Runtime_createTempClass, [177] = n_Runtime_getTickCount,
    [110] = n_Math_sin, [111] = n_Math_cos, [112] = n_Math_getAngle,
    [113] = n_Math_setSinusPeriod, [114] = n_Math_getSinusPeriod,
    [115] = n_Math_getSinOfPeriod, [116] = n_Math_getCosOfPeriod,
    [117] = n_Math_getCosPeriodPrecise, [118] = n_Math_getSinPeriodPrecise,
    [119] = n_Math_abs, [120] = n_Math_setRandSeed, [121] = n_Math_random, [122] = n_Math_sqrt,
    [147] = n_Debug_DisplayText, [148] = n_Debug_nop, [149] = n_Debug_nop,
    [150] = n_Debug_DisplayInt,
};

void natives_register(int index, ExNativeFn fn)
{
    if (index >= 0 && index < EXEN_NATIVE_COUNT)
        native_fn[index] = fn;
}

int natives_call(ExenVM *vm, uint32_t index, uint32_t *args)
{
    if (index >= EXEN_NATIVE_COUNT) {
        vm_halt(vm, "native index %u out of range", index);
        return 0;
    }
    vm->native_calls[index]++;
    ExNativeFn fn = native_fn[index];
    const NativeInfo *ni = &native_info[index];
    if (!fn) {
        if (vm->native_calls[index] == 1)
            vm_log(vm, "unimplemented native #%u %s.%s%s", index, ni->cls, ni->name, ni->desc);
        for (int i = 0; i < ni->retw; i++)
            args[i] = 0;
        return ni->retw;
    }
    vm->in_native = 1;
    int n = fn(vm, args);
    vm->in_native = 0;
    return n;
}

uint32_t exen_native_calls(ExenVM *vm, int index, const char **name)
{
    if (index < 0 || index >= EXEN_NATIVE_COUNT)
        return 0;
    if (name)
        *name = native_info[index].name;
    return vm->native_calls[index];
}

const char *native_class(int index) { return native_info[index].cls; }
int native_implemented(int index) { return native_fn[index] != NULL; }
