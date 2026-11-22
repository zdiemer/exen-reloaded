/* exen.Matrix3D (123-128) and exen.Vector3D (129-136): 16.16 fixed point.
 *
 * docs/re/natives/vec3d_matrix3d.c.txt, re-checked against gensimu.exe (0x426b20..0x426e29,
 * 0x42a020..0x42a305 and the helpers 0x41ceb4..0x41d47c). Every product is (a>>8)*(b>>8), so
 * the low 8 bits of both factors are lost. The matrix is Matrix3D.element, an int[16] in
 * row-major order; vectors multiply as rows (out = v x M) through the upper 3x3 only, so the
 * translation row is ignored. rotX/Y/Z overwrite the matrix rather than multiplying it.
 *
 * The original copies 16 ints without looking at the array length; we read 0 past the end of
 * a shorter array and drop writes past it. A null receiver field or argument is logged and
 * ignored where the original logs, and throws NullPointerException where it would crash.
 */
#include "extnatives.h"

enum { V_X, V_Y, V_Z };
enum { M_ELEMENT };

#define M8(a, b) ((int32_t)((uint32_t)((a) >> 8) * (uint32_t)((b) >> 8)))

static int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }

static uint32_t *vec(ExenVM *vm, ref_t v) { return inst(vm, v, ext_class(vm, EC_VECTOR3D)); }
static void vget(ExenVM *vm, ref_t v, int32_t o[3])
{
    const uint32_t *f = vec(vm, v);
    o[0] = (int32_t)f[V_X];
    o[1] = (int32_t)f[V_Y];
    o[2] = (int32_t)f[V_Z];
}
static void vset(ExenVM *vm, ref_t v, const int32_t o[3])
{
    uint32_t *f = vec(vm, v);
    f[V_X] = (uint32_t)o[0];
    f[V_Y] = (uint32_t)o[1];
    f[V_Z] = (uint32_t)o[2];
}

static ref_t mat_ref(ExenVM *vm, ref_t m) { return inst(vm, m, ext_class(vm, EC_MATRIX3D))[M_ELEMENT]; }

/* copy16 (0x41d47c), against an int[] */
static void mat_load(ExenVM *vm, ref_t arr, int32_t m[16])
{
    uint32_t n = arr_len(vm, arr);
    const uint8_t *d = arr_data(vm, arr);
    for (uint32_t i = 0; i < 16; i++)
        m[i] = i < n ? (int32_t)rd32(d + 4 * i) : 0;
}
static void mat_store(ExenVM *vm, ref_t arr, const int32_t m[16])
{
    uint32_t n = arr_len(vm, arr);
    uint8_t *d = arr_data(vm, arr);
    for (uint32_t i = 0; i < 16 && i < n; i++)
        wr32(d + 4 * i, (uint32_t)m[i]);
}

/* ------------------------------------------------------------ Matrix3D */

static int n_copyFrom(ExenVM *vm, uint32_t *a)
{
    ref_t dst = mat_ref(vm, a[0]);
    if (!dst || !a[1]) {
        vm_log(vm, "Matrix3D.copyFrom: null pointer");
        vm_halt(vm, "Matrix3D.copyFrom: null pointer (non catchable interrupt 1)");
        return 0;
    }
    int32_t m[16];
    mat_load(vm, a[1], m);
    mat_store(vm, dst, m);
    return 0;
}

static void rot(ExenVM *vm, uint32_t *a, int axis)
{
    ref_t dst = mat_ref(vm, a[0]);
    if (!dst) {
        vm_log(vm, "matrix 3d rot : null pointer");
        return;
    }
    int32_t m[16] = {0}, ang = (int32_t)a[1];
    int32_t c = exen_cos(vm, ang), s = exen_sin(vm, ang);
    m[15] = 0x10000;
    if (axis == 0) {        /* 0x41d25b */
        m[0] = 0x10000;
        m[5] = c; m[6] = s; m[9] = -s; m[10] = c;
    } else if (axis == 1) { /* 0x41d2c4 */
        m[0] = c; m[2] = -s; m[8] = s; m[10] = c;
        m[5] = 0x10000;
    } else {                /* 0x41d32e */
        m[0] = c; m[1] = s; m[4] = -s; m[5] = c;
        m[10] = 0x10000;
    }
    mat_store(vm, dst, m);
}
static int n_rotX(ExenVM *vm, uint32_t *a) { rot(vm, a, 0); return 0; }
static int n_rotY(ExenVM *vm, uint32_t *a) { rot(vm, a, 1); return 0; }
static int n_rotZ(ExenVM *vm, uint32_t *a) { rot(vm, a, 2); return 0; }

/* multiply(Matrix3D o): this = this x o (0x41d396) */
static int n_multiplyM(ExenVM *vm, uint32_t *a)
{
    if (!a[1]) {
        vm_throw_hash(vm, H_NPE);
        return 0;
    }
    ref_t t = mat_ref(vm, a[0]), o = mat_ref(vm, a[1]);
    if (!t || !o) {
        vm_log(vm, "matrix 3d multiply : null pointer");
        return 0;
    }
    int32_t A[16], B[16], C[16];
    mat_load(vm, t, A);
    mat_load(vm, o, B);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int32_t acc = 0;
            for (int k = 0; k < 4; k++)
                acc = add32(acc, M8(A[i * 4 + k], B[k * 4 + j]));
            C[i * 4 + j] = acc;
        }
    mat_store(vm, t, C);
    return 0;
}

