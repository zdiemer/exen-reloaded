/* Save states (exen.h: exen_state_size / exen_state_save / exen_state_load).
 *
 * When a state can be taken
 * -------------------------
 * Every host call runs to completion, so between calls the interpreter has no frames
 * (vm->fp == -1), no native is running, no exception is propagating and no GC roots are
 * pushed. That is the only point where we take a state, and the only state that exists then
 * is plain data: the arena, a handful of VM fields, and the platform structs.
 *
 * Container (all integers little-endian)
 * --------------------------------------
 *   header, 40 bytes:
 *     +0  "EXST"
 *     +4  u32 format version (STATE_VERSION); any other version is refused
 *     +8  u32 core revision (STATE_CORE_REV), informational: a state from another revision is
 *             loaded, with a log line, because its contents mean the same
 *     +12 u32 CRC-32 of the .exn file            } the loaded game and framework; a state for
 *     +16 u32 CRC-32 of the class blocks, as     } a different one is refused
 *             loaded (framework + game code)
 *     +20 u32 code_end    +24 u32 arena size (code + heap): must match the loading VM
 *     +28 u32 payload length   +32 u32 CRC-32 of the payload   +36 u32 0 (reserved)
 *   payload: chunks of { u32 tag, u32 length, data }. The tags below are required once each
 *   (CODE is optional); unknown tags are skipped. Bytes after the payload are ignored.
 *
 * Byte images (heap, stack, code) are stored as a run-length code for zeros: records of
 * { u32 zero count, u32 literal count, literal bytes } that exactly fill the image. Free heap
 * blocks are stored as their 8-byte header followed by zeros: their contents are never read
 * (heap_alloc clears a block before handing it out), and zeroing them makes the state
 * independent of garbage and keeps it small.
 *
 * Field audit
 * -----------
 * ExenVM (vm.h)
 *   mem [0, code_end)        not saved: the class blocks, identical in any VM that loaded the
 *                            same framework and game (header CRC). Nothing writes them at run
 *                            time; if something ever does, the region is saved as CODE.
 *   mem [heap_start, heap_end) saved (HEAP), free-block payloads zeroed
 *   mem_size, code_end, heap_start, heap_end  checked equal (derived from config and code)
 *   free_list, heap_used     saved (VM)
 *   objmap                   rebuilt from the heap blocks (and validated against them)
 *   classes[].statics        saved (VM); every other ExClass field is derived from the class
 *                            blocks by the loader
 *   nclasses, nfw, cls_*     derived (checked: nclasses)
 *   stack                    saved (STAK) up to the last nonzero word: the collector scans
 *                            stale words just past a native's arguments conservatively, so
 *                            they can keep objects alive and must match for determinism
 *   frames, fp               none live (fp == -1)
 *   roots, nroots, pending, native_top, in_native, native_exc  must be empty; reset on load
 *   last_ret_words           scratch of the call in progress, not saved
 *   halted, halt_reason      saved (VM)
 *   native_calls             diagnostics, not saved (counts continue in the loading VM)
 *   host, user               host's, not saved
 *   math.cos_table           derived (gensimu.exe)
 *   math.s0, s1, period      saved (VM): Math.random state and sinus period
 *   plat                     see below
 *   ext.cls                  cache of class indices, derived
 *   ext.warned               log-once bits, not saved
 *   ext.sms_sent, sms_status saved (VM): a Bootstrap.smsSent report queued for after the next
 *                            callback (a send made from within the smsSent handler)
 *   ext.delivering           0 between callbacks
 *   clock_offset             not saved as such: the VM clock value is saved (VM) and the
 *                            loading VM sets its offset so that the clock continues from it
 *   exn, exn_len, exn_crc, code_crc, main_class  the loaded game, checked by CRC
 *   error                    not saved
 * struct ExenPlatform (platform.h)
 *   width, height            checked equal (device profile)
 *   eeprom                   saved (EEPR): games read it back through loadCtx, so a state
 *                            restores the persistent data it was taken with
 *   font, have_font          derived (gensimu.exe)
 *   lut, lut_version, lut_valid  saved (PLAT). Invalidating would be cheaper but not exact:
 *                            the original's LUT is keyed by palette version only, so a stale
 *                            LUT is observable and must survive the round trip
 *   normal_video, timer_ms   saved (PLAT); the host's timer is re-armed on load (set_timer)
 *   fb                       saved (PLAT): what the LCD shows; presented on load
 *   melody                   saved (MELO), except the host settings rate, volume and the
 *                            melodies counter. If the loading VM renders at another rate, the
 *                            sample-based timing (wait, oscillator increments, release steps)
 *                            is converted.
 *   cls, warned              class cache and log-once bits, not saved
 * Floats (melody synthesizer) are stored as their IEEE-754 bit patterns. Heap slots are
 * accessed as host uint32_t words (obj_slots), so the arena, and with it a state, is
 * little-endian on the little-endian hosts the core targets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extnatives.h"
#include "natives.h"
#include "platform.h"

#define STATE_VERSION 1u
#define STATE_CORE_REV 1u
#define HDR_BYTES 40u

#define TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define T_VM   TAG('V', 'M', ' ', ' ')
#define T_HEAP TAG('H', 'E', 'A', 'P')
#define T_STAK TAG('S', 'T', 'A', 'K')
#define T_CODE TAG('C', 'O', 'D', 'E')
#define T_PLAT TAG('P', 'L', 'A', 'T')
#define T_EEPR TAG('E', 'E', 'P', 'R')
#define T_MELO TAG('M', 'E', 'L', 'O')

/* zero runs shorter than this stay in the literal */
#define RLE_MIN_ZEROS 16u

