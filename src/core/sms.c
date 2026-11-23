/* exen.Sms (89-100), the Gamelet SMS/nickname natives (78, 84, 85) and catalog.* (178-184).
 *
 * Sources: docs/re/natives/sms.c.txt and docs/re/NATIVES.md §2.2/§2.4, re-checked against
 * gensimu.exe (bit I/O 0x412ea0/0x413071, header 0x4215fa, natives 0x4298c8..0x429f3d,
 * sendSms 0x4250c7, nickname 0x4252ec/0x425372, catalog 0x4240a0..0x4243c0).
 *
 * The "config" these natives read (`[0x45fe8c]`) is the 2048-byte EEPROM image itself
 * (flash/eeprom.dat): the price table at +0x1a8 and the currency at +0x168 line up with the
 * shipped file. We read it from vm->plat->eeprom.
 *
 * Deviations, all because we have no network, no SIM and no catalog (docs/re/NATIVES.md §2.4):
 * - sendSms does not end the VM (the simulator stops the timer and the melody and exits with
 *   state 3 when the network check fails). The SMS goes to the mock In-Fusio server
 *   (smsserver.c, docs/re/SMS.md), which decides the Bootstrap.smsSent status from the host's
 *   SMS mode and may answer later through Bootstrap.smsReceived. The status is queued and
 *   delivered by the core after the current host callback returns (call_bootstrap in api.c),
 *   never re-entrantly. The platform's finishing touches (container length, statistics block
 *   0x41, end byte, checksum: 0x4218d3) only matter on the wire and are not applied.
 * - The SMS header's MCC/MNC are 0 and the language id is 1 (English).
 * - setNickName and Catalog.doEditBox open a modal edit box in the original (a busy-waiting
 *   PeekMessage loop). We do not block: they leave the stored value as it is and log once.
 * - doesGameExist, launchGameIfPresent and downloadGame see an empty catalog: false / nothing.
 */
#include <stdint.h>
#include <string.h>

#include "extnatives.h"

/* exen.Sms instance slots (tools/cvp.py dump) */
enum { SMS_SIZE, SMS_IDGAME, SMS_BUF, SMS_POS, SMS_TAG, SMS_LENGTH, SMS_OFFSET, SMS_TYPE };

#define GL_SMS_SENT_UNSUCCESSFULLY 1 /* exen.Gamelet static slots (tools/cvp.py dump) */
#define GL_SMS_SENT_SUCCESSFULLY 2

enum { WARN_NICK, WARN_EDITBOX, WARN_CATALOG };

/* ------------------------------------------------------------ native SMS buffer */

/* The original's native buffer is the data of Sms.java_exsms, a byte[142], used as 0x94 bytes:
 * [0..3] magic, [4..0x8f] the 140-byte payload (bit-addressed MSB first), [0x90..0x93] the u32
 * bit position. The last 6 bytes lie past the array's end; in our heap they fall in the
 * block's padding (a byte[142] occupies a 160-byte block), so we keep them there, as the
 * original does in its heap, after checking that the block really is that large. */
#define SMSBUF_SIZE 0x94u
#define SMS_POSOFF 0x90u

typedef struct {
    uint8_t *b; /* the 0x94 bytes */
} SmsBuf;

static int sms_buf(ExenVM *vm, ref_t sms, SmsBuf *s)
{
    int c = ext_class(vm, EC_SMS);
    if (!sms || c < 0)
        return 0;
    ref_t arr = inst(vm, sms, c)[SMS_BUF];
    if (!arr || !heap_is_object(vm, arr) || !is_array(vm, arr))
        return 0;
    uint32_t block = rd32(P(vm, arr)) & HDR_SIZE_MASK;
    if (12u + SMSBUF_SIZE > block)
        return 0;
    s->b = arr_data(vm, arr);
    return 1;
}

static uint32_t pos_get(const SmsBuf *s) { return rd32(s->b + SMS_POSOFF); }
static void pos_set(SmsBuf *s, uint32_t v) { wr32(s->b + SMS_POSOFF, v); }

/* Payload byte access. The payload is P = buf + 4; the bit routines address it without bounds,
 * so a position past the end reaches the position word and beyond. We allow the 0x94-byte
 * window and drop the rest. */
