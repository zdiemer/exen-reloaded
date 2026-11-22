/* Interpreter edge cases (docs/re/OPCODES.md), run on synthetic 4CVP classes loaded after the
 * real framework. Every expectation here is the original simulator's behaviour, including the
 * places where it differs from the JVM. */
#include <stdlib.h>

#include "support.h"
#include "test.h"

#define SELF 54      /* first game class */
#define OBJ 29       /* java.lang.Object */
#define STRING 33    /* java.lang.String */
#define EXC 31       /* java.lang.Exception */
#define THROWABLE 32 /* java.lang.Throwable */
#define OOM 45       /* java.lang.OutOfMemoryError */
#define PUB_STATIC (ACC_STATIC | 1)

static TClass C; /* the class under test; rebuilt by each test */

/* ------------------------------------------------------------------ helpers */

typedef struct {
    const char *name, *desc;
    int nargs, stack, locals;
    const char *hex;
} Spec;

/* A class of straight-line static methods given as hex bytecode. */
static void build_simple(TClass *c, const Spec *s, int n)
{
    int ids[TC_MAX];
    tc_init(c, SELF, OBJ);
    for (int i = 0; i < n; i++)
        ids[i] = tc_method(c, s[i].name, s[i].desc, PUB_STATIC, (uint16_t)s[i].nargs);
    tc_layout(c);
    for (int i = 0; i < n; i++) {
        tc_code(c, ids[i], (uint16_t)s[i].stack, (uint16_t)s[i].locals);
        tc_ops(c, s[i].hex);
    }
    tc_finish(c);
}

static ExenVM *load(TClass *c)
{
    ExenVM *vm = t_vm_framework(0);
    if (!vm)
        return NULL;
    TClass *a[] = {c};
    if (t_load_classes(vm, a, 1) < 0) {
        fprintf(stderr, "load: %s\n", exen_error(vm));
        exen_destroy(vm);
        return NULL;
    }
    return vm;
}

static int last_rc;

static int32_t call_i(ExenVM *vm, const char *name, const char *desc, const uint32_t *a, int n)
{
    uint32_t r[2] = {0xdeadbeefu, 0};
    last_rc = t_call(vm, SELF, name, desc, a, n, r);
    return (int32_t)r[0];
}

static int32_t i0(ExenVM *vm, const char *name) { return call_i(vm, name, "()I", NULL, 0); }

static int32_t i1(ExenVM *vm, const char *name, int32_t x)
{
    uint32_t a[1] = {(uint32_t)x};
    return call_i(vm, name, "(I)I", a, 1);
}

static int32_t i2(ExenVM *vm, const char *name, int32_t x, int32_t y)
{
    uint32_t a[2] = {(uint32_t)x, (uint32_t)y};
    return call_i(vm, name, "(II)I", a, 2);
}

static void put_long(uint32_t *a, int64_t v)
{
    a[0] = (uint32_t)(uint64_t)v;
    a[1] = (uint32_t)((uint64_t)v >> 32);
}

static int64_t get_long(const uint32_t *r) { return (int64_t)((uint64_t)r[0] | (uint64_t)r[1] << 32); }

static int64_t call_j(ExenVM *vm, const char *name, const char *desc, const uint32_t *a, int n)
{
    uint32_t r[2] = {0, 0};
    last_rc = t_call(vm, SELF, name, desc, a, n, r);
    return get_long(r);
}

static int64_t j2(ExenVM *vm, const char *name, int64_t x, int64_t y)
{
    uint32_t a[4];
    put_long(a, x);
    put_long(a + 2, y);
    return call_j(vm, name, "(JJ)J", a, 4);
}

static int64_t ji(ExenVM *vm, const char *name, int64_t x, int32_t s)
{
    uint32_t a[3];
    put_long(a, x);
    a[2] = (uint32_t)s;
    return call_j(vm, name, "(JI)J", a, 3);
}

/* Run name+desc on a fresh VM and expect the VM to halt with `needle` in the reason. */
static void expect_halt_(int line, TClass *c, const char *name, const char *desc,
                         const uint32_t *a, int n, const char *needle)
{
    ExenVM *vm = load(c);
    t_checks++;
    if (!vm) {
        T_FAIL("line %d: load failed", line);
        return;
    }
    uint32_t r[2];
    int rc = t_call(vm, SELF, name, desc, a, n, r);
    t_checks++;
    if (rc != -1 || !exen_halted(vm) || !strstr(exen_error(vm), needle))
        T_FAIL("line %d: %s%s: expected a halt containing \"%s\", got rc=%d halted=%d \"%s\"",
               line, name, desc, needle, rc, exen_halted(vm), exen_error(vm));
    exen_destroy(vm);
}
#define expect_halt(...) expect_halt_(__LINE__, __VA_ARGS__)

static void halt_ii(TClass *c, const char *name, int32_t x, int32_t y, const char *needle)
{
    uint32_t a[2] = {(uint32_t)x, (uint32_t)y};
    expect_halt(c, name, "(II)I", a, 2, needle);
}

static void halt_jj(TClass *c, const char *name, int64_t x, int64_t y, const char *needle)
{
    uint32_t a[4];
    put_long(a, x);
    put_long(a + 2, y);
    expect_halt(c, name, "(JJ)J", a, 4, needle);
}

#define ARITH "86f6360b"  /* java.lang.ArithmeticException: not in the framework, so fatal */
#define AIOOBE "1d3c3033" /* java.lang.ArrayIndexOutOfBoundsException */
#define NPE "364a8d65"
#define NEGSIZE "0794f618"
#define STACKOVF "a2c72bf1"
#define LINKAGE "13523f29"
#define ILLACCESS "39f3fe57"

/* ------------------------------------------------------------------ int arithmetic */

static const Spec INT_OPS[] = {
    {"add", "(II)I", 2, 2, 2, "d9 dd 60 e9"},
    {"sub", "(II)I", 2, 2, 2, "d9 dd 64 e9"},
    {"mul", "(II)I", 2, 2, 2, "d9 dd 68 e9"},
    {"div", "(II)I", 2, 2, 2, "d9 dd 6c e9"},
    {"rem", "(II)I", 2, 2, 2, "d9 dd 70 e9"},
    {"shl", "(II)I", 2, 2, 2, "d9 dd 78 e9"},
    {"shr", "(II)I", 2, 2, 2, "d9 dd 7a e9"},
    {"ushr", "(II)I", 2, 2, 2, "d9 dd 7c e9"},
    {"and", "(II)I", 2, 2, 2, "d9 dd 7e e9"},
    {"or", "(II)I", 2, 2, 2, "d9 dd 80 e9"},
    {"xor", "(II)I", 2, 2, 2, "d9 dd 82 e9"},
    {"neg", "(I)I", 1, 1, 1, "d9 74 e9"},
    {"swapsub", "(II)I", 2, 2, 2, "d9 dd 5f 64 e9"},      /* b - a */
    {"dupx1", "(II)I", 2, 3, 2, "d9 dd 5a 64 68 e9"},     /* b * (a - b) */
    {"i2c", "(I)I", 1, 1, 1, "d9 92 e9"},
    {"i2b", "(I)I", 1, 1, 1, "d9 91 e9"},
    {"i2s", "(I)I", 1, 1, 1, "d9 93 e9"},
    /* undefined opcodes are silent 1-byte no-ops: fconst_0, invokedynamic, impdep2 */
    {"undef", "()I", 0, 1, 0, "08 0b ba ff e9"},
    {"iconsts", "()I", 0, 7, 0, "02 03 04 05 06 07 08 60 60 60 60 60 60 e9"}, /* -1+0+..+5 */
    {"bipush", "()I", 0, 1, 0, "10 80 e9"},
};

