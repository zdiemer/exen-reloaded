/* The mock In-Fusio SMS server: see smsserver.h and docs/re/SMS.md for the protocol.
 *
 * What the original did (gensimu.exe, docs/re/SMS.md §1): Gamelet.sendSms finalised the
 * message (0x4218d3: container length, platform statistics block 0x41, end byte 0xff, checksum)
 * and EXManufSmsSend (0x434efe) sent it over TCP to the [SmsServerConfig] PC tool, reporting
 * success (1) or failure (2) as event 0x101 -> Bootstrap.smsSent. The tool's answers arrived on
 * a listening socket (0x435396), were queued for 500 ms (0x43544d) and delivered as event 0x100:
 * the core checked the checksum, walked the top-level blocks and handed the contents of the
 * gamelet container block 0x60 (unpacked into a 0xff-filled 136-byte buffer) to the running
 * gamelet's onSmsReceived (0x41509c, 0x422640).
 *
 * The mock stands in for the PC tool and for that receive path: it answers with the container
 * contents directly, built the way the games' SMS library parses them, and the core delivers
 * them on the VM clock after SMSV_REPLY_DELAY_MS, never from inside a native.
 */
#include <stdio.h>
#include <string.h>

#include "natives.h"
#include "platform.h"
#include "smsserver.h"

/* ------------------------------------------------------------ bits */

uint32_t smsv_getbits(const uint8_t *p, size_t bytes, uint32_t pos, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n && i < 32; i++) {
        uint32_t b = pos + i, byte = b >> 3;
        uint32_t bit = byte < bytes ? (p[byte] >> (7 - (b & 7))) & 1u : 0;
        v = v << 1 | bit;
    }
    return v;
}

void smsv_putbits(uint8_t *p, size_t bytes, uint32_t pos, uint32_t n, uint32_t v)
{
    for (uint32_t i = 0; i < n && i < 32; i++) {
        uint32_t b = pos + i, byte = b >> 3;
        if (byte >= bytes)
            return;
        uint8_t m = (uint8_t)(0x80u >> (b & 7));
        if ((v >> (n - 1 - i)) & 1u)
            p[byte] |= m;
        else
            p[byte] = (uint8_t)(p[byte] & ~m);
    }
}

/* ------------------------------------------------------------ messages */

int smsv_decode(const uint8_t payload[SMSV_PAYLOAD], uint32_t end, SmsvMessage *m)
{
    memset(m, 0, sizeof *m);
    const size_t B = SMSV_PAYLOAD;
    m->ver_hi = (uint8_t)smsv_getbits(payload, B, 0, 8);
    m->ver_lo = (uint8_t)smsv_getbits(payload, B, 8, 8);
    m->gamelet = (uint16_t)smsv_getbits(payload, B, 16, 16);
    for (int i = 0; i < 4; i++)
        m->reg[i] = (uint8_t)smsv_getbits(payload, B, 32 + 8u * (uint32_t)i, 8);
    m->mcc = (uint16_t)smsv_getbits(payload, B, 64, 12);
    m->mnc = (uint8_t)smsv_getbits(payload, B, 76, 8);
    m->lang = (uint8_t)smsv_getbits(payload, B, 84, 8);
    m->cfg164 = (uint16_t)smsv_getbits(payload, B, 92, 16);
    m->end = end;
    if (end < SMSV_BLOCKS_BIT || end > SMSV_PAYLOAD * 8u)
        return -1;
    if (smsv_getbits(payload, B, SMSV_HEADER_BITS, 8) != SMSB_CONTAINER)
        return -1;
    uint32_t pos = SMSV_BLOCKS_BIT;
    while (pos + 19 <= end && m->nblocks < SMSV_MAX_BLOCKS) {
        uint32_t id = smsv_getbits(payload, B, pos, 8), len = smsv_getbits(payload, B, pos + 8, 11);
        if (pos + 19 + len > end)
            return -1;
        m->blocks[m->nblocks].id = (uint8_t)id;
        m->blocks[m->nblocks].pos = pos + 19;
        m->blocks[m->nblocks].len = len;
        m->nblocks++;
        pos += 19 + len;
    }
    return pos == end ? 0 : -1;
}

