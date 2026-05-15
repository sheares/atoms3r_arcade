#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_timer.h"
#include "arcade.h"

#define CELL        8
#define GRID_W      (LCD_WIDTH  / CELL)         /* 16 */
#define GRID_H      (LCD_HEIGHT / CELL)         /* 16 */
#define HUD_ROWS    2
#define PLAY_Y0     HUD_ROWS
#define PLAY_W      GRID_W
#define PLAY_H      (GRID_H - HUD_ROWS)         /* 14 */
#define MAX_LEN     (PLAY_W * PLAY_H)
#define STICK_THRES 40

#define C_BG        RGB565(  8,  12,  20)
#define C_HUD       RGB565( 20,  24,  32)
#define C_BORDER    RGB565( 60,  70,  90)
#define C_SNAKE     RGB565( 80, 220, 130)
#define C_HEAD      RGB565(150, 255, 180)
#define C_FOOD      RGB565(245,  90, 110)
#define C_OVER_BG   RGB565( 40,  10,  20)

typedef struct { int8_t x, y; } cell_t;

static cell_t s_snake[MAX_LEN];
static int    s_len;
static int    s_dx, s_dy;
static int    s_next_dx, s_next_dy;
static cell_t s_food;
static int    s_score;
static int    s_best;
static bool   s_over;
static int64_t s_last_tick_us;

static int tick_period_ms(void)
{
    int p = 130 - (s_score * 3);
    return p < 55 ? 55 : p;
}

static bool cell_on_snake(int x, int y)
{
    for (int i = 0; i < s_len; i++)
        if (s_snake[i].x == x && s_snake[i].y == y) return true;
    return false;
}

static void spawn_food(void)
{
    for (int t = 0; t < 200; t++) {
        int x = rand() % PLAY_W;
        int y = rand() % PLAY_H;
        if (!cell_on_snake(x, y)) { s_food.x = x; s_food.y = y; return; }
    }
    for (int y = 0; y < PLAY_H; y++)
        for (int x = 0; x < PLAY_W; x++)
            if (!cell_on_snake(x, y)) { s_food.x = x; s_food.y = y; return; }
}

static void reset_round(void)
{
    s_snake[0] = (cell_t){PLAY_W / 2,     PLAY_H / 2};
    s_snake[1] = (cell_t){PLAY_W / 2 - 1, PLAY_H / 2};
    s_snake[2] = (cell_t){PLAY_W / 2 - 2, PLAY_H / 2};
    s_snake[3] = (cell_t){PLAY_W / 2 - 3, PLAY_H / 2};
    s_len      = 4;
    s_dx       = 1; s_dy      = 0;
    s_next_dx  = 1; s_next_dy = 0;
    s_score    = 0;
    s_over     = false;
    s_last_tick_us = esp_timer_get_time();
    spawn_food();
}

static void apply_input(const arcade_input_t *in)
{
    int wx = 0, wy = 0;
    if (in->jx >  STICK_THRES) wx =  1;
    if (in->jx < -STICK_THRES) wx = -1;
    if (in->jy >  STICK_THRES) wy =  1;
    if (in->jy < -STICK_THRES) wy = -1;
    if (wx && wy) {
        if (abs(in->jx) > abs(in->jy)) wy = 0; else wx = 0;
    }
    if (!wx && !wy) return;
    if (wx == -s_dx && wy == -s_dy) return;       /* no 180° flip */
    s_next_dx = wx;
    s_next_dy = wy;
}

static void tick(void)
{
    if (s_over) return;
    s_dx = s_next_dx;
    s_dy = s_next_dy;

    /* Wrap walls instead of dying — modulo into the play field. */
    int hx = (s_snake[0].x + s_dx + PLAY_W) % PLAY_W;
    int hy = (s_snake[0].y + s_dy + PLAY_H) % PLAY_H;

    bool eating = (hx == s_food.x && hy == s_food.y);
    int self_limit = eating ? s_len : s_len - 1;
    for (int i = 0; i < self_limit; i++) {
        if (s_snake[i].x == hx && s_snake[i].y == hy) {
            s_over = true;
            if (s_score > s_best) s_best = s_score;
            return;
        }
    }

    if (eating && s_len < MAX_LEN) s_len++;
    for (int i = s_len - 1; i > 0; i--) s_snake[i] = s_snake[i - 1];
    s_snake[0].x = hx;
    s_snake[0].y = hy;

    if (eating) {
        s_score++;
        spawn_food();
    }
}

