/* Internal VM definitions. See docs/re/CVP.md and docs/re/OPCODES.md.
 *
 * Memory model
 * ------------
 * All VM-visible memory lives in one arena (`vm->mem`). References are 32-bit byte offsets into
 * it, 0 is null. The arena holds, in order:
 *   - the class blocks (framework then game), copied verbatim, read-only by convention
 *   - the heap: objects, arrays and static areas
 * Keeping everything in one arena with offset references makes save states a plain copy and
 * keeps the core free of host pointers.
 *
 * Heap block layout (every block starts with an 8-byte header):
 *   +0 u32 size in bytes including the header, multiple of 8; bit 0 = free, bit 1 = GC mark
 *   +4 u16 class index (objects/statics), element class index (reference arrays)
 *   +6 u16 kind: EXEN_KIND_OBJECT, EXEN_KIND_STATICS, or EXEN_KIND_ARRAY | element type word
 * Objects and static areas: 32-bit slots from +8. Objects are flat: each class's slots follow
 * its superclass's (ExClass.inst_base). The original links one segment per class instead; the
 * flat layout is observably equivalent because every reference points at the whole object.
 * Arrays: +8 u16 unused, +10 u16 length, +12 elements. The (u16 length, elements) pair has the
 * same shape as a string literal, which is what String's m_strBuffer points at.
 */
#ifndef EXEN_VM_H
#define EXEN_VM_H

#include <stddef.h>
#include <stdint.h>

#include "exen.h"
#include "natives_gen.h"

#define EXEN_NATIVE_SLOTS EXEN_NATIVE_COUNT

typedef uint32_t ref_t;

#define EXEN_KIND_OBJECT  0x0099u
#define EXEN_KIND_STATICS 0x0019u
#define EXEN_KIND_ARRAY   0x8000u

#define HDR_FREE 1u
#define HDR_MARK 2u
#define HDR_SIZE_MASK (~7u)

#define ACC_PRIVATE   0x0002u
#define ACC_STATIC    0x0008u
#define ACC_NATIVE    0x0100u
#define ACC_INTERFACE 0x0200u

/* Type codes (docs/re/CVP.md §6): low byte = element type, high byte = array dimensions. */
#define T_VOID   0x00
#define T_SHORT  0x15
#define T_CHAR   0x55
#define T_CHAR2  0x50
#define T_INT    0x59
#define T_LONG   0x6e
#define T_BYTE   0x90
#define T_BOOL   0xd5
#define T_BOOL_E 0xd0
#define T_OBJECT 0x99

/* Well-known class hashes (CRC-32 of the dotted name). */
#define H_NPE         0x364a8d65u
#define H_AIOOBE      0x1d3c3033u
#define H_ARITH       0x86f6360bu
#define H_NEGSIZE     0x0794f618u
#define H_OOM         0xf2f8aa1cu
#define H_LINKAGE     0x13523f29u
#define H_CNF         0xf2985715u
#define H_ILLACCESS   0x39f3fe57u
#define H_ERROR       0xf217c377u
#define H_EXCEPTION   0x72737f61u
#define H_STACKOVF    0xa2c72bf1u
#define H_ILLARG      0x0u /* name unknown yet; resolved by name at boot if present */

typedef struct ExClass {
    uint32_t block;        /* arena offset of the 4CVP block */
    uint32_t size;
    uint32_t hash;
    uint16_t flags;
    int16_t super;         /* class index, -1 for java.lang.Object and interfaces */
    uint16_t nstatics;
    uint16_t ninst;        /* slots declared by this class */
    uint16_t inst_base;    /* first slot of this class in a flat object */
    uint16_t inst_total;   /* inst_base + ninst */
    uint8_t *inst_refmap;  /* bit per slot in [0, inst_total): slot holds a reference */
    uint8_t *static_refmap;/* bit per static slot */
    ref_t statics;         /* 0 until the class is initialised */
    const char *name;      /* framework debug name, or NULL */
} ExClass;

typedef struct ExFrame {
    uint16_t cls;
    uint16_t pc;
    uint32_t mrec;         /* block offset of the method record */
    uint32_t code;         /* block offset of the method code header */
    uint32_t *loc;         /* locals (the caller's argument area) */
    uint32_t *sp;          /* next free operand slot */
    uint32_t *limit;       /* end of this frame's operand stack */
    uint16_t jsr;          /* jsr return pc (a single register in the original) */
    uint8_t run_base;      /* first frame of a nested run started by vm_call */
    uint8_t base_of_run_pending; /* a vm_call is running above this frame: don't touch its sp */
} ExFrame;

typedef int (*ExNativeFn)(ExenVM *vm, uint32_t *args);

#define EXEN_MAX_FRAMES 1024
#define EXEN_STACK_WORDS (1u << 16)
#define EXEN_MAX_ROOTS 64

struct ExenVM {
    uint8_t *mem;
    uint32_t mem_size;
    uint32_t code_end;      /* end of the class blocks; the heap starts here */
    uint32_t heap_start, heap_end;
    uint32_t free_list;     /* offset of the first free block, 0 = none */
    uint8_t *objmap;        /* bit per 8 heap bytes: an allocated block starts here */
    uint32_t heap_used;

    ExClass *classes;
    int nclasses;
    int nfw;                /* framework classes */
    int cls_object, cls_string, cls_class, cls_bootstrap, cls_gamelet, cls_stringbuffer;

