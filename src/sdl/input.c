/* Remappable keyboard and gamepad input. See input.h. */
#include "input.h"

#include "exen.h"
#include "paths.h"

/* Bindable phone keys, in keypad order. The index is the slot in Input.held. */
static const struct {
    int code;
    const char *name, *alias[3];
    const char *what; /* for the config file comments */
} phone[INPUT_NPHONE] = {
    {'1', "1", {0}, "1"},
    {'2', "2", {"up"}, "2, up in most games"},
    {'3', "3", {0}, "3"},
    {'4', "4", {"left"}, "4, left in most games"},
    {'5', "5", {"fire"}, "5, fire/select in most games"},
    {'6', "6", {"right"}, "6, right in most games"},
    {'7', "7", {0}, "7"},
    {'8', "8", {"down"}, "8, down in most games"},
    {'9', "9", {0}, "9"},
    {'*', "star", {"*"}, "*"},
    {'0', "0", {0}, "0"},
    {'#', "pound", {"hash"}, "#"},
    {EXEN_KEY_OK, "ok", {"select", "val"}, "OK/select soft key (code -8, Gamelet.KEY_VAL)"},
    {EXEN_KEY_CLEAR, "clear", {"back"}, "clear/back soft key (code -7, Gamelet.KEY_CLEAR)"},
    {EXEN_KEY_PAD_UP, "pad-up", {0}, "joypad up (code -1); gensimu never sends it"},
    {EXEN_KEY_PAD_DOWN, "pad-down", {0}, "joypad down (code -6)"},
    {EXEN_KEY_PAD_LEFT, "pad-left", {0}, "joypad left (code -2)"},
    {EXEN_KEY_PAD_RIGHT, "pad-right", {0}, "joypad right (code -5)"},
};

/* The defaults, in the config syntax so that they resolve exactly like a config file would.
 * The keyboard follows the original simulator (docs/re/NATIVES.md §4.2), plus letter keys. */
static const char default_config[] =
    "[keyboard]\n"
    "1 = 1, Keypad 7, Q\n"
    "2 = 2, Up, Keypad 8, W\n"
    "3 = 3, Keypad 9, E\n"
    "4 = 4, Left, Keypad 4, A\n"
    "5 = 5, Keypad 5, Space\n"
    "6 = 6, Right, Keypad 6, D\n"
    "7 = 7, Keypad 1, Z\n"
    "8 = 8, Down, Keypad 2, S\n"
    "9 = 9, Keypad 3, C\n"
    "star = Keypad *, [\n"
    "0 = 0, Keypad 0, X\n"
    "pound = Keypad /, ]\n"
    "ok = Return, Keypad Enter\n"
    "clear = Escape, Backspace\n"
    "[gamepad]\n"
    "stick-press = 50\n"
    "stick-release = 35\n"
    "1 = leftshoulder\n"
    "2 = dpup, lefty-\n"
    "3 = rightshoulder\n"
    "4 = dpleft, leftx-\n"
    "5 = south\n"
    "6 = dpright, leftx+\n"
    "7 = lefttrigger\n"
    "8 = dpdown, lefty+\n"
    "9 = righttrigger\n"
    "star = west\n"
    "0 = leftstick, rightstick\n"
    "pound = north\n"
    "ok = start\n"
    "clear = east, back\n";