static uint8_t pb(const SmsBuf *s, uint32_t i) { return 4u + i < SMSBUF_SIZE ? s->b[4 + i] : 0; }
static void pbw(SmsBuf *s, uint32_t i, uint8_t v)
{
    if (4u + i < SMSBUF_SIZE)
        s->b[4 + i] = v;
}

static uint32_t shl(uint32_t v, uint32_t n) { return v << (n & 31); } /* x86 shift counts */
static uint32_t shr(uint32_t v, uint32_t n) { return v >> (n & 31); }
static uint32_t be32(const SmsBuf *s, uint32_t i)
{
    return (uint32_t)pb(s, i) << 24 | (uint32_t)pb(s, i + 1) << 16 | (uint32_t)pb(s, i + 2) << 8 |
           pb(s, i + 3);
}
static void wbe32(SmsBuf *s, uint32_t i, uint32_t w)
{
    pbw(s, i, (uint8_t)(w >> 24));
    pbw(s, i + 1, (uint8_t)(w >> 16));
    pbw(s, i + 2, (uint8_t)(w >> 8));
    pbw(s, i + 3, (uint8_t)w);
}

/* putbits 0x413071: v is NOT masked to n bits, so its high bits OR into the bits before it. */
static void putbits(SmsBuf *s, uint32_t v, uint32_t n)
{
    uint32_t pos = pos_get(s), bit = pos & 7, i = pos >> 3;
    uint32_t mask = shl(1, n) - 1;
    if (n + bit <= 8) {
        uint32_t sh = 8 - bit - n;
        uint32_t w = pb(s, i);
        w = (w & ~shl(mask, sh)) | shl(v, sh);
        pbw(s, i, (uint8_t)w);
    } else if (n + bit <= 16) {
        uint32_t sh = 16 - bit - n;
        uint32_t w = (uint32_t)pb(s, i) << 8 | pb(s, i + 1);
        w = (w & ~shl(mask, sh)) | shl(v, sh);
        pbw(s, i + 1, (uint8_t)w);
        pbw(s, i, (uint8_t)(w >> 8));
    } else if (n + bit <= 32) {
        uint32_t sh = 32 - bit - n;
        uint32_t w = be32(s, i);
        w = (w & ~shl(mask, sh)) | shl(v, sh);
        wbe32(s, i, w);
    } else {
        uint32_t e = bit + n - 32;
        uint32_t w = be32(s, i);
        w = (w & ~shr(mask, e)) | shr(v, e);
        wbe32(s, i, w);
        i += 4;
        w = be32(s, i);
        uint32_t m2 = shl(1, e) - 1, sh = 32 - e;
        w = (w & ~shl(m2, sh)) | shl(v, sh);
        wbe32(s, i, w);
    }
    pos_set(s, pos + n);
}

/* getbits 0x412ea0, including its odd 5-byte case */
static uint32_t getbits(SmsBuf *s, uint32_t n)
{
    uint32_t pos = pos_get(s), bit = pos & 7, i = pos >> 3;
    uint32_t mask = shl(1, n) - 1;
    pos_set(s, pos + n);
    if (n + bit <= 8)
        return shr(pb(s, i), 8 - bit - n) & mask;
    if (n + bit <= 16)
        return shr((uint32_t)pb(s, i) << 8 | pb(s, i + 1), 16 - bit - n) & mask;
    if (n + bit <= 32)
        return shr(be32(s, i), 32 - bit - n) & mask;
    uint32_t e = bit + n - 32;
    uint32_t r = shl(be32(s, i), e) & mask;
    uint32_t w2 = be32(s, i + 4);
    uint32_t m2 = shl(1, e) - 1;
    uint32_t t = shr(w2, 32 - e) & ~m2;
    return shl(r, 2) | t;
}

static void put11(SmsBuf *s, uint32_t v) /* 0x421b36 */
{
    putbits(s, v >> 8, 3);
    putbits(s, v, 8);
}
static uint32_t get11(SmsBuf *s) /* 0x421b7c */
{
    uint32_t h = getbits(s, 3) << 8;
    return h | getbits(s, 8);
}

static uint32_t *sms_fields(ExenVM *vm, ref_t sms) { return inst(vm, sms, ext_class(vm, EC_SMS)); }

