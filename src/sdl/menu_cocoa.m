/* Native macOS menus for the SDL frontend (menu.h): the model's top-level menus (File, Video,
 * State, Options, Input, Help) become items of the application's menu bar, after the
 * application menu SDL creates.
 *
 * Each menu is refilled from the model when it is about to open (NSMenuDelegate
 * menuNeedsUpdate:), so check marks, greyed items and the dynamic lists always reflect the
 * application's state. A chosen item posts its command (menu_post), which the main loop runs.
 * No key equivalents are set: shortcuts are handled by the main loop from SDL key events, as
 * on the other platforms (Cmd works like Ctrl there), so a key never acts twice.
 *
 * Built without ARC (manual retain/release), as CMake compiles it.
 */
#import <Cocoa/Cocoa.h>

#include "menu.h"

#define MAX_MENUS 32

static MenuQueryFn g_query;
static void *g_user;
static int g_attached;
static NSMutableArray *g_tops; /* the NSMenuItems added to the main menu */
static struct {
    NSMenu *menu;
    const MenuItem *items;
} g_menus[MAX_MENUS];
static int g_nmenus;

@interface ExenMenuTarget : NSObject <NSMenuDelegate>
- (void)choose:(id)sender;
@end

static ExenMenuTarget *g_target;

static NSString *title_of(const char *label)
{
    char plain[300];
    menu_plain_label(label, plain, sizeof plain);
    NSString *s = [NSString stringWithUTF8String:plain];
    return s ? s : @"";
}

static NSMenu *menu_for(const MenuItem *items, const char *label)
{
    for (int i = 0; i < g_nmenus; i++)
        if (g_menus[i].items == items)
            return g_menus[i].menu;
    if (g_nmenus == MAX_MENUS)
        return nil;
    NSMenu *m = [[NSMenu alloc] initWithTitle:title_of(label)]; /* owned by g_menus */
    [m setAutoenablesItems:NO];
    [m setDelegate:g_target];
    g_menus[g_nmenus].menu = m;
    g_menus[g_nmenus].items = items;
    g_nmenus++;
    return m;
}

static void fill(NSMenu *menu, const MenuItem *items)
{
    /* detach the submenus first: each may belong to one item at a time, and is reused */
    for (NSMenuItem *old in [menu itemArray])
        if ([old hasSubmenu])
            [old setSubmenu:nil];
    [menu removeAllItems];
    int last_sep = 1;
    for (const MenuItem *it = items; it->kind != MENU_END; it++) {
        if (it->kind == MENU_SEP) {
            if (!last_sep)
                [menu addItem:[NSMenuItem separatorItem]];
            last_sep = 1;
            continue;
        }
        MenuItemState st;
        menu_item_state(it, g_query, g_user, &st);
        if (st.hidden)
            continue;
        NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:title_of(st.text) action:NULL keyEquivalent:@""];
        if (it->kind == MENU_SUB) {
            NSMenu *sub = menu_for(it->sub, st.text);
            if (sub)
                [mi setSubmenu:sub];
        } else {
            [mi setTarget:g_target];
            [mi setAction:@selector(choose:)];
            [mi setTag:it->cmd];
            [mi setState:st.checked ? NSControlStateValueOn : NSControlStateValueOff];
        }
        [mi setEnabled:st.enabled ? YES : NO];
        [menu addItem:mi];
        [mi release];
        last_sep = 0;
    }
    NSInteger n = [menu numberOfItems];
    if (n > 0 && [[menu itemAtIndex:n - 1] isSeparatorItem])
        [menu removeItemAtIndex:n - 1];
}

@implementation ExenMenuTarget
- (void)choose:(id)sender
{
    menu_post((int)[sender tag]);
}

- (void)menuNeedsUpdate:(NSMenu *)menu
{
    for (int i = 0; i < g_nmenus; i++)
        if (g_menus[i].menu == menu) {
            fill(menu, g_menus[i].items);
            return;
        }
}
@end

int menu_native_attach(SDL_Window *win, MenuQueryFn query, void *user)
{
    (void)win;
    if (g_attached)
        return 0;
    @autoreleasepool {
        NSMenu *bar = [NSApp mainMenu];
        if (!bar)
            return 0;
        g_query = query;
        g_user = user;
        menu_event_type(); /* registered here, on the main thread */
        g_target = [[ExenMenuTarget alloc] init];
        g_tops = [[NSMutableArray alloc] init];
        /* after the application menu (index 0), before SDL's Window menu */
        NSInteger at = [bar numberOfItems] > 0 ? 1 : 0;
        for (const MenuItem *it = menu_bar(); it->kind != MENU_END; it++) {
            NSMenu *sub = menu_for(it->sub, it->label);
            if (!sub)
                continue;
            fill(sub, it->sub);
            NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:title_of(it->label) action:NULL keyEquivalent:@""];
            [top setSubmenu:sub];
            [bar insertItem:top atIndex:at++];
            [g_tops addObject:top];
            [top release];
        }
        g_attached = 1;
    }
    return 1;
}

void menu_native_detach(SDL_Window *win)
{
    (void)win;
    if (!g_attached)
        return;
    @autoreleasepool {
        NSMenu *bar = [NSApp mainMenu];
        for (NSMenuItem *top in g_tops)
            if ([bar indexOfItem:top] >= 0)
                [bar removeItem:top];
        [g_tops release];
        g_tops = nil;
        for (int i = 0; i < g_nmenus; i++) {
            [g_menus[i].menu setDelegate:nil];
            [g_menus[i].menu release];
        }
        g_nmenus = 0;
        [g_target release];
        g_target = nil;
    }
    g_attached = 0;
}

void menu_native_show(SDL_Window *win, int show)
{
    /* the menu bar is not part of the window; macOS hides it in fullscreen by itself */
    (void)win;
    (void)show;
}

void menu_native_fit(SDL_Window *win, int w, int h)
{
    (void)win;
    (void)w;
    (void)h;
}
