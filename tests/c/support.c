/* Test helpers: see support.h. */
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EXEN_GENSIMU_PATH
#define EXEN_GENSIMU_PATH "vendor/gensimu/gensimu.exe"
#endif
#ifndef EXEN_SOURCE_DIR
#define EXEN_SOURCE_DIR "."
#endif

/* ------------------------------------------------------------------ files */

uint8_t *t_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 1 << 16;
            uint8_t *nb = realloc(buf, nc);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap = nc;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0)
            break;
    }
    fclose(f);
    *len = n;
    return buf;
}

const char *t_gensimu_path(void)
{
    const char *env = getenv("EXEN_GENSIMU_PATH");
    return env && *env ? env : EXEN_GENSIMU_PATH;
}

const uint8_t *t_gensimu(size_t *len)
{
    static uint8_t *exe;
    static size_t exe_len;
    if (!exe) {
        exe = t_read_file(t_gensimu_path(), &exe_len);
        if (!exe) {
            fprintf(stderr, "cannot read %s (set EXEN_GENSIMU_PATH)\n", t_gensimu_path());
            exit(2);
        }
    }
    *len = exe_len;
    return exe;
}

const char *t_source_path(const char *rel)
{
    static char buf[2048];
    snprintf(buf, sizeof buf, "%s/%s", EXEN_SOURCE_DIR, rel);
    return buf;
}

static void t_log(void *user, const char *msg)
{
    (void)user;
    if (getenv("EXEN_TEST_VERBOSE"))
        fprintf(stderr, "  vm: %s\n", msg);
}

ExenVM *t_vm_framework(uint32_t heap_bytes)
{
    ExenConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.heap_bytes = heap_bytes;
    cfg.host.log = t_log;
    ExenVM *vm = exen_create(&cfg);
    if (!vm)
        return NULL;
    size_t len;
    const uint8_t *exe = t_gensimu(&len);
    if (exen_load_framework(vm, exe, len) < 0) {
        fprintf(stderr, "exen_load_framework: %s\n", exen_error(vm));
        exen_destroy(vm);
        return NULL;
    }
    return vm;
}

/* ------------------------------------------------------------------ class assembler */

static void die(const char *msg)
{
    fprintf(stderr, "test class assembler: %s\n", msg);
    exit(3);
}

static void put8(TClass *c, uint8_t v)
{
    if (c->n >= sizeof c->b)
        die("class block overflow");
    c->b[c->n++] = v;
}

static uint32_t align4(uint32_t x) { return (x + 3u) & ~3u; }

void tc_init(TClass *c, int self_index, int object_index)
{
    memset(c, 0, sizeof *c);
    c->self = self_index;
    c->flags = 0x0021; /* public super */
    c->n = 0x2e;       /* header of a non-interface class */
    c->cur = -1;
    tc_class(c, self_index);
    tc_class(c, object_index);
}

int tc_class(TClass *c, int class_index)
{
    if (c->laid_out || c->ncls >= TC_MAX)
        die("tc_class");
    c->cls_idx[c->ncls] = (uint16_t)class_index;
    return c->ncls++;
}

int tc_field(TClass *c, const char *name, const char *desc, uint16_t flags, uint16_t type,
             uint16_t slot)
{
    if (c->laid_out || c->nfld >= TC_MAX)
        die("tc_field");
    int i = c->nfld++;
    c->fld[i].hash = exen_member_hash(name, desc);
    c->fld[i].flags = flags;
    c->fld[i].type = type;
    c->fld[i].slot = slot;
    uint16_t words = (type >> 8) == 0 && (type & 0xff) == T_LONG ? 2 : 1;
    uint16_t *count = (flags & ACC_STATIC) ? &c->nstatics : &c->ninst;
    if (slot + words > *count)
        *count = (uint16_t)(slot + words);
    return i;
}

int tc_method(TClass *c, const char *name, const char *desc, uint16_t flags, uint16_t nargs)
{
    if (c->laid_out || c->nmeth >= TC_MAX)
        die("tc_method");
    int i = c->nmeth++;
    c->meth[i].hash = exen_member_hash(name, desc);
    c->meth[i].flags = flags;
    c->meth[i].nargs = nargs;
    return i;
}