static const char config_header[] =
    "; exen-reloaded controls.\n"
    ";\n"
    "; Each line binds a phone key to any number of inputs, separated by commas:\n"
    ";     phonekey = input, input, ...\n"
    "; A section ([keyboard] or [gamepad]) present in this file replaces all the default\n"
    "; bindings of that device; a missing section keeps its defaults. Lines starting with ; or #\n"
    "; are comments. Delete this file to get the defaults back.\n"
    ";\n"
    "; Phone keys:\n"
    "%s"
    ";   Aliases: up = 2, left = 4, right = 6, down = 8, fire = 5, select = ok, back = clear.\n"
    ";   Games were written for the keypad: most move with 2/4/6/8 and fire or select with 5 or\n"
    ";   ok; some use 1/3/7/9 for diagonals. The original simulator maps the arrow keys to\n"
    ";   2/4/6/8, never to the pad-* codes.\n"
    ";\n"
    "; Keyboard inputs are SDL key names: A, 5, Up, Space, Return, Escape, Keypad 8, Keypad *,\n"
    "; Left Shift, F1, ... Use \"comma\" for the , key. M, V, F11 and other hotkeys are not bindable.\n"
    ";\n"
    "; Gamepad inputs: south, east, west, north (the face buttons by position: A, B, X, Y on an\n"
    "; Xbox pad), dpup, dpdown, dpleft, dpright, start, back, guide, leftshoulder, rightshoulder,\n"
    "; leftstick, rightstick (stick clicks), misc1, paddle1-4, touchpad (SDL button names; a, b,\n"
    "; x, y also work and mean the same positions); axis directions leftx-, leftx+, lefty-\n"
    "; (up), lefty+ (down), rightx-, rightx+, righty-, righty+, lefttrigger, righttrigger.\n"
    "; stick-press / stick-release: how far (percent) a stick or trigger must move to press the\n"
    "; key, and how far back to release it. All connected gamepads share these bindings.\n"
    "\n";

static int phone_index(int code)
{
    for (int i = 0; i < INPUT_NPHONE; i++)
        if (phone[i].code == code)
            return i;
    return -1;
}

const char *input_phone_name(int code)
{
    int i = phone_index(code);
    return i < 0 ? "?" : phone[i].name;
}

int input_phone_from_name(const char *name)
{
    for (int i = 0; i < INPUT_NPHONE; i++) {
        if (!SDL_strcasecmp(name, phone[i].name))
            return phone[i].code;
        for (int j = 0; j < 3 && phone[i].alias[j]; j++)
            if (!SDL_strcasecmp(name, phone[i].alias[j]))
                return phone[i].code;
    }
    return 0;
}

/* Face buttons by position; SDL's own names (a, b, x, y) are the Xbox labels. */
static const char *const face[4] = {"south", "east", "west", "north"};

static int parse_button(const char *s, int *code)
{
    for (int i = 0; i < 4; i++)
        if (!SDL_strcasecmp(s, face[i])) {
            *code = SDL_GAMEPAD_BUTTON_SOUTH + i;
            return 1;
        }
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s);
    if (b == SDL_GAMEPAD_BUTTON_INVALID)
        return 0;
    *code = b;
    return 1;
}

static int is_trigger(int axis)
{
    return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
}

static int parse_axis(const char *s, int *axis, int *dir)
{
    char name[64];
    size_t n = SDL_strlen(s);
    int d = 0;
    if (n > 0 && (s[n - 1] == '+' || s[n - 1] == '-')) {
        d = s[n - 1] == '+' ? 1 : -1;
        n--;
    }
    if (n == 0 || n >= sizeof name)
        return 0;
    SDL_memcpy(name, s, n);
    name[n] = 0;
    SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name);
    if (a == SDL_GAMEPAD_AXIS_INVALID)
        return 0;
    if (is_trigger(a)) {
        if (d < 0) /* triggers rest at 0 and only go positive */
            return 0;
        d = 1;
    } else if (!d) {
        return 0;
    }
    *axis = a;
    *dir = d;
    return 1;
}

static void add_binding(Input *in, int kind, int code, int dir, int ph, const char *origin)
{
    for (int i = 0; i < in->nbind; i++) {
        const InputBinding *b = &in->bind[i];
        if (b->kind == kind && b->code == code && b->dir == dir && b->phone == ph)
            return;
    }
    if (in->nbind == INPUT_MAX_BINDINGS) {
        SDL_Log("%s: too many bindings, ignoring the rest", origin);
        return;
    }
    in->bind[in->nbind++] = (InputBinding){kind, code, dir, ph};
}