static void test_int_arithmetic(void)
{
    build_simple(&C, INT_OPS, (int)(sizeof INT_OPS / sizeof INT_OPS[0]));
    ExenVM *vm = load(&C);
    REQUIRE(vm);
    CHECK_EQ(i2(vm, "add", INT32_MAX, 1), INT32_MIN); /* wrapping */
    CHECK_EQ(last_rc, 1);
    CHECK_EQ(i2(vm, "sub", INT32_MIN, 1), INT32_MAX);
    CHECK_EQ(i2(vm, "mul", 0x10000, 0x10000), 0);
    CHECK_EQ(i2(vm, "mul", -3, 7), -21);
    CHECK_EQ(i2(vm, "div", 7, 2), 3);
    CHECK_EQ(i2(vm, "div", -7, 2), -3); /* truncates toward zero */
    CHECK_EQ(i2(vm, "div", 7, -2), -3);
    CHECK_EQ(i2(vm, "div", INT32_MIN, 2), INT32_MIN / 2);
    CHECK_EQ(i2(vm, "rem", -7, 2), -1);
    CHECK_EQ(i2(vm, "rem", 7, -2), 1);
    CHECK_EQ(i2(vm, "rem", INT32_MIN, 3), INT32_MIN % 3);
    CHECK_EQ(i2(vm, "shl", 1, 33), 2);   /* count & 31 */
    CHECK_EQ(i2(vm, "shl", 1, 31), INT32_MIN);
    CHECK_EQ(i2(vm, "shr", -8, 1), -4);
    CHECK_EQ(i2(vm, "shr", -1, 63), -1);
    CHECK_EQ(i2(vm, "ushr", -1, 28), 15);
    CHECK_EQ(i2(vm, "ushr", -8, 33), 0x7ffffffc);
    CHECK_EQ(i2(vm, "and", 0x0ff0, 0x00ff), 0x00f0);
    CHECK_EQ(i2(vm, "or", 0x0f00, 0x00f0), 0x0ff0);
    CHECK_EQ(i2(vm, "xor", -1, 0x0f), -16);
    CHECK_EQ(i1(vm, "neg", INT32_MIN), INT32_MIN);
    CHECK_EQ(i1(vm, "neg", 5), -5);
    CHECK_EQ(i2(vm, "swapsub", 10, 3), -7);
    CHECK_EQ(i2(vm, "dupx1", 7, 3), 12);
    /* chars are 8-bit: i2c masks with 0xff */
    CHECK_EQ(i1(vm, "i2c", 0x1ff), 0xff);
    CHECK_EQ(i1(vm, "i2c", -1), 0xff);
    CHECK_EQ(i1(vm, "i2c", 0x141), 0x41);
    CHECK_EQ(i1(vm, "i2b", 0xff), -1);
    CHECK_EQ(i1(vm, "i2b", 0x17f), 127);
    CHECK_EQ(i1(vm, "i2s", 0x18000), -32768);
    CHECK_EQ(i0(vm, "undef"), 5);
    CHECK_EQ(i0(vm, "iconsts"), 14);
    CHECK_EQ(i0(vm, "bipush"), -128);
    CHECK(!exen_halted(vm));
    exen_destroy(vm);
}

static void test_int_division_traps(void)
{
    build_simple(&C, INT_OPS, (int)(sizeof INT_OPS / sizeof INT_OPS[0]));
    /* ArithmeticException has no class in the framework, so the VM stops (0x40dccb) */
    halt_ii(&C, "div", 7, 0, ARITH);
    halt_ii(&C, "rem", 7, 0, ARITH);
    /* MIN_INT / -1 throws too (the JVM gives MIN_INT) */
    halt_ii(&C, "div", INT32_MIN, -1, ARITH);
    halt_ii(&C, "rem", INT32_MIN, -1, ARITH);
}

/* ------------------------------------------------------------------ long arithmetic */

static const Spec LONG_OPS[] = {
    {"ladd", "(JJ)J", 4, 4, 4, "db e3 61 ea"},
    {"lsub", "(JJ)J", 4, 4, 4, "db e3 65 ea"},
    {"lmul", "(JJ)J", 4, 4, 4, "db e3 69 ea"},
    {"ldiv", "(JJ)J", 4, 4, 4, "db e3 6d ea"},
    {"lrem", "(JJ)J", 4, 4, 4, "db e3 71 ea"},
    {"land", "(JJ)J", 4, 4, 4, "db e3 7f ea"},
    {"lor", "(JJ)J", 4, 4, 4, "db e3 81 ea"},
    {"lxor", "(JJ)J", 4, 4, 4, "db e3 83 ea"},
    {"lshl", "(JI)J", 3, 3, 3, "db e1 79 ea"},
    {"lshr", "(JI)J", 3, 3, 3, "db e1 7b ea"},
    {"lushr", "(JI)J", 3, 3, 3, "db e1 7d ea"},
    {"lcmp", "(JJ)I", 4, 4, 4, "db e3 94 e9"},
    {"lneg", "(J)J", 2, 2, 2, "db 75 ea"},
    {"i2l", "(I)J", 1, 2, 1, "d9 85 ea"},
    {"l2i", "(J)I", 2, 2, 2, "db 88 e9"},
    {"ldup2", "(J)J", 2, 4, 2, "db 5c 61 ea"},            /* x + x */
    {"lpop2", "(JJ)J", 4, 4, 4, "db e3 58 ea"},           /* x */
    {"lconst", "()J", 0, 4, 0, "0a 0a 61 ea"},            /* 1 + 1 */
    {"lstore", "(J)J", 2, 2, 4, "db e4 e3 ea"},           /* lstore_2 lload_2 */
};

