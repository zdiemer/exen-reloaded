/* End-to-end smoke test through the public API (exen.h) on the two .exn files vendored with
 * the simulator (vendor/gensimu/flash/). No other games are used. */
#include <stdlib.h>

#include "support.h"
#include "test.h"

typedef struct {
    uint32_t now;
    int period;
    int frames;
    int melodies;
    long nonblack;
} Host;

static void on_present(void *u, const uint8_t *px, int w, int h)
{
    Host *s = u;
    s->frames++;
    for (int i = 0; i < w * h; i++)
        s->nonblack += px[i] != 0;
}
static void on_timer(void *u, int ms) { ((Host *)u)->period = ms; }
static uint32_t on_ticks(void *u) { return ((Host *)u)->now; }
static void on_melody(void *u, const uint8_t *d, int n)
{
    (void)d;
    (void)n;
    ((Host *)u)->melodies++;
}

static void run_game(const char *rel, const char *main_class, int ticks, int want_frames,
                     int want_melody)
{
    size_t len, gl;
    const uint8_t *exe = t_gensimu(&len);
    uint8_t *game = t_read_file(t_source_path(rel), &gl);
    REQUIRE(game);

    ExenGameInfo gi;
    exen_game_info(game, gl, &gi);
    CHECK_STR(gi.main_class, main_class);

    Host host;
    memset(&host, 0, sizeof host);
    host.period = -1;
    ExenConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.screen_width = gi.width;
    cfg.screen_height = gi.height;
    cfg.audio_rate = 22050;
    cfg.host.user = &host;
    cfg.host.present = on_present;
    cfg.host.set_timer = on_timer;
    cfg.host.ticks_ms = on_ticks;
    cfg.host.melody_play = on_melody;
    ExenVM *vm = exen_create(&cfg);
    REQUIRE(vm);
    CHECK_EQ(exen_load_framework(vm, exe, len), 0);
    CHECK_EQ(exen_load_game(vm, game, gl), 0);
    CHECK_STR(vm->main_class, main_class);
    CHECK(vm->nclasses > 54);
    CHECK_EQ(exen_boot(vm), 0);
    static int16_t audio[2 * 4096];
    for (int t = 0; t < ticks && !exen_halted(vm); t++) {
        host.now += host.period > 0 ? (uint32_t)host.period : 100;
        CHECK_EQ(exen_tick(vm), 0);
        exen_audio_render(vm, audio, 1024);
    }
    if (exen_halted(vm))
        T_FAIL("%s halted: %s", rel, exen_error(vm));
    /* a key press and release go through Bootstrap.keypress / keyrelease */
    CHECK_EQ(exen_key(vm, EXEN_KEY_5, 1), 0);
    CHECK_EQ(exen_key(vm, EXEN_KEY_5, 0), 0);
    CHECK(!exen_halted(vm));
    CHECK(host.frames >= want_frames);
    CHECK(host.nonblack > 0);
    if (want_melody)
        CHECK(host.melodies > 0);
    CHECK(exen_native_calls(vm, 72, NULL) > 0); /* Gamelet.screenUpdate */
    printf("  %s: %d frames, period %d ms, %d melodies, heap %u bytes\n", rel, host.frames,
           host.period, host.melodies, vm->heap_used);
    exen_destroy(vm);
    free(game);
}

static void test_boot_catalog(void)
{
    run_game("vendor/gensimu/flash/catalog.exn", "testcatalog.FTM", 60, 1, 0);
}

static void test_boot_download1(void)
{
    run_game("vendor/gensimu/flash/download1.exn", "CrazyCobra.GameTopLevel", 150, 100, 1);
}

int main(void)
{
    RUN(test_boot_catalog);
    RUN(test_boot_download1);
    TEST_MAIN_END();
}
