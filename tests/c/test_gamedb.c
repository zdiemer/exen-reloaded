/* Game database lookup (src/core/gamedb.c, data/gamedb.tsv). The expectations are read from
 * data/gamedb.tsv itself, so the test keeps working as games are added. */
#include <stdlib.h>

#include "support.h"
#include "test.h"

typedef struct {
    uint32_t crc;
    char main_class[64], title[64];
    int w, h;
} Row;

static Row rows[512];
static int nrows;

static void load_tsv(void)
{
    size_t len;
    char *t = (char *)t_read_file(t_source_path("data/gamedb.tsv"), &len);
    if (!t) {
        fprintf(stderr, "cannot read data/gamedb.tsv\n");
        exit(2);
    }
    char *p = t, *end = t + len;
    while (p < end && nrows < 512) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *line_end = nl ? nl : end;
        if (line_end > p && line_end[-1] == '\r')
            line_end[-1] = 0;
        *line_end = 0;
        if (*p && *p != '#') {
            Row *r = &rows[nrows];
            char *f[6];
            int nf = 0;
            for (char *q = p; nf < 6;) {
                f[nf++] = q;
                char *tab = strchr(q, '\t');
                if (!tab)
                    break;
                *tab = 0;
                q = tab + 1;
            }
            if (nf >= 5) {
                r->crc = (uint32_t)strtoul(f[0], NULL, 16);
                snprintf(r->main_class, sizeof r->main_class, "%s", f[1]);
                snprintf(r->title, sizeof r->title, "%s", f[2]);
                r->w = atoi(f[3]);
                r->h = atoi(f[4]);
                nrows++;
            }
        }
        p = line_end + 1;
    }
    free(t);
}

static uint8_t hdr[0x48];

/* A NEXE header with the given main class (enough for exen_game_info). */
static void make_header(const char *main_class)
{
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "NEXE", 4);
    wr32(hdr + 4, 0x80);
    size_t n = strlen(main_class);
    memcpy(hdr + 0x14, main_class, n < 32 ? n : 32); /* NUL padded, not terminated */
}

/* Choose the last 4 bytes of buf[0..len) so that its CRC-32 is `target`: run four zero-input
 * CRC steps backwards from the target register (each step's table index is fixed by the top
 * byte) and xor the register the data produces. */
static void forge_crc(uint8_t *buf, size_t len, uint32_t target)
{
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
        table[i] = c;
    }
    uint32_t reg = exen_crc32(buf, len - 4) ^ 0xffffffffu; /* register after the prefix */
    uint32_t r = target ^ 0xffffffffu;
    for (int step = 0; step < 4; step++) {
        uint32_t idx = 0;
        while (idx < 256 && (table[idx] >> 24) != (r >> 24))
            idx++;
        r = ((r ^ table[idx]) << 8) | idx;
    }
    wr32(buf + len - 4, r ^ reg);
}

static void test_db_loaded(void)
{
    CHECK(nrows >= 40);
    for (int i = 0; i < nrows; i++) {
        CHECK(rows[i].w > 0 && rows[i].h > 0);
        CHECK(rows[i].main_class[0]);
    }
}

static void test_exact_match(void)
{
    /* every entry is found by file CRC */
    for (int i = 0; i < nrows; i++) {
        make_header(rows[i].main_class);
        forge_crc(hdr, sizeof hdr, rows[i].crc);
        t_current = rows[i].title;
        CHECK_EQ(exen_crc32(hdr, sizeof hdr), rows[i].crc);
        ExenGameInfo gi;
        CHECK_EQ(exen_game_info(hdr, sizeof hdr, &gi), EXEN_GAME_EXACT);
        CHECK_EQ(gi.match, EXEN_GAME_EXACT);
        CHECK_EQ(gi.width, rows[i].w);
        CHECK_EQ(gi.height, rows[i].h);
        CHECK_STR(gi.title, rows[i].title);
    }
}

static void test_main_class_fallback(void)
{
    int tested_same = 0, tested_conflict = 0;
    for (int i = 0; i < nrows; i++) {
        /* do all entries with this main class agree on the screen? */
        int agree = 1;
        for (int j = 0; j < nrows; j++)
            if (!strcmp(rows[j].main_class, rows[i].main_class) &&
                (rows[j].w != rows[i].w || rows[j].h != rows[i].h))
                agree = 0;
        make_header(rows[i].main_class);
        hdr[sizeof hdr - 1] ^= 0x5a; /* some other dump of the game: CRC unknown */
        t_current = rows[i].main_class;
        ExenGameInfo gi;
        int rc = exen_game_info(hdr, sizeof hdr, &gi);
        CHECK_STR(gi.main_class, rows[i].main_class);
        if (agree) {
            CHECK_EQ(rc, EXEN_GAME_SAME_MAIN_CLASS);
            CHECK_EQ(gi.width, rows[i].w);
            CHECK_EQ(gi.height, rows[i].h);
            tested_same++;
        } else {
            /* e.g. the Tomb Raider titles share a class name: no guess */
            CHECK_EQ(rc, EXEN_GAME_UNKNOWN);
            CHECK_EQ(gi.width, EXEN_DEFAULT_WIDTH);
            CHECK_EQ(gi.height, EXEN_DEFAULT_HEIGHT);
            tested_conflict++;
        }
    }
    t_current = "test_main_class_fallback";
    CHECK(tested_same > 0);
    printf("  %d main-class matches, %d ambiguous main classes\n", tested_same, tested_conflict);
}

static void test_unknown_game(void)
{
    ExenGameInfo gi;
    make_header("no.such.Game");
    CHECK_EQ(exen_game_info(hdr, sizeof hdr, &gi), EXEN_GAME_UNKNOWN);
    CHECK_EQ(gi.width, 132);
    CHECK_EQ(gi.height, 176);
    CHECK_STR(gi.main_class, "no.such.Game");
    CHECK_STR(gi.title, "no.such.Game"); /* the title defaults to the main class */

    /* a 32-character main class has no terminator in the header */
    make_header("abcdefghijklmnopqrstuvwxyz012345");
    CHECK_EQ(exen_game_info(hdr, sizeof hdr, &gi), EXEN_GAME_UNKNOWN);
    CHECK_EQ(strlen(gi.main_class), 32);

    /* not a NEXE file, or too short */
    static const uint8_t junk[0x40] = {'X', 'E', 'X', 'E'};
    CHECK_EQ(exen_game_info(junk, sizeof junk, &gi), EXEN_GAME_UNKNOWN);
    CHECK_EQ(gi.width, EXEN_DEFAULT_WIDTH);
    CHECK_EQ(gi.height, EXEN_DEFAULT_HEIGHT);
    make_header("x");
    CHECK_EQ(exen_game_info(hdr, 0x20, &gi), EXEN_GAME_UNKNOWN);
}

int main(void)
{
    load_tsv();
    RUN(test_db_loaded);
    RUN(test_exact_match);
    RUN(test_main_class_fallback);
    RUN(test_unknown_game);
    TEST_MAIN_END();
}