int tc_mref(TClass *c, uint32_t hash, uint16_t nargs, int cref)
{
    if (c->laid_out || c->nmref >= TC_MAX)
        die("tc_mref");
    int i = c->nmref++;
    c->mref[i].hash = hash;
    c->mref[i].nargs = nargs;
    c->mref[i].cref = cref;
    return i;
}

int tc_fref(TClass *c, uint32_t hash, uint16_t type, int cref)
{
    if (c->laid_out || c->nfref >= TC_MAX)
        die("tc_fref");
    int i = c->nfref++;
    c->fref[i].hash = hash;
    c->fref[i].type = type;
    c->fref[i].cref = cref;
    return i;
}

/* A table: u16 count, entries from the next 4-byte boundary (CVP.md §2). */
static uint32_t table_begin(TClass *c, int count)
{
    c->n = align4(c->n);
    uint32_t off = c->n;
    wr16(c->b + off, (uint16_t)count);
    c->n = align4(off + 2);
    return off;
}

void tc_layout(TClass *c)
{
    if (c->laid_out)
        die("tc_layout twice");
    uint16_t t_cls = 0, t_fld = 0, t_meth = 0, t_mref = 0, t_fref = 0;

    t_cls = (uint16_t)table_begin(c, c->ncls);
    for (int i = 0; i < c->ncls; i++) {
        c->cls_off[i] = (uint16_t)c->n;
        wr32(c->b + c->n, c->cls_idx[i]);
        c->n += 4;
    }
    if (c->nfld) {
        t_fld = (uint16_t)table_begin(c, c->nfld);
        for (int i = 0; i < c->nfld; i++) {
            uint8_t *r = c->b + c->n;
            c->fld_off[i] = (uint16_t)c->n;
            wr32(r, c->fld[i].hash);
            wr16(r + 4, c->fld[i].flags);
            wr16(r + 6, c->fld[i].type);
            wr16(r + 8, c->fld[i].slot);
            c->n += 12;
        }
    }
    if (c->nmeth) {
        t_meth = (uint16_t)table_begin(c, c->nmeth);
        for (int i = 0; i < c->nmeth; i++) {
            uint8_t *r = c->b + c->n;
            c->meth_off[i] = (uint16_t)c->n;
            wr32(r, c->meth[i].hash);
            wr16(r + 4, c->meth[i].flags);
            wr16(r + 6, c->meth[i].nargs);
            wr16(r + 8, 0); /* code offset, set by tc_code */
            c->n += 12;
        }
    }
    if (c->nmref) {
        t_mref = (uint16_t)table_begin(c, c->nmref);
        for (int i = 0; i < c->nmref; i++) {
            uint8_t *r = c->b + c->n;
            c->mref_off[i] = (uint16_t)c->n;
            wr32(r, c->mref[i].hash);
            wr16(r + 4, c->mref[i].nargs);
            wr16(r + 6, c->cls_off[c->mref[i].cref]);
            c->n += 8;
        }
    }
    if (c->nfref) {
        t_fref = (uint16_t)table_begin(c, c->nfref);
        for (int i = 0; i < c->nfref; i++) {
            uint8_t *r = c->b + c->n;
            c->fref_off[i] = (uint16_t)c->n;
            wr32(r, c->fref[i].hash);
            wr16(r + 4, c->fref[i].type);
            wr16(r + 6, c->cls_off[c->fref[i].cref]);
            c->n += 8;
        }
    }

    uint8_t *h = c->b;
    memcpy(h, "4CVP", 4);
    wr16(h + 0x06, 0x0100);
    wr32(h + 0x08, 0x27dd);
    char name[32];
    snprintf(name, sizeof name, "test.Synthetic%d", c->self);
    wr32(h + 0x0c, exen_crc32(name, strlen(name)));
    wr32(h + 0x10, 0);
    wr16(h + 0x14, c->flags);
    wr16(h + 0x16, 0); /* stripped, like every game class */
    wr16(h + 0x18, c->nstatics);
    wr16(h + 0x1a, 0); /* no <clinit> */
    wr16(h + 0x1c, 0); /* no interfaces */
    wr16(h + 0x1e, t_fld);
    wr16(h + 0x20, t_meth);
    wr16(h + 0x22, t_cls);
    wr16(h + 0x24, t_fref);
    wr16(h + 0x26, t_mref);
    wr16(h + 0x28, c->cls_off[1]); /* super = java.lang.Object */
    wr16(h + 0x2a, c->ninst);
    wr16(h + 0x2c, 0); /* no default constructor */
    c->laid_out = 1;
}