static void reset_cursor(ExenVM *vm, ref_t sms) /* 0x429870 */
{
    uint32_t *f = sms_fields(vm, sms);
    f[SMS_POS] = f[SMS_TAG] = f[SMS_LENGTH] = f[SMS_OFFSET] = 0;
}

static const uint8_t *cfg(ExenVM *vm) { return vm->plat->eeprom; }
static int32_t cfg_s8(ExenVM *vm, uint32_t off) { return (int8_t)cfg(vm)[off]; }

/* ------------------------------------------------------------ exen.Sms */

static int n_deleteSms(ExenVM *vm, uint32_t *a) { (void)vm; (void)a; return 0; }

static int n_createSms(ExenVM *vm, uint32_t *a) /* 0x4298c8 -> header 0x4215fa */
{
    SmsBuf s;
    if (!sms_buf(vm, a[0], &s)) {
        vm_log(vm, "Sms.createSms: SMS NOT SENT null pointer");
        return 0;
    }
    pos_set(&s, 0);
    memset(s.b, 0, SMSBUF_SIZE);
    memcpy(s.b, "EXEN", 4);
    putbits(&s, (uint32_t)cfg_s8(vm, 5), 8);
    putbits(&s, (uint32_t)cfg_s8(vm, 7), 8);
    putbits(&s, 0, 8); /* 16-bit gamelet record id (0x4219c5); no record: 0 */
    putbits(&s, 0, 8);
    for (uint32_t i = 0; i < 4; i++)
        putbits(&s, (uint32_t)cfg_s8(vm, 0x1e + i), 8);
    uint16_t mcc = 0; /* EXManufGetMccMnc: no SIM card */
    uint8_t mnc = 0;
    mcc = (uint16_t)((((mcc & 0x0f00u) << 4) | ((mcc & 0xf000u) >> 4) | ((mcc & 0x000fu) << 4) |
                      ((mcc & 0x00f0u) >> 4)) >> 4);
    mnc = (uint8_t)(((mnc & 0x0fu) << 4) | ((mnc & 0xf0u) >> 4));
    putbits(&s, mcc, 12);
    putbits(&s, mnc, 8);
    putbits(&s, 1, 8); /* EXManufGetLanguageId: English */
    putbits(&s, rd16(cfg(vm) + 0x164), 16);
    pos_set(&s, pos_get(&s) + 20);
    putbits(&s, 0x60, 8); /* 0x421982 */
    put11(&s, 0);
    reset_cursor(vm, a[0]);
    return 0;
}

static int n_createSmsBytes(ExenVM *vm, uint32_t *a) /* 0x42993b: a received SMS */
{
    SmsBuf s;
    if (!sms_buf(vm, a[0], &s) || !a[1]) {
        vm_log(vm, "Sms.createSms: SMS NOT SENT null pointer");
        return 0;
    }
    uint32_t n = arr_len(vm, a[1]);
    const uint8_t *in = arr_data(vm, a[1]);
    pos_set(&s, 0);
    for (uint32_t i = 0; i < 0x88; i++) /* the input length is not checked */
        s.b[4 + i] = i < n ? in[i] : 0;
    memset(s.b, 0, 4);
    pos_set(&s, 0);
    reset_cursor(vm, a[0]);
    return 0;
}

static int n_createBlock(ExenVM *vm, uint32_t *a) /* 0x429a20 */
{
    SmsBuf s;
    if (!sms_buf(vm, a[0], &s) || pos_get(&s) > 0x42d)
        return 0;
    uint32_t *f = sms_fields(vm, a[0]);
    int32_t id = (int8_t)a[1];
    f[SMS_POS] = pos_get(&s);
    f[SMS_TAG] = (uint32_t)id;
    f[SMS_LENGTH] = 0;
    f[SMS_OFFSET] = 0;
    putbits(&s, (uint32_t)id, 8);
    pos_set(&s, pos_get(&s) + 11); /* length placeholder */
    return 0;
}

static int n_readBits(ExenVM *vm, uint32_t *a) /* 0x429ac0: no limit check */
{
    SmsBuf s;
    if (!sms_buf(vm, a[0], &s)) {
        a[0] = 0; /* the original returns no result word here, which unbalances the stack */
        return 1;
    }
    a[0] = getbits(&s, a[1]);
    return 1;
}

