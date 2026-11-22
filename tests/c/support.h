/* Shared helpers for the C unit tests: the vendored framework, VM setup, and an assembler for
 * synthetic 4CVP class blocks (docs/re/CVP.md layouts, docs/re/OPCODES.md encodings).
 *
 * Synthetic classes are loaded as "game" classes after the real framework, exactly like the
 * code chunk of an .exn, so the interpreter runs them through its normal paths.
 */
#ifndef EXEN_TEST_SUPPORT_H
#define EXEN_TEST_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#include "vm.h"

/* Defined by api.c / loader.c without a header. */
int exen_find_framework(const uint8_t *exe, size_t len, size_t *off, size_t *size);
int exen_finalize_classes(ExenVM *vm);

/* Path of vendor/gensimu/gensimu.exe (EXEN_GENSIMU_PATH, overridable with the environment
 * variable of the same name), and its contents, read once. Exits the test program if missing. */
const char *t_gensimu_path(void);
const uint8_t *t_gensimu(size_t *len);
/* Read a whole file; NULL if it cannot be read. The caller frees. */
uint8_t *t_read_file(const char *path, size_t *len);
/* Path of a file in the source tree (EXEN_SOURCE_DIR + "/" + rel), in a static buffer. */
const char *t_source_path(const char *rel);

/* A VM with the framework loaded (no game yet). heap_bytes 0 = default. */
ExenVM *t_vm_framework(uint32_t heap_bytes);

/* ------------------------------------------------------------------ class assembler */

#define TC_MAX 64

typedef struct TClass {
    uint8_t b[0x8000];
    uint32_t n;
    int self;                   /* global class index the block will get when loaded */
    uint16_t flags;
    uint16_t ninst, nstatics;
    int laid_out;

    int ncls;                   /* class-ref table; entry 0 = self, 1 = java.lang.Object */
    uint16_t cls_idx[TC_MAX], cls_off[TC_MAX];

    int nfld;
    struct { uint32_t hash; uint16_t flags, type, slot; } fld[TC_MAX];
    uint16_t fld_off[TC_MAX];   /* field record offsets (floc operands) */

    int nmeth;
    struct { uint32_t hash; uint16_t flags, nargs; } meth[TC_MAX];
    uint16_t meth_off[TC_MAX];  /* method record offsets (mloc operands) */

    int nmref;
    struct { uint32_t hash; uint16_t nargs; int cref; } mref[TC_MAX];
    uint16_t mref_off[TC_MAX];  /* method-ref entry offsets (mext operands) */

    int nfref;
    struct { uint32_t hash; uint16_t type; int cref; } fref[TC_MAX];
    uint16_t fref_off[TC_MAX];  /* field-ref entry offsets (fext operands) */

    /* code emission */
    int cur;                    /* method being emitted */
    uint32_t code_hdr[TC_MAX];  /* block offset of each method's {max_stack, max_locals, exc} */
    uint32_t label[256];        /* bound block offset, 0 = unbound */
    int nlabel;
    struct { uint32_t at; int label; } lpatch[512];
    int nlpatch;
    struct { uint32_t at; uint8_t data[260]; uint16_t len; uint8_t align; } pool[128];
    int npool;
    struct { int method; int start, end, end_adj, handler; int cref; } exc[16];
    int nexc;
} TClass;

/* Start a class that will be global class `self_index`, extending java.lang.Object (class index
 * `object_index`). Class-ref ids 0 and 1 are self and Object. */
void tc_init(TClass *c, int self_index, int object_index);
/* Declarations; call before tc_layout. Each returns an id used with the *_off arrays. */
int tc_class(TClass *c, int class_index);
int tc_field(TClass *c, const char *name, const char *desc, uint16_t flags, uint16_t type,
             uint16_t slot);
int tc_method(TClass *c, const char *name, const char *desc, uint16_t flags, uint16_t nargs);
int tc_mref(TClass *c, uint32_t hash, uint16_t nargs, int cref);
int tc_fref(TClass *c, uint32_t hash, uint16_t type, int cref);
/* Write the header and all tables. */
void tc_layout(TClass *c);

