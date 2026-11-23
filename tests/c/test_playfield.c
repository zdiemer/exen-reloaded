/* PlayField natives whose original behaviour was buggy and is fixed in exen-reloaded:
 * moveTiles (a real block move, overlap safe) and the z sort of the sprite list (stable, and
 * every neighbour relinked). Built on the real framework classes; no game needed. */
#include <string.h>

#include "platform.h"
#include "support.h"
#include "test.h"

enum { PF_STATE, PF_CHARSET, PF_ANIMTILEINDEX, PF_ANIMTILEMAX, PF_CHARW, PF_CHARH, PF_BACKGROUND,
       PF_BGW, PF_BGH, PF_NBBITS, PF_VIEWX, PF_VIEWY, PF_VIEWW, PF_VIEWH, PF_FIRST, PF_LAST };
enum { SP_ANIM, SP_FRAME, SP_X, SP_Y, SP_Z, SP_VISIBLE, SP_TRANSFORM, SP_NEXT, SP_PREV };

#define W 8
#define H 6

static ExenVM *setup(void)
{
    ExenVM *vm = t_vm_framework(0);
    CHECK(vm != NULL);
    CHECK(exen_finalize_classes(vm) == 0);
    CHECK(heap_init(vm) == 0);
    return vm;
}

static ref_t new_playfield(ExenVM *vm)
{
    int cls = plat_class(vm, PC_PLAYFIELD);
    CHECK(cls >= 0);
    ref_t pf = vm_new_object(vm, cls);
    root_push(vm, pf);
    ref_t bg = vm_new_array(vm, T_BYTE, 0, W * H);
    uint32_t *f = inst(vm, pf, cls);
    f[PF_BACKGROUND] = bg;
    f[PF_BGW] = W;
    f[PF_BGH] = H;
    f[PF_NBBITS] = 8;
    f[PF_ANIMTILEMAX] = 0;
    for (int i = 0; i < W * H; i++)
        arr_data(vm, bg)[i] = (uint8_t)i; /* cell (x, y) = y * W + x */
    return pf;
}

static uint8_t cell(ExenVM *vm, ref_t pf, int x, int y)
{
    uint32_t *f = inst(vm, pf, plat_class(vm, PC_PLAYFIELD));
    return arr_data(vm, f[PF_BACKGROUND])[y * W + x];
}

static void move_tiles(ExenVM *vm, ref_t pf, int x1, int y1, int x2, int y2, int w, int h)
{
    uint32_t a[7] = {pf, (uint32_t)x1, (uint32_t)y1, (uint32_t)x2, (uint32_t)y2, (uint32_t)w,
                     (uint32_t)h};
    natives_call(vm, 47, a);
}