static int n_writeBits(ExenVM *vm, uint32_t *a) /* 0x429b0a */
{
    SmsBuf s;
    int32_t n = (int8_t)a[2];
    if (!sms_buf(vm, a[0], &s) || pos_get(&s) > 0x42d)
        return 0;
    if (n > 7)
        n = 8;
    putbits(&s, (uint32_t)(int8_t)a[1] & 0xffu, (uint32_t)n);
    return 0;
}

static int n_endBlock(ExenVM *vm, uint32_t *a) /* 0x429b8d */
{
    SmsBuf s;
    if (!sms_buf(vm, a[0], &s) || pos_get(&s) > 0x42d) {
        a[0] = 0;
        return 1;
    }
    uint32_t *f = sms_fields(vm, a[0]);
    int16_t len = (int16_t)(pos_get(&s) - (f[SMS_POS] + 0x13));
    uint16_t save = (uint16_t)pos_get(&s);
    pos_set(&s, (uint32_t)(int32_t)(int16_t)(f[SMS_POS] + 8));
    put11(&s, (uint32_t)(int32_t)len);
    pos_set(&s, (uint32_t)(int32_t)(int16_t)save);
    a[0] = (uint32_t)(int32_t)len;
    return 1;
}

static int n_nextBlock(ExenVM *vm, uint32_t *a) /* 0x429c43 */
{
    SmsBuf s;
    if (!sms_buf(vm, a[0], &s) || pos_get(&s) > 0x42d)
        return 0;
    int16_t len = (int16_t)get11(&s);
    pos_set(&s, (uint32_t)(int32_t)(int16_t)(pos_get(&s) + (uint32_t)(int32_t)len));
    reset_cursor(vm, a[0]);
    return 0;
}

static int n_getIdBlock(ExenVM *vm, uint32_t *a) /* 0x429cb0 */
{
    SmsBuf s;
    if (!sms_buf(vm, a[0], &s)) {
        a[0] = 0;
        return 1;
    }
    if (pos_get(&s) >= 0x460) {
        a[0] = (uint32_t)-1;
        return 1;
    }
    uint32_t *f = sms_fields(vm, a[0]);
    f[SMS_POS] = pos_get(&s);
    f[SMS_TAG] = getbits(&s, 8);
    a[0] = f[SMS_TAG];
    return 1;
}

static int n_getLengthBlock(ExenVM *vm, uint32_t *a) /* 0x429d2a */
{
    SmsBuf s;
    if (!sms_buf(vm, a[0], &s) || pos_get(&s) > 0x42d) {
        a[0] = 0;
        return 1;
    }
    uint32_t *f = sms_fields(vm, a[0]);
    f[SMS_LENGTH] = get11(&s);
    a[0] = f[SMS_LENGTH];
    return 1;
}

static int n_skipBits(ExenVM *vm, uint32_t *a) /* 0x429d86 */
{
    SmsBuf s;
    if (!sms_buf(vm, a[0], &s))
        return 0;
    uint32_t np = pos_get(&s) + a[1];
    if (np > 0x460)
        return 0;
    pos_set(&s, (uint32_t)(int32_t)(int16_t)np);
    uint32_t *f = sms_fields(vm, a[0]);
    f[SMS_OFFSET] = (uint32_t)(int32_t)(int16_t)(f[SMS_OFFSET] + a[1]);
    return 0;
}

/* itoa 0x422bc8: at most width-1 digits, right to left, then shifted left; 0xff-terminated.
 * (So the 2-digit cents field keeps only its last digit: 0.11 prints as "0.1".) */
static int itoa_w(int32_t v, uint32_t width, uint8_t *buf, size_t cap)
{
    if (v == 0) {
        buf[0] = '0';
        buf[1] = 0xff;
        return 1;
    }
    int neg = 0;
    if (v < 0) {
        neg = 1;
        if (v == INT32_MIN)
            return -5;
        v = -v;
    }
    if (width >= cap)
        return -5;
    buf[width] = 0xff;
    width--;
    while (v > 0 && width != 0) {
        buf[width] = (uint8_t)('0' + v % 10);
        v /= 10;
        width--;
    }
    if (neg) {
        buf[width] = '-';
        width--;
    }
    width++;
    if (width != 0 && width < cap) {
        uint32_t i = 0;
        while (i + width < cap && buf[i + width] != 0xff) {
            buf[i] = buf[i + width];
            buf[i + width] = 0xff;
            i++;
        }
        buf[i] = 0xff;
    }
    return 1;
}