/* ------------------------------------------------------------ CRC-32 (table driven) */

static uint32_t crc_table[256];

static uint32_t crc32_fast(const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
            crc_table[i] = c;
        }
    uint32_t c = 0xffffffffu;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static void set_err(ExenVM *vm, const char *msg)
{
    size_t n = strlen(msg);
    if (n >= sizeof vm->error)
        n = sizeof vm->error - 1;
    memcpy(vm->error, msg, n);
    vm->error[n] = 0;
    vm->error_after_halt = vm->halted; /* exen_error reports this, not the halt reason */
}

/* ------------------------------------------------------------ writer */

/* Writes into buf when it is non-NULL and has room; always counts. */
typedef struct {
    uint8_t *buf;
    size_t cap, pos;
    /* RLE encoder */
    size_t rec;     /* position of the open record's header */
    int open;
    uint32_t zpend; /* zeros not yet emitted */
    uint32_t lit;   /* literal bytes in the open record */
} W;

static void w_bytes(W *w, const void *p, size_t n)
{
    if (w->buf && w->pos + n <= w->cap && n)
        memcpy(w->buf + w->pos, p, n);
    w->pos += n;
}

static void w_u32(W *w, uint32_t v)
{
    uint8_t b[4];
    wr32(b, v);
    w_bytes(w, b, 4);
}

static void w_patch32(W *w, size_t at, uint32_t v)
{
    if (w->buf && at + 4 <= w->cap)
        wr32(w->buf + at, v);
}

static void w_i32(W *w, int32_t v) { w_u32(w, (uint32_t)v); }

static void w_f32(W *w, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    w_u32(w, u);
}

static size_t chunk_begin(W *w, uint32_t tag)
{
    w_u32(w, tag);
    w_u32(w, 0);
    return w->pos;
}

static void chunk_end(W *w, size_t start) { w_patch32(w, start - 4, (uint32_t)(w->pos - start)); }

static void rle_close(W *w)
{
    if (w->open) {
        w_patch32(w, w->rec + 4, w->lit);
        w->open = 0;
    }
}

static void rle_begin(W *w)
{
    w->open = 0;
    w->zpend = 0;
    w->lit = 0;
}

static void rle_zeros(W *w, uint32_t n) { w->zpend += n; }

static void rle_bytes(W *w, const uint8_t *p, size_t n)
{
    static const uint8_t zeros[RLE_MIN_ZEROS];
    for (size_t i = 0; i < n; i++) {
        if (!p[i]) {
            w->zpend++;
            continue;
        }
        if (w->open && w->zpend < RLE_MIN_ZEROS) {
            w_bytes(w, zeros, w->zpend); /* a short zero run stays in the literal */
            w->lit += w->zpend;
        } else {
            rle_close(w);
            w->rec = w->pos;
            w_u32(w, w->zpend);
            w_u32(w, 0);
            w->open = 1;
            w->lit = 0;
        }
        w->zpend = 0;
        /* copy the whole nonzero run at once */
        size_t j = i;
        while (j < n && p[j])
            j++;
        w_bytes(w, p + i, j - i);
        w->lit += (uint32_t)(j - i);
        i = j - 1;
    }
}

static void rle_end(W *w)
{
    rle_close(w);
    if (w->zpend) {
        w_u32(w, w->zpend);
        w_u32(w, 0);
        w->zpend = 0;
    }
}

/* ------------------------------------------------------------ reader */