/* Begin method `m`'s code (after tc_layout). */
void tc_code(TClass *c, int m, uint16_t max_stack, uint16_t max_locals);
uint32_t tc_pc(const TClass *c);
void tc_u8(TClass *c, uint8_t v);
void tc_ops(TClass *c, const char *hex); /* raw bytes from a hex string, spaces ignored */
void tc_pad2(TClass *c);                 /* pad to an even pc (the a2 rule) */
void tc_pad4(TClass *c);
void tc_u16(TClass *c, uint16_t v);
void tc_u32(TClass *c, uint32_t v);
/* op, a2, u16 */
void tc_op16(TClass *c, uint8_t op, uint16_t v);
/* labels for absolute branch targets */
int tc_label(TClass *c);
void tc_bind(TClass *c, int label);
void tc_target(TClass *c, int label);    /* emit a u16 that will hold the label's offset */
void tc_br(TClass *c, uint8_t op, int label);
/* constants: placed in a pool after the code, operand patched at tc_finish */
void tc_ldc(TClass *c, int32_t v);       /* 0x12 */
void tc_ldc2(TClass *c, int64_t v);      /* 0x14 */
void tc_ldstr(TClass *c, const char *s); /* 0xd0 */
void tc_bipush(TClass *c, int8_t v);
void tc_sipush(TClass *c, int16_t v);
/* Exception-table entry for method m. The stored range is [label start, label end + end_adj],
 * inclusive at both ends as in the original (OPCODES.md, Exceptions); catch is a class-ref id,
 * or -1 for "any". */
void tc_exc(TClass *c, int m, int start, int end, int end_adj, int handler, int cref);
/* Finish the block (pools, exception tables, label patches, size); returns its length,
 * a multiple of 4. */
uint32_t tc_finish(TClass *c);

/* Wrap finished class blocks in a NEXE container (no resources) and load them as the game.
 * Returns exen_load_game's result. */
int t_load_classes(ExenVM *vm, TClass *const *classes, int n);
/* The same container, for tests that need the bytes. Caller frees. */
uint8_t *t_make_exn(const uint8_t *code, size_t code_len, const char *main_class, size_t *len);

/* Call static method name+desc of class `cls` with `nargs` argument words. Returns vm_call's
 * result (result words, or -1 if the VM halted or an exception escaped). */
int t_call(ExenVM *vm, int cls, const char *name, const char *desc, const uint32_t *args,
           int nargs, uint32_t *ret);

