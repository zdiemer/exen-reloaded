/* exen.Resource natives (30-42). docs/re/NATIVES.md §2.3 and docs/re/natives/Resource.c.txt.
 *
 * The original reads every value with fseek+fread on the open .exn; we serve the same bytes from
 * the game image the VM keeps in memory (vm->exn). Reads use the absolute file offsets from the
 * .exn resource table, so a read that runs past a resource (readBytes on a packed resource,
 * readUTF/readString) sees the following bytes of the file, as in the original.
 *
 * Quirks kept: little-endian scalars but a big-endian readUTF length; readChar reads one byte
 * and writes only the low 16 bits of the result word (the high half keeps the receiver ref's
 * bits), as does readShort on failure; array lengths are truncated to u16 and string lengths to
 * u8; init() returns null and does not set _id; readString does not advance the cursor.
 */
#include <string.h>

#include "codec.h"
#include "platform.h"

enum { R_START, R_LEN, R_OFFSET, R_ID, R_ARRAY };

static uint32_t res_count(ExenVM *vm)
{
    if (!vm->exn || vm->exn_len < 0x38)
        return 0;
    uint32_t n = rd32(vm->exn + 0x34);
    if (0x38 + 4ull * (n + 1) + n > vm->exn_len)
        return 0;
    return n;
}

static uint32_t res_offset(ExenVM *vm, uint32_t i) { return rd32(vm->exn + 0x38 + 4 * i); }
static uint8_t res_type(ExenVM *vm, uint32_t i)
{
    uint32_t n = res_count(vm);
    return vm->exn[0x38 + 4 * (n + 1) + i];
}

static uint32_t *rf(ExenVM *vm, ref_t r) { return inst(vm, r, plat_class(vm, PC_RESOURCE)); }

/* Read n bytes at the cursor (no bounds check against _len: callers do that). */
static const uint8_t *at_cursor(ExenVM *vm, ref_t r, uint32_t n)
{
    const uint32_t *f = rf(vm, r);
    uint64_t pos = (uint64_t)f[R_START] + f[R_OFFSET];
    if (pos + n > vm->exn_len)
        return NULL;
    return vm->exn + pos;
}

/* The scalar reads: 0 if _offset + n > _len, else the bytes at the cursor, cursor += n. */
static const uint8_t *scalar(ExenVM *vm, ref_t r, uint32_t n)
{
    if (!r || plat_class(vm, PC_RESOURCE) < 0)
        return NULL;
    uint32_t *f = rf(vm, r);
    if ((int64_t)(int32_t)f[R_OFFSET] + n > (int64_t)(int32_t)f[R_LEN])
        return NULL;
    const uint8_t *p = at_cursor(vm, r, n);
    if (p)
        f[R_OFFSET] += n;
    return p;
}

static int n_init(ExenVM *vm, uint32_t *a)
{
    int32_t id = (int32_t)a[1];
    uint32_t *f = rf(vm, a[0]);
    f[R_START] = f[R_LEN] = f[R_OFFSET] = 0;
    uint32_t n = res_count(vm);
    if (id >= 0 && (uint32_t)id < n) {
        f[R_START] = res_offset(vm, (uint32_t)id);
        f[R_LEN] = res_offset(vm, (uint32_t)id + 1) - res_offset(vm, (uint32_t)id);
    } else {
        vm_log(vm, "Resource init : ID out of range !! (%d)", id);
        vm_halt(vm, "Resource.init: id %d out of range (non catchable interrupt 0)", id);
    }
    a[0] = 0;
    return 1;
}

static int n_readBoolean(ExenVM *vm, uint32_t *a)
{
    const uint8_t *p = scalar(vm, a[0], 1);
    a[0] = p ? (uint32_t)(p[0] & 1) : 0;
    return 1;
}

static int n_readInt(ExenVM *vm, uint32_t *a)
{
    const uint8_t *p = scalar(vm, a[0], 4);
    a[0] = p ? rd32(p) : 0;
    return 1;
}

