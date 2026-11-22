/* Tests for the SDL frontend's input bindings (src/sdl/input.c): config parsing, defaults,
 * and the phone key press/release sequences produced by synthetic keyboard and gamepad
 * events, including keys held from several devices and stick hysteresis. No window or
 * gamepad hardware is needed. Exit status 0 on success. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exen.h"
#include "input.h"

static char seq[1024];
static int failures;

static void sink(void *u, int code, int pressed)
{
    (void)u;
    char one[32];
    SDL_snprintf(one, sizeof one, "%s%s%s", *seq ? " " : "", pressed ? "+" : "-", input_phone_name(code));
    SDL_strlcat(seq, one, sizeof seq);
}

/* Compare the transitions since the last check with `want`, then clear them. */
static void expect(const char *what, const char *want, int line)
{
    if (strcmp(seq, want)) {
        printf("FAIL line %d: %s: got \"%s\", want \"%s\"\n", line, what, seq, want);
        failures++;
    }
    seq[0] = 0;
}
#define EXPECT(what, want) expect(what, want, __LINE__)
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL line %d: %s\n", __LINE__, #cond);                \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static void key(Input *in, SDL_Keycode k, int down, int repeat)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.key = k;
    ev.key.down = down;
    ev.key.repeat = repeat;
    input_handle_event(in, &ev);
}

static void button(Input *in, int pad, int b, int down)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    ev.gbutton.which = (SDL_JoystickID)pad;
    ev.gbutton.button = (Uint8)b;
    ev.gbutton.down = down;
    input_handle_event(in, &ev);
}

static void axis(Input *in, int pad, int a, int value)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    ev.gaxis.which = (SDL_JoystickID)pad;
    ev.gaxis.axis = (Uint8)a;
    ev.gaxis.value = (Sint16)value;
    input_handle_event(in, &ev);
}

static int bound(const Input *in, int kind, int code, int dir, int phone)
{
    for (int i = 0; i < in->nbind; i++)
        if (in->bind[i].kind == kind && in->bind[i].code == code && in->bind[i].dir == dir &&
            in->bind[i].phone == phone)
            return 1;
    return 0;
}