/* Opcodes used by the tests (In-Fusio numbering where it differs from the JVM). */
enum {
    OP_NOP = 0x00, OP_ACONST_NULL = 0x01, OP_ICONST_M1 = 0x02, OP_ICONST_0 = 0x03,
    OP_ICONST_1 = 0x04, OP_ICONST_2 = 0x05, OP_ICONST_3 = 0x06, OP_ICONST_4 = 0x07,
    OP_ICONST_5 = 0x08, OP_LCONST_0 = 0x09, OP_LCONST_1 = 0x0a, OP_BIPUSH = 0x10,
    OP_SIPUSH = 0x11, OP_ALOAD = 0x19, OP_ALOAD_0 = 0x2a, OP_ALOAD_1 = 0x2b, OP_ALOAD_2 = 0x2c,
    OP_IALOAD = 0x2e, OP_LALOAD = 0x2f, OP_AALOAD = 0x32, OP_BALOAD = 0x33, OP_CALOAD = 0x34,
    OP_SALOAD = 0x35, OP_ASTORE = 0x3a, OP_ASTORE_0 = 0x4b, OP_ASTORE_1 = 0x4c,
    OP_ASTORE_2 = 0x4d, OP_IASTORE = 0x4f, OP_LASTORE = 0x50, OP_AASTORE = 0x53,
    OP_BASTORE = 0x54, OP_CASTORE = 0x55, OP_SASTORE = 0x56, OP_POP = 0x57, OP_POP2 = 0x58,
    OP_DUP = 0x59, OP_DUP_X1 = 0x5a, OP_DUP2 = 0x5c, OP_SWAP = 0x5f, OP_IADD = 0x60,
    OP_LADD = 0x61, OP_ISUB = 0x64, OP_LSUB = 0x65, OP_IMUL = 0x68, OP_LMUL = 0x69,
    OP_IDIV = 0x6c, OP_LDIV = 0x6d, OP_IREM = 0x70, OP_LREM = 0x71, OP_INEG = 0x74,
    OP_LNEG = 0x75, OP_ISHL = 0x78, OP_LSHL = 0x79, OP_ISHR = 0x7a, OP_LSHR = 0x7b,
    OP_IUSHR = 0x7c, OP_LUSHR = 0x7d, OP_IAND = 0x7e, OP_LAND = 0x7f, OP_IOR = 0x80,
    OP_LOR = 0x81, OP_IXOR = 0x82, OP_LXOR = 0x83, OP_IINC = 0x84, OP_I2L = 0x85,
    OP_L2I = 0x88, OP_I2B = 0x91, OP_I2C = 0x92, OP_I2S = 0x93, OP_LCMP = 0x94,
    OP_IFEQ = 0x99, OP_IFNE = 0x9a, OP_IFLT = 0x9b, OP_IFGE = 0x9c, OP_IFGT = 0x9d,
    OP_IFLE = 0x9e, OP_IF_ICMPEQ = 0x9f, OP_IF_ICMPNE = 0xa0, OP_IF_ICMPLT = 0xa1,
    OP_IF_ICMPGE = 0xa2, OP_IF_ICMPGT = 0xa3, OP_IF_ICMPLE = 0xa4, OP_IF_ACMPEQ = 0xa5,
    OP_IF_ACMPNE = 0xa6, OP_GOTO = 0xa7, OP_TABLESWITCH = 0xaa, OP_LOOKUPSWITCH = 0xab,
    OP_ARETURN = 0xb0, OP_RETURN = 0xb1, OP_NEW = 0xbb, OP_NEWARRAY = 0xbc,
    OP_ARRAYLENGTH = 0xbe, OP_ATHROW = 0xbf, OP_INSTANCEOF = 0xc1, OP_MULTIANEWARRAY = 0xc5,
    OP_IFNULL = 0xc6, OP_IFNONNULL = 0xc7, OP_LOOKUPSWITCH_S16 = 0xcc,
    OP_TABLESWITCH_S16 = 0xcd, OP_LDC_STRING = 0xd0, OP_ILOAD = 0xd5, OP_ISTORE = 0xd6,
    OP_LLOAD = 0xd7, OP_LSTORE = 0xd8,
    /* {iload, istore, lload, lstore}_n = 0xd9 + 4n + k */
    OP_ILOAD_0 = 0xd9, OP_ISTORE_0 = 0xda, OP_LLOAD_0 = 0xdb, OP_LSTORE_0 = 0xdc,
    OP_ILOAD_1 = 0xdd, OP_ISTORE_1 = 0xde, OP_LLOAD_1 = 0xdf, OP_LSTORE_1 = 0xe0,
    OP_ILOAD_2 = 0xe1, OP_ISTORE_2 = 0xe2, OP_LLOAD_2 = 0xe3, OP_LSTORE_2 = 0xe4,
    OP_ILOAD_3 = 0xe5, OP_ISTORE_3 = 0xe6, OP_LLOAD_3 = 0xe7, OP_LSTORE_3 = 0xe8,
    OP_IRETURN = 0xe9, OP_LRETURN = 0xea, OP_INVOKESPECIAL_LOCAL = 0xed,
    OP_INVOKEVIRTUAL = 0xee, OP_INVOKEVIRTUAL_LOCAL = 0xef, OP_INVOKESPECIAL = 0xf0,
    OP_INVOKESTATIC_LOCAL = 0xf1, OP_INVOKESTATIC = 0xf2, OP_GETSTATIC_LOCAL = 0xf3,
    OP_PUTSTATIC_LOCAL = 0xf4, OP_GETFIELD_LOCAL = 0xf5, OP_PUTFIELD_LOCAL = 0xf6,
    OP_GETSTATIC = 0xf7, OP_PUTSTATIC = 0xf8, OP_GETFIELD = 0xf9, OP_PUTFIELD = 0xfa,
};

#endif