    uint32_t stack[EXEN_STACK_WORDS];
    ExFrame frames[EXEN_MAX_FRAMES];
    int fp;                 /* index of the current frame, -1 = none */

    ref_t roots[EXEN_MAX_ROOTS];
    int nroots;

    ref_t pending;          /* exception object propagating to an outer run, 0 = none */
    uint32_t *native_top;   /* while a native runs: end of its argument area */
    int in_native;          /* natives defer throws to native_exc */
    uint32_t native_exc;    /* class hash a native asked to throw */
    int last_ret_words;
    int halted;             /* fatal VM error or exitVm */
    char halt_reason[256];

    uint32_t native_calls[EXEN_NATIVE_SLOTS];
    ExenHost host;
    void *user;

    struct {
        uint16_t cos_table[513]; /* from gensimu.exe (VA 0x4567f8) */
        int32_t s0, s1, period;
    } math;

    /* Platform state owned by the native layer (natives_*.c). */
    struct ExenPlatform *plat;

    /* State of the natives in extnatives.h. Plain data, zero-initialised with the VM. */
    struct {
        int cls[16];        /* framework class index + 1 (0 = unresolved), see ext_class() */
        uint32_t warned;    /* log-once bits */
        int sms_sent;       /* a Bootstrap.smsSent(status) delivery is queued (api.c) */
        int32_t sms_status;
        int delivering;     /* api.c is running a queued delivery */
    } ext;

    /* The VM clock (Gamelet.getTimerTickCount) is host.ticks_ms() + clock_offset; a state load
     * sets the offset so that the clock continues from the saved value (state.c). */
    uint32_t clock_offset;

    /* the loaded game */
    uint8_t *exn;
    size_t exn_len;
    uint32_t exn_crc;       /* CRC-32 of the .exn file */
    uint32_t code_crc;      /* CRC-32 of the arena's class blocks [0, code_end) as loaded */
    int error_after_halt;   /* error was set by a state call on a halted VM: report it */
    char main_class[33];
    char error[256];
};

/* arena access */
static inline uint8_t *P(ExenVM *vm, uint32_t off) { return vm->mem + off; }
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline const uint8_t *BLK(ExenVM *vm, int cls) { return vm->mem + vm->classes[cls].block; }

/* heap objects */
static inline uint16_t obj_cls(ExenVM *vm, ref_t r) { return rd16(P(vm, r + 4)); }
static inline uint16_t obj_kind(ExenVM *vm, ref_t r) { return rd16(P(vm, r + 6)); }
static inline uint32_t *obj_slots(ExenVM *vm, ref_t r) { return (uint32_t *)P(vm, r + 8); }
static inline int is_array(ExenVM *vm, ref_t r) { return (obj_kind(vm, r) & EXEN_KIND_ARRAY) != 0; }
static inline uint16_t arr_len(ExenVM *vm, ref_t r) { return rd16(P(vm, r + 10)); }
static inline uint8_t *arr_data(ExenVM *vm, ref_t r) { return P(vm, r + 12); }
static inline uint16_t arr_type(ExenVM *vm, ref_t r) { return (uint16_t)(obj_kind(vm, r) & 0x7fff); }

/* heap.c */
int heap_init(ExenVM *vm);
ref_t heap_alloc(ExenVM *vm, uint32_t payload, uint16_t cls, uint16_t kind);
int heap_is_object(ExenVM *vm, uint32_t off);
void heap_gc(ExenVM *vm);
int elem_size(uint16_t type_word);
int elem_is_ref(uint16_t type_word);

/* loader.c */
int load_image(ExenVM *vm, const uint8_t *img, size_t len, int is_framework);
int find_class(ExenVM *vm, uint32_t hash);
uint32_t find_method(ExenVM *vm, int cls, uint32_t hash, int walk_super, int *found_cls);
uint32_t find_field(ExenVM *vm, int cls, uint32_t hash, int walk_super, int *found_cls);
int is_subclass(ExenVM *vm, int cls, int of);

/* interp.c */
ref_t vm_new_object(ExenVM *vm, int cls);
ref_t vm_new_array(ExenVM *vm, uint16_t type_word, uint16_t elem_cls, int32_t n);
int vm_init_class(ExenVM *vm, int cls);
void vm_throw_hash(ExenVM *vm, uint32_t class_hash);
void vm_throw(ExenVM *vm, ref_t exc);
void vm_halt(ExenVM *vm, const char *fmt, ...);
int vm_invoke(ExenVM *vm, int cls, uint32_t mrec, uint32_t *args, int has_this);
int vm_call(ExenVM *vm, int cls, uint32_t mrec, uint32_t *args, int nargs, uint32_t *ret);
uint32_t *vm_stack_top(ExenVM *vm);

/* natives.c */
int natives_call(ExenVM *vm, uint32_t index, uint32_t *args);
uint32_t *field_ptr(ExenVM *vm, ref_t obj, int cls, uint32_t hash);
ref_t vm_new_string(ExenVM *vm, const uint8_t *chars, int len);
int vm_string_get(ExenVM *vm, ref_t s, const uint8_t **chars);

/* member hash helpers (hash.c) */
uint32_t exen_crc32(const void *data, size_t len);
uint16_t exen_crc16(const void *data, size_t len);
uint32_t exen_member_hash(const char *name, const char *desc);

#endif