static void drop_bindings(Input *in, int pad)
{
    int n = 0;
    for (int i = 0; i < in->nbind; i++) {
        int k = in->bind[i].kind;
        if (pad ? k == INPUT_KEY : k != INPUT_KEY)
            in->bind[n++] = in->bind[i];
    }
    in->nbind = n;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + SDL_strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static int parse_percent(const char *s, int lo, int *out)
{
    char *end;
    long v = SDL_strtol(s, &end, 10);
    if (end == s || *trim(end) || v < lo || v > 100)
        return 0;
    *out = (int)(v * 32767 / 100);
    return 1;
}

int input_parse_config(Input *in, const char *text, const char *origin)
{
    enum { NONE, KEYBOARD, GAMEPAD, UNKNOWN } section = NONE;
    int seen_kbd = 0, seen_pad = 0, errors = 0, lineno = 0;
    char *copy = SDL_strdup(text), *next = copy;
    while (next) {
        char *line = next;
        next = SDL_strchr(line, '\n');
        if (next)
            *next++ = 0;
        lineno++;
        line = trim(line);
        if (!*line || *line == ';' || *line == '#')
            continue;
        if (*line == '[') {
            char *close = SDL_strchr(line, ']');
            if (close)
                *close = 0;
            const char *name = trim(line + 1);
            if (!close) {
                section = UNKNOWN;
            } else if (!SDL_strcasecmp(name, "keyboard")) {
                section = KEYBOARD;
                if (!seen_kbd++)
                    drop_bindings(in, 0);
                continue;
            } else if (!SDL_strcasecmp(name, "gamepad")) {
                section = GAMEPAD;
                if (!seen_pad++)
                    drop_bindings(in, 1);
                continue;
            }
            section = UNKNOWN;
            SDL_Log("%s:%d: unknown section [%s], ignored", origin, lineno, name);
            errors++;
            continue;
        }
        if (section == UNKNOWN)
            continue;
        char *eq = SDL_strchr(line, '=');
        if (!eq || section == NONE) {
            SDL_Log("%s:%d: %s, ignored: %s", origin, lineno,
                    !eq ? "expected 'phonekey = inputs'" : "binding outside a [keyboard] or [gamepad] section",
                    line);
            errors++;
            continue;
        }
        *eq = 0;
        const char *key = trim(line);
        char *val = trim(eq + 1);
        if (section == GAMEPAD &&
            (!SDL_strcasecmp(key, "stick-press") || !SDL_strcasecmp(key, "stick-release"))) {
            int press = !SDL_strcasecmp(key, "stick-press");
            if (!parse_percent(val, press ? 1 : 0, press ? &in->axis_press : &in->axis_release)) {
                SDL_Log("%s:%d: %s must be a percentage, ignored: %s", origin, lineno, key, val);
                errors++;
            }
            continue;
        }
        int ph = input_phone_from_name(key);
        if (!ph) {
            SDL_Log("%s:%d: unknown phone key '%s', ignored (use 0-9, star, pound, ok, clear, "
                    "pad-up, pad-down, pad-left, pad-right)", origin, lineno, key);
            errors++;
            continue;
        }
        for (char *tok = val; tok;) {
            char *comma = SDL_strchr(tok, ',');
            if (comma)
                *comma++ = 0;
            char *t = trim(tok);
            tok = comma;
            if (!*t)
                continue;
            int code, dir = 0, ok;
            int kind = INPUT_KEY;
            if (section == KEYBOARD) {
                SDL_Keycode k = !SDL_strcasecmp(t, "comma") ? SDLK_COMMA : SDL_GetKeyFromName(t);
                ok = k != SDLK_UNKNOWN;
                code = (int)k;
            } else if ((ok = parse_button(t, &code))) {
                kind = INPUT_BUTTON;
            } else {
                ok = parse_axis(t, &code, &dir);
                kind = INPUT_AXIS;
            }
            if (!ok) {
                SDL_Log("%s:%d: unknown %s input '%s', ignored", origin, lineno,
                        section == KEYBOARD ? "keyboard" : "gamepad", t);
                errors++;
                continue;
            }
            add_binding(in, kind, code, dir, ph, origin);
        }
    }
    SDL_free(copy);
    if (in->axis_release > in->axis_press) {
        SDL_Log("%s: stick-release is above stick-press; using stick-press for both", origin);
        in->axis_release = in->axis_press;
        errors++;
    }
    return errors;
}

int input_load_config(Input *in, const char *path)
{
    size_t len;
    char *text = SDL_LoadFile(path, &len);
    if (!text)
        return 0;
    input_parse_config(in, text, path);
    SDL_free(text);
    return 1;
}

int input_load_or_create(Input *in, const char *path, int create)
{
    if (input_load_config(in, path))
        return 1;
    if (!create)
        return 0;
    char *text = input_format_config(in, 1);
    int ok = text && SDL_SaveFile(path, text, SDL_strlen(text));
    if (ok)
        SDL_Log("wrote the default controls to %s", path);
    else
        SDL_Log("cannot write %s: %s", path, SDL_GetError());
    SDL_free(text);
    return ok ? 2 : 0;
}

void input_init(Input *in, InputKeyFn sink, void *user)
{
    SDL_zerop(in);
    in->sink = sink;
    in->user = user;
    in->axis_press = 50 * 32767 / 100;
    in->axis_release = 35 * 32767 / 100;
    input_parse_config(in, default_config, "built-in controls");
}

/* --- formatting --- */

typedef struct {
    char *s;
    size_t len, cap;
} Buf;

static void put(Buf *b, const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(2);
static void put(Buf *b, const char *fmt, ...)
{
    if (!b->s) {
        b->s = SDL_malloc(b->cap = 1024);
        if (!b->s)
            return;
        b->s[0] = 0;
    }
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = SDL_vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if (b->len + (size_t)n < b->cap) {
            b->len += (size_t)n;
            return;
        }
        size_t cap = b->cap * 2;
        while (cap <= b->len + (size_t)n)
            cap *= 2;
        char *s = SDL_realloc(b->s, cap);
        if (!s)
            return;
        b->s = s;
        b->cap = cap;
    }
}

static void put_input(Buf *b, const InputBinding *x)
{
    if (x->kind == INPUT_KEY) {
        const char *n = x->code == SDLK_COMMA ? "comma" : SDL_GetKeyName((SDL_Keycode)x->code);
        put(b, "%s", n && *n ? n : "?");
    } else if (x->kind == INPUT_BUTTON) {
        if (x->code >= SDL_GAMEPAD_BUTTON_SOUTH && x->code <= SDL_GAMEPAD_BUTTON_NORTH)
            put(b, "%s", face[x->code - SDL_GAMEPAD_BUTTON_SOUTH]);
        else
            put(b, "%s", SDL_GetGamepadStringForButton((SDL_GamepadButton)x->code));
    } else {
        put(b, "%s%s", SDL_GetGamepadStringForAxis((SDL_GamepadAxis)x->code),
            is_trigger(x->code) ? "" : x->dir > 0 ? "+" : "-");
    }
}

static void put_section(Buf *b, const Input *in, int pad)
{
    put(b, "[%s]\n", pad ? "gamepad" : "keyboard");
    if (pad)
        put(b, "stick-press = %d\nstick-release = %d\n", (in->axis_press * 100 + 16383) / 32767,
            (in->axis_release * 100 + 16383) / 32767);
    for (int p = 0; p < INPUT_NPHONE; p++) {
        put(b, "%s =", phone[p].name);
        int first = 1;
        for (int i = 0; i < in->nbind; i++) {
            const InputBinding *x = &in->bind[i];
            if (x->phone != phone[p].code || (x->kind != INPUT_KEY) != pad)
                continue;
            put(b, first ? " " : ", ");
            put_input(b, x);
            first = 0;
        }
        put(b, "\n");
    }
}

void input_describe_source(int kind, int code, int dir, char *buf, size_t n)
{
    Buf b = {0};
    InputBinding x = {kind, code, dir, 0};
    put_input(&b, &x);
    SDL_strlcpy(buf, b.s ? b.s : "", n);
    SDL_free(b.s);
}

void input_describe(const Input *in, int code, int pad, char *buf, size_t n)
{
    Buf b = {0};
    put(&b, "%s", "");
    for (int i = 0; i < in->nbind; i++) {
        const InputBinding *x = &in->bind[i];
        if (x->phone != code || (x->kind != INPUT_KEY) != !!pad)
            continue;
        if (b.len)
            put(&b, ", ");
        put_input(&b, x);
    }
    SDL_strlcpy(buf, b.s ? b.s : "", n);
    SDL_free(b.s);
}

int input_phone_at(int index)
{
    return index >= 0 && index < INPUT_NPHONE ? phone[index].code : 0;
}

const char *input_phone_label(int code)
{
    static const struct {
        int code;
        const char *label;
    } labels[] = {
        {'2', "2 (up)"},     {'4', "4 (left)"},          {'5', "5 (fire)"},
        {'6', "6 (right)"},  {'8', "8 (down)"},          {'*', "* (star)"},
        {'#', "# (pound)"},  {EXEN_KEY_OK, "OK (select)"}, {EXEN_KEY_CLEAR, "Clear (back)"},
        {EXEN_KEY_PAD_UP, "Pad up"},     {EXEN_KEY_PAD_DOWN, "Pad down"},
        {EXEN_KEY_PAD_LEFT, "Pad left"}, {EXEN_KEY_PAD_RIGHT, "Pad right"},
    };
    for (size_t i = 0; i < sizeof labels / sizeof *labels; i++)
        if (labels[i].code == code)
            return labels[i].label;
    return input_phone_name(code);
}

int input_bind(Input *in, int kind, int code, int dir, int ph)
{
    if (phone_index(ph) < 0)
        return 0;
    input_release_all(in);
    int n = in->nbind;
    add_binding(in, kind, code, dir, ph, "controls");
    return in->nbind > n;
}

void input_unbind(Input *in, int code, int pad)
{
    input_release_all(in);
    int n = 0;
    for (int i = 0; i < in->nbind; i++) {
        const InputBinding *x = &in->bind[i];
        if (x->phone == code && (x->kind != INPUT_KEY) == !!pad)
            continue;
        in->bind[n++] = *x;
    }
    in->nbind = n;
}

void input_reset_defaults(Input *in)
{
    input_release_all(in);
    in->nbind = 0;
    in->axis_press = 50 * 32767 / 100;
    in->axis_release = 35 * 32767 / 100;
    input_parse_config(in, default_config, "built-in controls");
}

int input_save_config(const Input *in, const char *path)
{
    char *text = input_format_config(in, 1);
    int r = text ? exen_write_file(path, text, SDL_strlen(text)) : -1;
    SDL_free(text);
    return r;
}

char *input_format_config(const Input *in, int header)
{
    Buf b = {0};
    if (header) {
        Buf keys = {0};
        for (int p = 0; p < INPUT_NPHONE; p++)
            put(&keys, ";   %-10s %s\n", phone[p].name, phone[p].what);
        put(&b, config_header, keys.s ? keys.s : "");
        SDL_free(keys.s);
    }
    put_section(&b, in, 0);
    put(&b, "\n");
    put_section(&b, in, 1);
    return b.s;
}

/* --- held state --- */

static int find_active(const Input *in, int kind, int dev, int code, int dir)
{
    for (int i = 0; i < in->nactive; i++)
        if (in->active[i].kind == kind && in->active[i].dev == dev && in->active[i].code == code &&
            in->active[i].dir == dir)
            return i;
    return -1;
}

static void hold(Input *in, int kind, int code, int dir, int delta)
{
    for (int i = 0; i < in->nbind; i++) {
        const InputBinding *b = &in->bind[i];
        if (b->kind != kind || b->code != code || b->dir != dir)
            continue;
        int p = phone_index(b->phone);
        if (p < 0)
            continue;
        in->held[p] += delta;
        if (in->sink && ((delta > 0 && in->held[p] == 1) || (delta < 0 && in->held[p] == 0)))
            in->sink(in->user, b->phone, delta > 0);
    }
}

static int is_bound(const Input *in, int kind, int code, int dir)
{
    for (int i = 0; i < in->nbind; i++)
        if (in->bind[i].kind == kind && in->bind[i].code == code && in->bind[i].dir == dir)
            return 1;
    return 0;
}

void input_source(Input *in, int kind, int dev, int code, int dir, int down)
{
    int a = find_active(in, kind, dev, code, dir);
    if (down) {
        if (a >= 0 || in->nactive == INPUT_MAX_ACTIVE || !is_bound(in, kind, code, dir))
            return;
        in->active[in->nactive++] = (InputSource){kind, dev, code, dir};
        hold(in, kind, code, dir, 1);
    } else if (a >= 0) {
        in->active[a] = in->active[--in->nactive];
        hold(in, kind, code, dir, -1);
    }
}

void input_axis(Input *in, int dev, int axis, int value)
{
    /* hysteresis: press at axis_press, release below axis_release; releases go first so a
     * stick flicked across the centre releases one direction before pressing the other */
    for (int d = -1; d <= 1; d += 2)
        if (value * d < in->axis_release)
            input_source(in, INPUT_AXIS, dev, axis, d, 0);
    for (int d = -1; d <= 1; d += 2)
        if (value * d >= in->axis_press)
            input_source(in, INPUT_AXIS, dev, axis, d, 1);
}

void input_release_device(Input *in, int kind, int dev)
{
    /* backwards: removing entry i moves the last (already visited) entry into its place */
    for (int i = in->nactive - 1; i >= 0; i--) {
        if (in->active[i].dev == dev && (!kind || in->active[i].kind == kind))
            input_source(in, in->active[i].kind, dev, in->active[i].code, in->active[i].dir, 0);
    }
}

void input_release_all(Input *in)
{
    while (in->nactive > 0) {
        int i = in->nactive - 1;
        input_source(in, in->active[i].kind, in->active[i].dev, in->active[i].code, in->active[i].dir, 0);
    }
}

/* --- SDL events and gamepads --- */

static void pad_added(Input *in, SDL_JoystickID id)
{
    for (int i = 0; i < in->npads; i++)
        if (SDL_GetGamepadID(in->pads[i]) == id)
            return;
    if (in->npads == INPUT_MAX_PADS)
        return;
    SDL_Gamepad *p = SDL_OpenGamepad(id);
    if (!p) {
        SDL_Log("cannot open gamepad: %s", SDL_GetError());
        return;
    }
    in->pads[in->npads++] = p;
    const char *name = SDL_GetGamepadName(p);
    SDL_Log("gamepad connected: %s", name ? name : "(unnamed)");
}

static void pad_removed(Input *in, SDL_JoystickID id)
{
    input_release_device(in, 0, (int)id);
    for (int i = 0; i < in->npads; i++)
        if (SDL_GetGamepadID(in->pads[i]) == id) {
            SDL_Log("gamepad disconnected");
            SDL_CloseGamepad(in->pads[i]);
            in->pads[i] = in->pads[--in->npads];
            return;
        }
}

int input_handle_event(Input *in, const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (!is_bound(in, INPUT_KEY, (int)ev->key.key, 0))
            return 0;
        if (!ev->key.repeat) /* the original drops auto-repeat */
            input_source(in, INPUT_KEY, 0, (int)ev->key.key, 0, ev->key.down);
        return 1;
    case SDL_EVENT_GAMEPAD_ADDED:
        pad_added(in, ev->gdevice.which);
        return 1;
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_removed(in, ev->gdevice.which);
        return 1;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        input_source(in, INPUT_BUTTON, (int)ev->gbutton.which, ev->gbutton.button, 0, ev->gbutton.down);
        return 1;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        input_axis(in, (int)ev->gaxis.which, ev->gaxis.axis, ev->gaxis.value);
        return 1;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        /* gamepad events stop in the background; do not leave keys stuck down */
        input_release_all(in);
        return 0;
    }
    return 0;
}

void input_open_gamepads(Input *in)
{
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        SDL_Log("gamepads disabled: %s", SDL_GetError());
}

void input_close_gamepads(Input *in)
{
    input_release_all(in);
    for (int i = 0; i < in->npads; i++)
        SDL_CloseGamepad(in->pads[i]);
    in->npads = 0;
}