void tc_code(TClass *c, int m, uint16_t max_stack, uint16_t max_locals)
{
    if (!c->laid_out || m < 0 || m >= c->nmeth)
        die("tc_code");
    if (c->n & 1)
        put8(c, 0);
    c->cur = m;
    c->code_hdr[m] = c->n;
    wr16(c->b + c->meth_off[m] + 8, (uint16_t)c->n);
    tc_u16(c, max_stack);
    tc_u16(c, max_locals);
    tc_u16(c, 0); /* exception table, set by tc_finish */
}

uint32_t tc_pc(const TClass *c) { return c->n; }

void tc_u8(TClass *c, uint8_t v) { put8(c, v); }

void tc_ops(TClass *c, const char *hex)
{
    int hi = -1;
    for (const char *p = hex; *p; p++) {
        int d;
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            continue;
        if (hi < 0) {
            hi = d;
        } else {
            put8(c, (uint8_t)(hi << 4 | d));
            hi = -1;
        }
    }
    if (hi >= 0)
        die("odd hex string");
}

void tc_pad2(TClass *c)
{
    if (c->n & 1)
        put8(c, 0);
}

void tc_pad4(TClass *c)
{
    while (c->n & 3)
        put8(c, 0);
}

void tc_u16(TClass *c, uint16_t v)
{
    put8(c, (uint8_t)v);
    put8(c, (uint8_t)(v >> 8));
}

void tc_u32(TClass *c, uint32_t v)
{
    tc_u16(c, (uint16_t)v);
    tc_u16(c, (uint16_t)(v >> 16));
}

void tc_op16(TClass *c, uint8_t op, uint16_t v)
{
    put8(c, op);
    tc_pad2(c);
    tc_u16(c, v);
}

int tc_label(TClass *c)
{
    if (c->nlabel >= (int)(sizeof c->label / sizeof c->label[0]))
        die("too many labels");
    return c->nlabel++;
}

void tc_bind(TClass *c, int label) { c->label[label] = c->n; }

void tc_target(TClass *c, int label)
{
    if (c->nlpatch >= (int)(sizeof c->lpatch / sizeof c->lpatch[0]))
        die("too many label references");
    c->lpatch[c->nlpatch].at = c->n;
    c->lpatch[c->nlpatch].label = label;
    c->nlpatch++;
    tc_u16(c, 0);
}

void tc_br(TClass *c, uint8_t op, int label)
{
    put8(c, op);
    tc_pad2(c);
    tc_target(c, label);
}

static void pool_ref(TClass *c, uint8_t op, const void *data, uint16_t len, uint8_t align)
{
    if (c->npool >= (int)(sizeof c->pool / sizeof c->pool[0]) || len > sizeof c->pool[0].data)
        die("constant pool full");
    put8(c, op);
    tc_pad2(c);
    c->pool[c->npool].at = c->n;
    memcpy(c->pool[c->npool].data, data, len);
    c->pool[c->npool].len = len;
    c->pool[c->npool].align = align;
    c->npool++;
    tc_u16(c, 0);
}

void tc_ldc(TClass *c, int32_t v)
{
    uint8_t d[4];
    wr32(d, (uint32_t)v);
    pool_ref(c, 0x12, d, 4, 4);
}

void tc_ldc2(TClass *c, int64_t v)
{
    uint8_t d[8];
    wr32(d, (uint32_t)(uint64_t)v);
    wr32(d + 4, (uint32_t)((uint64_t)v >> 32));
    pool_ref(c, 0x14, d, 8, 4);
}

void tc_ldstr(TClass *c, const char *s)
{
    uint8_t d[260];
    size_t n = strlen(s);
    if (n > 250)
        die("string literal too long");
    wr16(d, (uint16_t)n);
    memcpy(d + 2, s, n);
    uint16_t len = (uint16_t)(2 + n);
    if (len & 1)
        d[len++] = 0; /* strings are padded to an even length (CVP.md §2) */
    pool_ref(c, 0xd0, d, len, 2);
}

void tc_bipush(TClass *c, int8_t v)
{
    put8(c, OP_BIPUSH);
    put8(c, (uint8_t)v);
}

void tc_sipush(TClass *c, int16_t v) { tc_op16(c, OP_SIPUSH, (uint16_t)v); }

