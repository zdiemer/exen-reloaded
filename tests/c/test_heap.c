/* Heap allocator and mark-sweep collector (src/core/heap.c). */
#include <stdlib.h>

#include "support.h"
#include "test.h"

#define SELF 54
#define OBJ 29

static TClass C;

static uint32_t blk_size(ExenVM *vm, ref_t r) { return rd32(P(vm, r)) & HDR_SIZE_MASK; }

/* Walk every block: they must tile [heap_start, heap_end), the object map must have a bit at
 * exactly the allocated block starts, the sizes of the allocated blocks must add up to
 * heap_used, no mark bit may survive a collection, and the free list must only link free
 * blocks. */
static void check_heap_(int line, ExenVM *vm)
{
    uint32_t off = vm->heap_start, used = 0, nfree = 0;
    int ok = 1;
    while (off < vm->heap_end) {
        uint32_t hdr = rd32(P(vm, off));
        uint32_t sz = hdr & HDR_SIZE_MASK;
        if (sz < 16 || (sz & 7) || off + sz > vm->heap_end) {
            T_FAIL("line %d: bad block size %u at %#x", line, sz, off);
            return;
        }
        int is_free = (hdr & HDR_FREE) != 0;
        if (heap_is_object(vm, off) != !is_free) {
            T_FAIL("line %d: objmap disagrees with block at %#x (free=%d)", line, off, is_free);
            ok = 0;
        }
        if (hdr & HDR_MARK) {
            T_FAIL("line %d: mark bit left on block at %#x", line, off);
            ok = 0;
        }
        /* no interior offset of a block may look like an object */
        for (uint32_t k = 8; k < sz && k < 64; k += 8)
            if (heap_is_object(vm, off + k)) {
                T_FAIL("line %d: interior offset %#x flagged as object", line, off + k);
                ok = 0;
            }
        if (is_free)
            nfree++;
        else
            used += sz;
        off += sz;
    }
    t_checks++;
    if (off != vm->heap_end) {
        T_FAIL("line %d: blocks end at %#x, heap ends at %#x", line, off, vm->heap_end);
        ok = 0;
    }
    t_checks++;
    if (used != vm->heap_used) {
        T_FAIL("line %d: allocated blocks sum to %u, heap_used is %u", line, used, vm->heap_used);
        ok = 0;
    }
    uint32_t n = 0;
    for (uint32_t f = vm->free_list; f; f = rd32(P(vm, f + 4))) {
        if (f < vm->heap_start || f >= vm->heap_end || !(rd32(P(vm, f)) & HDR_FREE) ||
            ++n > nfree) {
            T_FAIL("line %d: bad free list entry %#x", line, f);
            ok = 0;
            break;
        }
    }
    t_checks++;
    (void)ok;
}
#define check_heap(vm) check_heap_(__LINE__, vm)

static ExenVM *setup(void)
{
    tc_init(&C, SELF, OBJ);
    tc_field(&C, "keep", "[I", ACC_STATIC | 1, 0x100 | T_INT, 0);   /* static int[] */
    tc_field(&C, "num", "I", ACC_STATIC | 1, T_INT, 1);             /* not a reference */
    tc_field(&C, "next", "Ljava/lang/Object;", 1, T_OBJECT, 0);      /* instance ref */
    tc_field(&C, "val", "I", 1, T_INT, 1);
    int m = tc_method(&C, "churn", "(I)I", ACC_STATIC | 1, 1);
    tc_layout(&C);
    /* int[] keep = new int[16]; keep[0] = 1234;
     * for (i = n; i > 0; i--) new int[200];   (garbage)
     * return keep[0];      -- keep lives only in a frame local */
    tc_code(&C, m, 3, 2);
    {
        int top = tc_label(&C), end = tc_label(&C);
        tc_bipush(&C, 16);
        tc_op16(&C, OP_NEWARRAY, T_INT);
        tc_ops(&C, "4c 2b 03");       /* astore_1 aload_1 iconst_0 */
        tc_sipush(&C, 1234);
        tc_u8(&C, OP_IASTORE);
        tc_bind(&C, top);
        tc_u8(&C, OP_ILOAD_0);
        tc_br(&C, OP_IFLE, end);
        tc_sipush(&C, 200);
        tc_op16(&C, OP_NEWARRAY, T_INT);
        tc_u8(&C, OP_POP);
        tc_ops(&C, "84 00 ff");       /* iinc 0 -1 */
        tc_br(&C, OP_GOTO, top);
        tc_bind(&C, end);
        tc_ops(&C, "2b 03 2e e9");    /* aload_1 iconst_0 iaload ireturn */
    }
    tc_finish(&C);
    ExenVM *vm = t_vm_framework(64 * 1024);
    if (!vm)
        return NULL;
    TClass *a[] = {&C};
    if (t_load_classes(vm, a, 1) < 0) {
        exen_destroy(vm);
        return NULL;
    }
    return vm;
}