uint32_t smsv_put_block(uint8_t *out, size_t bytes, uint32_t pos, uint8_t id, const uint8_t *src,
                        uint32_t src_pos, uint32_t nbits)
{
    if (nbits > 0x7ff || pos + 19 + nbits > bytes * 8u)
        return 0;
    smsv_putbits(out, bytes, pos, 8, id);
    smsv_putbits(out, bytes, pos + 8, 11, nbits);
    for (uint32_t i = 0; i < nbits; i++)
        smsv_putbits(out, bytes, pos + 19 + i, 1, src ? smsv_getbits(src, bytes, src_pos + i, 1) : 0);
    return pos + 19 + nbits;
}

static const char *block_name(uint32_t id)
{
    switch (id) {
    case SMSB_MT_ERROR: return "MT_ERROR";
    case SMSB_MO_UNLOCK_ITEM: return "MO_UNLOCK_ITEM";
    case SMSB_MT_UNLOCK_ITEM: return "MT_UNLOCK_ITEM";
    case SMSB_MO_SCORE: return "MO_SCORE";
    case SMSB_MT_RANKING: return "MT_RANKING";
    case SMSB_MO_RANKING: return "MO_RANKING";
    case SMSB_MO_DOWNLOAD_ITEM: return "MO_DOWNLOAD_ITEM";
    case SMSB_MT_REQUESTED_ITEM: return "MT_REQUESTED_ITEM";
    case SMSB_MO_LOAD: return "MO_LOAD";
    case SMSB_MT_LOAD: return "MT_LOAD";
    case SMSB_MO_SAVE: return "MO_SAVE";
    case SMSB_MT_SAVE: return "MT_SAVE";
    case SMSB_MO_RESTORE: return "MO_RESTORE";
    case SMSB_MT_RESTORE: return "MT_RESTORE";
    case SMSB_MO_NICKNAME: return "MO_NICKNAME";
    case SMSB_MO_VOTE: return "MO_VOTE";
    case SMSB_MT_VOTES: return "MT_VOTES";
    case SMSB_MO_PAY: return "MO_PAY";
    case SMSB_MO_TIME_TRIAL: return "MO_TIME_TRIAL";
    default: return "block";
    }
}

/* ------------------------------------------------------------ server */

/* A nickname of 7-bit characters, ended by '\n' (the vote pads with it), 0 or 127. */
static uint8_t read_name(const uint8_t *p, uint32_t pos, uint32_t nchars, uint8_t *name)
{
    uint8_t n = 0;
    for (uint32_t i = 0; i < nchars && n < SMSV_NAME; i++) {
        uint8_t c = (uint8_t)smsv_getbits(p, SMSV_PAYLOAD, pos + 7 * i, 7);
        if (c == 10 || c == 0 || c == 127)
            break;
        name[n++] = c;
    }
    while (n && name[n - 1] == ' ')
        n--;
    return n;
}

/* `nchars` 7-bit characters, padded with `pad` */
static void put_name(uint8_t *out, uint32_t pos, const uint8_t *name, uint32_t len,
                     uint32_t nchars, uint8_t pad)
{
    for (uint32_t i = 0; i < nchars; i++)
        smsv_putbits(out, SMSV_PAYLOAD, pos + 7 * i, 7, i < len ? name[i] & 0x7fu : pad);
}

static void name_str(const uint8_t *name, uint32_t len, char *out)
{
    uint32_t i;
    for (i = 0; i < len && i < SMSV_NAME; i++)
        out[i] = name[i] >= 32 && name[i] < 127 && name[i] != '"' ? (char)name[i] : '?';
    out[i] = 0;
}

