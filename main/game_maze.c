#include <string.h>
#include <stdio.h>

#include "esp_timer.h"
#include "esp_random.h"
#include "arcade.h"
#include "maze.h"

#define STICK_DEAD         25
#define ANIM_BOOST_FRAMES  4
#define ANIM_NORMAL_FRAMES 8

#define BALL_R             4
#define WALL_COLOR         COLOR_BLACK
#define FLOOR_COLOR        COLOR_WHITE
#define BALL_COLOR         COLOR_RED
#define BALL_HIGHLIGHT     COLOR_YELLOW
#define PICKUP_COLOR       COLOR_BLUE
#define COUNTER_COLOR      COLOR_YELLOW

#define BOOST_DURATION_US  3000000
#define PICKUP_COUNT       20
#define PICKUP_R           2

static int s_br, s_bc;          /* current cell */
static int s_tr, s_tc;          /* target cell while animating */
static int s_anim_step = 0;
static int s_px, s_py;
static int s_view_x, s_view_y;
static uint8_t s_pickup[MAZE_H][MAZE_W];
static int s_collected = 0;
static int64_t s_boost_until_us = 0;
static bool s_jbtn_prev = false;
static int64_t s_jbtn_down_us = 0;

static int absi(int v) { return v < 0 ? -v : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int anim_frames(void)
{
    return (esp_timer_get_time() < s_boost_until_us) ? ANIM_BOOST_FRAMES : ANIM_NORMAL_FRAMES;
}

static void spawn_pickups(void)
{
    memset(s_pickup, 0, sizeof(s_pickup));
    int placed = 0;
    int safety = 0;
    while (placed < PICKUP_COUNT && safety < PICKUP_COUNT * 10) {
        safety++;
        int r = esp_random() % MAZE_H;
        int c = esp_random() % MAZE_W;
        if (s_pickup[r][c]) continue;
        if (r == MAZE_H / 2 && c == MAZE_W / 2) continue;
        s_pickup[r][c] = 1;
        placed++;
    }
}

static void try_collect(void)
{
    if (s_pickup[s_br][s_bc]) {
        s_pickup[s_br][s_bc] = 0;
        s_collected++;
        int64_t now = esp_timer_get_time();
        if (s_boost_until_us < now) s_boost_until_us = now;
        s_boost_until_us += BOOST_DURATION_US;
    }
}

static void pick_direction(const arcade_input_t *in, int *dr, int *dc)
{
    *dr = *dc = 0;
    int ax = absi(in->jx), ay = absi(in->jy);
    if (ax < STICK_DEAD && ay < STICK_DEAD) return;
    /* Rotated 90° CW from native — matches sheares/atoms3r_maze convention. */
    if (ay > ax)  *dr = (in->jy > 0) ?  1 : -1;
    else          *dc = (in->jx > 0) ?  1 : -1;
}

static int max_view_x(void) { return MAZE_W * CELL_PX - LCD_WIDTH;  }
static int max_view_y(void) { return MAZE_H * CELL_PX - LCD_HEIGHT; }

static void compute_view(void)
{
    s_view_x = clampi(s_px - LCD_WIDTH  / 2, 0, max_view_x());
    s_view_y = clampi(s_py - LCD_HEIGHT / 2, 0, max_view_y());
}

static void update_ball_pixel(void)
{
    int sx = s_bc * CELL_PX + CELL_PX / 2;
    int sy = s_br * CELL_PX + CELL_PX / 2;
    int tx = s_tc * CELL_PX + CELL_PX / 2;
    int ty = s_tr * CELL_PX + CELL_PX / 2;
    int af = anim_frames();
    if (s_anim_step > 0) {
        s_px = sx + (tx - sx) * s_anim_step / af;
        s_py = sy + (ty - sy) * s_anim_step / af;
    } else {
        s_px = sx;
        s_py = sy;
    }
}

static void draw_world(void)
{
    gc9107_fill_screen(FLOOR_COLOR);
    int first_c = s_view_x / CELL_PX;
    int last_c  = (s_view_x + LCD_WIDTH  - 1) / CELL_PX;
    int first_r = s_view_y / CELL_PX;
    int last_r  = (s_view_y + LCD_HEIGHT - 1) / CELL_PX;
    if (last_c >= MAZE_W) last_c = MAZE_W - 1;
    if (last_r >= MAZE_H) last_r = MAZE_H - 1;

    for (int r = first_r; r <= last_r; r++) {
        for (int c = first_c; c <= last_c; c++) {
            const maze_cell_t *cell = maze_cell(r, c);
            if (!cell) continue;
            int x = c * CELL_PX - s_view_x;
            int y = r * CELL_PX - s_view_y;
            if (cell->walls & WALL_N) gc9107_draw_hline(x, y, CELL_PX, WALL_COLOR);
            if (cell->walls & WALL_W) gc9107_draw_vline(x, y, CELL_PX, WALL_COLOR);
        }
    }
    if (last_c == MAZE_W - 1)
        gc9107_draw_vline(MAZE_W * CELL_PX - s_view_x - 1, 0, LCD_HEIGHT, WALL_COLOR);
    if (last_r == MAZE_H - 1)
        gc9107_draw_hline(0, MAZE_H * CELL_PX - s_view_y - 1, LCD_WIDTH, WALL_COLOR);
}

static void draw_pickups(void)
{
    int first_c = s_view_x / CELL_PX;
    int last_c  = (s_view_x + LCD_WIDTH  - 1) / CELL_PX;
    int first_r = s_view_y / CELL_PX;
    int last_r  = (s_view_y + LCD_HEIGHT - 1) / CELL_PX;
    if (last_c >= MAZE_W) last_c = MAZE_W - 1;
    if (last_r >= MAZE_H) last_r = MAZE_H - 1;
    for (int r = first_r; r <= last_r; r++) {
        for (int c = first_c; c <= last_c; c++) {
            if (!s_pickup[r][c]) continue;
            int cx = c * CELL_PX + CELL_PX / 2 - s_view_x;
            int cy = r * CELL_PX + CELL_PX / 2 - s_view_y;
            gc9107_fill_rect(cx - PICKUP_R, cy - PICKUP_R,
                             PICKUP_R * 2, PICKUP_R * 2, PICKUP_COLOR);
        }
    }
}

static void draw_ball(int cx, int cy)
{
    gc9107_fill_rect(cx - BALL_R, cy - BALL_R, BALL_R * 2, BALL_R * 2, BALL_COLOR);
    gc9107_draw_pixel(cx - 1, cy - 1, BALL_HIGHLIGHT);
}

static void draw_counter(void)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "x%d", s_collected);
    int box_w = (int)strlen(buf) * 6 + 4;
    int box_h = 10;
    int x = LCD_WIDTH - box_w - 2;
    int y = 2;
    gc9107_fill_rect(x, y, box_w, box_h, COLOR_BLACK);
    gc9107_draw_string(x + 2, y + 1, buf, COUNTER_COLOR, COLOR_BLACK, 1);
}

