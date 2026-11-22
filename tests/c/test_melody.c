/* Melody sequencer timing and synthesis (src/core/melody.c, docs/re/NATIVES.md §3.2).
 *
 * The original player arms a 10 ms timer, then re-arms it for 15 ms * duration units after
 * each note (tempo 120: 1875 / 120 = 15 ms per unit). At 8000 Hz one millisecond is 8 frames,
 * so every event lands on an exact frame. */
#include <stdlib.h>

#include "platform.h"
#include "support.h"
#include "test.h"

#define RATE 8000
#define MS(x) ((x) * (RATE / 1000))
#define TOTAL MS(1000)

static int16_t pcm[2 * TOTAL];
static int rendered;

static ExenVM *audio_vm(void)
{
    ExenConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.audio_rate = RATE;
    rendered = 0;
    return exen_create(&cfg);
}

static void render_to(ExenVM *vm, int frame)
{
    if (frame > TOTAL)
        frame = TOTAL;
    if (frame > rendered) {
        int n = exen_audio_render(vm, pcm + 2 * rendered, frame - rendered);
        CHECK_EQ(n, frame - rendered);
        rendered = frame;
    }
}

static int sounding(ExenVM *vm, int note)
{
    const Melody *m = &vm->plat->melody;
    for (int i = 0; i < MELODY_VOICES; i++)
        if (m->voice[i].note == note && (m->voice[i].state == 1 || m->voice[i].state == 2))
            return 1;
    return 0;
}

static int any_voice(ExenVM *vm)
{
    const Melody *m = &vm->plat->melody;
    for (int i = 0; i < MELODY_VOICES; i++)
        if (m->voice[i].state)
            return 1;
    return 0;
}

static int peak(int from, int to)
{
    int p = 0;
    for (int f = from; f < to; f++) {
        int v = abs(pcm[2 * f]);
        if (v > p)
            p = v;
    }
    return p;
}

static int rising_crossings(int from, int to)
{
    int n = 0;
    for (int f = from + 1; f < to; f++)
        if (pcm[2 * (f - 1)] < 0 && pcm[2 * f] >= 0)
            n++;
    return n;
}

static void test_two_note_timing(void)
{
    /* A4 (69) for duration index 7 (16 units = 240 ms), C5 (72) for index 5 (32 units =
     * 480 ms), then the 0 end marker */
    static const uint8_t mel[] = {69, 7, 72, 5, 0, 0};
    ExenVM *vm = audio_vm();
    REQUIRE(vm);
    CHECK_EQ(exen_audio_rate(vm), RATE);
    CHECK(!exen_audio_active(vm));
    melody_play(vm, mel, sizeof mel);
    const Melody *m = &vm->plat->melody;
    CHECK(m->playing);
    CHECK(exen_audio_active(vm));

    /* the first timer firing is 10 ms after play */
    render_to(vm, MS(10));
    CHECK(!any_voice(vm));
    CHECK_EQ(peak(0, MS(10)), 0);
    render_to(vm, MS(10) + 1);
    CHECK(sounding(vm, 69));
    CHECK_EQ(m->period_ms, 240);

    /* the second note starts 240 ms later */
    render_to(vm, MS(250));
    CHECK(sounding(vm, 69));
    CHECK(!sounding(vm, 72));
    render_to(vm, MS(250) + 1);
    CHECK(sounding(vm, 72));
    CHECK(!sounding(vm, 69)); /* monophonic: the previous note is released */
    CHECK_EQ(m->period_ms, 480);

    /* and the end marker 480 ms after that */
    render_to(vm, MS(730));
    CHECK(m->playing);
    CHECK(sounding(vm, 72));
    render_to(vm, MS(730) + 1);
    CHECK(!m->playing);
    CHECK(!sounding(vm, 72));

    render_to(vm, TOTAL);
    /* non-silent while the notes play, at the right pitch, then silence */
    CHECK(peak(MS(20), MS(250)) > 1000);
    CHECK(peak(MS(260), MS(730)) > 1000);
    int c1 = rising_crossings(MS(10), MS(250));  /* 440 Hz * 0.24 s = 105.6 */
    int c2 = rising_crossings(MS(250), MS(730)); /* 523.25 Hz * 0.48 s = 251.2 */
    CHECK(c1 >= 100 && c1 <= 111);
    CHECK(c2 >= 244 && c2 <= 258);
    CHECK(peak(MS(800), TOTAL) <= 1);
    CHECK(!exen_audio_active(vm));
    /* balanced: both channels carry the same signal */
    int same = 1;
    for (int f = 0; f < TOTAL; f++)
        same &= pcm[2 * f] == pcm[2 * f + 1];
    CHECK(same);
    printf("  crossings %d (A4) %d (C5), peak %d\n", c1, c2, peak(0, TOTAL));
    exen_destroy(vm);
}

static void test_rest_ends_melody(void)
{
    /* 0x82 ("rest") neither advances nor re-arms the timer in the original: the melody goes
     * silent there for good, and the note after it never plays */
    static const uint8_t mel[] = {69, 7, 0x82, 5, 72, 7};
    ExenVM *vm = audio_vm();
    REQUIRE(vm);
    melody_play(vm, mel, sizeof mel);
    render_to(vm, MS(250) + 1);
    CHECK(!vm->plat->melody.playing);
    render_to(vm, TOTAL);
    CHECK(!sounding(vm, 72));
    CHECK(peak(MS(300), TOTAL) <= 1);
    exen_destroy(vm);
}

static void test_mute_keeps_time(void)
{
    static const uint8_t mel[] = {69, 7, 72, 5, 0, 0};
    ExenVM *vm = audio_vm();
    REQUIRE(vm);
    exen_audio_set_volume(vm, 0);
    melody_play(vm, mel, sizeof mel);
    render_to(vm, MS(250) + 1);
    CHECK(sounding(vm, 72)); /* the sequencer ran */
    CHECK_EQ(peak(0, MS(250)), 0);
    melody_stop(vm);
    CHECK(!vm->plat->melody.playing);
    CHECK(!sounding(vm, 72));
    exen_destroy(vm);
}

static void test_idle_render_is_silent(void)
{
    ExenVM *vm = audio_vm();
    REQUIRE(vm);
    render_to(vm, MS(100));
    CHECK_EQ(peak(0, MS(100)), 0);
    CHECK(!exen_audio_active(vm));
    exen_destroy(vm);
}

int main(void)
{
    RUN(test_two_note_timing);
    RUN(test_rest_ends_melody);
    RUN(test_mute_keeps_time);
    RUN(test_idle_render_is_silent);
    TEST_MAIN_END();
}