void smsv_reset(ExSmsServer *s)
{
    int mode = s->mode, verbose = s->verbose;
    int32_t y = s->year, mo = s->month, d = s->day;
    memset(s, 0, sizeof *s);
    s->mode = mode;
    s->verbose = verbose;
    s->year = y;
    s->month = mo;
    s->day = d;
}

static void queue(ExSmsServer *s, uint32_t now, const uint8_t *msg, uint32_t bits)
{
    memcpy(s->reply, msg, SMSV_PAYLOAD);
    /* the platform unpacks the container into a 0xff-filled buffer (0x415176): bytes after the
     * last block read as 0xff, the end marker of a block list */
    for (uint32_t i = (bits + 7) / 8; i < SMSV_PAYLOAD; i++)
        s->reply[i] = 0xff;
    s->pending = 1;
    s->due = now + SMSV_REPLY_DELAY_MS;
}

/* Inserts a score, best first (a tie goes before the older scores, as the games' own score
 * tables do). Returns the 1-based rank it takes, which may be past the stored entries. */
static uint32_t add_score(ExSmsServer *s, int32_t score, int32_t level, const uint8_t *name,
                          uint8_t name_len)
{
    uint32_t at = 0;
    while (at < s->nranks && s->ranks[at].score > score)
        at++;
    if (at < SMSV_RANKS) {
        uint32_t n = s->nranks < SMSV_RANKS ? s->nranks : SMSV_RANKS - 1;
        memmove(&s->ranks[at + 1], &s->ranks[at], (n - at) * sizeof s->ranks[0]);
        SmsvRank *r = &s->ranks[at];
        memset(r, 0, sizeof *r);
        r->score = score;
        r->level = level;
        memcpy(r->name, name, name_len);
        r->name_len = name_len;
        if (s->nranks < SMSV_RANKS)
            s->nranks++;
    }
    return at + 1;
}

/* MT_RANKING: the 993-bit record the games copy into their context (124 bytes + 1 bit) and
 * display with their score-table class. `rank` is the player's (0 = not ranked). */
static uint32_t build_ranking(const ExSmsServer *s, uint8_t *msg, uint32_t rank, int32_t level,
                              int32_t score)
{
    uint8_t b[SMSV_PAYLOAD];
    memset(b, 0, sizeof b);
    const size_t B = sizeof b;
    smsv_putbits(b, B, 4, 1, rank != 0);
    smsv_putbits(b, B, 5, 32, rank);
    smsv_putbits(b, B, 37, 14, rank ? (uint32_t)level & 0x3fffu : 0);
    smsv_putbits(b, B, 51, 32, rank ? (uint32_t)score : 0);
    put_name(b, 83, s->nick, s->nick_len, SMSV_NAME, 127);
    smsv_putbits(b, B, 188, 8, rank < 256 ? rank : 255);
    int32_t y = s->year ? s->year : 2004, mo = s->month ? s->month : 1, d = s->day ? s->day : 1;
    smsv_putbits(b, B, 196, 4, (uint32_t)mo);
    smsv_putbits(b, B, 200, 5, (uint32_t)d);
    smsv_putbits(b, B, 205, 12, (uint32_t)y);
    /* 217: the contest end date, 0 = open (the games show "********") */
    for (uint32_t i = 0; i < SMSV_RANKS; i++) {
        uint32_t p = 238 + 151 * i;
        const SmsvRank *r = i < s->nranks ? &s->ranks[i] : NULL;
        put_name(b, p, r ? r->name : NULL, r ? r->name_len : 0, SMSV_NAME, 127);
        smsv_putbits(b, B, p + 105, 14, r ? (uint32_t)r->level & 0x3fffu : 0);
        smsv_putbits(b, B, p + 119, 32, r ? (uint32_t)r->score : 0);
    }
    return smsv_put_block(msg, SMSV_PAYLOAD, 0, SMSB_MT_RANKING, b, 0, 993);
}