static int n_readShort(ExenVM *vm, uint32_t *a)
{
    const uint8_t *p = scalar(vm, a[0], 2);
    if (p)
        a[0] = (uint32_t)(int32_t)(int16_t)rd16(p);
    else
        a[0] &= 0xffff0000u; /* mov word [args], 0 */
    return 1;
}

static int n_readByte(ExenVM *vm, uint32_t *a)
{
    const uint8_t *p = scalar(vm, a[0], 1);
    a[0] = p ? (uint32_t)(int32_t)(int8_t)p[0] : 0;
    return 1;
}

static int n_readChar(ExenVM *vm, uint32_t *a)
{
    const uint8_t *p = scalar(vm, a[0], 1);
    a[0] = (a[0] & 0xffff0000u) | (p ? p[0] : 0); /* only the low 16 bits are written */
    return 1;
}

static int n_readBytes(ExenVM *vm, uint32_t *a)
{
    int32_t n = (int32_t)a[1];
    ref_t r = a[0];
    if (n <= 0 || !r) {
        a[0] = 0;
        return 1;
    }
    ref_t arr = vm_new_array(vm, T_BYTE, 0, (uint16_t)n);
    if (!arr) {
        vm_log(vm, "[Not Enough Memory] - Resource.readBytes");
        vm_halt(vm, "Resource.readBytes: out of memory (non catchable interrupt 0)");
        a[0] = 0;
        return 1;
    }
    uint32_t *f = rf(vm, r);
    uint16_t alen = arr_len(vm, arr);
    uint32_t id = f[R_ID], cnt = res_count(vm);
    if (id < cnt && res_type(vm, id) == 0x81) {
        int32_t remain = (int32_t)f[R_LEN] - (int32_t)f[R_OFFSET];
        const uint8_t *src = remain > 0 ? at_cursor(vm, r, (uint32_t)remain) : NULL;
        if (!src) {
            a[0] = 0;
            return 1;
        }
        size_t got = 0;
        int rc = exen_unpack(src, (size_t)remain, arr_data(vm, arr), alen, &got);
        if (rc != EXEN_CODEC_OK)
            plat_warn_once(vm, 8, "Resource.readBytes: unpack of resource %u failed (%d)", id, rc);
    } else {
        if ((int64_t)(int32_t)f[R_OFFSET] + n > (int64_t)(int32_t)f[R_LEN]) {
            a[0] = 0;
            return 1;
        }
        const uint8_t *src = at_cursor(vm, r, alen);
        if (!src) {
            a[0] = 0;
            return 1;
        }
        memcpy(arr_data(vm, arr), src, alen);
    }
    f[R_OFFSET] += (uint32_t)n; /* by n (the unpacked size) even for packed resources */
    a[0] = arr;
    return 1;
}

static int read_array(ExenVM *vm, uint32_t *a, uint16_t tw, uint32_t esz)
{
    int32_t n = (int32_t)a[1];
    ref_t r = a[0];
    a[0] = 0;
    if (!r || n <= 0)
        return 1;
    uint32_t *f = rf(vm, r);
    int32_t remain = (int32_t)f[R_LEN] - (int32_t)f[R_OFFSET];
    if (remain < 0 || n > remain / (int32_t)esz)
        return 1;
    ref_t arr = vm_new_array(vm, tw, 0, (uint16_t)n);
    if (!arr)
        return 1;
    uint16_t alen = arr_len(vm, arr);
    const uint8_t *src = at_cursor(vm, r, alen * esz);
    if (!src)
        return 1;
    memcpy(arr_data(vm, arr), src, (size_t)alen * esz);
    f = rf(vm, r);
    f[R_OFFSET] += (uint32_t)n * esz;
    a[0] = arr;
    return 1;
}

static int n_readShorts(ExenVM *vm, uint32_t *a) { return read_array(vm, a, T_SHORT, 2); }
static int n_readInts(ExenVM *vm, uint32_t *a) { return read_array(vm, a, T_INT, 4); }