static void test_move_tiles(void)
{
    ExenVM *vm = setup();
    ref_t pf = new_playfield(vm);
    uint8_t before[H][W];
    /* disjoint block: 2x2 at (5,3) to (0,0) */
    move_tiles(vm, pf, 0, 0, 5, 3, 2, 2);
    CHECK_EQ(cell(vm, pf, 0, 0), 3 * W + 5);
    CHECK_EQ(cell(vm, pf, 1, 0), 3 * W + 6);
    CHECK_EQ(cell(vm, pf, 0, 1), 4 * W + 5);
    CHECK_EQ(cell(vm, pf, 1, 1), 4 * W + 6);
    CHECK_EQ(cell(vm, pf, 2, 0), 2); /* outside the block: untouched */
    CHECK_EQ(cell(vm, pf, 5, 3), 3 * W + 5); /* the source stays (a copy, as the original) */

    /* overlapping moves in all four directions must equal a move through a temporary copy */
    static const int dirs[4][2] = {{1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
    for (int d = 0; d < 4; d++) {
        uint32_t *f = inst(vm, pf, plat_class(vm, PC_PLAYFIELD));
        for (int i = 0; i < W * H; i++)
            arr_data(vm, f[PF_BACKGROUND])[i] = (uint8_t)i;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                before[y][x] = cell(vm, pf, x, y);
        int x2 = 2, y2 = 2, x1 = x2 + dirs[d][0], y1 = y2 + dirs[d][1];
        move_tiles(vm, pf, x1, y1, x2, y2, 4, 3);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int in = x >= x1 && x < x1 + 4 && y >= y1 && y < y1 + 3;
                int want = in ? before[y - y1 + y2][x - x1 + x2] : before[y][x];
                if (cell(vm, pf, x, y) != want) {
                    CHECK_EQ(cell(vm, pf, x, y), want);
                    goto next;
                }
            }
    next:;
    }

    /* clipping: a block hanging off the right and bottom edges moves only what fits */
    uint32_t *f = inst(vm, pf, plat_class(vm, PC_PLAYFIELD));
    for (int i = 0; i < W * H; i++)
        arr_data(vm, f[PF_BACKGROUND])[i] = (uint8_t)i;
    move_tiles(vm, pf, 6, 4, 0, 0, 5, 5);
    CHECK_EQ(cell(vm, pf, 6, 4), 0);
    CHECK_EQ(cell(vm, pf, 7, 5), W + 1);
    CHECK(!vm->halted);
    exen_destroy(vm);
}

static ref_t new_sprite(ExenVM *vm, int32_t z)
{
    int cs = plat_class(vm, PC_SPRITE);
    CHECK(cs >= 0);
    ref_t s = vm_new_object(vm, cs);
    root_push(vm, s);
    inst(vm, s, cs)[SP_Z] = (uint32_t)z;
    return s;
}

static void link_list(ExenVM *vm, ref_t pf, const ref_t *s, int n)
{
    int cs = plat_class(vm, PC_SPRITE);
    uint32_t *f = inst(vm, pf, plat_class(vm, PC_PLAYFIELD));
    for (int i = 0; i < n; i++) {
        inst(vm, s[i], cs)[SP_PREV] = i ? s[i - 1] : 0;
        inst(vm, s[i], cs)[SP_NEXT] = i + 1 < n ? s[i + 1] : 0;
    }
    f[PF_FIRST] = s[0];
    f[PF_LAST] = s[n - 1];
}

/* Walk the list both ways: every sprite present once, z ascending, prev/next consistent. */
static void check_sorted(ExenVM *vm, ref_t pf, const ref_t *s, int n, const int *order)
{
    int cs = plat_class(vm, PC_SPRITE);
    uint32_t *f = inst(vm, pf, plat_class(vm, PC_PLAYFIELD));
    ref_t cur = f[PF_FIRST], prev = 0;
    for (int i = 0; i < n; i++) {
        CHECK_EQ(cur, s[order[i]]);
        CHECK_EQ(inst(vm, cur, cs)[SP_PREV], prev);
        prev = cur;
        cur = inst(vm, cur, cs)[SP_NEXT];
    }
    CHECK_EQ(cur, 0);
    CHECK_EQ(f[PF_LAST], s[order[n - 1]]);
}

static void test_sort(void)
{
    ExenVM *vm = setup();
    ref_t pf = new_playfield(vm);
    uint32_t *f = inst(vm, pf, plat_class(vm, PC_PLAYFIELD));

    /* The case that broke the original: A(z0), B(z5), C(z3) lost C from the list. */
    ref_t abc[3] = {new_sprite(vm, 0), new_sprite(vm, 5), new_sprite(vm, 3)};
    link_list(vm, pf, abc, 3);
    playfield_sort_sprites(vm, f);
    check_sorted(vm, pf, abc, 3, (const int[]){0, 2, 1});

    /* A longer list with ties: stable (equal z keep their order), all relinked. */
    static const int32_t zs[8] = {7, 3, 3, -2, 9, 3, 0, 7};
    ref_t s[8];
    for (int i = 0; i < 8; i++)
        s[i] = new_sprite(vm, zs[i]);
    link_list(vm, pf, s, 8);
    playfield_sort_sprites(vm, f);
    check_sorted(vm, pf, s, 8, (const int[]){3, 6, 1, 2, 5, 0, 7, 4});

    /* Already sorted and single-element lists are left as they are. */
    playfield_sort_sprites(vm, f);
    check_sorted(vm, pf, s, 8, (const int[]){3, 6, 1, 2, 5, 0, 7, 4});
    ref_t one[1] = {new_sprite(vm, 4)};
    link_list(vm, pf, one, 1);
    playfield_sort_sprites(vm, f);
    check_sorted(vm, pf, one, 1, (const int[]){0});
    exen_destroy(vm);
}

int main(void)
{
    RUN(test_move_tiles);
    RUN(test_sort);
    TEST_MAIN_END();
}
