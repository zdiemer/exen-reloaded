/* Tests for the SDL frontend's settings.ini (src/sdl/settings.c): defaults, parsing (valid,
 * invalid and unknown entries), the recent list, per-game handset overrides, and a write /
 * read round trip through a real file. Exit status 0 on success. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "exen.h"
#include "settings.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL line %d: %s\n", __LINE__, #cond);                \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static Settings s, t;

static void test_defaults(void)
{
    settings_defaults(&s);
    CHECK(s.scale == 4 && !s.fullscreen && s.voice == EXEN_VOICE_RINGTONE && s.volume == 100 && !s.mute);
    CHECK(s.sms == EXEN_SMS_SERVER && s.nrecent == 0 && s.nhandset == 0 && !s.last_dir[0]);
    CHECK(settings_parse(&s, "") == 0 && s.scale == 4);
    CHECK(settings_parse(&s, NULL) == 0);
}

static void test_parse(void)
{
    settings_defaults(&s);
    int errors = settings_parse(&s,
                                "; comment\n"
                                "# another\n"
                                "[video]\r\n"
                                "scale = 3\r\n"
                                "fullscreen=1\n"
                                "  [ Sound ]  \n"
                                "voice = GM\n"
                                "volume = 50\n"
                                "mute = 1\n"
                                "[sms]\n"
                                "mode = offline\n"
                                "[files]\n"
                                "last-dir = C:\\Games\\ExEn\\\n"
                                "recent = C:\\Games\\ExEn\\a=b.exn\n"
                                "recent = /home/me/games/Worms.exn\n"
                                "recent =\n"
                                "[handset]\n"
                                "Worms = 128x128\n"
                                "0badc0de = 101X80\n"
                                "[future]\n"
                                "anything = goes\n");
    CHECK(errors == 0);
    CHECK(s.scale == 3 && s.fullscreen == 1);
    CHECK(s.voice == EXEN_VOICE_PIANO && s.volume == 50 && s.mute == 1);
    CHECK(s.sms == EXEN_SMS_OFFLINE);
    CHECK(!strcmp(s.last_dir, "C:\\Games\\ExEn\\"));
    CHECK(s.nrecent == 2);
    CHECK(!strcmp(s.recent[0], "C:\\Games\\ExEn\\a=b.exn")); /* split at the first = only */
    CHECK(!strcmp(s.recent[1], "/home/me/games/Worms.exn"));
    int w = 0, h = 0;
    CHECK(settings_get_handset(&s, "Worms", &w, &h) && w == 128 && h == 128);
    CHECK(settings_get_handset(&s, "0badc0de", &w, &h) && w == 101 && h == 80);
    CHECK(!settings_get_handset(&s, "worms", &w, &h)); /* keys are file names: exact */

    /* invalid values are skipped and keep what was there */
    errors = settings_parse(&s,
                            "[video]\n"
                            "scale = 9\n"
                            "scale = two\n"
                            "fullscreen = 2\n"
                            "[sound]\n"
                            "voice = kazoo\n"
                            "volume = -1\n"
                            "volume = 101\n"
                            "no equals sign\n"
                            "[sms]\n"
                            "mode = carrier pigeon\n"
                            "[handset]\n"
                            "Worms = huge\n"
                            "Tiny = 8x8\n");
    CHECK(errors == 10);
    CHECK(s.scale == 3 && s.fullscreen == 1 && s.voice == EXEN_VOICE_PIANO && s.volume == 50);
    CHECK(s.sms == EXEN_SMS_OFFLINE);
    CHECK(settings_get_handset(&s, "Worms", &w, &h) && w == 128 && h == 128);
    CHECK(!settings_get_handset(&s, "Tiny", &w, &h));

    /* a file's recent list replaces the current one */
    settings_parse(&s, "[files]\nrecent = x.exn\n");
    CHECK(s.nrecent == 1 && !strcmp(s.recent[0], "x.exn"));
    /* more than the maximum: the first ones are kept */
    char text[2048] = "[files]\n";
    for (int i = 0; i < SETTINGS_MAX_RECENT + 3; i++) {
        char line[64];
        SDL_snprintf(line, sizeof line, "recent = game%d.exn\n", i);
        SDL_strlcat(text, line, sizeof text);
    }
    settings_parse(&s, text);
    CHECK(s.nrecent == SETTINGS_MAX_RECENT && !strcmp(s.recent[SETTINGS_MAX_RECENT - 1], "game9.exn"));
}