static void fill(ExenVM *vm, ref_t a, uint8_t seed)
{
    uint8_t *d = arr_data(vm, a);
    uint32_t n = (uint32_t)arr_len(vm, a) * (uint32_t)elem_size(arr_type(vm, a));
    for (uint32_t i = 0; i < n; i++)
        d[i] = (uint8_t)(seed + i * 7);
}

static int intact(ExenVM *vm, ref_t a, uint8_t seed)
{
    const uint8_t *d = arr_data(vm, a);
    uint32_t n = (uint32_t)arr_len(vm, a) * (uint32_t)elem_size(arr_type(vm, a));
    for (uint32_t i = 0; i < n; i++)
        if (d[i] != (uint8_t)(seed + i * 7))
            return 0;
    return 1;
}

static void test_alloc_basics(void)
{
    ExenVM *vm = setup();
    REQUIRE(vm);
    CHECK_EQ(vm->heap_used, 0);
    CHECK((vm->heap_start & 7) == 0);
    CHECK(vm->heap_start >= vm->code_end);
    check_heap(vm);

    ref_t a = vm_new_array(vm, T_INT, 0, 10);
    REQUIRE(a);
    CHECK((a & 7) == 0);
    CHECK(heap_is_object(vm, a));
    CHECK(is_array(vm, a));
    CHECK_EQ(arr_len(vm, a), 10);
    CHECK_EQ(arr_type(vm, a), T_INT);
    CHECK_EQ(blk_size(vm, a), (8 + 4 + 40 + 7) & ~7u);
    for (int i = 0; i < 40; i++)
        CHECK_EQ(arr_data(vm, a)[i], 0); /* zeroed */
    ref_t o = vm_new_object(vm, SELF);
    REQUIRE(o);
    CHECK_EQ(obj_cls(vm, o), SELF);
    CHECK_EQ(obj_kind(vm, o), EXEN_KIND_OBJECT);
    CHECK(!is_array(vm, o));
    CHECK_EQ(vm->heap_used, blk_size(vm, a) + blk_size(vm, o));

    /* sizes: chars and bytes are 1 byte, booleans too, longs 8, nested arrays 4 */
    CHECK_EQ(elem_size(T_CHAR), 1);
    CHECK_EQ(elem_size(T_BYTE), 1);
    CHECK_EQ(elem_size(T_BOOL), 1);
    CHECK_EQ(elem_size(T_SHORT), 2);
    CHECK_EQ(elem_size(T_INT), 4);
    CHECK_EQ(elem_size(T_LONG), 8);
    CHECK_EQ(elem_size(0x100 | T_BYTE), 4);
    CHECK(elem_is_ref(T_OBJECT));
    CHECK(elem_is_ref(0x100 | T_INT));
    CHECK(!elem_is_ref(T_INT));

    /* limits */
    CHECK(vm_new_array(vm, T_INT, 0, -1) == 0);
    CHECK(vm_new_array(vm, T_BYTE, 0, 0x10000) == 0);
    CHECK(heap_alloc(vm, 64u * 1024 * 1024, 0, EXEN_KIND_OBJECT) == 0); /* larger than the heap */
    CHECK(!heap_is_object(vm, 0));
    CHECK(!heap_is_object(vm, a + 4));
    CHECK(!heap_is_object(vm, a + 8));
    CHECK(!heap_is_object(vm, vm->heap_end));
    CHECK(!heap_is_object(vm, vm->heap_start - 8));
    check_heap(vm);
    exen_destroy(vm);
}