static void test_long_arithmetic(void)
{
    build_simple(&C, LONG_OPS, (int)(sizeof LONG_OPS / sizeof LONG_OPS[0]));
    ExenVM *vm = load(&C);
    REQUIRE(vm);
    const int64_t BIG = INT64_C(0x123456789abcdef0);
    CHECK_EQ(j2(vm, "ladd", INT64_MAX, 1), INT64_MIN);
    CHECK_EQ(last_rc, 2);
    CHECK_EQ(j2(vm, "ladd", 0xffffffffLL, 1), 0x100000000LL); /* carry across words */
    CHECK_EQ(j2(vm, "lsub", 0x100000000LL, 1), 0xffffffffLL);
    CHECK_EQ(j2(vm, "lmul", BIG, 16), (int64_t)((uint64_t)BIG * 16u));
    CHECK_EQ(j2(vm, "lmul", -3, 0x100000000LL), -0x300000000LL);
    CHECK_EQ(j2(vm, "ldiv", -BIG, 7), -BIG / 7);
    CHECK_EQ(j2(vm, "lrem", -BIG, 7), -BIG % 7);
    /* MIN / -1 goes through __alldiv and wraps; MIN / MIN is the buggy trap (below) */
    CHECK_EQ(j2(vm, "ldiv", INT64_MIN, -1), INT64_MIN);
    CHECK_EQ(j2(vm, "lrem", INT64_MIN, -1), 0);
    CHECK_EQ(j2(vm, "land", BIG, 0xffff0000ffffLL), BIG & 0xffff0000ffffLL);
    CHECK_EQ(j2(vm, "lor", BIG, 1), BIG | 1);
    CHECK_EQ(j2(vm, "lxor", BIG, -1), ~BIG);
    CHECK_EQ(ji(vm, "lshl", 1, 1), 2);
    CHECK_EQ(ji(vm, "lshl", 1, 63), INT64_MIN);
    CHECK_EQ(ji(vm, "lshl", 1, 32), 0x100000000LL);
    /* lshl count is not masked: __allshl gives 0 for counts >= 64 (the JVM masks with 63) */
    CHECK_EQ(ji(vm, "lshl", 1, 64), 0);
    CHECK_EQ(ji(vm, "lshl", 1, 100), 0);
    CHECK_EQ(ji(vm, "lshl", -1, 64), 0);
    CHECK_EQ(ji(vm, "lshr", -8, 1), -4);
    CHECK_EQ(ji(vm, "lshr", BIG, 36), BIG >> 36);
    /* lshr count >= 64 gives the sign fill */
    CHECK_EQ(ji(vm, "lshr", -8, 64), -1);
    CHECK_EQ(ji(vm, "lshr", -8, 200), -1);
    CHECK_EQ(ji(vm, "lshr", 8, 64), 0);
    CHECK_EQ(ji(vm, "lshr", INT64_MAX, 70), 0);
    /* lushr is masked with 63 */
    CHECK_EQ(ji(vm, "lushr", -1, 60), 15);
    CHECK_EQ(ji(vm, "lushr", -1, 64), -1);
    CHECK_EQ(ji(vm, "lushr", -1, 65), INT64_MAX);
    {
        uint32_t a[4];
        put_long(a, -1);
        put_long(a + 2, 1);
        CHECK_EQ(call_i(vm, "lcmp", "(JJ)I", a, 4), -1);
        put_long(a, 0x100000000LL);
        put_long(a + 2, 0xffffffffLL);
        CHECK_EQ(call_i(vm, "lcmp", "(JJ)I", a, 4), 1);
        put_long(a + 2, 0x100000000LL);
        CHECK_EQ(call_i(vm, "lcmp", "(JJ)I", a, 4), 0);
        put_long(a, INT64_MIN);
        CHECK_EQ(call_j(vm, "lneg", "(J)J", a, 2), INT64_MIN);
        put_long(a, BIG);
        CHECK_EQ(call_j(vm, "lneg", "(J)J", a, 2), -BIG);
        CHECK_EQ(call_i(vm, "l2i", "(J)I", a, 2), (int32_t)(uint32_t)(uint64_t)BIG);
        CHECK_EQ(call_j(vm, "ldup2", "(J)J", a, 2), BIG * 2);
        CHECK_EQ(call_j(vm, "lstore", "(J)J", a, 2), BIG);
        put_long(a + 2, 5);
        CHECK_EQ(call_j(vm, "lpop2", "(JJ)J", a, 4), BIG);
        a[0] = (uint32_t)-5;
        CHECK_EQ(call_j(vm, "i2l", "(I)J", a, 1), -5);
        CHECK_EQ(call_j(vm, "lconst", "()J", NULL, 0), 2);
    }
    CHECK(!exen_halted(vm));
    exen_destroy(vm);

    halt_jj(&C, "ldiv", 1, 0, ARITH);
    halt_jj(&C, "lrem", 1, 0, ARITH);
    /* the original's overflow check compares the divisor with MIN instead of -1 */
    halt_jj(&C, "ldiv", INT64_MIN, INT64_MIN, ARITH);
    halt_jj(&C, "lrem", INT64_MIN, INT64_MIN, ARITH);
}

/* ------------------------------------------------------------------ constants */

static void test_constants(void)
{
    tc_init(&C, SELF, OBJ);
    int m32 = tc_method(&C, "c32", "()I", PUB_STATIC, 0);
    int m64 = tc_method(&C, "c64", "()J", PUB_STATIC, 0);
    int msi = tc_method(&C, "si", "()I", PUB_STATIC, 0);
    int msi1 = tc_method(&C, "si1", "()I", PUB_STATIC, 0);
    tc_layout(&C);
    tc_code(&C, m32, 1, 0);
    tc_ldc(&C, (int32_t)0xdeadbeefu);
    tc_u8(&C, OP_IRETURN);
    tc_code(&C, m64, 2, 0);
    tc_u8(&C, OP_NOP); /* make the operand need padding */
    tc_ldc2(&C, INT64_C(0x0123456789abcdef));
    tc_u8(&C, OP_LRETURN);
    tc_code(&C, msi, 1, 0);
    tc_sipush(&C, -12345);
    tc_u8(&C, OP_IRETURN);
    tc_code(&C, msi1, 1, 0);
    tc_u8(&C, OP_NOP);
    tc_sipush(&C, 0x7abc);
    tc_u8(&C, OP_IRETURN);
    tc_finish(&C);
    ExenVM *vm = load(&C);
    REQUIRE(vm);
    CHECK_EQ((uint32_t)i0(vm, "c32"), 0xdeadbeefu);
    CHECK_EQ(call_j(vm, "c64", "()J", NULL, 0), INT64_C(0x0123456789abcdef));
    CHECK_EQ(i0(vm, "si"), -12345);
    CHECK_EQ(i0(vm, "si1"), 0x7abc);
    exen_destroy(vm);
}

/* ------------------------------------------------------------------ branches and switches */

static const uint8_t IF_OPS[] = {OP_IFEQ, OP_IFNE, OP_IFLT, OP_IFGE, OP_IFGT, OP_IFLE};
static const uint8_t ICMP_OPS[] = {OP_IF_ICMPEQ, OP_IF_ICMPNE, OP_IF_ICMPLT,
                                   OP_IF_ICMPGE, OP_IF_ICMPGT, OP_IF_ICMPLE};

static int c_cond(uint8_t op, int32_t x, int32_t y)
{
    switch (op) {
    case OP_IFEQ: case OP_IF_ICMPEQ: case OP_IF_ACMPEQ: return x == y;
    case OP_IFNE: case OP_IF_ICMPNE: case OP_IF_ACMPNE: return x != y;
    case OP_IFLT: case OP_IF_ICMPLT: return x < y;
    case OP_IFGE: case OP_IF_ICMPGE: return x >= y;
    case OP_IFGT: case OP_IF_ICMPGT: return x > y;
    case OP_IFLE: case OP_IF_ICMPLE: return x <= y;
    case OP_IFNULL: return x == 0;
    case OP_IFNONNULL: return x != 0;
    }
    return -1;
}

/* op taken -> 1, not taken -> 0 */
static void emit_cond(TClass *c, uint8_t op, int two)
{
    int l = tc_label(c);
    tc_u8(c, OP_ILOAD_0);
    if (two)
        tc_u8(c, OP_ILOAD_1);
    tc_br(c, op, l);
    tc_u8(c, OP_ICONST_0);
    tc_u8(c, OP_IRETURN);
    tc_bind(c, l);
    tc_u8(c, OP_ICONST_1);
    tc_u8(c, OP_IRETURN);
}