int smsv_handle(ExSmsServer *s, const uint8_t payload[SMSV_PAYLOAD], uint32_t end, uint32_t now,
                const char *title, char *log, size_t logcap)
{
    SmsvMessage m;
    size_t lp = 0;
#define LOG(...)                                                                              \
    do {                                                                                      \
        if (log && lp < logcap) {                                                             \
            int k_ = snprintf(log + lp, logcap - lp, __VA_ARGS__);                            \
            lp += k_ > 0 ? (size_t)k_ : 0;                                                    \
        }                                                                                     \
    } while (0)
    if (log && logcap)
        log[0] = 0;
    s->sent++;
    if (smsv_decode(payload, end, &m) < 0) {
        LOG("malformed SMS (%u bits), no reply", end);
        return -1;
    }
    uint8_t msg[SMSV_PAYLOAD];
    memset(msg, 0, sizeof msg);
    uint32_t bits = 0;
    const char *reply = NULL;
    char extra[96] = "";
    if (m.nblocks == 0)
        LOG("empty SMS");
    for (int i = 0; i < m.nblocks; i++) {
        uint32_t id = m.blocks[i].id, p = m.blocks[i].pos, len = m.blocks[i].len;
        char nm[SMSV_NAME + 1];
        LOG("%s%s(%u)", i ? " + " : "", block_name(id), id);
        if (reply)
            continue; /* one answer per SMS: the first request that has one */
        switch (id) {
        case SMSB_MO_SCORE: {
            if (len < 52)
                break;
            int32_t score = (int32_t)smsv_getbits(payload, SMSV_PAYLOAD, p, 32);
            int32_t level = (int16_t)smsv_getbits(payload, SMSV_PAYLOAD, p + 36, 16);
            uint8_t name[SMSV_NAME];
            uint8_t nl = read_name(payload, p + 52, (len - 52) / 7, name);
            memcpy(s->nick, name, nl);
            s->nick_len = nl;
            uint32_t rank = add_score(s, score, level, name, nl);
            name_str(name, nl, nm);
            LOG(" score=%d level=%d name=\"%s\"", (int)score, (int)level, nm);
            bits = build_ranking(s, msg, rank, level, score);
            reply = "MT_RANKING";
            if (rank <= s->nranks)
                snprintf(extra, sizeof extra, ", rank %u of %u", rank, s->nranks);
            else
                snprintf(extra, sizeof extra, ", rank %u", rank);
            break;
        }
        case SMSB_MO_RANKING: {
            const SmsvRank *best = s->nranks ? &s->ranks[0] : NULL;
            bits = build_ranking(s, msg, best ? 1 : 0, best ? best->level : 0,
                                 best ? best->score : 0);
            reply = "MT_RANKING";
            snprintf(extra, sizeof extra, best ? ", rank 1 of %u" : ", not ranked yet",
                     s->nranks);
            break;
        }
        case SMSB_MO_VOTE: {
            if (len < 3)
                break;
            uint32_t mark = smsv_getbits(payload, SMSV_PAYLOAD, p, 3);
            uint8_t name[SMSV_NAME];
            uint8_t nl = read_name(payload, p + 3, (len - 3) / 7, name);
            if (nl) {
                memcpy(s->nick, name, nl);
                s->nick_len = nl;
            }
            s->votes++;
            s->vote_sum += mark;
            uint32_t avg = (s->vote_sum + s->votes / 2) / s->votes;
            name_str(name, nl, nm);
            LOG(" mark=%u name=\"%s\"", mark, nm);
            /* MT_VOTES: average mark (3), votes (32), then 5 x {mark (3), game name (27 x 7)}.
             * The only game this server has votes for is this one. */
            uint8_t b[SMSV_PAYLOAD];
            memset(b, 0, sizeof b);
            smsv_putbits(b, sizeof b, 0, 3, avg > 7 ? 7 : avg);
            smsv_putbits(b, sizeof b, 3, 32, s->votes);
            for (uint32_t k = 0; k < SMSV_RANKS; k++) {
                uint32_t q = 35 + 192 * k;
                uint8_t t[27];
                uint32_t tl = 0;
                if (k == 0 && title)
                    for (; title[tl] && tl < sizeof t; tl++)
                        t[tl] = (uint8_t)title[tl] < 127 ? (uint8_t)title[tl] : '?';
                smsv_putbits(b, sizeof b, q, 3, k == 0 ? (avg > 7 ? 7 : avg) : 0);
                put_name(b, q + 3, t, tl, 27, ' ');
            }
            bits = smsv_put_block(msg, SMSV_PAYLOAD, 0, SMSB_MT_VOTES, b, 0, 1000);
            reply = "MT_VOTES";
            snprintf(extra, sizeof extra, ", average %u of %u vote%s", avg, s->votes,
                     s->votes == 1 ? "" : "s");
            break;
        }
        case SMSB_MO_UNLOCK_ITEM: {
            /* granted as asked: the same bitmap back */
            uint32_t ones = 0;
            for (uint32_t k = 0; k < len; k++)
                if (smsv_getbits(payload, SMSV_PAYLOAD, p + k, 1)) {
                    ones++;
                    if (k < SMSV_UNLOCK_BITS)
                        s->unlocked[k >> 3] |= (uint8_t)(0x80u >> (k & 7));
                }
            LOG(" %u of %u items", ones, len);
            bits = smsv_put_block(msg, SMSV_PAYLOAD, 0, SMSB_MT_UNLOCK_ITEM, payload, p, len);
            reply = "MT_UNLOCK_ITEM";
            break;
        }
        case SMSB_MO_DOWNLOAD_ITEM:
            if (len >= 32)
                LOG(" item=%u", smsv_getbits(payload, SMSV_PAYLOAD, p, 32));
            bits = smsv_put_block(msg, SMSV_PAYLOAD, 0, SMSB_MT_REQUESTED_ITEM, payload, p, len);
            reply = "MT_REQUESTED_ITEM";
            break;
        case SMSB_MO_LOAD: {
            /* MT_LOAD restores what this server knows: nickname (15 x 7), the unlocks granted
             * (400 bits), the three best scores as {level 16, score 32} */
            uint8_t b[SMSV_PAYLOAD];
            memset(b, 0, sizeof b);
            put_name(b, 0, s->nick, s->nick_len, SMSV_NAME, 127);
            for (uint32_t k = 0; k < SMSV_UNLOCK_BITS; k++)
                smsv_putbits(b, sizeof b, 105 + k, 1, (s->unlocked[k >> 3] >> (7 - (k & 7))) & 1u);
            for (uint32_t k = 0; k < 3; k++) {
                const SmsvRank *r = k < s->nranks ? &s->ranks[k] : NULL;
                smsv_putbits(b, sizeof b, 505 + 48 * k, 16, r ? (uint32_t)r->level & 0xffffu : 0);
                smsv_putbits(b, sizeof b, 521 + 48 * k, 32, r ? (uint32_t)r->score : 0);
            }
            bits = smsv_put_block(msg, SMSV_PAYLOAD, 0, SMSB_MT_LOAD, b, 0, 649);
            reply = "MT_LOAD";
            break;
        }
        default:
            break; /* pay-per-use (25) and the rest: the send is the whole transaction */
        }
    }
    if (reply && !bits) {
        LOG(", the %s does not fit in an SMS", reply);
        reply = NULL;
    }
    if (!reply) {
        LOG(", no reply");
        return 0;
    }
    queue(s, now, msg, bits);
    LOG(" -> %s%s in %u.%u s", reply, extra, SMSV_REPLY_DELAY_MS / 1000,
        SMSV_REPLY_DELAY_MS % 1000 / 100);
#undef LOG
    return 1;
}