static void test_gc_roots(void)
{
    ExenVM *vm = setup();
    REQUIRE(vm);
    REQUIRE(vm_init_class(vm, SELF) == 0);
    ref_t statics = vm->classes[SELF].statics;
    REQUIRE(statics);
    int base = vm->classes[SELF].inst_base;

    ref_t a = vm_new_array(vm, T_INT, 0, 100);     /* host root */
    ref_t b = vm_new_array(vm, T_INT, 0, 50);      /* static reference field */
    ref_t o = vm_new_object(vm, SELF);             /* host root ... */
    ref_t d = vm_new_array(vm, T_BYTE, 0, 33);     /* ... reached through o.next */
    ref_t e = vm_new_array(vm, T_INT, 0, 200);     /* garbage */
    ref_t x = vm_new_object(vm, SELF);             /* garbage cycle x <-> y */
    ref_t y = vm_new_object(vm, SELF);
    ref_t num = vm_new_array(vm, T_INT, 0, 8);     /* only in a non-reference static slot */
    ref_t val = vm_new_array(vm, T_INT, 0, 8);     /* only in a non-reference field */
    ref_t stk = vm_new_array(vm, T_SHORT, 0, 9);   /* only on the VM stack */
    REQUIRE(a && b && o && d && e && x && y && num && val && stk);
    fill(vm, a, 1);
    fill(vm, b, 2);
    fill(vm, d, 3);
    fill(vm, stk, 4);
    vm->roots[vm->nroots++] = a;
    vm->roots[vm->nroots++] = o;
    obj_slots(vm, statics)[0] = b;
    obj_slots(vm, statics)[1] = num;
    obj_slots(vm, o)[base + 0] = d;
    obj_slots(vm, o)[base + 1] = val;
    obj_slots(vm, x)[base + 0] = y;
    obj_slots(vm, y)[base + 0] = x;
    /* a native's argument area counts as stack (vm_stack_top) */
    vm->stack[0] = stk;
    vm->native_top = vm->stack + 1;

    uint32_t garbage = blk_size(vm, e) + blk_size(vm, x) + blk_size(vm, y) +
                       blk_size(vm, num) + blk_size(vm, val);
    uint32_t before = vm->heap_used;
    check_heap(vm);
    heap_gc(vm);
    check_heap(vm);
    CHECK_EQ(vm->heap_used, before - garbage);
    CHECK(heap_is_object(vm, statics));
    CHECK(heap_is_object(vm, a) && intact(vm, a, 1));
    CHECK(heap_is_object(vm, b) && intact(vm, b, 2));
    CHECK(heap_is_object(vm, o));
    CHECK(heap_is_object(vm, d) && intact(vm, d, 3));
    CHECK(heap_is_object(vm, stk) && intact(vm, stk, 4));
    CHECK(!heap_is_object(vm, e));
    CHECK(!heap_is_object(vm, x)); /* cycles are collected (the original leaks them) */
    CHECK(!heap_is_object(vm, y));
    CHECK(!heap_is_object(vm, num));
    CHECK(!heap_is_object(vm, val));

    /* drop the roots one by one */
    vm->native_top = NULL;
    vm->nroots = 1; /* keeps a, drops o (and so d) */
    heap_gc(vm);
    check_heap(vm);
    CHECK(heap_is_object(vm, a));
    CHECK(!heap_is_object(vm, o));
    CHECK(!heap_is_object(vm, d));
    CHECK(!heap_is_object(vm, stk));
    obj_slots(vm, statics)[0] = 0;
    heap_gc(vm);
    CHECK(!heap_is_object(vm, b));
    vm->nroots = 0;
    heap_gc(vm);
    check_heap(vm);
    /* only the static area is left */
    CHECK_EQ(vm->heap_used, blk_size(vm, statics));
    /* everything else coalesced back: a big allocation fits again */
    CHECK(heap_alloc(vm, (vm->heap_end - vm->heap_start) / 2, 0, EXEN_KIND_OBJECT) != 0);
    check_heap(vm);
    exen_destroy(vm);
}

static void test_gc_on_exhaustion(void)
{
    ExenVM *vm = setup();
    REQUIRE(vm);
    REQUIRE(vm_init_class(vm, SELF) == 0);
    ref_t keep = vm_new_array(vm, T_INT, 0, 64);
    REQUIRE(keep);
    fill(vm, keep, 9);
    obj_slots(vm, vm->classes[SELF].statics)[0] = keep;
    uint32_t heap = vm->heap_end - vm->heap_start;
    /* allocate far more garbage than the heap holds; allocation must collect, not fail */
    int collections = 0, failures = 0;
    uint32_t prev = vm->heap_used, peak = 0;
    for (int i = 0; i < 4000; i++) {
        ref_t g = vm_new_array(vm, T_INT, 0, 255);
        if (!g) {
            failures++;
            break;
        }
        if (vm->heap_used < prev)
            collections++;
        prev = vm->heap_used;
        if (prev > peak)
            peak = prev;
    }
    CHECK_EQ(failures, 0);
    CHECK(collections >= 2);
    CHECK(peak <= heap);
    CHECK(heap_is_object(vm, keep) && intact(vm, keep, 9));
    check_heap(vm);
    exen_destroy(vm);
}

static void test_gc_from_bytecode(void)
{
    /* the interpreter allocates until the heap is full; the only reference to `keep` is a
     * frame local, which the collector scans conservatively */
    ExenVM *vm = setup();
    REQUIRE(vm);
    uint32_t heap = vm->heap_end - vm->heap_start;
    uint32_t a[1] = {5000}; /* 5000 * 808 bytes, several times the heap */
    uint32_t r[1] = {0};
    REQUIRE(5000u * 808u > 3u * heap);
    CHECK_EQ(t_call(vm, SELF, "churn", "(I)I", a, 1, r), 1);
    CHECK_EQ(r[0], 1234);
    CHECK(!exen_halted(vm));
    CHECK(vm->heap_used <= heap);
    check_heap(vm);
    heap_gc(vm); /* nothing is rooted any more: only the static area survives */
    CHECK_EQ(vm->heap_used, blk_size(vm, vm->classes[SELF].statics));
    check_heap(vm);
    exen_destroy(vm);
}

int main(void)
{
    RUN(test_alloc_basics);
    RUN(test_gc_roots);
    RUN(test_gc_on_exhaustion);
    RUN(test_gc_from_bytecode);
    TEST_MAIN_END();
}