static void test_branches(void)
{
    char names[32][8];
    int ids[32], n = 0;
    uint8_t ops[32];
    int two[32];
    tc_init(&C, SELF, OBJ);
    for (size_t i = 0; i < sizeof IF_OPS; i++, n++) {
        ops[n] = IF_OPS[i];
        two[n] = 0;
    }
    for (size_t i = 0; i < sizeof ICMP_OPS; i++, n++) {
        ops[n] = ICMP_OPS[i];
        two[n] = 1;
    }
    ops[n] = OP_IF_ACMPEQ, two[n++] = 1;
    ops[n] = OP_IF_ACMPNE, two[n++] = 1;
    ops[n] = OP_IFNULL, two[n++] = 0;
    ops[n] = OP_IFNONNULL, two[n++] = 0;
    for (int i = 0; i < n; i++) {
        snprintf(names[i], sizeof names[i], "b%02x", ops[i]);
        ids[i] = tc_method(&C, names[i], two[i] ? "(II)I" : "(I)I", PUB_STATIC,
                           (uint16_t)(two[i] ? 2 : 1));
    }
    int msum = tc_method(&C, "sum", "(I)I", PUB_STATIC, 1);
    int mfar = tc_method(&C, "far", "()I", PUB_STATIC, 0);
    tc_layout(&C);
    for (int i = 0; i < n; i++) {
        tc_code(&C, ids[i], 2, (uint16_t)(two[i] ? 2 : 1));
        if (i & 1)
            tc_u8(&C, OP_NOP); /* exercise both operand alignments */
        emit_cond(&C, ops[i], two[i]);
    }
    /* sum 1..n: a forward exit and a backward goto, both absolute block offsets */
    tc_code(&C, msum, 2, 2);
    {
        int top = tc_label(&C), end = tc_label(&C);
        tc_ops(&C, "03 de");          /* iconst_0 istore_1 */
        tc_bind(&C, top);
        tc_u8(&C, OP_ILOAD_0);
        tc_br(&C, OP_IFLE, end);
        tc_ops(&C, "dd d9 60 de");    /* iload_1 iload_0 iadd istore_1 */
        tc_ops(&C, "84 00 ff");       /* iinc 0 -1 */
        tc_br(&C, OP_GOTO, top);
        tc_bind(&C, end);
        tc_ops(&C, "dd e9");          /* iload_1 ireturn */
    }
    /* a jump over 600 bytes, so the target needs both bytes of the u16 */
    tc_code(&C, mfar, 1, 0);
    {
        int l = tc_label(&C);
        tc_br(&C, OP_GOTO, l);
        for (int i = 0; i < 600; i++)
            tc_u8(&C, OP_ICONST_M1); /* never executed */
        tc_bind(&C, l);
        tc_ops(&C, "06 e9");
    }
    tc_finish(&C);
    REQUIRE(C.label[C.nlabel - 1] > 0x100);

    ExenVM *vm = load(&C);
    REQUIRE(vm);
    static const int32_t vals[] = {-2, 0, 3};
    for (int i = 0; i < n; i++) {
        t_current = names[i];
        for (int x = 0; x < 3; x++) {
            if (!two[i]) {
                CHECK_EQ(i1(vm, names[i], vals[x]), c_cond(ops[i], vals[x], 0));
                continue;
            }
            for (int y = 0; y < 3; y++)
                CHECK_EQ(i2(vm, names[i], vals[x], vals[y]), c_cond(ops[i], vals[x], vals[y]));
        }
    }
    t_current = "test_branches";
    CHECK_EQ(i1(vm, "sum", 100), 5050);
    CHECK_EQ(i1(vm, "sum", 0), 0);
    CHECK_EQ(i1(vm, "sum", -5), 0);
    CHECK_EQ(i0(vm, "far"), 3);
    CHECK(!exen_halted(vm));
    exen_destroy(vm);
}

static void emit_cases(TClass *c, const int *labels, int n, int def)
{
    for (int i = 0; i < n; i++) {
        tc_bind(c, labels[i]);
        tc_bipush(c, (int8_t)(10 + i));
        tc_u8(c, OP_IRETURN);
    }
    tc_bind(c, def);
    tc_u8(c, OP_ICONST_M1);
    tc_u8(c, OP_IRETURN);
}

static void test_switches(void)
{
    static const int32_t lkeys[3] = {-100000, 5, 70000};
    static const int16_t skeys[3] = {-300, 7, 1000};
    int m[8];
    tc_init(&C, SELF, OBJ);
    m[0] = tc_method(&C, "tsw0", "(I)I", PUB_STATIC, 1);
    m[1] = tc_method(&C, "tsw1", "(I)I", PUB_STATIC, 1);
    m[2] = tc_method(&C, "lsw0", "(I)I", PUB_STATIC, 1);
    m[3] = tc_method(&C, "lsw1", "(I)I", PUB_STATIC, 1);
    m[4] = tc_method(&C, "ctsw0", "(I)I", PUB_STATIC, 1);
    m[5] = tc_method(&C, "ctsw1", "(I)I", PUB_STATIC, 1);
    m[6] = tc_method(&C, "clsw0", "(I)I", PUB_STATIC, 1);
    m[7] = tc_method(&C, "clsw1", "(I)I", PUB_STATIC, 1);
    tc_layout(&C);
    for (int pad = 0; pad < 2; pad++) {
        int def, L[4];
        /* tableswitch: a2 u16 default, a4 s32 low, s32 high, u16 targets */
        tc_code(&C, m[0 + pad], 1, 1);
        for (int i = 0; i < pad; i++)
            tc_u8(&C, OP_NOP);
        def = tc_label(&C);
        for (int i = 0; i < 4; i++)
            L[i] = tc_label(&C);
        tc_u8(&C, OP_ILOAD_0);
        tc_u8(&C, OP_TABLESWITCH);
        tc_pad2(&C);
        tc_target(&C, def);
        tc_pad4(&C);
        tc_u32(&C, (uint32_t)-1);
        tc_u32(&C, 2);
        for (int i = 0; i < 4; i++)
            tc_target(&C, L[i]);
        emit_cases(&C, L, 4, def);

        /* lookupswitch: a2 u16 default, u16 n, a4 s32 keys[n], u16 targets[n] */
        tc_code(&C, m[2 + pad], 1, 1);
        for (int i = 0; i < pad; i++)
            tc_u8(&C, OP_NOP);
        def = tc_label(&C);
        for (int i = 0; i < 3; i++)
            L[i] = tc_label(&C);
        tc_u8(&C, OP_ILOAD_0);
        tc_u8(&C, OP_LOOKUPSWITCH);
        tc_pad2(&C);
        tc_target(&C, def);
        tc_u16(&C, 3);
        tc_pad4(&C);
        for (int i = 0; i < 3; i++)
            tc_u32(&C, (uint32_t)lkeys[i]);
        for (int i = 0; i < 3; i++)
            tc_target(&C, L[i]);
        emit_cases(&C, L, 3, def);

        /* tableswitch_s16: a2 u16 default, s16 low, s16 high, u16 targets */
        tc_code(&C, m[4 + pad], 1, 1);
        for (int i = 0; i < pad; i++)
            tc_u8(&C, OP_NOP);
        def = tc_label(&C);
        for (int i = 0; i < 4; i++)
            L[i] = tc_label(&C);
        tc_u8(&C, OP_ILOAD_0);
        tc_u8(&C, OP_TABLESWITCH_S16);
        tc_pad2(&C);
        tc_target(&C, def);
        tc_u16(&C, (uint16_t)-1);
        tc_u16(&C, 2);
        for (int i = 0; i < 4; i++)
            tc_target(&C, L[i]);
        emit_cases(&C, L, 4, def);

        /* lookupswitch_s16: a2 u16 default, u16 n, s16 keys[n], u16 targets[n] */
        tc_code(&C, m[6 + pad], 1, 1);
        for (int i = 0; i < pad; i++)
            tc_u8(&C, OP_NOP);
        def = tc_label(&C);
        for (int i = 0; i < 3; i++)
            L[i] = tc_label(&C);
        tc_u8(&C, OP_ILOAD_0);
        tc_u8(&C, OP_LOOKUPSWITCH_S16);
        tc_pad2(&C);
        tc_target(&C, def);
        tc_u16(&C, 3);
        for (int i = 0; i < 3; i++)
            tc_u16(&C, (uint16_t)skeys[i]);
        for (int i = 0; i < 3; i++)
            tc_target(&C, L[i]);
        emit_cases(&C, L, 3, def);
    }
    tc_finish(&C);
    ExenVM *vm = load(&C);
    REQUIRE(vm);
    for (int pad = 0; pad < 2; pad++) {
        const char *tsw = pad ? "tsw1" : "tsw0", *ctsw = pad ? "ctsw1" : "ctsw0";
        const char *lsw = pad ? "lsw1" : "lsw0", *clsw = pad ? "clsw1" : "clsw0";
        for (int v = -3; v <= 4; v++) {
            int want = v >= -1 && v <= 2 ? 10 + (v + 1) : -1;
            CHECK_EQ(i1(vm, tsw, v), want);
            CHECK_EQ(i1(vm, ctsw, v), want);
        }
        CHECK_EQ(i1(vm, tsw, INT32_MIN), -1);
        CHECK_EQ(i1(vm, tsw, INT32_MAX), -1);
        for (int k = 0; k < 3; k++) {
            CHECK_EQ(i1(vm, lsw, lkeys[k]), 10 + k);
            CHECK_EQ(i1(vm, clsw, skeys[k]), 10 + k);
        }
        CHECK_EQ(i1(vm, lsw, 6), -1);
        CHECK_EQ(i1(vm, lsw, 70000 - 65536), -1); /* keys are full 32-bit */
        CHECK_EQ(i1(vm, clsw, 0), -1);
        /* the compact form compares (int)(s16)key: 65536 - 300 is not -300 */
        CHECK_EQ(i1(vm, clsw, 65536 - 300), -1);
    }
    CHECK(!exen_halted(vm));
    exen_destroy(vm);
}

