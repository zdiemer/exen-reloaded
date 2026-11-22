/* Loading the framework out of the vendored gensimu.exe and a game's 4CVP chunk
 * (docs/re/CVP.md §1, §3, §9). */
#include <stdlib.h>

#include "support.h"
#include "test.h"

static void test_find_framework(void)
{
    size_t len, off = 0, size = 0;
    const uint8_t *exe = t_gensimu(&len);
    REQUIRE(exen_find_framework(exe, len, &off, &size) == 0);
    /* VA 0x4494f0, length 0xaf8c (immediates at 0x40714c / 0x407153) */
    CHECK_EQ(off, 0x494f0);
    CHECK_EQ(size, 0xaf8c);
    CHECK(memcmp(exe + off, "4CVP", 4) == 0);

    uint8_t junk[0x400];
    memset(junk, 0, sizeof junk);
    CHECK(exen_find_framework(junk, sizeof junk, &off, &size) < 0);
    junk[0] = 'M';
    junk[1] = 'Z';
    CHECK(exen_find_framework(junk, sizeof junk, &off, &size) < 0);
}

static const char *const FW_NAMES[54] = {
    "exen.Animation", "exen.Graphics", "exen.Image", "exen.TextureMap", "exen.Rectangle",
    "exen.Palette", "exen.Resource", "exen.AnimBitmap", "exen.PlayField", "exen.Sprite",
    "exen.AnimFlash", "exen.Command", "exen.CommandListener", "exen.Displayable",
    "exen.Gamelet", "exen.Component", "exen.Sms", "exen.DialogBox", "exen.FX",
    "exen.GameletEnhanced", "exen.List", "exen.Math", "exen.Matrix3D", "exen.Vector3D",
    "exen.Palette_RAW", "exen.Point2D", "exen.RawData", "exen.RayCast", "exen.util.Debug",
    "java.lang.Object", "java.lang.Class", "java.lang.Exception", "java.lang.Throwable",
    "java.lang.String", "java.lang.ClassLoader", "java.lang.StringBuffer", "java.lang.System",
    "java.lang.Error", "java.lang.Boolean", "java.lang.Byte", "java.lang.Character",
    "java.lang.Cloneable", "java.lang.CloneNotSupportedException", "java.lang.Integer",
    "java.lang.Long", "java.lang.OutOfMemoryError", "java.lang.VirtualMachineError",
    "java.lang.Short", "java.io.Serializable", "vm.sys.Runtime", "vm.sys.Application",
    "vm.sys.Bootstrap", "catalog.Catalog", "catalog.GameProperty",
};

static void test_framework_classes(void)
{
    ExenVM *vm = t_vm_framework(0);
    REQUIRE(vm);
    CHECK_EQ(vm->nclasses, 54);
    CHECK_EQ(vm->nfw, 54);
    for (int i = 0; i < vm->nclasses && i < 54; i++) {
        const ExClass *c = &vm->classes[i];
        CHECK_STR(c->name, FW_NAMES[i]);
        if (c->name)
            CHECK_EQ(c->hash, exen_crc32(c->name, strlen(c->name)));
        CHECK(find_class(vm, c->hash) == i);
        CHECK(memcmp(P(vm, c->block), "4CVP", 4) == 0);
    }
    /* the well-known exception classes: only OutOfMemoryError exists in the framework,
     * besides java.lang.Exception / Error (CVP.md §12) */
    CHECK_EQ(find_class(vm, H_OOM), 45);
    CHECK_EQ(find_class(vm, H_EXCEPTION), 31);
    CHECK_EQ(find_class(vm, H_ERROR), 37);
    CHECK(find_class(vm, H_ARITH) < 0);
    CHECK(find_class(vm, H_NPE) < 0);
    CHECK(find_class(vm, H_AIOOBE) < 0);
    /* interfaces have no superclass */
    CHECK(vm->classes[0].flags & ACC_INTERFACE);
    CHECK_EQ(vm->classes[0].super, -1);
    CHECK_EQ(vm->classes[29].super, -1); /* java.lang.Object */
    CHECK_EQ(vm->classes[19].super, 14); /* GameletEnhanced extends Gamelet */
    CHECK_EQ(vm->classes[31].super, 32); /* Exception extends Throwable */
    CHECK(is_subclass(vm, 31, 32));
    CHECK(is_subclass(vm, 19, 29));
    CHECK(!is_subclass(vm, 29, 19));
    CHECK(is_subclass(vm, 33, 48));      /* String implements Serializable */

    /* the cosine table from .rdata: cos(0) is the maximum, the table is symmetric-ish */
    CHECK(vm->math.cos_table[0] > 0);
    exen_destroy(vm);
}