void tc_exc(TClass *c, int m, int start, int end, int end_adj, int handler, int cref)
{
    if (c->nexc >= (int)(sizeof c->exc / sizeof c->exc[0]))
        die("too many exception entries");
    c->exc[c->nexc].method = m;
    c->exc[c->nexc].start = start;
    c->exc[c->nexc].end = end;
    c->exc[c->nexc].end_adj = end_adj;
    c->exc[c->nexc].handler = handler;
    c->exc[c->nexc].cref = cref;
    c->nexc++;
}

static uint16_t label_at(TClass *c, int l)
{
    if (l < 0 || l >= c->nlabel || !c->label[l])
        die("unbound label");
    return (uint16_t)c->label[l];
}

uint32_t tc_finish(TClass *c)
{
    for (int i = 0; i < c->npool; i++) {
        while (c->n % c->pool[i].align)
            put8(c, 0);
        uint32_t off = c->n;
        for (int k = 0; k < c->pool[i].len; k++)
            put8(c, c->pool[i].data[k]);
        wr16(c->b + c->pool[i].at, (uint16_t)off);
    }
    /* exception tables (CVP.md §4.5): u16 count, then {start, end inclusive, handler, cref} */
    for (int m = 0; m < c->nmeth; m++) {
        int count = 0;
        for (int i = 0; i < c->nexc; i++)
            count += c->exc[i].method == m;
        if (!count)
            continue;
        tc_pad2(c);
        wr16(c->b + c->code_hdr[m] + 4, (uint16_t)c->n);
        tc_u16(c, (uint16_t)count);
        for (int i = 0; i < c->nexc; i++) {
            if (c->exc[i].method != m)
                continue;
            tc_u16(c, label_at(c, c->exc[i].start));
            tc_u16(c, (uint16_t)(label_at(c, c->exc[i].end) + c->exc[i].end_adj));
            tc_u16(c, label_at(c, c->exc[i].handler));
            tc_u16(c, c->exc[i].cref >= 0 ? c->cls_off[c->exc[i].cref] : 0);
        }
    }
    for (int i = 0; i < c->nlpatch; i++)
        wr16(c->b + c->lpatch[i].at, label_at(c, c->lpatch[i].label));
    tc_pad4(c);
    if (c->n > 0xffff)
        die("class block larger than 64 KiB");
    wr16(c->b + 4, (uint16_t)c->n);
    return c->n;
}

/* ------------------------------------------------------------------ containers and calls */

uint8_t *t_make_exn(const uint8_t *code, size_t code_len, const char *main_class, size_t *len)
{
    /* NEXE header (docs/FORMAT.md): no resources, so offsets[0] is the code chunk */
    size_t hdr = 0x40;
    uint8_t *x = calloc(1, hdr + code_len);
    if (!x)
        return NULL;
    memcpy(x, "NEXE", 4);
    wr32(x + 4, 0x80);
    strncpy((char *)x + 0x14, main_class, 32);
    wr32(x + 0x34, 0);
    wr32(x + 0x38, (uint32_t)hdr);
    memcpy(x + hdr, code, code_len);
    *len = hdr + code_len;
    return x;
}

int t_load_classes(ExenVM *vm, TClass *const *classes, int n)
{
    size_t total = 0;
    for (int i = 0; i < n; i++)
        total += classes[i]->n;
    uint8_t *code = malloc(total ? total : 1);
    if (!code)
        return -1;
    size_t p = 0;
    for (int i = 0; i < n; i++) {
        memcpy(code + p, classes[i]->b, classes[i]->n);
        p += classes[i]->n;
    }
    size_t len;
    uint8_t *exn = t_make_exn(code, total, "test.Synthetic", &len);
    free(code);
    if (!exn)
        return -1;
    int rc = exen_load_game(vm, exn, len);
    free(exn);
    return rc;
}

int t_call(ExenVM *vm, int cls, const char *name, const char *desc, const uint32_t *args,
           int nargs, uint32_t *ret)
{
    int found = -1;
    uint32_t m = find_method(vm, cls, exen_member_hash(name, desc), 0, &found);
    if (!m) {
        fprintf(stderr, "t_call: no method %s%s in class %d\n", name, desc, cls);
        return -2;
    }
    if (vm_init_class(vm, found) < 0)
        return -1;
    uint32_t a[32];
    if (nargs > 32)
        return -2;
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];
    return vm_call(vm, found, m, a, nargs, ret);
}