static void test_iinc(void)
{
    static const Spec S[] = {
        {"inc", "(I)I", 1, 1, 1, "84 00 64 84 00 fd d9 e9"},              /* x + 100 - 3 */
        {"inc5", "()I", 0, 1, 6, "03 d6 05 84 05 80 84 05 7f d5 05 e9"},  /* 0 - 128 + 127 */
        {"incw", "(I)I", 1, 1, 1, "84 00 01 d9 e9"},                      /* wraps */
    };
    build_simple(&C, S, 3);
    ExenVM *vm = load(&C);
    REQUIRE(vm);
    CHECK_EQ(i1(vm, "inc", 3), 100);
    CHECK_EQ(i0(vm, "inc5"), -1);
    CHECK_EQ(i1(vm, "incw", INT32_MAX), INT32_MIN);
    exen_destroy(vm);
}

/* ------------------------------------------------------------------ strings */

static void test_string_literals(void)
{
    tc_init(&C, SELF, OBJ);
    int cs = tc_class(&C, STRING);
    int len = tc_mref(&C, exen_member_hash("length", "()I"), 0, cs);
    int m_len = tc_method(&C, "len", "()I", PUB_STATIC, 0);
    int m_len0 = tc_method(&C, "len0", "()I", PUB_STATIC, 0);
    int m_str = tc_method(&C, "str", "()Ljava/lang/String;", PUB_STATIC, 0);
    int m_fresh = tc_method(&C, "fresh", "()I", PUB_STATIC, 0);
    tc_layout(&C);

    tc_code(&C, m_len, 1, 0);
    tc_ldstr(&C, "hello");
    tc_op16(&C, OP_INVOKEVIRTUAL, C.mref_off[len]);
    tc_u8(&C, OP_IRETURN);

    tc_code(&C, m_len0, 1, 0);
    tc_u8(&C, OP_NOP);
    tc_ldstr(&C, "");
    tc_op16(&C, OP_INVOKEVIRTUAL, C.mref_off[len]);
    tc_u8(&C, OP_IRETURN);

    tc_code(&C, m_str, 1, 0);
    tc_ldstr(&C, "ExEn!");
    tc_u8(&C, OP_ARETURN);

    /* run the same ldc_string twice and compare the references: literals are not interned */
    tc_code(&C, m_fresh, 2, 2);
    {
        int top = tc_label(&C), cmp = tc_label(&C), diff = tc_label(&C);
        tc_ops(&C, "01 4c");            /* aconst_null astore_1 */
        tc_bind(&C, top);
        tc_ldstr(&C, "a");
        tc_u8(&C, OP_ALOAD_1);
        tc_br(&C, OP_IFNONNULL, cmp);
        tc_u8(&C, OP_ASTORE_1);
        tc_br(&C, OP_GOTO, top);
        tc_bind(&C, cmp);
        tc_u8(&C, OP_ALOAD_1);
        tc_br(&C, OP_IF_ACMPNE, diff);
        tc_ops(&C, "03 e9");
        tc_bind(&C, diff);
        tc_ops(&C, "04 e9");
    }
    tc_finish(&C);

    ExenVM *vm = load(&C);
    REQUIRE(vm);
    uint32_t before = exen_native_calls(vm, 158, NULL);
    const char *nm = NULL;
    exen_native_calls(vm, 158, &nm);
    CHECK_STR(nm, "length");
    CHECK_EQ(i0(vm, "len"), 5);
    CHECK_EQ(last_rc, 1);
    CHECK_EQ(exen_native_calls(vm, 158, NULL), before + 1); /* String.length is native #158 */
    CHECK_EQ(i0(vm, "len0"), 0);
    CHECK_EQ(i0(vm, "fresh"), 1);

    uint32_t r[1] = {0};
    CHECK_EQ(t_call(vm, SELF, "str", "()Ljava/lang/String;", NULL, 0, r), 1);
    REQUIRE(r[0] != 0);
    CHECK(heap_is_object(vm, r[0]));
    CHECK_EQ(obj_cls(vm, r[0]), STRING);
    const uint8_t *chars = NULL;
    int n = vm_string_get(vm, r[0], &chars);
    CHECK_EQ(n, 5);
    CHECK(chars && memcmp(chars, "ExEn!", 5) == 0);
    /* the String points straight at the literal inside the class block */
    CHECK(chars > P(vm, vm->classes[SELF].block) &&
          chars < P(vm, vm->classes[SELF].block + vm->classes[SELF].size));
    CHECK(!exen_halted(vm));
    exen_destroy(vm);
}

/* ------------------------------------------------------------------ arrays */