/* multiply(Vector3D in, Vector3D out): out = in x M, upper 3x3 (0x41d17b) */
static int n_multiplyV(ExenVM *vm, uint32_t *a)
{
    ref_t t = mat_ref(vm, a[0]);
    if (!t || !a[1] || !a[2]) {
        vm_log(vm, "matrix 3d multiply vector : null pointer");
        return 0;
    }
    int32_t m[16], v[3], o[3];
    mat_load(vm, t, m);
    vget(vm, a[1], v);
    for (int c = 0; c < 3; c++)
        o[c] = add32(add32(M8(m[c], v[0]), M8(m[4 + c], v[1])), M8(m[8 + c], v[2]));
    vset(vm, a[2], o);
    return 0;
}

/* ------------------------------------------------------------ Vector3D */

static int32_t sqlen(const int32_t v[3]) /* 0x41ceb4 */
{
    return add32(add32(M8(v[0], v[0]), M8(v[1], v[1])), M8(v[2], v[2]));
}
static int32_t len3(const int32_t v[3]) { return (int32_t)((uint32_t)exen_isqrt(sqlen(v)) << 8); }

/* The natives turn a helper result of -5 (the error code) into -1. */
static int n_squareLength(ExenVM *vm, uint32_t *a)
{
    int32_t v[3];
    vget(vm, a[0], v);
    int32_t r = sqlen(v);
    a[0] = (uint32_t)(r == -5 ? -1 : r);
    return 1;
}

static int n_length(ExenVM *vm, uint32_t *a)
{
    int32_t v[3];
    vget(vm, a[0], v);
    int32_t r = len3(v);
    a[0] = (uint32_t)(r == -5 ? -1 : r);
    return 1;
}

static int n_normalise(ExenVM *vm, uint32_t *a) /* 0x41cf17 */
{
    int32_t v[3];
    vget(vm, a[0], v);
    int32_t l = len3(v);
    if (l <= 0) {
        a[0] = (uint32_t)-1;
        return 1;
    }
    if (l < 0x10000) {
        int32_t f = 0x10000 / l;
        for (int i = 0; i < 3; i++)
            v[i] = (int32_t)((uint32_t)v[i] * (uint32_t)f);
    } else {
        int32_t d = l >> 16;
        for (int i = 0; i < 3; i++)
            v[i] /= d;
    }
    vset(vm, a[0], v);
    a[0] = 1;
    return 1;
}

static int other(ExenVM *vm, uint32_t *a, int32_t v[3], int32_t o[3])
{
    if (!a[1]) {
        vm_throw_hash(vm, H_NPE);
        return 0;
    }
    vget(vm, a[0], v);
    vget(vm, a[1], o);
    return 1;
}

static int n_sum(ExenVM *vm, uint32_t *a)
{
    int32_t v[3], o[3];
    if (other(vm, a, v, o)) {
        for (int i = 0; i < 3; i++)
            v[i] = add32(v[i], o[i]);
        vset(vm, a[0], v);
    }
    return 0;
}

static int n_minus(ExenVM *vm, uint32_t *a)
{
    int32_t v[3], o[3];
    if (other(vm, a, v, o)) {
        for (int i = 0; i < 3; i++)
            v[i] = sub32(v[i], o[i]);
        vset(vm, a[0], v);
    }
    return 0;
}

static int n_dot(ExenVM *vm, uint32_t *a)
{
    int32_t v[3], o[3];
    if (!other(vm, a, v, o))
        return 1;
    a[0] = (uint32_t)add32(add32(M8(v[0], o[0]), M8(v[1], o[1])), M8(v[2], o[2]));
    return 1;
}

static int n_crossProduct(ExenVM *vm, uint32_t *a)
{
    int32_t v[3], o[3], r[3];
    if (other(vm, a, v, o)) {
        r[0] = sub32(M8(v[1], o[2]), M8(v[2], o[1]));
        r[1] = sub32(M8(v[2], o[0]), M8(v[0], o[2]));
        r[2] = sub32(M8(v[0], o[1]), M8(v[1], o[0]));
        vset(vm, a[0], r);
    }
    return 0;
}

static int n_scale(ExenVM *vm, uint32_t *a)
{
    int32_t v[3], s = (int32_t)a[1];
    vget(vm, a[0], v);
    for (int i = 0; i < 3; i++)
        v[i] = M8(v[i], s);
    vset(vm, a[0], v);
    return 0;
}

void vec3d_register(void)
{
    natives_register(123, n_copyFrom);
    natives_register(124, n_rotX);
    natives_register(125, n_rotY);
    natives_register(126, n_rotZ);
    natives_register(127, n_multiplyM);
    natives_register(128, n_multiplyV);
    natives_register(129, n_squareLength);
    natives_register(130, n_length);
    natives_register(131, n_normalise);
    natives_register(132, n_sum);
    natives_register(133, n_minus);
    natives_register(134, n_dot);
    natives_register(135, n_crossProduct);
    natives_register(136, n_scale);
}