static int n_readUTF(ExenVM *vm, uint32_t *a)
{
    ref_t r = a[0];
    a[0] = 0;
    if (!r)
        return 1;
    uint32_t *f = rf(vm, r);
    int32_t remain = (int32_t)f[R_LEN] - (int32_t)f[R_OFFSET];
    if (remain <= 0)
        return 1;
    const uint8_t *buf = at_cursor(vm, r, (uint32_t)remain);
    if (!buf || remain < 2)
        return 1;
    int32_t blen = buf[0] << 8 | buf[1];
    if (blen == 0 || (int64_t)(int32_t)f[R_OFFSET] + blen + 2 > (int64_t)(int32_t)f[R_LEN])
        return 1;
    const uint8_t *s = buf + 2;
    /* count, then decode modified UTF-8 to 8-bit chars */
    int nchars = 0;
    for (int32_t i = 0; i < blen;) {
        uint8_t b = s[i];
        if (b == 0) i += 2;
        else if (b < 0x7f) i += 1;
        else if ((b & 0xf0) == 0xe0) i += 3;
        else i += 2;
        nchars++;
    }
    uint8_t out[256];
    int k = 0, cap = (uint8_t)nchars; /* char count truncated to 8 bits */
    for (int32_t i = 0; i < blen && k < cap;) {
        uint8_t b = s[i], b1 = i + 1 < blen ? s[i + 1] : 0, b2 = i + 2 < blen ? s[i + 2] : 0;
        uint32_t c;
        if (b == 0) { c = 0; i += 2; }
        else if (b < 0x7f) { c = b; i += 1; }
        else if ((b & 0xf0) == 0xe0) { c = (uint32_t)(b & 0x0f) << 12 | (uint32_t)(b1 & 0x3f) << 6 | (b2 & 0x3f); i += 3; }
        else { c = (uint32_t)(b & 0x1f) << 6 | (b1 & 0x3f); i += 2; }
        out[k++] = (uint8_t)c;
    }
    ref_t str = vm_new_string(vm, out, k);
    if (!str)
        return 1;
    f = rf(vm, r);
    f[R_OFFSET] += (uint32_t)blen + 2;
    a[0] = str;
    return 1;
}

static int n_readString(ExenVM *vm, uint32_t *a)
{
    ref_t r = a[0];
    int32_t k = (int32_t)a[1];
    a[0] = 0;
    if (!r)
        return 1;
    uint32_t *f = rf(vm, r);
    int32_t remain = (int32_t)f[R_LEN] - (int32_t)f[R_OFFSET];
    if (remain <= 0)
        return 1;
    const uint8_t *buf = at_cursor(vm, r, (uint32_t)remain);
    if (!buf)
        return 1;
    int32_t p = 0;
    for (int32_t i = 0; i < k; i++) {
        while (p < remain && buf[p] != 0xff)
            p++;
        if (p >= remain)
            return 1; /* fewer than k separators */
        p++;
    }
    int32_t e = p;
    while (e < remain && buf[e] != 0xff)
        e++;
    uint8_t tmp[256];
    int n = (uint8_t)(e - p); /* length truncated to 8 bits */
    memcpy(tmp, buf + p, (size_t)n);
    a[0] = vm_new_string(vm, tmp, n);
    return 1;
}

static int n_getNbResources(ExenVM *vm, uint32_t *a)
{
    a[0] = res_count(vm);
    return 1;
}

static int n_getResourceType(ExenVM *vm, uint32_t *a)
{
    ref_t r = a[0];
    uint32_t id = r ? rf(vm, r)[R_ID] : 0xffffffffu;
    a[0] = id < res_count(vm) ? res_type(vm, id) : 0;
    return 1;
}

void resource_register(void)
{
    natives_register(30, n_init);
    natives_register(31, n_readBoolean);
    natives_register(32, n_readInt);
    natives_register(33, n_readShort);
    natives_register(34, n_readByte);
    natives_register(35, n_readChar);
    natives_register(36, n_readBytes);
    natives_register(37, n_readShorts);
    natives_register(38, n_readInts);
    natives_register(39, n_readUTF);
    natives_register(40, n_readString);
    natives_register(41, n_getNbResources);
    natives_register(42, n_getResourceType);
}