int smsv_take(ExSmsServer *s, uint32_t now, uint8_t out[SMSV_PAYLOAD])
{
    if (!s->pending || (int32_t)(now - s->due) < 0)
        return 0;
    memcpy(out, s->reply, SMSV_PAYLOAD);
    s->pending = 0;
    s->replies++;
    return 1;
}

/* ------------------------------------------------------------ save states */

#define SMSV_FORMAT 1u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* u32 format, u32 pending, u32 due, reply[136], u32 nranks, ranks {i32 score, i32 level,
 * u32 name_len, name[15]}, u32 nick_len, nick[15], unlocked[50], u32 votes, vote_sum, sent,
 * replies */
#define SMSV_STATE_BYTES (4 * 3 + SMSV_PAYLOAD + 4 + SMSV_RANKS * (12 + SMSV_NAME) + 4 + SMSV_NAME + \
                          SMSV_UNLOCK_BITS / 8 + 4 * 4)

size_t smsv_save(const ExSmsServer *s, uint8_t *out, size_t cap)
{
    if (cap < SMSV_STATE_BYTES)
        return SMSV_STATE_BYTES;
    uint8_t *p = out;
    put32(p, SMSV_FORMAT), p += 4;
    put32(p, s->pending), p += 4;
    put32(p, s->due), p += 4;
    memcpy(p, s->reply, SMSV_PAYLOAD), p += SMSV_PAYLOAD;
    put32(p, s->nranks), p += 4;
    for (int i = 0; i < SMSV_RANKS; i++) {
        put32(p, (uint32_t)s->ranks[i].score), p += 4;
        put32(p, (uint32_t)s->ranks[i].level), p += 4;
        put32(p, s->ranks[i].name_len), p += 4;
        memcpy(p, s->ranks[i].name, SMSV_NAME), p += SMSV_NAME;
    }
    put32(p, s->nick_len), p += 4;
    memcpy(p, s->nick, SMSV_NAME), p += SMSV_NAME;
    memcpy(p, s->unlocked, sizeof s->unlocked), p += sizeof s->unlocked;
    put32(p, s->votes), p += 4;
    put32(p, s->vote_sum), p += 4;
    put32(p, s->sent), p += 4;
    put32(p, s->replies), p += 4;
    return (size_t)(p - out);
}