static void draw_hud(void)
{
    gc9107_fill_rect(0, 0, LCD_WIDTH, HUD_ROWS * CELL, C_HUD);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", s_score);
    gc9107_draw_string(  4, 4, "SCORE", A_C_DIM,  C_HUD, 1);
    gc9107_draw_string( 40, 4, buf,     A_C_TEXT, C_HUD, 1);
    snprintf(buf, sizeof(buf), "%d", s_best);
    gc9107_draw_string( 70, 4, "BEST",  A_C_DIM,  C_HUD, 1);
    gc9107_draw_string(102, 4, buf,     A_C_TEXT, C_HUD, 1);
    gc9107_fill_rect(0, HUD_ROWS * CELL - 1, LCD_WIDTH, 1, A_C_BORDER);
}

static void draw_field(void)
{
    gc9107_fill_rect(0, PLAY_Y0 * CELL, LCD_WIDTH, PLAY_H * CELL, C_BG);

    for (int i = 1; i < s_len; i++) {
        gc9107_fill_rect(s_snake[i].x * CELL + 1,
                         (s_snake[i].y + PLAY_Y0) * CELL + 1,
                         CELL - 2, CELL - 2, C_SNAKE);
    }
    if (s_len > 0) {
        gc9107_fill_rect(s_snake[0].x * CELL,
                         (s_snake[0].y + PLAY_Y0) * CELL,
                         CELL, CELL, C_HEAD);
    }
    int fx = s_food.x * CELL;
    int fy = (s_food.y + PLAY_Y0) * CELL;
    gc9107_fill_rect(fx + 1, fy + 1, CELL - 2, CELL - 2, C_FOOD);
    gc9107_fill_rect(fx + 2, fy,             CELL - 4, 1, C_FOOD);
    gc9107_fill_rect(fx + 2, fy + CELL - 1,  CELL - 4, 1, C_FOOD);
    gc9107_fill_rect(fx,             fy + 2, 1, CELL - 4, C_FOOD);
    gc9107_fill_rect(fx + CELL - 1,  fy + 2, 1, CELL - 4, C_FOOD);
}

static void draw_game_over(void)
{
    int bw = 100, bh = 70;
    int bx = (LCD_WIDTH - bw) / 2;
    int by = (LCD_HEIGHT - bh) / 2;
    gc9107_fill_rect(bx, by, bw, bh, C_OVER_BG);
    gc9107_draw_rect(bx, by, bw, bh, A_C_BORDER);
    arcade_draw_centered(by +  8, "GAME", A_C_TEXT, C_OVER_BG, 2);
    arcade_draw_centered(by + 26, "OVER", A_C_TEXT, C_OVER_BG, 2);
    char buf[24];
    snprintf(buf, sizeof(buf), "score %d", s_score);
    arcade_draw_centered(by + 48, buf, A_C_DIM, C_OVER_BG, 1);
    arcade_draw_centered(by + 58, "click to retry", A_C_DIM, C_OVER_BG, 1);
}

/* ── arcade_game_t implementation ─────────────────────────────────────── */

static void snake_enter(void)
{
    s_best = 0;            /* per-session best; persists across retries below */
    reset_round();
}

static bool snake_update(const arcade_input_t *in)
{
    apply_input(in);

    if (s_over) {
        /* Retry on joystick click (short-press AtomS3R btn is reserved for long-press exit). */
        if (in->jbtn_pressed) reset_round();
    }

    int64_t now = esp_timer_get_time();
    if (!s_over && (now - s_last_tick_us) >= (int64_t)tick_period_ms() * 1000) {
        s_last_tick_us = now;
        tick();
    }

    draw_hud();
    draw_field();
    if (s_over) draw_game_over();
    gc9107_flush();

    return true;     /* never self-quit; menu owns exit via long-press */
}

const arcade_game_t game_snake = {
    .name     = "Snake",
    .on_enter = snake_enter,
    .update   = snake_update,
    .on_exit  = NULL,
};
