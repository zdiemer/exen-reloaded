/* exen-run: headless runner.
 *
 *   exen-run [--frames N] [--sim gensimu.exe] game.exn
 *
 * Boots the game and delivers N timer ticks on a virtual clock (no sleeping), then prints a
 * report: frames presented, timer period, halt reason, and native-call counts with the
 * unimplemented ones flagged. The compatibility harness builds on this.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exen.h"

const char *native_class(int index);
int native_implemented(int index);

typedef struct {
    uint32_t now_ms;
    int period;
    int frames;
    int quiet;
} Run;

static void on_present(void *u, const uint8_t *px, int w, int h) { (void)px; (void)w; (void)h; ((Run *)u)->frames++; }
static void on_timer(void *u, int ms) { ((Run *)u)->period = ms; }
static uint32_t on_ticks(void *u) { return ((Run *)u)->now_ms; }
static void on_log(void *u, const char *m)
{
    if (!((Run *)u)->quiet)
        fprintf(stderr, "[vm] %s\n", m);
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

int main(int argc, char **argv)
{
    const char *sim = "vendor/gensimu/gensimu.exe", *game = NULL;
    int frames = 300;
    Run run = {0, -1, 0, 0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sim") && i + 1 < argc) sim = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) run.quiet = 1;
        else game = argv[i];
    }
    if (!game) {
        fprintf(stderr, "usage: exen-run [--frames N] [--sim gensimu.exe] [--quiet] game.exn\n");
        return 2;
    }
    size_t sl, gl;
    uint8_t *simb = slurp(sim, &sl), *gameb = slurp(game, &gl);
    if (!simb || !gameb) {
        fprintf(stderr, "cannot read %s\n", simb ? game : sim);
        return 2;
    }
    ExenConfig cfg = {0};
    cfg.host = (ExenHost){&run, on_present, on_timer, on_ticks, NULL, NULL, on_log};
    ExenVM *vm = exen_create(&cfg);
    if (!vm || exen_load_framework(vm, simb, sl) < 0 || exen_load_game(vm, gameb, gl) < 0) {
        fprintf(stderr, "load failed: %s\n", vm ? exen_error(vm) : "out of memory");
        return 1;
    }
    int rc = exen_boot(vm);
    int ticks = 0;
    for (; rc == 0 && ticks < frames && !exen_halted(vm); ticks++) {
        run.now_ms += run.period > 0 ? (uint32_t)run.period : 100;
        rc = exen_tick(vm);
    }
    printf("game: %s\nticks: %d  frames presented: %d  timer period: %d ms\n", game, ticks,
           run.frames, run.period);
    printf("status: %s\n", exen_halted(vm) ? exen_error(vm) : "running");
    printf("natives called:\n");
    for (int i = 0; i < 256; i++) {
        const char *name;
        uint32_t n = exen_native_calls(vm, i, &name);
        if (n)
            printf("  #%-3d %-24s %-22s %8u%s\n", i, native_class(i), name, n,
                   native_implemented(i) ? "" : "   (unimplemented)");
    }
    int halted = exen_halted(vm);
    exen_destroy(vm);
    free(simb);
    free(gameb);
    return halted ? 1 : 0;
}