int smsv_load(ExSmsServer *s, const uint8_t *in, size_t len)
{
    if (len != SMSV_STATE_BYTES || get32(in) != SMSV_FORMAT)
        return -1;
    ExSmsServer t = *s; /* keeps the host settings */
    smsv_reset(&t);
    const uint8_t *p = in + 4;
    t.pending = get32(p), p += 4;
    t.due = get32(p), p += 4;
    memcpy(t.reply, p, SMSV_PAYLOAD), p += SMSV_PAYLOAD;
    t.nranks = get32(p), p += 4;
    for (int i = 0; i < SMSV_RANKS; i++) {
        t.ranks[i].score = (int32_t)get32(p), p += 4;
        t.ranks[i].level = (int32_t)get32(p), p += 4;
        uint32_t nl = get32(p);
        p += 4;
        if (nl > SMSV_NAME)
            return -1;
        t.ranks[i].name_len = (uint8_t)nl;
        memcpy(t.ranks[i].name, p, SMSV_NAME), p += SMSV_NAME;
    }
    t.nick_len = get32(p), p += 4;
    memcpy(t.nick, p, SMSV_NAME), p += SMSV_NAME;
    memcpy(t.unlocked, p, sizeof t.unlocked), p += sizeof t.unlocked;
    t.votes = get32(p), p += 4;
    t.vote_sum = get32(p), p += 4;
    t.sent = get32(p), p += 4;
    t.replies = get32(p), p += 4;
    if (t.pending > 1 || t.nranks > SMSV_RANKS || t.nick_len > SMSV_NAME || t.vote_sum > 7u * t.votes)
        return -1;
    *s = t;
    return 0;
}

