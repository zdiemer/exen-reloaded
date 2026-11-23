/* The SMS codec and the mock In-Fusio server (src/core/smsserver.c, docs/re/SMS.md), on
 * synthetic messages laid out the way exen.Sms writes them: no game, no VM. */
#include <stdlib.h>

#include "smsserver.h"
#include "test.h"

#define B SMSV_PAYLOAD

/* A message under construction, as Sms.createSms / createBlock / writeBits / endBlock build it. */
typedef struct {
    uint8_t p[B];
    uint32_t pos, block;
} Msg;

static void put(Msg *m, uint32_t n, uint32_t v)
{
    smsv_putbits(m->p, B, m->pos, n, v);
    m->pos += n;
}

static void msg_init(Msg *m)
{
    memset(m, 0, sizeof *m);
    put(m, 8, 0x85); /* cfg+5, cfg+7 */
    put(m, 8, 0x80);
    put(m, 16, 0x0080); /* gamelet id */
    put(m, 32, 0x00000001);
    put(m, 12, 0);
    put(m, 8, 0);
    put(m, 8, 1); /* English */
    put(m, 16, 0x1234);
    m->pos += 20;
    put(m, 8, 0x60);
    put(m, 11, 0);
}

static void block_begin(Msg *m, uint8_t id)
{
    m->block = m->pos;
    put(m, 8, id);
    m->pos += 11;
}

static void block_end(Msg *m) { smsv_putbits(m->p, B, m->block + 8, 11, m->pos - (m->block + 19)); }

static void put_name(Msg *m, const char *s, int pad_to)
{
    int i = 0;
    for (; s[i]; i++)
        put(m, 7, (uint8_t)s[i] & 0x7f);
    for (; i < pad_to; i++)
        put(m, 7, 10);
}

static void test_bits(void)
{
    uint8_t b[8] = {0};
    smsv_putbits(b, sizeof b, 3, 11, 0x5a5);
    CHECK_EQ(smsv_getbits(b, sizeof b, 3, 11), 0x5a5);
    CHECK_EQ(b[0], 0x16); /* 000 10110 */
    CHECK_EQ(b[1], 0x94); /* 100101 00 */
    smsv_putbits(b, sizeof b, 20, 32, 0xdeadbeefu);
    CHECK_EQ(smsv_getbits(b, sizeof b, 20, 32), 0xdeadbeefu);
    CHECK_EQ(smsv_getbits(b, sizeof b, 3, 11), 0x5a5);
    /* past the end: reads 0, writes nothing */
    CHECK_EQ(smsv_getbits(b, sizeof b, 60, 8), smsv_getbits(b, sizeof b, 60, 4) << 4);
    smsv_putbits(b, sizeof b, 62, 8, 0xff);
    CHECK_EQ(b[7] & 3, 3);
}

static void test_decode(void)
{
    Msg m;
    msg_init(&m);
    block_begin(&m, SMSB_MO_PAY);
    block_end(&m);
    block_begin(&m, SMSB_MO_SCORE);
    put(&m, 32, 12345);
    put(&m, 4, 0);
    put(&m, 16, 3);
    put_name(&m, "ZED", 0);
    block_end(&m);
    SmsvMessage d;
    REQUIRE(smsv_decode(m.p, m.pos, &d) == 0);
    CHECK_EQ(d.ver_hi, 0x85);
    CHECK_EQ(d.gamelet, 0x80);
    CHECK_EQ(d.reg[3], 1);
    CHECK_EQ(d.lang, 1);
    CHECK_EQ(d.cfg164, 0x1234);
    REQUIRE(d.nblocks == 2);
    CHECK_EQ(d.blocks[0].id, SMSB_MO_PAY);
    CHECK_EQ(d.blocks[0].len, 0);
    CHECK_EQ(d.blocks[1].id, SMSB_MO_SCORE);
    CHECK_EQ(d.blocks[1].len, 32 + 4 + 16 + 3 * 7);
    CHECK_EQ(d.blocks[1].pos, SMSV_BLOCKS_BIT + 19 + 19);
    /* encoder round trip: copy the score block into a reply buffer */
    uint8_t out[B];
    memset(out, 0, sizeof out);
    uint32_t end = smsv_put_block(out, B, 5, 0x42, m.p, d.blocks[1].pos, d.blocks[1].len);
    CHECK_EQ(end, 5 + 19 + d.blocks[1].len);
    CHECK_EQ(smsv_getbits(out, B, 5, 8), 0x42);
    CHECK_EQ(smsv_getbits(out, B, 13, 11), d.blocks[1].len);
    CHECK_EQ(smsv_getbits(out, B, 24, 32), 12345);
    CHECK_EQ(smsv_getbits(out, B, 24 + 36, 16), 3);
    CHECK_EQ(smsv_put_block(out, B, B * 8 - 20, 1, NULL, 0, 2), 0); /* does not fit */

    /* a block that runs past the end, or no container */
    CHECK(smsv_decode(m.p, m.pos - 1, &d) < 0);
    Msg bad = m;
    smsv_putbits(bad.p, B, SMSV_HEADER_BITS, 8, 0x41);
    CHECK(smsv_decode(bad.p, bad.pos, &d) < 0);
}