static void regen(void)
{
    maze_generate(0);
    spawn_pickups();
    s_collected = 0;
    s_boost_until_us = 0;
    s_br = s_tr = MAZE_H / 2;
    s_bc = s_tc = MAZE_W / 2;
    s_anim_step = 0;
}

/* ── arcade_game_t hooks ──────────────────────────────────────────────── */

static void maze_enter(void)
{
    regen();
    s_jbtn_prev = false;
    s_jbtn_down_us = 0;
}

static bool maze_update(const arcade_input_t *in)
{
    int64_t now = esp_timer_get_time();

    /* Hold the joystick stick click for 0.5 s to regenerate the maze. */
    if (in->jbtn_down && !s_jbtn_prev) s_jbtn_down_us = now;
    if (in->jbtn_down) {
        if ((now - s_jbtn_down_us) > 500000) {
            regen();
            s_jbtn_down_us = now + 1000000000LL;     /* one-shot per press */
        }
    } else {
        s_jbtn_down_us = 0;
    }
    s_jbtn_prev = in->jbtn_down;

    /* Accept a new direction only when not animating. */
    if (s_anim_step == 0) {
        int dr, dc;
        pick_direction(in, &dr, &dc);
        if ((dr || dc) && maze_can_move(s_br, s_bc, dr, dc)) {
            s_tr = s_br + dr;
            s_tc = s_bc + dc;
            s_anim_step = 1;
        }
    }

    if (s_anim_step > 0) {
        s_anim_step++;
        if (s_anim_step >= anim_frames()) {
            s_br = s_tr;
            s_bc = s_tc;
            s_anim_step = 0;
            try_collect();
        }
    }

    update_ball_pixel();
    compute_view();
    draw_world();
    draw_pickups();
    draw_ball(s_px - s_view_x, s_py - s_view_y);
    draw_counter();
    gc9107_flush();

    return true;
}

const arcade_game_t game_maze = {
    .name     = "Tilt Maze",
    .on_enter = maze_enter,
    .update   = maze_update,
    .on_exit  = NULL,
};