static uint32_t ff_len(const uint8_t *s, uint32_t max) /* 0x422f34, bounded */
{
    uint32_t n = 0;
    while (n < max && s[n] != 0xff)
        n++;
    return n;
}

static int n_getPrice(ExenVM *vm, uint32_t *a) /* 0x429e0a, price format 0x422e57 */
{
    int32_t idx = (int32_t)sms_fields(vm, a[0])[SMS_TYPE];
    int64_t off = 0x1a8 + 0x1c * (int64_t)idx;
    int32_t v = off >= 0 && off + 4 <= 2048 ? (int32_t)rd32(cfg(vm) + off) : 0;
    uint8_t buf[0x4c + 0x40];
    memset(buf, 0xff, sizeof buf);
    int32_t ip = v >> 16;
    int32_t cents = (int32_t)(((int64_t)(v & 0xffff) * 100) >> 16);
    itoa_w(ip, 4, buf, 0x20);
    uint32_t n = ff_len(buf, 0x20);
    buf[n++] = '.';
    if (cents < 10)
        buf[n++] = '0';
    itoa_w(cents, 2, buf + n, 0x10);
    n = ff_len(buf, 0x30);
    buf[n++] = ' ';
    buf[n] = 0xff;
    /* currency string at cfg+0x168, 0xff-terminated; we also stop at 0 (an all-zero EEPROM)
     * and at the end of its 0x40-byte field */
    uint32_t cl = 0;
    while (cl < 0x40 && cfg(vm)[0x168 + cl] != 0xff && cfg(vm)[0x168 + cl] != 0)
        cl++;
    memcpy(buf + n, cfg(vm) + 0x168, cl);
    n += cl;
    ref_t str = vm_new_string(vm, buf, (int)n);
    a[0] = str; /* null if the allocation failed, as in the original */
    return 1;
}

/* ------------------------------------------------------------ Gamelet */

/* sendSms(Sms) 0x4250c7. The original finalises the message and sends it to the In-Fusio
 * server; the result comes back later as Bootstrap.smsSent(status). We hand it to the mock
 * server (smsserver.c), which says whether the send succeeds (per the host's SMS mode) and may
 * queue an answer. The status is queued here and delivered by the core after the current host
 * callback returns (call_bootstrap in api.c), never re-entrantly. */
static int n_sendSms(ExenVM *vm, uint32_t *a)
{
    SmsBuf s;
    if (!a[1] || !sms_buf(vm, a[1], &s))
        return 0; /* the original does nothing without a buffer */
    /* the price index goes into the buffer's last payload word before the send */
    wr32(s.b + 0x8c, sms_fields(vm, a[1])[SMS_TYPE]);
    int ok = smsv_send(vm, s.b);
    int cg = plat_class(vm, PC_GAMELET);
    /* Gamelet.<clinit>: SMS_SENT_UNSUCCESSFULLY (static slot 1) = 0, SUCCESSFULLY (2) = 1 */
    int slot = ok ? GL_SMS_SENT_SUCCESSFULLY : GL_SMS_SENT_UNSUCCESSFULLY;
    int32_t status = ok;
    if (cg >= 0 && vm->classes[cg].statics && vm->classes[cg].nstatics > slot)
        status = (int32_t)obj_slots(vm, vm->classes[cg].statics)[slot];
    vm->ext.sms_sent = 1;
    vm->ext.sms_status = status;
    return 0;
}

/* The nickname is a 16-byte, 0xff-terminated field of the EEPROM config at +0x66 (0x423b63).
 * We also stop at a 0 byte so that an all-zero EEPROM gives an empty name. */