static void test_names(void)
{
    CHECK(input_phone_from_name("5") == '5');
    CHECK(input_phone_from_name("FIRE") == '5');
    CHECK(input_phone_from_name("up") == '2');
    CHECK(input_phone_from_name("star") == '*');
    CHECK(input_phone_from_name("pound") == '#');
    CHECK(input_phone_from_name("ok") == EXEN_KEY_OK);
    CHECK(input_phone_from_name("select") == EXEN_KEY_OK);
    CHECK(input_phone_from_name("clear") == EXEN_KEY_CLEAR);
    CHECK(input_phone_from_name("pad-right") == EXEN_KEY_PAD_RIGHT);
    CHECK(input_phone_from_name("nope") == 0);
    /* the SDL name lookups the config relies on */
    CHECK(SDL_GetKeyFromName("Keypad 8") == SDLK_KP_8);
    CHECK(SDL_GetKeyFromName("Return") == SDLK_RETURN);
    CHECK(SDL_GetKeyFromName("W") == SDLK_W);
    CHECK(SDL_GetKeyFromName("w") == SDLK_W);
    CHECK(SDL_GetGamepadButtonFromString("a") == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(SDL_GetGamepadButtonFromString("dpup") == SDL_GAMEPAD_BUTTON_DPAD_UP);
    CHECK(SDL_GetGamepadAxisFromString("lefty") == SDL_GAMEPAD_AXIS_LEFTY);
}

static void test_defaults(void)
{
    Input in;
    input_init(&in, sink, NULL);
    /* the original simulator's keys (docs/re/NATIVES.md §4.2) */
    CHECK(bound(&in, INPUT_KEY, SDLK_RETURN, 0, EXEN_KEY_OK));
    CHECK(bound(&in, INPUT_KEY, SDLK_ESCAPE, 0, EXEN_KEY_CLEAR));
    CHECK(bound(&in, INPUT_KEY, SDLK_UP, 0, '2'));
    CHECK(bound(&in, INPUT_KEY, SDLK_LEFT, 0, '4'));
    CHECK(bound(&in, INPUT_KEY, SDLK_RIGHT, 0, '6'));
    CHECK(bound(&in, INPUT_KEY, SDLK_DOWN, 0, '8'));
    CHECK(bound(&in, INPUT_KEY, SDLK_KP_7, 0, '1'));
    CHECK(bound(&in, INPUT_KEY, SDLK_KP_1, 0, '7'));
    CHECK(bound(&in, INPUT_KEY, SDLK_KP_MULTIPLY, 0, '*'));
    CHECK(bound(&in, INPUT_KEY, SDLK_KP_DIVIDE, 0, '#'));
    CHECK(bound(&in, INPUT_KEY, SDLK_0, 0, '0'));
    CHECK(bound(&in, INPUT_KEY, SDLK_9, 0, '9'));
    CHECK(bound(&in, INPUT_BUTTON, SDL_GAMEPAD_BUTTON_SOUTH, 0, '5'));
    CHECK(bound(&in, INPUT_BUTTON, SDL_GAMEPAD_BUTTON_START, 0, EXEN_KEY_OK));
    CHECK(bound(&in, INPUT_AXIS, SDL_GAMEPAD_AXIS_LEFTY, -1, '2'));
    CHECK(bound(&in, INPUT_AXIS, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1, '7'));
    /* nothing bound to a hotkey */
    for (int i = 0; i < in.nbind; i++)
        CHECK(!(in.bind[i].kind == INPUT_KEY && (in.bind[i].code == SDLK_M || in.bind[i].code == SDLK_F11)));
}

static void test_keyboard(void)
{
    Input in;
    input_init(&in, sink, NULL);
    seq[0] = 0;
    key(&in, SDLK_UP, 1, 0);
    EXPECT("arrow down", "+2");
    key(&in, SDLK_UP, 1, 1);
    EXPECT("auto-repeat ignored", "");
    key(&in, SDLK_KP_8, 1, 0);
    EXPECT("second key on 2", "");
    key(&in, SDLK_UP, 0, 0);
    EXPECT("first key released", "");
    key(&in, SDLK_KP_8, 0, 0);
    EXPECT("last key released", "-2");
    key(&in, SDLK_KP_8, 0, 0);
    EXPECT("stray release", "");
    key(&in, SDLK_F1, 1, 0);
    key(&in, SDLK_F1, 0, 0);
    EXPECT("unbound key", "");
    key(&in, SDLK_RETURN, 1, 0);
    key(&in, SDLK_ESCAPE, 1, 0);
    key(&in, SDLK_RETURN, 0, 0);
    key(&in, SDLK_ESCAPE, 0, 0);
    EXPECT("soft keys", "+ok +clear -ok -clear");
}

static void test_multi_device(void)
{
    Input in;
    input_init(&in, sink, NULL);
    seq[0] = 0;
    button(&in, 11, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    EXPECT("pad 1 fire", "+5");
    button(&in, 12, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    key(&in, SDLK_SPACE, 1, 0);
    button(&in, 11, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    EXPECT("same key from pad 2, keyboard, duplicate down", "");
    button(&in, 11, SDL_GAMEPAD_BUTTON_SOUTH, 0);
    EXPECT("pad 1 up, others hold", "");
    input_release_device(&in, 0, 12); /* pad 2 unplugged while holding */
    EXPECT("pad 2 removed, keyboard holds", "");
    key(&in, SDLK_SPACE, 0, 0);
    EXPECT("last holder released", "-5");

    button(&in, 11, SDL_GAMEPAD_BUTTON_DPAD_UP, 1);
    axis(&in, 11, SDL_GAMEPAD_AXIS_LEFTY, -30000);
    EXPECT("dpad then stick", "+2");
    button(&in, 11, SDL_GAMEPAD_BUTTON_DPAD_UP, 0);
    EXPECT("dpad up, stick holds", "");
    axis(&in, 11, SDL_GAMEPAD_AXIS_LEFTY, 0);
    EXPECT("stick centred", "-2");

    button(&in, 11, SDL_GAMEPAD_BUTTON_EAST, 1);
    axis(&in, 12, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    key(&in, SDLK_LEFT, 1, 0);
    EXPECT("three keys held", "+clear +6 +4");
    input_release_device(&in, 0, 11);
    EXPECT("pad 1 removed", "-clear");
    input_release_all(&in);
    EXPECT("focus lost", "-6 -4");
    button(&in, 11, SDL_GAMEPAD_BUTTON_EAST, 0);
    axis(&in, 12, SDL_GAMEPAD_AXIS_LEFTX, 0);
    key(&in, SDLK_LEFT, 0, 0);
    EXPECT("releases after release-all", "");
}

static void test_hysteresis(void)
{
    Input in;
    input_init(&in, sink, NULL); /* press 50%, release 35% */
    seq[0] = 0;
    int ax = SDL_GAMEPAD_AXIS_LEFTY;
    axis(&in, 1, ax, -10000);
    EXPECT("inside deadzone", "");
    axis(&in, 1, ax, -16000);
    EXPECT("below press threshold", "");
    axis(&in, 1, ax, -16383);
    EXPECT("press threshold", "+2");
    axis(&in, 1, ax, -12000);
    EXPECT("between thresholds stays held", "");
    axis(&in, 1, ax, -15000);
    EXPECT("wobble", "");
    axis(&in, 1, ax, -11000);
    EXPECT("release threshold", "-2");
    axis(&in, 1, ax, -15000);
    EXPECT("between thresholds stays released", "");
    axis(&in, 1, ax, -32768);
    axis(&in, 1, ax, 32767);
    EXPECT("flick across centre", "+2 -2 +8");
    axis(&in, 1, ax, 0);
    EXPECT("centre", "-8");
    axis(&in, 1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    axis(&in, 1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    EXPECT("trigger", "+9 -9");
    axis(&in, 1, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
    EXPECT("unbound axis", "");
    CHECK(in.nactive == 0);
}

static void test_config(void)
{
    Input in;
    input_init(&in, sink, NULL);
    const char *cfg =
        "; comment\n"
        "# comment\n"
        "orphan = Up\n"                 /* error: outside a section */
        "[gamepad]\r\n"
        "stick-press = 60\n"
        "stick-release = 20\n"
        "ok = south, Start\n"
        "UP = lefty-, rightstick\n"
        "fire = x, dpbogus\n"           /* error: dpbogus */
        "6 = leftx\n"                   /* error: stick axis without direction */
        "[keyboard]\n"
        "fire = Space, X, comma\n"
        "ok = Space\n"
        "  2   =   Up ,  Keypad 8 ,  \n"
        "pad-left = J\n"
        "bogus = Up\n"                  /* error: unknown phone key */
        "5 = NotAKey\n"                 /* error */
        "[mouse]\n"                     /* error: unknown section */
        "5 = button1\n";
    int errors = input_parse_config(&in, cfg, "test.ini");
    CHECK(errors == 6);
    CHECK(in.axis_press == 60 * 32767 / 100);
    CHECK(in.axis_release == 20 * 32767 / 100);
    /* the sections replaced the defaults */
    CHECK(!bound(&in, INPUT_KEY, SDLK_W, 0, '2'));
    CHECK(!bound(&in, INPUT_KEY, SDLK_RETURN, 0, EXEN_KEY_OK));
    CHECK(!bound(&in, INPUT_BUTTON, SDL_GAMEPAD_BUTTON_SOUTH, 0, '5'));
    CHECK(bound(&in, INPUT_BUTTON, SDL_GAMEPAD_BUTTON_SOUTH, 0, EXEN_KEY_OK));
    CHECK(bound(&in, INPUT_BUTTON, SDL_GAMEPAD_BUTTON_START, 0, EXEN_KEY_OK));
    CHECK(bound(&in, INPUT_BUTTON, SDL_GAMEPAD_BUTTON_WEST, 0, '5'));
    CHECK(bound(&in, INPUT_BUTTON, SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0, '2'));
    CHECK(bound(&in, INPUT_AXIS, SDL_GAMEPAD_AXIS_LEFTY, -1, '2'));
    CHECK(bound(&in, INPUT_KEY, SDLK_COMMA, 0, '5'));
    CHECK(bound(&in, INPUT_KEY, SDLK_KP_8, 0, '2'));
    CHECK(bound(&in, INPUT_KEY, SDLK_J, 0, EXEN_KEY_PAD_LEFT));
    CHECK(in.nbind == 12);

    /* one input on two phone keys */
    seq[0] = 0;
    key(&in, SDLK_SPACE, 1, 0);
    key(&in, SDLK_X, 1, 0);
    key(&in, SDLK_SPACE, 0, 0);
    key(&in, SDLK_X, 0, 0);
    EXPECT("space on 5 and ok", "+5 +ok -ok -5");

    /* a file with only one section keeps the other's defaults */
    input_init(&in, sink, NULL);
    CHECK(input_parse_config(&in, "[gamepad]\n5 = north\n", "test2.ini") == 0);
    CHECK(bound(&in, INPUT_KEY, SDLK_UP, 0, '2'));
    CHECK(bound(&in, INPUT_BUTTON, SDL_GAMEPAD_BUTTON_NORTH, 0, '5'));
    CHECK(!bound(&in, INPUT_BUTTON, SDL_GAMEPAD_BUTTON_SOUTH, 0, '5'));

    /* bad thresholds */
    input_init(&in, sink, NULL);
    CHECK(input_parse_config(&in, "[gamepad]\nstick-press = 0\nstick-release = 120\n", "t3") == 2);
    CHECK(input_parse_config(&in, "[gamepad]\nstick-press = 30\nstick-release = 40\n", "t4") == 1);
    CHECK(in.axis_release == in.axis_press);
}

static void test_round_trip(void)
{
    Input a, b;
    input_init(&a, NULL, NULL);
    char *text = input_format_config(&a, 1);
    CHECK(text != NULL);
    if (!text)
        return;
    SDL_zero(b);
    int errors = input_parse_config(&b, text, "round-trip");
    CHECK(errors == 0);
    CHECK(a.nbind == b.nbind);
    for (int i = 0; i < a.nbind; i++)
        CHECK(bound(&b, a.bind[i].kind, a.bind[i].code, a.bind[i].dir, a.bind[i].phone));
    CHECK(a.axis_press == b.axis_press && a.axis_release == b.axis_release);
    char *again = input_format_config(&b, 1);
    CHECK(again && !strcmp(text, again));
    if (getenv("INPUT_TEST_PRINT"))
        fputs(text, stdout);
    SDL_free(again);
    SDL_free(text);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    test_names();
    test_defaults();
    test_keyboard();
    test_multi_device();
    test_hysteresis();
    test_config();
    test_round_trip();
    if (failures) {
        printf("sdl_input_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("sdl_input_test: all passed\n");
    return 0;
}