static void test_recent(void)
{
    settings_defaults(&s);
    settings_add_recent(&s, "a.exn");
    settings_add_recent(&s, "b.exn");
    settings_add_recent(&s, "c.exn");
    CHECK(s.nrecent == 3 && !strcmp(s.recent[0], "c.exn") && !strcmp(s.recent[2], "a.exn"));
    settings_add_recent(&s, "a.exn"); /* moves to the top, no duplicate */
    CHECK(s.nrecent == 3 && !strcmp(s.recent[0], "a.exn") && !strcmp(s.recent[1], "c.exn") &&
          !strcmp(s.recent[2], "b.exn"));
    settings_add_recent(&s, s.recent[2]); /* a path from the list itself */
    CHECK(s.nrecent == 3 && !strcmp(s.recent[0], "b.exn") && !strcmp(s.recent[1], "a.exn"));
    settings_add_recent(&s, "");
    settings_add_recent(&s, NULL);
    CHECK(s.nrecent == 3);
#ifdef SDL_PLATFORM_WINDOWS
    settings_add_recent(&s, "C:\\G\\x.exn");
    settings_add_recent(&s, "c:/g/X.EXN"); /* the same file on Windows */
    CHECK(s.nrecent == 4 && !strcmp(s.recent[0], "c:/g/X.EXN"));
#endif
    for (int i = 0; i < 20; i++) {
        char p[32];
        SDL_snprintf(p, sizeof p, "g%d.exn", i);
        settings_add_recent(&s, p);
    }
    CHECK(s.nrecent == SETTINGS_MAX_RECENT && !strcmp(s.recent[0], "g19.exn") &&
          !strcmp(s.recent[SETTINGS_MAX_RECENT - 1], "g10.exn"));
    settings_remove_recent(&s, 0);
    CHECK(s.nrecent == SETTINGS_MAX_RECENT - 1 && !strcmp(s.recent[0], "g18.exn"));
    settings_remove_recent(&s, s.nrecent - 1);
    CHECK(s.nrecent == SETTINGS_MAX_RECENT - 2 && !strcmp(s.recent[s.nrecent - 1], "g11.exn"));
    settings_remove_recent(&s, 99);
    settings_remove_recent(&s, -1);
    CHECK(s.nrecent == SETTINGS_MAX_RECENT - 2);
}

static void test_handset(void)
{
    settings_defaults(&s);
    int w, h;
    settings_set_handset(&s, "A", 128, 160);
    settings_set_handset(&s, "B", 101, 80);
    settings_set_handset(&s, "A", 120, 143); /* replaces */
    CHECK(s.nhandset == 2 && settings_get_handset(&s, "A", &w, &h) && w == 120 && h == 143);
    settings_set_handset(&s, "A", 0, 0); /* Auto: removed */
    CHECK(s.nhandset == 1 && !settings_get_handset(&s, "A", &w, &h));
    CHECK(settings_get_handset(&s, "B", &w, &h) && w == 101);
    settings_set_handset(&s, "missing", 0, 0);
    CHECK(s.nhandset == 1);
    for (int i = 0; i < SETTINGS_MAX_HANDSETS + 5; i++) {
        char k[16];
        SDL_snprintf(k, sizeof k, "g%d", i);
        settings_set_handset(&s, k, 128, 128);
    }
    CHECK(s.nhandset == SETTINGS_MAX_HANDSETS);
}

static void test_round_trip(void)
{
    settings_defaults(&s);
    s.scale = 2;
    s.fullscreen = 1;
    s.voice = EXEN_VOICE_PIANO;
    s.volume = 75;
    s.mute = 1;
    s.sms = EXEN_SMS_DELIVERED;
    SDL_strlcpy(s.last_dir, "/games/", sizeof s.last_dir);
    settings_add_recent(&s, "/games/one.exn");
    settings_add_recent(&s, "/games/two words.exn");
    settings_set_handset(&s, "Crash Bandicoot", 128, 141);
    char *text = settings_format(&s);
    CHECK(text != NULL);
    if (!text)
        return;
    settings_defaults(&t);
    CHECK(settings_parse(&t, text) == 0);
    CHECK(t.scale == 2 && t.fullscreen == 1 && t.voice == EXEN_VOICE_PIANO && t.volume == 75 && t.mute == 1);
    CHECK(t.sms == EXEN_SMS_DELIVERED && !strcmp(t.last_dir, "/games/"));
    CHECK(t.nrecent == 2 && !strcmp(t.recent[0], "/games/two words.exn") && !strcmp(t.recent[1], "/games/one.exn"));
    int w, h;
    CHECK(settings_get_handset(&t, "Crash Bandicoot", &w, &h) && w == 128 && h == 141);
    char *again = settings_format(&t);
    CHECK(again && !strcmp(text, again));
    SDL_free(again);

    /* through a file */
    const char *path = "sdl-settings-test.ini"; /* in the test's working directory */
    CHECK(settings_save(&s, path) == 0);
    Settings *u = SDL_malloc(sizeof *u);
    CHECK(u && settings_load(u, path) == 1);
    if (u) {
        CHECK(u->scale == 2 && u->nrecent == 2 && u->sms == EXEN_SMS_DELIVERED);
        char *third = settings_format(u);
        CHECK(third && !strcmp(text, third));
        SDL_free(third);
        SDL_RemovePath(path);
        CHECK(settings_load(u, path) == 0 && u->scale == 4); /* missing: the defaults */
        SDL_free(u);
    }
    SDL_free(text);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    test_defaults();
    test_parse();
    test_recent();
    test_handset();
    test_round_trip();
    if (failures) {
        printf("sdl_settings_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("sdl_settings_test: all passed\n");
    return 0;
}