static void send_score(ExSmsServer *s, uint32_t now, int32_t score, int level, const char *name)
{
    Msg m;
    msg_init(&m);
    block_begin(&m, SMSB_MO_SCORE);
    put(&m, 32, (uint32_t)score);
    put(&m, 4, 0);
    put(&m, 16, (uint32_t)level);
    put_name(&m, name, 0);
    block_end(&m);
    char log[256];
    CHECK_EQ(smsv_handle(s, m.p, m.pos, now, "Test", log, sizeof log), 1);
}

static void name_at(const uint8_t *r, uint32_t pos, char *out)
{
    int i;
    for (i = 0; i < SMSV_NAME; i++) {
        uint32_t c = smsv_getbits(r, B, pos + 7 * (uint32_t)i, 7);
        if (c == 127)
            break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

static void test_ranking(void)
{
    ExSmsServer s;
    memset(&s, 0, sizeof s);
    s.year = 2003;
    s.month = 7;
    s.day = 14;
    uint8_t r[B];

    send_score(&s, 1000, 500, 2, "AL");
    CHECK(s.pending);
    CHECK_EQ(smsv_take(&s, 1000 + SMSV_REPLY_DELAY_MS - 1, r), 0); /* not yet due */
    REQUIRE(smsv_take(&s, 1000 + SMSV_REPLY_DELAY_MS, r) == 1);
    CHECK(!s.pending);
    CHECK_EQ(smsv_take(&s, 99999, r), 0); /* taken once */

    /* MT_RANKING: id, 11-bit length 993, then the record */
    CHECK_EQ(r[0], SMSB_MT_RANKING);
    CHECK_EQ(smsv_getbits(r, B, 8, 11), 993);
    const uint32_t P = 19;
    CHECK_EQ(smsv_getbits(r, B, P + 4, 1), 1);    /* ranked */
    CHECK_EQ(smsv_getbits(r, B, P + 5, 32), 1);   /* rank */
    CHECK_EQ(smsv_getbits(r, B, P + 37, 14), 2);  /* level */
    CHECK_EQ(smsv_getbits(r, B, P + 51, 32), 500);
    CHECK_EQ(smsv_getbits(r, B, P + 196, 4), 7);  /* ranking date */
    CHECK_EQ(smsv_getbits(r, B, P + 200, 5), 14);
    CHECK_EQ(smsv_getbits(r, B, P + 205, 12), 2003);
    CHECK_EQ(smsv_getbits(r, B, P + 217, 21), 0); /* open contest */
    char nm[SMSV_NAME + 1];
    name_at(r, P + 238, nm);
    CHECK_STR(nm, "AL");
    CHECK_EQ(smsv_getbits(r, B, P + 238 + 119, 32), 500);
    /* past the block (19 + 993 bits = 126.5 bytes): 0xff, like the platform's unpack buffer */
    CHECK_EQ(r[127], 0xff);
    CHECK_EQ(r[B - 1], 0xff);

    /* a better and a worse score: best first, the rank is the new score's */
    send_score(&s, 5000, 900, 4, "AL");
    REQUIRE(smsv_take(&s, 5000 + SMSV_REPLY_DELAY_MS, r));
    CHECK_EQ(smsv_getbits(r, B, P + 5, 32), 1);
    send_score(&s, 9000, 100, 1, "AL");
    REQUIRE(smsv_take(&s, 9000 + SMSV_REPLY_DELAY_MS, r));
    CHECK_EQ(smsv_getbits(r, B, P + 5, 32), 3);
    CHECK_EQ(smsv_getbits(r, B, P + 51, 32), 100);
    CHECK_EQ(smsv_getbits(r, B, P + 238 + 119, 32), 900);
    CHECK_EQ(smsv_getbits(r, B, P + 238 + 151 + 119, 32), 500);
    CHECK_EQ(smsv_getbits(r, B, P + 238 + 302 + 119, 32), 100);
    CHECK_EQ(s.nranks, 3);

    /* MO_RANKING: the player's best */
    Msg m;
    msg_init(&m);
    block_begin(&m, SMSB_MO_RANKING);
    put(&m, 4, 15);
    block_end(&m);
    CHECK_EQ(smsv_handle(&s, m.p, m.pos, 20000, NULL, NULL, 0), 1);
    REQUIRE(smsv_take(&s, 30000, r));
    CHECK_EQ(r[0], SMSB_MT_RANKING);
    CHECK_EQ(smsv_getbits(r, B, P + 51, 32), 900);

    /* and on a server without scores: "not ranked yet" */
    ExSmsServer e;
    memset(&e, 0, sizeof e);
    CHECK_EQ(smsv_handle(&e, m.p, m.pos, 0, NULL, NULL, 0), 1);
    REQUIRE(smsv_take(&e, SMSV_REPLY_DELAY_MS, r));
    CHECK_EQ(smsv_getbits(r, B, P + 4, 1), 0);
    CHECK_EQ(smsv_getbits(r, B, P + 5, 32), 0);
    CHECK_EQ(smsv_getbits(r, B, P + 196, 4), 1); /* the default date, 2004-01-01 */
    CHECK_EQ(smsv_getbits(r, B, P + 205, 12), 2004);
}

static void test_requests(void)
{
    ExSmsServer s;
    memset(&s, 0, sizeof s);
    uint8_t r[B];
    char log[256];
    Msg m;

    /* "Your opinion": mark 4 */
    msg_init(&m);
    block_begin(&m, SMSB_MO_VOTE);
    put(&m, 3, 4);
    put_name(&m, "ZED", 15);
    put(&m, 7, 10);
    block_end(&m);
    CHECK_EQ(smsv_handle(&s, m.p, m.pos, 0, "My Game", log, sizeof log), 1);
    CHECK(strstr(log, "MO_VOTE") != NULL);
    REQUIRE(smsv_take(&s, SMSV_REPLY_DELAY_MS, r));
    CHECK_EQ(r[0], SMSB_MT_VOTES);
    CHECK_EQ(smsv_getbits(r, B, 8, 11), 1000);
    CHECK_EQ(smsv_getbits(r, B, 19, 3), 4);
    CHECK_EQ(smsv_getbits(r, B, 22, 32), 1);
    CHECK_EQ(smsv_getbits(r, B, 19 + 35, 3), 4);
    CHECK_EQ(smsv_getbits(r, B, 19 + 38, 7), 'M');
    CHECK_EQ(smsv_getbits(r, B, 19 + 38 + 7 * 6, 7), 'e');
    CHECK_EQ(smsv_getbits(r, B, 19 + 38 + 7 * 7, 7), ' ');

    /* unlock: granted as asked */
    msg_init(&m);
    block_begin(&m, SMSB_MO_UNLOCK_ITEM);
    for (int i = 0; i < 400; i++)
        put(&m, 1, i == 12 || i == 13 || i == 200);
    block_end(&m);
    CHECK_EQ(smsv_handle(&s, m.p, m.pos, 0, NULL, log, sizeof log), 1);
    CHECK(strstr(log, "3 of 400") != NULL);
    REQUIRE(smsv_take(&s, SMSV_REPLY_DELAY_MS, r));
    CHECK_EQ(r[0], SMSB_MT_UNLOCK_ITEM);
    CHECK_EQ(smsv_getbits(r, B, 8, 11), 400);
    for (uint32_t i = 0; i < 400; i++)
        if (smsv_getbits(r, B, 19 + i, 1) != (i == 12 || i == 13 || i == 200))
            T_FAIL("unlock bit %u", i);

    /* restore: the nickname, the unlocks granted, no scores yet */
    msg_init(&m);
    block_begin(&m, SMSB_MO_LOAD);
    block_end(&m);
    CHECK_EQ(smsv_handle(&s, m.p, m.pos, 0, NULL, NULL, 0), 1);
    REQUIRE(smsv_take(&s, SMSV_REPLY_DELAY_MS, r));
    CHECK_EQ(r[0], SMSB_MT_LOAD);
    CHECK_EQ(smsv_getbits(r, B, 8, 11), 649);
    CHECK_EQ(smsv_getbits(r, B, 19, 7), 'Z');
    CHECK_EQ(smsv_getbits(r, B, 19 + 105 + 12, 2), 3);
    CHECK_EQ(smsv_getbits(r, B, 19 + 105 + 200, 1), 1);
    CHECK_EQ(smsv_getbits(r, B, 19 + 105 + 201, 1), 0);
    CHECK_EQ(smsv_getbits(r, B, 19 + 505 + 16, 32), 0);

    /* download: the item comes back */
    msg_init(&m);
    block_begin(&m, SMSB_MO_DOWNLOAD_ITEM);
    put(&m, 32, 77);
    block_end(&m);
    CHECK_EQ(smsv_handle(&s, m.p, m.pos, 0, NULL, NULL, 0), 1);
    REQUIRE(smsv_take(&s, SMSV_REPLY_DELAY_MS, r));
    CHECK_EQ(r[0], SMSB_MT_REQUESTED_ITEM);
    CHECK_EQ(smsv_getbits(r, B, 19, 32), 77);

    /* pay-per-use: no answer, nothing queued */
    msg_init(&m);
    block_begin(&m, SMSB_MO_PAY);
    block_end(&m);
    CHECK_EQ(smsv_handle(&s, m.p, m.pos, 0, NULL, log, sizeof log), 0);
    CHECK(strstr(log, "no reply") != NULL);
    CHECK(!s.pending);

    /* malformed */
    msg_init(&m);
    CHECK_EQ(smsv_handle(&s, m.p, 10, 0, NULL, NULL, 0), -1);
}

static void test_state(void)
{
    ExSmsServer s;
    memset(&s, 0, sizeof s);
    s.mode = 1;
    send_score(&s, 100, 4242, 7, "PAT");
    s.votes = 2;
    s.vote_sum = 9;
    s.unlocked[3] = 0x81;
    uint8_t buf[SMSV_STATE_MAX];
    size_t n = smsv_save(&s, buf, sizeof buf);
    REQUIRE(n > 0 && n <= sizeof buf);
    CHECK_EQ(smsv_save(&s, buf, 10), n); /* too small: says how much it needs */

    ExSmsServer t;
    memset(&t, 0, sizeof t);
    t.mode = 2;
    t.year = 1999;
    REQUIRE(smsv_load(&t, buf, n) == 0);
    CHECK_EQ(t.mode, 2); /* host settings are kept */
    CHECK_EQ(t.year, 1999);
    CHECK_EQ(t.pending, 1);
    CHECK_EQ(t.due, 100 + SMSV_REPLY_DELAY_MS);
    CHECK(memcmp(t.reply, s.reply, B) == 0);
    CHECK_EQ(t.nranks, 1);
    CHECK_EQ(t.ranks[0].score, 4242);
    CHECK_EQ(t.ranks[0].name_len, 3);
    CHECK_EQ(t.votes, 2);
    CHECK_EQ(t.unlocked[3], 0x81);
    uint8_t again[SMSV_STATE_MAX];
    CHECK_EQ(smsv_save(&t, again, sizeof again), n);
    CHECK(memcmp(buf, again, n) == 0);

    /* refused: wrong size, format, or values out of range */
    CHECK(smsv_load(&t, buf, n - 1) < 0);
    buf[0] = 9;
    CHECK(smsv_load(&t, buf, n) < 0);
    buf[0] = 1;
    buf[4] = 2; /* pending */
    CHECK(smsv_load(&t, buf, n) < 0);
    CHECK_EQ(t.ranks[0].score, 4242); /* unchanged by a refused load */

    smsv_reset(&t);
    CHECK_EQ(t.nranks, 0);
    CHECK_EQ(t.pending, 0);
    CHECK_EQ(t.mode, 2);
}

int main(void)
{
    RUN(test_bits);
    RUN(test_decode);
    RUN(test_ranking);
    RUN(test_requests);
    RUN(test_state);
    TEST_MAIN_END();
}
