/* Name hashes (docs/re/CVP.md §5): class hash = CRC-32 of the dotted name, member hash =
 * crc16(descriptor) << 16 | crc16(name) with CRC-16/MCRF4XX. */
#include "support.h"
#include "test.h"

static void test_crc32_reference_values(void)
{
    /* the standard CRC-32 check value and the empty string */
    CHECK_EQ(exen_crc32("123456789", 9), 0xcbf43926u);
    CHECK_EQ(exen_crc32("", 0), 0);
    CHECK_EQ(exen_crc32("exen.Graphics", 13), 0xc6ed8e2au);
}

static void test_crc32_well_known_classes(void)
{
    struct { const char *name; uint32_t hash; } k[] = {
        {"java.lang.NullPointerException", H_NPE},
        {"java.lang.ArrayIndexOutOfBoundsException", H_AIOOBE},
        {"java.lang.ArithmeticException", H_ARITH},
        {"java.lang.NegativeArraySizeException", H_NEGSIZE},
        {"java.lang.OutOfMemoryError", H_OOM},
        {"java.lang.LinkageError", H_LINKAGE},
        {"java.lang.ClassNotFoundException", H_CNF},
        {"java.lang.IllegalAccessException", H_ILLACCESS},
        {"java.lang.Error", H_ERROR},
        {"java.lang.Exception", H_EXCEPTION},
        {"java.lang.Object", 0x4161c4a6u},
        {"java.lang.String", 0x7772dde3u},
        {"vm.sys.Bootstrap", 0x6551f7dcu},
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        t_current = k[i].name;
        CHECK_EQ(exen_crc32(k[i].name, strlen(k[i].name)), k[i].hash);
    }
}

static void test_crc16_reference_values(void)
{
    /* CRC-16/MCRF4XX check value */
    CHECK_EQ(exen_crc16("123456789", 9), 0x6f91);
    CHECK_EQ(exen_crc16("", 0), 0xffff);
}

static void test_member_hashes(void)
{
    CHECK_EQ(exen_member_hash("<init>", "()V"), 0x3f52ef2fu);
    CHECK_EQ(exen_member_hash("drawLine", "(IIII)V"), 0xcf20ea2fu);
    CHECK_EQ(exen_member_hash("<clinit>", "()V"), 0x3f5201b3u);
    CHECK_EQ(exen_member_hash("<init>", "(Lexen/Image;)V"), 0xd5e1ef2fu);
    CHECK_EQ(exen_member_hash("PM_COPY", "S"), 0x6f994aa8u);
    CHECK_EQ(exen_member_hash("finalize", "()V"), 0x3f52edf8u);
    /* Bootstrap entry points (CVP.md §9) */
    CHECK_EQ(exen_member_hash("init", "([C)V"), 0x35b0f11eu);
    CHECK_EQ(exen_member_hash("keypress", "(I)V"), 0x305a6d35u);
    CHECK_EQ(exen_member_hash("tick", "()V"), 0x3f522033u);
    /* the descriptor half is shared by every member with the same descriptor */
    CHECK_EQ(exen_member_hash("a", "()V") >> 16, 0x3f52u);
    CHECK_EQ(exen_member_hash("length", "()I"), 0xd724ffd6u);
}

int main(void)
{
    RUN(test_crc32_reference_values);
    RUN(test_crc32_well_known_classes);
    RUN(test_crc16_reference_values);
    RUN(test_member_hashes);
    TEST_MAIN_END();
}