static void test_arrays(void)
{
    tc_init(&C, SELF, OBJ);
    int m_alen = tc_method(&C, "alen", "(I)I", PUB_STATIC, 1);
    int m_aget = tc_method(&C, "aget", "(I)I", PUB_STATIC, 1);
    int m_chars = tc_method(&C, "chars", "(I)I", PUB_STATIC, 1);
    int m_bytes = tc_method(&C, "bytes", "(I)I", PUB_STATIC, 1);
    int m_bools = tc_method(&C, "bools", "(I)I", PUB_STATIC, 1);
    int m_shorts = tc_method(&C, "shorts", "(I)I", PUB_STATIC, 1);
    int m_longs = tc_method(&C, "longs", "(J)J", PUB_STATIC, 2);
    int m_refs = tc_method(&C, "refs", "(I)I", PUB_STATIC, 1);
    int m_mna = tc_method(&C, "mna", "(I)I", PUB_STATIC, 1);
    int m_wrong = tc_method(&C, "wrong", "()I", PUB_STATIC, 0);
    int m_null = tc_method(&C, "anull", "()I", PUB_STATIC, 0);
    tc_layout(&C);

    tc_code(&C, m_alen, 1, 1);
    tc_u8(&C, OP_ILOAD_0);
    tc_op16(&C, OP_NEWARRAY, T_INT);
    tc_ops(&C, "be e9"); /* arraylength ireturn */

    tc_code(&C, m_aget, 3, 2);
    tc_bipush(&C, 4);
    tc_op16(&C, OP_NEWARRAY, T_INT);
    tc_ops(&C, "4c 2b 05");     /* astore_1 aload_1 iconst_2 */
    tc_bipush(&C, 42);
    tc_ops(&C, "4f 2b d9 2e e9"); /* iastore aload_1 iload_0 iaload ireturn */

    /* store the argument at [0] and read it back with the type's own load */
    struct { int m; uint16_t tw; const char *ops; } st[] = {
        {m_chars, T_CHAR, "4c 2b 03 d9 55 2b 03 34 e9"},  /* castore / caload */
        {m_bytes, T_BYTE, "4c 2b 03 d9 54 2b 03 33 e9"},  /* bastore / baload */
        {m_bools, T_BOOL, "4c 2b 03 d9 54 2b 03 33 e9"},  /* baload accepts boolean[] */
        {m_shorts, T_SHORT, "4c 2b 03 d9 56 2b 03 35 e9"}, /* sastore / saload */
    };
    for (size_t i = 0; i < sizeof st / sizeof st[0]; i++) {
        tc_code(&C, st[i].m, 3, 2);
        tc_u8(&C, OP_ICONST_1);
        tc_op16(&C, OP_NEWARRAY, st[i].tw);
        tc_ops(&C, st[i].ops);
    }

    tc_code(&C, m_longs, 4, 3);
    tc_u8(&C, OP_ICONST_2);
    tc_op16(&C, OP_NEWARRAY, T_LONG);
    tc_ops(&C, "4d 2c 04 db 50 2c 04 2f ea"); /* astore_2 aload_2 iconst_1 lload_0 lastore
                                                 aload_2 iconst_1 laload lreturn */

    /* Object[2]: a[1] = a; return a[arg].length (a[0] is null -> NPE) */
    tc_code(&C, m_refs, 4, 2);
    tc_u8(&C, OP_ICONST_2);
    tc_op16(&C, OP_NEWARRAY, T_OBJECT);
    tc_pad2(&C);
    tc_u16(&C, C.cls_off[1]);
    tc_ops(&C, "4c 2b 04 2b 53 2b d9 32 be e9"); /* astore_1 aload_1 iconst_1 aload_1 aastore
                                                    aload_1 iload_0 aaload arraylength ireturn */

    /* new int[2][3]; return the length of dimension arg (0 -> 2, 1 -> 3) */
    tc_code(&C, m_mna, 2, 2);
    {
        int sub = tc_label(&C);
        tc_ops(&C, "05 06");    /* iconst_2 iconst_3 */
        tc_u8(&C, OP_MULTIANEWARRAY);
        tc_u8(&C, 2);
        tc_pad2(&C);
        tc_u16(&C, 0x100 | T_INT); /* [[I */
        tc_ops(&C, "4c d9");    /* astore_1 iload_0 */
        tc_br(&C, OP_IFNE, sub);
        tc_ops(&C, "2b be e9");
        tc_bind(&C, sub);
        tc_ops(&C, "2b 04 32 be e9"); /* aload_1 iconst_1 aaload arraylength ireturn */
    }

    /* iaload on a byte[]: the type mask fails -> java.lang.Exception (catchable) */
    tc_code(&C, m_wrong, 2, 0);
    tc_u8(&C, OP_ICONST_1);
    tc_op16(&C, OP_NEWARRAY, T_BYTE);
    tc_ops(&C, "03 2e e9");

    tc_code(&C, m_null, 1, 0);
    tc_ops(&C, "01 be e9"); /* aconst_null arraylength */
    tc_finish(&C);

    ExenVM *vm = load(&C);
    REQUIRE(vm);
    CHECK_EQ(i1(vm, "alen", 0), 0);
    CHECK_EQ(i1(vm, "alen", 7), 7);
    CHECK_EQ(i1(vm, "alen", 0xffff), 0xffff);
    CHECK_EQ(i1(vm, "aget", 2), 42);
    CHECK_EQ(i1(vm, "aget", 0), 0); /* zero-initialised */
    CHECK_EQ(i1(vm, "aget", 3), 0);
    /* char[] elements are one byte, loaded zero-extended; byte[] loads sign-extend */
    CHECK_EQ(i1(vm, "chars", -1), 0xff);
    CHECK_EQ(i1(vm, "chars", 0x141), 0x41);
    CHECK_EQ(i1(vm, "bytes", 0xff), -1);
    CHECK_EQ(i1(vm, "bytes", 0x7f), 127);
    CHECK_EQ(i1(vm, "bools", 1), 1);
    CHECK_EQ(i1(vm, "shorts", 0x18000), -32768);
    CHECK_EQ(i1(vm, "shorts", 0x1234), 0x1234);
    {
        uint32_t a[2];
        put_long(a, INT64_C(-0x123456789));
        CHECK_EQ(call_j(vm, "longs", "(J)J", a, 2), INT64_C(-0x123456789));
    }
    CHECK_EQ(i1(vm, "refs", 1), 2);
    CHECK_EQ(i1(vm, "mna", 0), 2);
    CHECK_EQ(i1(vm, "mna", 1), 3);
    CHECK(!exen_halted(vm));

    /* the wrong-type access throws java.lang.Exception, which the framework has: it is a
     * normal exception that escapes to the caller, not a VM halt */
    CHECK_EQ(i0(vm, "wrong"), (int32_t)0xdeadbeefu);
    CHECK_EQ(last_rc, -1);
    CHECK(!exen_halted(vm));
    CHECK(vm->pending != 0);
    if (vm->pending)
        CHECK_EQ(obj_cls(vm, vm->pending), EXC);
    exen_destroy(vm);

    uint32_t a1[1];
    a1[0] = 4;
    expect_halt(&C, "aget", "(I)I", a1, 1, AIOOBE);
    a1[0] = (uint32_t)-1;
    expect_halt(&C, "aget", "(I)I", a1, 1, AIOOBE);
    a1[0] = 1u << 16;
    expect_halt(&C, "aget", "(I)I", a1, 1, AIOOBE);
    a1[0] = (uint32_t)-1;
    expect_halt(&C, "alen", "(I)I", a1, 1, NEGSIZE);
    a1[0] = 0;
    expect_halt(&C, "refs", "(I)I", a1, 1, NPE); /* a[0] is null */
    expect_halt(&C, "anull", "()I", NULL, 0, NPE);
}

/* ------------------------------------------------------------------ fields */