static int n_getNickName(ExenVM *vm, uint32_t *a)
{
    const uint8_t *nick = cfg(vm) + 0x66;
    int n = 0;
    while (n < 16 && nick[n] != 0xff && nick[n] != 0)
        n++;
    ref_t arr = new_char_array(vm, nick, n);
    if (!arr)
        vm_throw_hash(vm, H_OOM);
    a[0] = arr;
    return 1;
}

static int n_setNickName(ExenVM *vm, uint32_t *a)
{
    (void)a;
    ext_warn_once(vm, WARN_NICK, "Gamelet.setNickName: modal edit box not supported, nickname kept");
    return 0;
}

/* ------------------------------------------------------------ catalog */

static int n_doesGameExist(ExenVM *vm, uint32_t *a) /* 0x4240a0: no catalog table */
{
    (void)vm;
    a[0] = 0;
    return 1;
}

static int n_launchGameIfPresent(ExenVM *vm, uint32_t *a) /* 0x4240e1 */
{
    ext_warn_once(vm, WARN_CATALOG, "catalog: no catalog, no game to launch or download");
    a[0] = 0;
    return 1;
}

static int n_isUserRegistred(ExenVM *vm, uint32_t *a) /* 0x423b7f */
{
    const uint8_t *c = cfg(vm);
    a[0] = (c[0x1e] | c[0x1f] | c[0x20] | c[0x21]) != 0;
    return 1;
}

static int n_downloadGame(ExenVM *vm, uint32_t *a) /* 0x424194 */
{
    (void)a;
    ext_warn_once(vm, WARN_CATALOG, "catalog: no catalog, no game to launch or download");
    return 0;
}

static int n_doEditBox(ExenVM *vm, uint32_t *a) /* 0x4242b2 */
{
    (void)a;
    ext_warn_once(vm, WARN_EDITBOX, "Catalog.doEditBox: modal edit box not supported");
    return 0;
}

static int n_atoi(ExenVM *vm, uint32_t *a) /* 0x424375 -> 0x422ad2 */
{
    if (!a[1]) {
        vm_throw_hash(vm, H_NPE);
        a[0] = 0;
        return 1;
    }
    const uint8_t *p;
    int32_t len = vm_string_get(vm, a[1], &p);
    int32_t acc = -1, neg = 0;
    if (!p) {
        a[0] = (uint32_t)-1;
        return 1;
    }
    int32_t i = 0;
#define CH(k) ((k) < (int32_t)rd16(p - 2) ? (int8_t)p[k] : 0)
    while (CH(i) == ' ') {
        i++;
        if (--len <= 0) {
            a[0] = (uint32_t)-1;
            return 1;
        }
    }
    if (CH(i) == '-') {
        neg = 1;
        i++;
        if (--len <= 0) {
            a[0] = (uint32_t)-1;
            return 1;
        }
    }
    while (CH(i) >= '0' && CH(i) <= '9' && len > 0) {
        if (acc == -1)
            acc = 0;
        acc = (int32_t)((uint32_t)acc * 10u + (uint32_t)(CH(i) - '0'));
        i++;
        len--;
    }
#undef CH
    if (acc == -1)
        a[0] = (uint32_t)-1;
    else
        a[0] = (uint32_t)(neg ? (int32_t)(0u - (uint32_t)acc) : acc);
    return 1;
}

static int n_joke(ExenVM *vm, uint32_t *a) { (void)vm; (void)a; return 0; }

void sms_register(void)
{
    natives_register(89, n_deleteSms);
    natives_register(90, n_createSms);
    natives_register(91, n_createSmsBytes);
    natives_register(92, n_createBlock);
    natives_register(93, n_readBits);
    natives_register(94, n_writeBits);
    natives_register(95, n_endBlock);
    natives_register(96, n_nextBlock);
    natives_register(97, n_getIdBlock);
    natives_register(98, n_getLengthBlock);
    natives_register(99, n_skipBits);
    natives_register(100, n_getPrice);
    natives_register(78, n_sendSms);
    natives_register(84, n_getNickName);
    natives_register(85, n_setNickName);
    natives_register(178, n_doesGameExist);
    natives_register(179, n_launchGameIfPresent);
    natives_register(180, n_isUserRegistred);
    natives_register(181, n_downloadGame);
    natives_register(182, n_doEditBox);
    natives_register(183, n_atoi);
    natives_register(184, n_joke);
}