static void test_bootstrap_entry_points(void)
{
    ExenVM *vm = t_vm_framework(0);
    REQUIRE(vm);
    int bs = find_class(vm, exen_crc32("vm.sys.Bootstrap", 16));
    CHECK_EQ(bs, 51);
    CHECK_EQ(vm->classes[51].hash, 0x6551f7dcu);
    struct { const char *name, *desc; uint32_t hash; } ep[] = {
        {"keypress", "(I)V", 0x305a6d35u},  {"keyrelease", "(I)V", 0x305a1e56u},
        {"tick", "()V", 0x3f522033u},       {"init", "([C)V", 0x35b0f11eu},
        {"smsReceived", "([B)V", 0x6f6c0565u}, {"smsSent", "(I)V", 0x305a7631u},
        {"NickNameChanged", "([C)V", 0x35b0015cu}, {"exit", "()V", 0x3f523566u},
    };
    for (size_t i = 0; i < sizeof ep / sizeof ep[0]; i++) {
        t_current = ep[i].name;
        CHECK_EQ(exen_member_hash(ep[i].name, ep[i].desc), ep[i].hash);
        int found = -1;
        uint32_t m = find_method(vm, bs, ep[i].hash, 0, &found);
        CHECK(m != 0);
        CHECK_EQ(found, bs);
        if (m) {
            const uint8_t *b = BLK(vm, bs);
            CHECK_EQ(rd32(b + m), ep[i].hash);
            CHECK(rd16(b + m + 4) & ACC_STATIC);
        }
    }
    t_current = "test_bootstrap_entry_points";
    /* a virtual lookup walks the superclass chain: GameletEnhanced inherits Gamelet methods */
    int found = -1;
    uint32_t h = exen_member_hash("getScreenWidth", "()I");
    CHECK(find_method(vm, 19, h, 0, &found) == 0);
    CHECK(find_method(vm, 19, h, 1, &found) != 0);
    CHECK_EQ(found, 14);
    exen_destroy(vm);
}

static void test_natives_in_framework(void)
{
    /* every native method's code word is an index below EXEN_NATIVE_COUNT, and the generated
     * table agrees with the method's hash */
    ExenVM *vm = t_vm_framework(0);
    REQUIRE(vm);
    int natives = 0;
    for (int i = 0; i < vm->nclasses; i++) {
        const uint8_t *b = BLK(vm, i);
        uint16_t t = rd16(b + 0x20);
        if (!t)
            continue;
        uint16_t n = rd16(b + t);
        uint32_t r = (t + 2u + 3u) & ~3u;
        for (uint16_t k = 0; k < n; k++, r += 12) {
            if (!(rd16(b + r + 4) & ACC_NATIVE))
                continue;
            uint32_t idx = rd32(b + rd16(b + r + 8));
            CHECK(idx < EXEN_NATIVE_COUNT);
            natives++;
        }
    }
    CHECK_EQ(natives, EXEN_NATIVE_COUNT);
    exen_destroy(vm);
}

static void test_load_synthetic_game(void)
{
    ExenVM *vm = t_vm_framework(0);
    REQUIRE(vm);
    static TClass a, b;
    tc_init(&a, vm->nclasses, 29);
    int ma = tc_method(&a, "f", "()I", ACC_STATIC | 1, 0);
    tc_layout(&a);
    tc_code(&a, ma, 1, 0);
    tc_ops(&a, "08 e9"); /* iconst_5 ireturn */
    tc_finish(&a);
    tc_init(&b, vm->nclasses + 1, 29);
    int mb = tc_method(&b, "g", "()I", ACC_STATIC | 1, 0);
    tc_layout(&b);
    tc_code(&b, mb, 1, 0);
    tc_ops(&b, "07 e9"); /* iconst_4 ireturn */
    tc_finish(&b);
    TClass *cls[] = {&a, &b};
    REQUIRE(t_load_classes(vm, cls, 2) == 0);
    CHECK_EQ(vm->nclasses, 56);
    CHECK_EQ(vm->nfw, 54);
    CHECK_EQ(vm->cls_object, 29);
    CHECK_EQ(vm->cls_string, 33);
    CHECK_EQ(vm->cls_class, 30);
    CHECK_EQ(vm->cls_bootstrap, 51);
    CHECK_EQ(vm->cls_gamelet, 14);
    CHECK_EQ(vm->cls_stringbuffer, 35);
    CHECK_STR(vm->main_class, "test.Synthetic");
    CHECK(vm->heap_start >= vm->code_end);
    CHECK_EQ(vm->heap_used, 0);
    uint32_t r = 0;
    CHECK_EQ(t_call(vm, 54, "f", "()I", NULL, 0, &r), 1);
    CHECK_EQ(r, 5);
    CHECK_EQ(t_call(vm, 55, "g", "()I", NULL, 0, &r), 1);
    CHECK_EQ(r, 4);
    exen_destroy(vm);
}

static void test_reject_malformed(void)
{
    ExenVM *vm = t_vm_framework(0);
    REQUIRE(vm);
    uint8_t junk[0x80];
    memset(junk, 0x11, sizeof junk);
    CHECK(exen_load_game(vm, junk, sizeof junk) < 0); /* no NEXE magic */
    CHECK_CONTAINS(exen_error(vm), "NEXE");

    /* NEXE whose code offset points outside the file */
    size_t len;
    uint8_t blk[8] = {'4', 'C', 'V', 'P', 0, 0, 0, 0};
    uint8_t *x = t_make_exn(blk, sizeof blk, "x", &len);
    REQUIRE(x);
    wr32(x + 0x38, 0x1000);
    CHECK(exen_load_game(vm, x, len) < 0);
    /* a block whose size field is below the minimum header */
    wr32(x + 0x38, 0x40);
    CHECK(exen_load_game(vm, x, len) < 0);
    /* truncated resource table */
    wr32(x + 0x34, 0x10000);
    CHECK(exen_load_game(vm, x, len) < 0);
    free(x);
    exen_destroy(vm);

    /* a framework load from something that is not gensimu.exe */
    ExenConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    vm = exen_create(&cfg);
    REQUIRE(vm);
    CHECK(exen_load_framework(vm, junk, sizeof junk) < 0);
    CHECK_CONTAINS(exen_error(vm), "framework");
    exen_destroy(vm);
}

int main(void)
{
    RUN(test_find_framework);
    RUN(test_framework_classes);
    RUN(test_bootstrap_entry_points);
    RUN(test_natives_in_framework);
    RUN(test_load_synthetic_game);
    RUN(test_reject_malformed);
    TEST_MAIN_END();
}