static void test_fields(void)
{
    tc_init(&C, SELF, OBJ);
    int f_i = tc_field(&C, "fi", "I", 1, T_INT, 0);
    int f_b = tc_field(&C, "fb", "B", 1, T_BYTE, 1);
    int f_s = tc_field(&C, "fs", "S", 1, T_SHORT, 2);
    int f_c = tc_field(&C, "fc", "C", 1, T_CHAR, 3);
    int f_j = tc_field(&C, "fj", "J", 1, T_LONG, 4);
    int f_z = tc_field(&C, "fz", "Z", 1, T_BOOL, 6);
    int s_i = tc_field(&C, "si", "I", ACC_STATIC | 1, T_INT, 0);
    int s_b = tc_field(&C, "sb", "B", ACC_STATIC | 1, T_BYTE, 1);
    int s_p = tc_field(&C, "sp", "I", ACC_STATIC | ACC_PRIVATE, T_INT, 2);
    int r_b = tc_fref(&C, C.fld[f_b].hash, T_BYTE, 0);
    int r_si = tc_fref(&C, C.fld[s_i].hash, T_INT, 0);
    int r_sp = tc_fref(&C, C.fld[s_p].hash, T_INT, 0);
    int r_missing = tc_fref(&C, 0x12345678u, T_INT, 0);
    struct { const char *name; int fld; int ext; } pg[] = {
        {"pg_i", f_i, 0}, {"pg_b", f_b, 0}, {"pg_s", f_s, 0},
        {"pg_c", f_c, 0}, {"pg_z", f_z, 0}, {"pg_xb", r_b, 1},
    };
    int m_pg[6];
    for (int i = 0; i < 6; i++)
        m_pg[i] = tc_method(&C, pg[i].name, "(I)I", PUB_STATIC, 1);
    int m_j = tc_method(&C, "pg_j", "(J)J", PUB_STATIC, 2);
    int m_two = tc_method(&C, "two", "()I", PUB_STATIC, 0);
    int m_sb = tc_method(&C, "st_b", "(I)I", PUB_STATIC, 1);
    int m_si = tc_method(&C, "st_xi", "(I)I", PUB_STATIC, 1);
    int m_sp = tc_method(&C, "st_priv", "()I", PUB_STATIC, 0);
    int m_miss = tc_method(&C, "st_missing", "()I", PUB_STATIC, 0);
    int m_npe = tc_method(&C, "npe", "()I", PUB_STATIC, 0);
    tc_layout(&C);
    CHECK_EQ(C.ninst, 7);
    CHECK_EQ(C.nstatics, 3);

    for (int i = 0; i < 6; i++) {
        uint16_t off = pg[i].ext ? C.fref_off[pg[i].fld] : C.fld_off[pg[i].fld];
        tc_code(&C, m_pg[i], 2, 2);
        tc_op16(&C, OP_NEW, C.cls_off[0]);
        tc_ops(&C, "4c 2b d9");     /* astore_1 aload_1 iload_0 */
        tc_op16(&C, pg[i].ext ? OP_PUTFIELD : OP_PUTFIELD_LOCAL, off);
        tc_u8(&C, OP_ALOAD_1);
        tc_op16(&C, pg[i].ext ? OP_GETFIELD : OP_GETFIELD_LOCAL, off);
        tc_u8(&C, OP_IRETURN);
    }
    tc_code(&C, m_j, 3, 3);
    tc_op16(&C, OP_NEW, C.cls_off[0]);
    tc_ops(&C, "4d 2c db");        /* astore_2 aload_2 lload_0 */
    tc_op16(&C, OP_PUTFIELD_LOCAL, C.fld_off[f_j]);
    tc_u8(&C, OP_ALOAD_2);
    tc_op16(&C, OP_GETFIELD_LOCAL, C.fld_off[f_j]);
    tc_u8(&C, OP_LRETURN);

    /* two objects keep separate fields: o1.fi = 5; o2.fi = 3; return o1.fi * 10 + o2.fi */
    tc_code(&C, m_two, 3, 3);
    tc_op16(&C, OP_NEW, C.cls_off[0]);
    tc_u8(&C, OP_ASTORE_1);
    tc_op16(&C, OP_NEW, C.cls_off[0]);
    tc_u8(&C, OP_ASTORE_2);
    tc_ops(&C, "2b 08");
    tc_op16(&C, OP_PUTFIELD_LOCAL, C.fld_off[f_i]);
    tc_ops(&C, "2c 06");
    tc_op16(&C, OP_PUTFIELD_LOCAL, C.fld_off[f_i]);
    tc_u8(&C, OP_ALOAD_1);
    tc_op16(&C, OP_GETFIELD_LOCAL, C.fld_off[f_i]);
    tc_bipush(&C, 10);
    tc_u8(&C, OP_IMUL);
    tc_u8(&C, OP_ALOAD_2);
    tc_op16(&C, OP_GETFIELD_LOCAL, C.fld_off[f_i]);
    tc_ops(&C, "60 e9");

    tc_code(&C, m_sb, 1, 1);
    tc_u8(&C, OP_ILOAD_0);
    tc_op16(&C, OP_PUTSTATIC_LOCAL, C.fld_off[s_b]);
    tc_op16(&C, OP_GETSTATIC_LOCAL, C.fld_off[s_b]);
    tc_u8(&C, OP_IRETURN);

    tc_code(&C, m_si, 1, 1);
    tc_u8(&C, OP_ILOAD_0);
    tc_op16(&C, OP_PUTSTATIC, C.fref_off[r_si]);
    tc_op16(&C, OP_GETSTATIC, C.fref_off[r_si]);
    tc_u8(&C, OP_IRETURN);

    tc_code(&C, m_sp, 1, 0);
    tc_op16(&C, OP_GETSTATIC, C.fref_off[r_sp]);
    tc_u8(&C, OP_IRETURN);

    tc_code(&C, m_miss, 1, 0);
    tc_op16(&C, OP_GETSTATIC, C.fref_off[r_missing]);
    tc_u8(&C, OP_IRETURN);

    tc_code(&C, m_npe, 1, 0);
    tc_u8(&C, OP_ACONST_NULL);
    tc_op16(&C, OP_GETFIELD_LOCAL, C.fld_off[f_i]);
    tc_u8(&C, OP_IRETURN);
    tc_finish(&C);

    ExenVM *vm = load(&C);
    REQUIRE(vm);
    CHECK_EQ(vm->classes[SELF].ninst, 7);
    CHECK_EQ(vm->classes[SELF].inst_base, vm->classes[OBJ].inst_total);
    CHECK_EQ(i1(vm, "pg_i", -123456), -123456);
    /* byte and boolean fields store the low byte and read back zero-extended: -1 -> 255 */
    CHECK_EQ(i1(vm, "pg_b", -1), 255);
    CHECK_EQ(i1(vm, "pg_b", 0x1234), 0x34);
    CHECK_EQ(i1(vm, "pg_xb", -1), 255); /* same through the external (by hash) form */
    CHECK_EQ(i1(vm, "pg_z", 0x101), 1);
    CHECK_EQ(i1(vm, "pg_c", -2), 0xfe);
    /* short fields store 32 bits and read back sign-extended 16 */
    CHECK_EQ(i1(vm, "pg_s", 0x18000), -32768);
    CHECK_EQ(i1(vm, "pg_s", 0x7fff), 0x7fff);
    {
        uint32_t a[2];
        put_long(a, INT64_C(0x7edcba9876543210));
        CHECK_EQ(call_j(vm, "pg_j", "(J)J", a, 2), INT64_C(0x7edcba9876543210));
    }
    CHECK_EQ(i0(vm, "two"), 53);
    CHECK_EQ(i1(vm, "st_b", -1), 255);
    CHECK(vm->classes[SELF].statics != 0);
    if (vm->classes[SELF].statics)
        CHECK_EQ(obj_slots(vm, vm->classes[SELF].statics)[1], 0xff);
    CHECK_EQ(i1(vm, "st_xi", 77), 77);
    if (vm->classes[SELF].statics)
        CHECK_EQ(obj_slots(vm, vm->classes[SELF].statics)[0], 77);
    CHECK(!exen_halted(vm));
    exen_destroy(vm);

    expect_halt(&C, "st_priv", "()I", NULL, 0, ILLACCESS);
    expect_halt(&C, "st_missing", "()I", NULL, 0, LINKAGE);
    expect_halt(&C, "npe", "()I", NULL, 0, NPE);
}

/* ------------------------------------------------------------------ invokes */