/* ------------------------------------------------------------ VM glue */

static void log_hex(ExenVM *vm, const char *what, const uint8_t *p, uint32_t n)
{
    char line[2 * SMSV_PAYLOAD + 64];
    int k = snprintf(line, sizeof line, "sms: %s", what);
    for (uint32_t i = 0; i < n && k + 3 < (int)sizeof line; i++)
        k += snprintf(line + k, sizeof line - (size_t)k, "%02x", p[i]);
    vm_log(vm, "%s", line);
}

int smsv_send(ExenVM *vm, const uint8_t *buf)
{
    ExSmsServer *s = &vm->smsv;
    const uint8_t *payload = buf + 4;
    uint32_t end = rd32(buf + 0x90);
    char log[400];
    if (s->verbose)
        log_hex(vm, "sent ", payload, SMSV_PAYLOAD);
    if (s->mode == EXEN_SMS_OFFLINE) {
        vm_log(vm, "sms: no network (offline mode), the SMS is reported as not sent");
        return 0;
    }
    if (s->mode == EXEN_SMS_DELIVERED) {
        SmsvMessage m;
        int ok = smsv_decode(payload, end, &m) == 0;
        vm_log(vm, "sms: sent %s%s(%u), reported as delivered, no reply (delivered mode)",
               ok ? "" : "malformed ", ok && m.nblocks ? block_name(m.blocks[0].id) : "",
               ok && m.nblocks ? m.blocks[0].id : 0);
        s->sent++;
        return 1;
    }
    ExenGameInfo gi;
    exen_game_info(vm->exn, vm->exn_len, &gi);
    smsv_handle(s, payload, end, exen_vm_ticks(vm), gi.title[0] ? gi.title : NULL, log,
                sizeof log);
    vm_log(vm, "sms: sent %s", log);
    return 1;
}

uint32_t smsv_take_array(ExenVM *vm)
{
    ExSmsServer *s = &vm->smsv;
    uint8_t msg[SMSV_PAYLOAD];
    if (!s->pending || (int32_t)(exen_vm_ticks(vm) - s->due) < 0)
        return 0;
    ref_t a = vm_new_array(vm, T_BYTE, 0, (int32_t)SMSV_PAYLOAD);
    if (!a) {
        vm_log(vm, "sms: out of memory, the reply is dropped");
        s->pending = 0;
        return 0;
    }
    smsv_take(s, exen_vm_ticks(vm), msg);
    memcpy(arr_data(vm, a), msg, SMSV_PAYLOAD);
    vm_log(vm, "sms: received %s(%u)", block_name(msg[0]), msg[0]);
    if (s->verbose)
        log_hex(vm, "received ", msg, SMSV_PAYLOAD);
    return a;
}

/* ------------------------------------------------------------ host settings (exen.h) */

void exen_sms_set_mode(ExenVM *vm, int mode)
{
    vm->smsv.mode = mode == EXEN_SMS_DELIVERED || mode == EXEN_SMS_OFFLINE ? mode : EXEN_SMS_SERVER;
}

int exen_sms_mode(ExenVM *vm) { return vm->smsv.mode; }

void exen_sms_set_log(ExenVM *vm, int verbose) { vm->smsv.verbose = verbose != 0; }

void exen_sms_set_date(ExenVM *vm, int year, int month, int day)
{
    int ok = year >= 1 && year <= 4095 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
    vm->smsv.year = ok ? year : 0;
    vm->smsv.month = ok ? month : 0;
    vm->smsv.day = ok ? day : 0;
}

_Static_assert(SMSV_STATE_BYTES <= SMSV_STATE_MAX, "SMSV_STATE_MAX too small");