typedef struct {
    const uint8_t *p;
    size_t len, pos;
    int bad;
} R;

static int r_need(R *r, size_t n)
{
    if (r->bad || n > r->len - r->pos) {
        r->bad = 1;
        return 0;
    }
    return 1;
}

static uint32_t r_u32(R *r)
{
    if (!r_need(r, 4))
        return 0;
    uint32_t v = rd32(r->p + r->pos);
    r->pos += 4;
    return v;
}

static int32_t r_i32(R *r) { return (int32_t)r_u32(r); }

static float r_f32(R *r)
{
    uint32_t u = r_u32(r);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void r_bytes(R *r, void *dst, size_t n)
{
    if (!r_need(r, n))
        return;
    memcpy(dst, r->p + r->pos, n);
    r->pos += n;
}

/* Decode an RLE image of exactly `n` bytes; the chunk must end with it. */
static int rle_decode(R *r, uint8_t *out, size_t n)
{
    size_t o = 0;
    while (o < n) {
        uint32_t z = r_u32(r), l = r_u32(r);
        if (r->bad || z > n - o || l > n - o - z || (z == 0 && l == 0))
            return -1;
        memset(out + o, 0, z);
        o += z;
        if (!r_need(r, l))
            return -1;
        memcpy(out + o, r->p + r->pos, l);
        r->pos += l;
        o += l;
    }
    return r->pos == r->len ? 0 : -1;
}

/* ------------------------------------------------------------ capture */

static int can_capture(ExenVM *vm)
{
    if (!vm->exn || !vm->plat) {
        set_err(vm, "save state: no game loaded");
        return 0;
    }
    /* A halted VM may have been stopped inside a call; it never runs again, so its state is
     * just "halted" and the frames it left behind do not matter. */
    if (!vm->halted && (vm->fp != -1 || vm->nroots || vm->pending || vm->native_top ||
                        vm->in_native || vm->ext.delivering)) {
        set_err(vm, "save state: the VM is running (states are taken between host calls)");
        return 0;
    }
    return 1;
}

static void write_heap(W *w, ExenVM *vm)
{
    rle_begin(w);
    uint32_t off = vm->heap_start;
    while (off < vm->heap_end) {
        uint32_t hdr = rd32(P(vm, off));
        uint32_t sz = hdr & HDR_SIZE_MASK;
        if (sz < 8 || sz > vm->heap_end - off) { /* corrupt: store the rest verbatim */
            rle_bytes(w, P(vm, off), vm->heap_end - off);
            break;
        }
        if (hdr & HDR_FREE) {
            rle_bytes(w, P(vm, off), 8);
            rle_zeros(w, sz - 8);
        } else {
            rle_bytes(w, P(vm, off), sz);
        }
        off += sz;
    }
    rle_end(w);
}

static void write_melody(W *w, const Melody *m)
{
    w_i32(w, m->rate);
    w_u32(w, m->len);
    w_bytes(w, m->data, m->len);
    w_u32(w, m->pos);
    w_u32(w, m->events);
    w_u32(w, m->prev);
    w_u32(w, m->ch);
    w_u32(w, m->cur_dur);
    w_u32(w, m->playing);
    w_u32(w, m->vol_l);
    w_u32(w, m->vol_r);
    w_bytes(w, m->program, sizeof m->program);
    w_i32(w, m->period_ms);
    w_u32(w, (uint32_t)(uint64_t)m->wait);
    w_u32(w, (uint32_t)((uint64_t)m->wait >> 32));
    w_u32(w, MELODY_VOICES);
    for (int i = 0; i < MELODY_VOICES; i++) {
        const MelodyVoice *v = &m->voice[i];
        w_u32(w, v->state);
        w_u32(w, v->note);
        w_u32(w, v->ch);
        w_u32(w, v->drum);
        w_f32(w, v->phase);
        w_f32(w, v->inc);
        w_f32(w, v->env);
        w_f32(w, v->rel_step);
        w_u32(w, v->noise);
        w_f32(w, v->lp);
    }
    w_f32(w, m->gain_l);
    w_f32(w, m->gain_r);
    w_f32(w, m->lp_l);
    w_f32(w, m->lp_r);
}

static size_t write_state(ExenVM *vm, W *w)
{
    struct ExenPlatform *p = vm->plat;
    /* header; the payload length and CRC are patched in at the end */
    w_bytes(w, "EXST", 4);
    w_u32(w, STATE_VERSION);
    w_u32(w, STATE_CORE_REV);
    w_u32(w, vm->exn_crc);
    w_u32(w, vm->code_crc);
    w_u32(w, vm->code_end);
    w_u32(w, vm->mem_size);
    w_u32(w, 0);
    w_u32(w, 0);
    w_u32(w, 0);

    size_t c = chunk_begin(w, T_VM);
    w_u32(w, (uint32_t)vm->halted);
    uint32_t rl = 0;
    while (vm->halted && rl < sizeof vm->halt_reason - 1 && vm->halt_reason[rl])
        rl++;
    w_u32(w, rl);
    w_bytes(w, vm->halt_reason, rl);
    w_u32(w, vm->heap_start);
    w_u32(w, vm->heap_end);
    w_u32(w, vm->free_list);
    w_u32(w, vm->heap_used);
    w_i32(w, vm->math.s0);
    w_i32(w, vm->math.s1);
    w_i32(w, vm->math.period);
    w_u32(w, (uint32_t)vm->ext.sms_sent);
    w_i32(w, vm->ext.sms_status);
    w_u32(w, exen_vm_ticks(vm));
    w_u32(w, (uint32_t)vm->nclasses);
    for (int i = 0; i < vm->nclasses; i++)
        w_u32(w, vm->classes[i].statics);
    chunk_end(w, c);

    c = chunk_begin(w, T_HEAP);
    w_u32(w, vm->heap_end - vm->heap_start);
    write_heap(w, vm);
    chunk_end(w, c);

    c = chunk_begin(w, T_STAK);
    uint32_t nw = EXEN_STACK_WORDS;
    while (nw && !vm->stack[nw - 1])
        nw--;
    w_u32(w, nw);
    rle_begin(w);
    for (uint32_t i = 0; i < nw; i++) {
        uint8_t b[4];
        wr32(b, vm->stack[i]);
        rle_bytes(w, b, 4);
    }
    rle_end(w);
    chunk_end(w, c);

    /* the class blocks, only if something modified them since loading */
    if (crc32_fast(vm->mem, vm->code_end) != vm->code_crc) {
        c = chunk_begin(w, T_CODE);
        rle_begin(w);
        rle_bytes(w, vm->mem, vm->code_end);
        rle_end(w);
        chunk_end(w, c);
    }

    c = chunk_begin(w, T_PLAT);
    w_i32(w, p->width);
    w_i32(w, p->height);
    w_i32(w, p->normal_video);
    w_i32(w, p->timer_ms);
    w_i32(w, p->lut_valid);
    w_u32(w, p->lut_version);
    w_bytes(w, p->lut, sizeof p->lut);
    w_bytes(w, p->fb, (size_t)p->width * (size_t)p->height);
    chunk_end(w, c);

    c = chunk_begin(w, T_EEPR);
    w_bytes(w, p->eeprom, EXEN_EEPROM_SIZE);
    chunk_end(w, c);

    c = chunk_begin(w, T_MELO);
    write_melody(w, &p->melody);
    chunk_end(w, c);

    size_t total = w->pos;
    w_patch32(w, 28, (uint32_t)(total - HDR_BYTES));
    if (w->buf && total <= w->cap)
        w_patch32(w, 32, crc32_fast(w->buf + HDR_BYTES, total - HDR_BYTES));
    return total;
}

size_t exen_state_size(ExenVM *vm)
{
    if (!can_capture(vm))
        return 0;
    W w = {0};
    return write_state(vm, &w);
}

int exen_state_save(ExenVM *vm, void *buf, size_t cap)
{
    if (!can_capture(vm))
        return -1;
    if (!buf) {
        set_err(vm, "save state: no buffer");
        return -1;
    }
    W w = {0};
    w.buf = buf;
    w.cap = cap;
    size_t n = write_state(vm, &w);
    if (n > cap) {
        set_err(vm, "save state: buffer too small (see exen_state_size)");
        return -1;
    }
    if (n > 0x7fffffffu) {
        set_err(vm, "save state: too large");
        return -1;
    }
    return (int)n;
}

/* ------------------------------------------------------------ restore */

typedef struct {
    /* VM */
    int halted;
    char reason[256];
    uint32_t free_list, heap_used, ticks;
    int32_t s0, s1, period, sms_status;
    int sms_sent;
    ref_t *statics;
    /* images */
    uint8_t *heap, *objmap, *code;
    uint32_t stack_words;
    uint32_t *stack;
    const char *why;        /* check_heap's finding */
    /* PLAT */
    int32_t normal_video, timer_ms, lut_valid;
    uint32_t lut_version;
    uint8_t lut[256];
    uint8_t *fb;
    uint8_t eeprom[EXEN_EEPROM_SIZE];
    Melody mel;
    int32_t mel_rate;
} Staged;

static void staged_free(Staged *s)
{
    free(s->statics);
    free(s->heap);
    free(s->objmap);
    free(s->code);
    free(s->stack);
    free(s->fb);
}

static int parse_vm(ExenVM *vm, R *r, Staged *s)
{
    s->halted = r_u32(r) != 0;
    uint32_t rl = r_u32(r);
    if (rl >= sizeof s->reason || (!s->halted && rl))
        return -1;
    r_bytes(r, s->reason, rl);
    s->reason[rl] = 0;
    uint32_t hs = r_u32(r), he = r_u32(r);
    if (hs != vm->heap_start || he != vm->heap_end)
        return -1;
    s->free_list = r_u32(r);
    s->heap_used = r_u32(r);
    s->s0 = r_i32(r);
    s->s1 = r_i32(r);
    s->period = r_i32(r);
    s->sms_sent = r_u32(r) != 0;
    s->sms_status = r_i32(r);
    s->ticks = r_u32(r);
    uint32_t n = r_u32(r);
    if (r->bad || n != (uint32_t)vm->nclasses)
        return -1;
    s->statics = calloc(n ? n : 1, sizeof *s->statics);
    if (!s->statics)
        return -1;
    for (uint32_t i = 0; i < n; i++)
        s->statics[i] = r_u32(r);
    /* the sinus period divides: 0 would trap */
    if (s->period == 0)
        return -1;
    return r->bad || r->pos != r->len ? -1 : 0;
}

/* Heap image checks: the block chain tiles the heap, the free list holds exactly the free
 * blocks, heap_used matches, every allocated block is a well-formed object, static area or
 * array of a known class, and each initialised class owns exactly its static area. Builds the
 * objmap. Reference fields and the elements of object arrays must hold null or an object.
 * Arrays of arrays are not checked: real states have non-references there (the leaf rows of a
 * multianewarray carry an array-of-arrays type, interp.c build_multi), and the collector
 * tolerates them. */
static int check_heap(ExenVM *vm, Staged *s)
{
    uint32_t hs = vm->heap_start, he = vm->heap_end, span = he - hs;
    const uint8_t *h = s->heap;
    size_t bits = span >> 3;
    s->objmap = calloc((bits + 7) / 8, 1);
    uint8_t *freemap = calloc((bits + 7) / 8, 1);
    int rc = -1;
#define FAIL(msg) do { s->why = (msg); goto out; } while (0)
    if (!s->objmap || !freemap)
        FAIL("out of memory");
#define BIT(map, off) ((map)[((off) - hs) >> 6] & (1u << ((((off) - hs) >> 3) & 7)))
#define SETBIT(map, off) ((map)[((off) - hs) >> 6] |= (uint8_t)(1u << ((((off) - hs) >> 3) & 7)))
    uint32_t used = 0, nfree = 0;
    for (uint32_t off = hs; off < he;) {
        uint32_t hdr = rd32(h + (off - hs)), sz = hdr & HDR_SIZE_MASK;
        if (sz < 16 || sz > he - off || (hdr & HDR_MARK) || (hdr & 4))
            FAIL("bad block header");
        if (hdr & HDR_FREE) {
            SETBIT(freemap, off);
            nfree++;
        } else {
            SETBIT(s->objmap, off);
            used += sz;
        }
        off += sz;
    }
    if (used != s->heap_used)
        FAIL("heap_used mismatch");
    /* the free list: only free blocks, each once, all of them */
    uint32_t seen = 0;
    for (uint32_t f = s->free_list; f; f = rd32(h + (f - hs) + 4)) {
        if (f < hs || f >= he || (f & 7) || !BIT(freemap, f) || seen >= nfree)
            FAIL("bad free list entry");
        freemap[(f - hs) >> 6] &= (uint8_t)~(1u << (((f - hs) >> 3) & 7)); /* visited */
        seen++;
    }
    if (seen != nfree)
        FAIL("free list misses free blocks");
    /* the blocks */
#define ISOBJ(v) ((v) >= hs && (v) < he && !((v) & 7) && BIT(s->objmap, (v)))
    for (uint32_t off = hs; off < he;) {
        const uint8_t *b = h + (off - hs);
        uint32_t hdr = rd32(b), sz = hdr & HDR_SIZE_MASK;
        if (!(hdr & HDR_FREE)) {
            uint16_t cls = rd16(b + 4), kind = rd16(b + 6);
            if (cls >= vm->nclasses)
                FAIL("bad class index");
            const ExClass *c = &vm->classes[cls];
            uint32_t nslots = 0;
            if (kind & EXEN_KIND_ARRAY) {
                uint16_t tw = kind & 0x7fff;
                uint32_t len = rd16(b + 10);
                if (12u + len * (uint32_t)elem_size(tw) > sz)
                    FAIL("array overruns its block");
                if (tw == T_OBJECT) /* object arrays: null or objects; see above for T[][] */
                    for (uint32_t i = 0; i < len; i++) {
                        uint32_t v = rd32(b + 12 + 4 * i);
                        if (v && !ISOBJ(v))
                            FAIL("an object array element does not point at an object");
                    }
            } else if (kind == EXEN_KIND_OBJECT) {
                nslots = c->inst_total;
            } else if (kind == EXEN_KIND_STATICS) {
                nslots = c->nstatics;
                if (s->statics[cls] != off)
                    FAIL("a static area its class does not own");
            } else {
                FAIL("bad block kind");
            }
            if (8u + 4u * nslots > sz)
                FAIL("object overruns its block");
            /* reference fields hold null or an object (their types are exact, unlike arrays) */
            const uint8_t *map = kind == EXEN_KIND_OBJECT ? c->inst_refmap
                               : kind == EXEN_KIND_STATICS ? c->static_refmap : NULL;
            for (uint32_t i = 0; map && i < nslots; i++)
                if (map[i >> 3] & (1u << (i & 7))) {
                    uint32_t v = rd32(b + 8 + 4 * i);
                    if (v && !ISOBJ(v))
                        FAIL("a reference field does not point at an object");
                }
        }
        off += sz;
    }
    /* every initialised class owns a static area */
    for (int i = 0; i < vm->nclasses; i++) {
        ref_t st = s->statics[i];
        if (st && (!ISOBJ(st) || rd16(h + (st - hs) + 6) != EXEN_KIND_STATICS))
            FAIL("bad static area ref");
    }
    rc = 0;
#undef FAIL
#undef ISOBJ
#undef BIT
#undef SETBIT
out:
    free(freemap);
    return rc;
}

static int parse_plat(ExenVM *vm, R *r, Staged *s)
{
    struct ExenPlatform *p = vm->plat;
    int32_t w = r_i32(r), h = r_i32(r);
    if (w != p->width || h != p->height)
        return -1;
    s->normal_video = r_i32(r);
    s->timer_ms = r_i32(r);
    s->lut_valid = r_i32(r);
    s->lut_version = r_u32(r);
    r_bytes(r, s->lut, sizeof s->lut);
    size_t n = (size_t)p->width * (size_t)p->height;
    s->fb = malloc(n ? n : 1);
    if (!s->fb)
        return -1;
    r_bytes(r, s->fb, n);
    if (s->timer_ms != -1 && (s->timer_ms < 10 || s->timer_ms > 60000))
        return -1;
    return r->bad || r->pos != r->len ? -1 : 0;
}

static int parse_melody(R *r, Staged *s)
{
    Melody *m = &s->mel;
    memset(m, 0, sizeof *m);
    s->mel_rate = r_i32(r);
    if (s->mel_rate < 8000 || s->mel_rate > 192000)
        return -1;
    m->len = r_u32(r);
    if (m->len > MELODY_MAX)
        return -1;
    r_bytes(r, m->data, m->len);
    m->pos = r_u32(r);
    m->events = r_u32(r);
    uint32_t prev = r_u32(r), ch = r_u32(r), dur = r_u32(r), playing = r_u32(r);
    uint32_t vl = r_u32(r), vr = r_u32(r);
    if (m->pos > m->len || prev > 0xffff || ch > 15 || dur > 0xff || playing > 1 || vl > 0xff ||
        vr > 0xff)
        return -1;
    m->prev = (uint16_t)prev;
    m->ch = (uint8_t)ch;
    m->cur_dur = (uint8_t)dur;
    m->playing = (uint8_t)playing;
    m->vol_l = (uint8_t)vl;
    m->vol_r = (uint8_t)vr;
    r_bytes(r, m->program, sizeof m->program);
    m->period_ms = r_i32(r);
    uint32_t lo = r_u32(r), hi = r_u32(r);
    m->wait = (int64_t)((uint64_t)hi << 32 | lo);
    if (m->period_ms < 0 || m->period_ms > 1000000)
        return -1;
    if (r_u32(r) != MELODY_VOICES)
        return -1;
    for (int i = 0; i < MELODY_VOICES; i++) {
        MelodyVoice *v = &m->voice[i];
        uint32_t st = r_u32(r), note = r_u32(r), vch = r_u32(r), drum = r_u32(r);
        if (st > 3 || note > 0xff || vch > 15 || drum > 1)
            return -1;
        v->state = (uint8_t)st;
        v->note = (uint8_t)note;
        v->ch = (uint8_t)vch;
        v->drum = (uint8_t)drum;
        v->phase = r_f32(r);
        v->inc = r_f32(r);
        v->env = r_f32(r);
        v->rel_step = r_f32(r);
        v->noise = r_u32(r);
        v->lp = r_f32(r);
    }
    m->gain_l = r_f32(r);
    m->gain_r = r_f32(r);
    m->lp_l = r_f32(r);
    m->lp_r = r_f32(r);
    return r->bad || r->pos != r->len ? -1 : 0;
}

/* The saved melody in the loading VM, converting sample-based timing to its output rate. */
static void commit_melody(Melody *dst, const Staged *s)
{
    int32_t rate = dst->rate, volume = dst->volume;
    uint32_t melodies = dst->melodies;
    *dst = s->mel;
    dst->rate = rate;
    dst->volume = volume;
    dst->melodies = melodies;
    if (rate != s->mel_rate) {
        double k = (double)rate / (double)s->mel_rate;
        dst->wait = (int64_t)((double)dst->wait * k);
        for (int i = 0; i < MELODY_VOICES; i++) {
            dst->voice[i].inc = (float)(dst->voice[i].inc / k);
            dst->voice[i].rel_step = (float)(dst->voice[i].rel_step / k);
        }
    }
}

int exen_state_load(ExenVM *vm, const void *buf, size_t len)
{
    const uint8_t *b = buf;
    if (!vm->exn || !vm->plat) {
        set_err(vm, "load state: no game loaded");
        return -1;
    }
    if (!vm->halted && (vm->fp != -1 || vm->native_top || vm->in_native || vm->ext.delivering)) {
        set_err(vm, "load state: the VM is running (states are loaded between host calls)");
        return -1;
    }
    if (!b || len < HDR_BYTES || memcmp(b, "EXST", 4) != 0) {
        set_err(vm, "load state: not an exen-reloaded save state");
        return -1;
    }
    if (rd32(b + 4) != STATE_VERSION) {
        set_err(vm, "load state: unsupported save state version");
        return -1;
    }
    if (rd32(b + 12) != vm->exn_crc) {
        set_err(vm, "load state: the state is for a different game");
        return -1;
    }
    if (rd32(b + 16) != vm->code_crc || rd32(b + 20) != vm->code_end) {
        set_err(vm, "load state: the state was saved with a different framework (gensimu.exe)");
        return -1;
    }
    if (rd32(b + 24) != vm->mem_size) {
        set_err(vm, "load state: the state was saved with a different heap size");
        return -1;
    }
    uint32_t plen = rd32(b + 28);
    if (plen > len - HDR_BYTES) {
        set_err(vm, "load state: truncated save state");
        return -1;
    }
    if (crc32_fast(b + HDR_BYTES, plen) != rd32(b + 32)) {
        set_err(vm, "load state: the save state is damaged (checksum mismatch)");
        return -1;
    }

    /* find the chunks */
    static const uint32_t req[] = {T_VM, T_HEAP, T_STAK, T_PLAT, T_EEPR, T_MELO};
    enum { NREQ = sizeof req / sizeof req[0] };
    R chunk[NREQ + 1];
    int have[NREQ + 1] = {0};
    R top = {b + HDR_BYTES, plen, 0, 0};
    while (top.pos < top.len) {
        uint32_t tag = r_u32(&top), clen = r_u32(&top);
        if (!r_need(&top, clen))
            break;
        int k = -1;
        for (int i = 0; i < NREQ; i++)
            if (req[i] == tag)
                k = i;
        if (tag == T_CODE)
            k = NREQ;
        if (k >= 0) {
            if (have[k]) {
                top.bad = 1;
                break;
            }
            have[k] = 1;
            chunk[k] = (R){top.p + top.pos, clen, 0, 0};
        }
        top.pos += clen;
    }
    for (int i = 0; i < NREQ; i++)
        if (!have[i])
            top.bad = 1;
    if (top.bad) {
        set_err(vm, "load state: the save state is malformed");
        return -1;
    }

    Staged s;
    memset(&s, 0, sizeof s);
    const char *why = NULL;
    char msg[160];
    uint32_t span = vm->heap_end - vm->heap_start;
    if (parse_vm(vm, &chunk[0], &s) < 0)
        why = "load state: bad VM data";
    if (!why) {
        R *r = &chunk[1];
        s.heap = malloc(span);
        if (!s.heap)
            why = "load state: out of memory";
        else if (r_u32(r) != span || rle_decode(r, s.heap, span) < 0)
            why = "load state: bad heap data";
        else if (check_heap(vm, &s) < 0) {
            snprintf(msg, sizeof msg, "load state: the heap in the save state is inconsistent (%s)",
                     s.why ? s.why : "?");
            why = msg;
        }
    }
    if (!why) {
        R *r = &chunk[2];
        s.stack_words = r_u32(r);
        if (s.stack_words > EXEN_STACK_WORDS) {
            why = "load state: bad stack data";
        } else {
            uint8_t *tmp = malloc((size_t)s.stack_words * 4 + 1);
            s.stack = malloc((size_t)s.stack_words * 4 + 4);
            if (!tmp || !s.stack)
                why = "load state: out of memory";
            else if (rle_decode(r, tmp, (size_t)s.stack_words * 4) < 0)
                why = "load state: bad stack data";
            else
                for (uint32_t i = 0; i < s.stack_words; i++)
                    s.stack[i] = rd32(tmp + 4 * i);
            free(tmp);
        }
    }
    if (!why && have[NREQ]) {
        s.code = malloc(vm->code_end);
        if (!s.code)
            why = "load state: out of memory";
        else if (rle_decode(&chunk[NREQ], s.code, vm->code_end) < 0)
            why = "load state: bad code data";
    }
    if (!why && parse_plat(vm, &chunk[3], &s) < 0)
        why = "load state: bad display data (or a different screen size)";
    if (!why) {
        R *r = &chunk[4];
        r_bytes(r, s.eeprom, EXEN_EEPROM_SIZE);
        if (r->bad || r->pos != r->len)
            why = "load state: bad EEPROM data";
    }
    if (!why && parse_melody(&chunk[5], &s) < 0)
        why = "load state: bad melody data";
    if (why) {
        staged_free(&s);
        set_err(vm, why);
        return -1;
    }

    if (rd32(b + 8) != STATE_CORE_REV)
        vm_log(vm, "load state: saved by core revision %u (this is %u)", rd32(b + 8),
               STATE_CORE_REV);

    /* commit: nothing below can fail */
    struct ExenPlatform *p = vm->plat;
    if (s.code)
        memcpy(vm->mem, s.code, vm->code_end);
    memcpy(vm->mem + vm->heap_start, s.heap, span);
    free(vm->objmap);
    vm->objmap = s.objmap;
    s.objmap = NULL;
    vm->free_list = s.free_list;
    vm->heap_used = s.heap_used;
    for (int i = 0; i < vm->nclasses; i++)
        vm->classes[i].statics = s.statics[i];
    memset(vm->stack, 0, sizeof vm->stack);
    memcpy(vm->stack, s.stack, (size_t)s.stack_words * 4);
    vm->fp = -1;
    vm->nroots = 0;
    vm->pending = 0;
    vm->native_top = NULL;
    vm->in_native = 0;
    vm->native_exc = 0;
    vm->last_ret_words = 0;
    vm->halted = s.halted;
    vm->error_after_halt = 0;
    memcpy(vm->halt_reason, s.reason, sizeof vm->halt_reason);
    vm->math.s0 = s.s0;
    vm->math.s1 = s.s1;
    vm->math.period = s.period;
    vm->ext.sms_sent = s.sms_sent;
    vm->ext.sms_status = s.sms_status;
    vm->ext.delivering = 0;
    uint32_t host_now = vm->host.ticks_ms ? vm->host.ticks_ms(vm->host.user) : 0;
    vm->clock_offset = s.ticks - host_now;
    p->normal_video = s.normal_video;
    p->timer_ms = s.timer_ms;
    p->lut_valid = s.lut_valid;
    p->lut_version = s.lut_version;
    memcpy(p->lut, s.lut, sizeof p->lut);
    memcpy(p->fb, s.fb, (size_t)p->width * (size_t)p->height);
    memcpy(p->eeprom, s.eeprom, EXEN_EEPROM_SIZE);
    commit_melody(&p->melody, &s);
    staged_free(&s);

    /* bring the host in line */
    if (vm->host.set_timer)
        vm->host.set_timer(vm->host.user, p->timer_ms);
    if (vm->host.present)
        vm->host.present(vm->host.user, p->fb, p->width, p->height);
    if (vm->host.eeprom_changed)
        vm->host.eeprom_changed(vm->host.user);
    return 0;
}