static void test_invokes(void)
{
    tc_init(&C, SELF, OBJ);
    int m_fact = tc_method(&C, "fact", "(I)I", PUB_STATIC, 1);
    int m_factx = tc_method(&C, "factx", "(I)I", PUB_STATIC, 1);
    int m_twice = tc_method(&C, "twice", "(I)I", 1, 1); /* instance method */
    int m_virt = tc_method(&C, "virt", "(I)I", PUB_STATIC, 1);
    int m_spec = tc_method(&C, "spec", "(I)I", PUB_STATIC, 1);
    int m_nullrecv = tc_method(&C, "nullrecv", "()I", PUB_STATIC, 0);
    int m_forever = tc_method(&C, "forever", "()V", PUB_STATIC, 0);
    int m_nomethod = tc_method(&C, "nomethod", "()I", PUB_STATIC, 0);
    int r_factx = tc_mref(&C, exen_member_hash("factx", "(I)I"), 1, 0);
    int r_twice = tc_mref(&C, exen_member_hash("twice", "(I)I"), 1, 0);
    int r_bogus = tc_mref(&C, 0x12345678u, 0, 0);
    tc_layout(&C);

    /* fact(n) = n <= 0 ? 1 : n * fact(n - 1), once local and once through a method ref */
    for (int ext = 0; ext < 2; ext++) {
        int rec = tc_label(&C);
        tc_code(&C, ext ? m_factx : m_fact, 3, 1);
        tc_u8(&C, OP_ILOAD_0);
        tc_br(&C, OP_IFGT, rec);
        tc_ops(&C, "04 e9");
        tc_bind(&C, rec);
        tc_ops(&C, "d9 d9 04 64");  /* iload_0 iload_0 iconst_1 isub */
        if (ext)
            tc_op16(&C, OP_INVOKESTATIC, C.mref_off[r_factx]);
        else
            tc_op16(&C, OP_INVOKESTATIC_LOCAL, C.meth_off[m_fact]);
        tc_ops(&C, "68 e9");
    }
    tc_code(&C, m_twice, 2, 2);
    tc_ops(&C, "dd dd 60 e9");      /* iload_1 iload_1 iadd ireturn */

    tc_code(&C, m_virt, 2, 1);
    tc_op16(&C, OP_NEW, C.cls_off[0]);
    tc_u8(&C, OP_ILOAD_0);
    tc_op16(&C, OP_INVOKEVIRTUAL, C.mref_off[r_twice]);
    tc_u8(&C, OP_IRETURN);

    tc_code(&C, m_spec, 2, 1);
    tc_op16(&C, OP_NEW, C.cls_off[0]);
    tc_u8(&C, OP_ILOAD_0);
    tc_op16(&C, OP_INVOKESPECIAL_LOCAL, C.meth_off[m_twice]);
    tc_u8(&C, OP_IRETURN);

    tc_code(&C, m_nullrecv, 2, 0);
    tc_ops(&C, "01 04");
    tc_op16(&C, OP_INVOKEVIRTUAL, C.mref_off[r_twice]);
    tc_u8(&C, OP_IRETURN);

    tc_code(&C, m_forever, 1, 0);
    tc_op16(&C, OP_INVOKESTATIC_LOCAL, C.meth_off[m_forever]);
    tc_u8(&C, OP_RETURN);

    tc_code(&C, m_nomethod, 1, 0);
    tc_op16(&C, OP_INVOKESTATIC, C.mref_off[r_bogus]);
    tc_u8(&C, OP_IRETURN);
    tc_finish(&C);

    ExenVM *vm = load(&C);
    REQUIRE(vm);
    CHECK_EQ(i1(vm, "fact", 10), 3628800);
    CHECK_EQ(i1(vm, "fact", 12), 479001600);
    CHECK_EQ(i1(vm, "factx", 10), 3628800);
    CHECK_EQ(i1(vm, "virt", 21), 42);
    CHECK_EQ(i1(vm, "spec", -4), -8);
    CHECK_EQ(vm->fp, -1); /* every frame popped */
    CHECK(!exen_halted(vm));
    exen_destroy(vm);

    expect_halt(&C, "nullrecv", "()I", NULL, 0, NPE);
    /* runaway recursion: StackOverflowError is not in the framework either */
    expect_halt(&C, "forever", "()V", NULL, 0, STACKOVF);
    expect_halt(&C, "nomethod", "()I", NULL, 0, LINKAGE);
}

/* ------------------------------------------------------------------ exceptions */

static void test_exceptions(void)
{
    tc_init(&C, SELF, OBJ);
    int c_exc = tc_class(&C, EXC);
    int c_thr = tc_class(&C, THROWABLE);
    int c_oom = tc_class(&C, OOM);
    struct { const char *name; int cref; int end_adj; } k[] = {
        {"catch_exc", c_exc, 0},    /* range ends exactly at the pc after the throw */
        {"catch_any", -1, 0},
        {"catch_super", c_thr, 0},
        {"catch_wrong", c_oom, 0},  /* OutOfMemoryError does not match */
        {"catch_short", c_exc, -1}, /* range ends at the throwing opcode: not covered */
    };
    int m[5];
    for (int i = 0; i < 5; i++)
        m[i] = tc_method(&C, k[i].name, "()I", PUB_STATIC, 0);
    int m_thrower = tc_method(&C, "thrower", "()I", PUB_STATIC, 0);
    int m_outer = tc_method(&C, "outer", "()I", PUB_STATIC, 0);
    int m_rethrow = tc_method(&C, "rethrow", "()I", PUB_STATIC, 0);
    int m_athrow_null = tc_method(&C, "athrow_null", "()I", PUB_STATIC, 0);
    tc_layout(&C);

    /* push 9; throw (arraylength on an int); handler: pop the exception, return what is left
     * on the operand stack (9), because unwinding does not reset it */
    for (int i = 0; i < 5; i++) {
        int s = tc_label(&C), e = tc_label(&C), h = tc_label(&C);
        tc_code(&C, m[i], 3, 0);
        tc_bipush(&C, 9);
        tc_bind(&C, s);
        tc_ops(&C, "08 be");
        tc_bind(&C, e);
        tc_ops(&C, "03 e9");
        tc_bind(&C, h);
        tc_ops(&C, "57 e9");
        tc_exc(&C, m[i], s, e, k[i].end_adj, h, k[i].cref);
    }
    tc_code(&C, m_thrower, 1, 0);
    tc_ops(&C, "08 be e9");
    {
        /* the handler is in the caller's frame: unwinding crosses frames */
        int s = tc_label(&C), e = tc_label(&C), h = tc_label(&C);
        tc_code(&C, m_outer, 2, 0);
        tc_bind(&C, s);
        tc_op16(&C, OP_INVOKESTATIC_LOCAL, C.meth_off[m_thrower]);
        tc_bind(&C, e);
        tc_u8(&C, OP_IRETURN);
        tc_bind(&C, h);
        tc_u8(&C, OP_POP);
        tc_bipush(&C, 7);
        tc_u8(&C, OP_IRETURN);
        tc_exc(&C, m_outer, s, e, 0, h, c_exc);
    }
    {
        /* catch, then athrow the same object: nothing catches it the second time */
        int s = tc_label(&C), e = tc_label(&C), h = tc_label(&C);
        tc_code(&C, m_rethrow, 2, 0);
        tc_bind(&C, s);
        tc_ops(&C, "08 be");
        tc_bind(&C, e);
        tc_ops(&C, "e9");
        tc_bind(&C, h);
        tc_ops(&C, "bf");
        tc_exc(&C, m_rethrow, s, e, 0, h, -1);
    }
    tc_code(&C, m_athrow_null, 1, 0);
    tc_ops(&C, "01 bf");
    tc_finish(&C);

    ExenVM *vm = load(&C);
    REQUIRE(vm);
    CHECK_EQ(i0(vm, "catch_exc"), 9);
    CHECK_EQ(last_rc, 1);
    CHECK_EQ(i0(vm, "catch_any"), 9);
    CHECK_EQ(i0(vm, "catch_super"), 9); /* Exception is a Throwable */
    CHECK_EQ(i0(vm, "outer"), 7);
    CHECK(vm->pending == 0);
    CHECK(!exen_halted(vm));

    i0(vm, "catch_wrong");
    CHECK_EQ(last_rc, -1);
    CHECK(!exen_halted(vm));
    CHECK(vm->pending && obj_cls(vm, vm->pending) == EXC);
    vm->pending = 0;

    i0(vm, "catch_short");
    CHECK_EQ(last_rc, -1);
    CHECK(vm->pending && obj_cls(vm, vm->pending) == EXC);
    vm->pending = 0;

    i0(vm, "rethrow");
    CHECK_EQ(last_rc, -1);
    CHECK(vm->pending && obj_cls(vm, vm->pending) == EXC);
    vm->pending = 0;
    CHECK_EQ(vm->fp, -1);
    CHECK(!exen_halted(vm));
    /* the VM keeps working after an escaped exception */
    CHECK_EQ(i0(vm, "catch_exc"), 9);
    exen_destroy(vm);

    expect_halt(&C, "athrow_null", "()I", NULL, 0, NPE);
}

int main(void)
{
    RUN(test_int_arithmetic);
    RUN(test_int_division_traps);
    RUN(test_long_arithmetic);
    RUN(test_constants);
    RUN(test_branches);
    RUN(test_switches);
    RUN(test_iinc);
    RUN(test_string_literals);
    RUN(test_arrays);
    RUN(test_fields);
    RUN(test_invokes);
    RUN(test_exceptions);
    TEST_MAIN_END();
}
