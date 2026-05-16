#include <stdio.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "arcade.h"

#define BRICK_COLS    8
#define BRICK_ROWS    5
#define BRICK_W       15
#define BRICK_H        6
#define BRICK_GAP_X    1
#define BRICK_GAP_Y    2
#define BRICK_TOP     14

#define PADDLE_W      30
#define PADDLE_H       4
#define PADDLE_Y     118
#define BALL_SZ        3

#define C_BG          RGB565(  8,  12,  20)
#define C_PADDLE      RGB565( 90, 200, 255)
#define C_BALL        RGB565(255, 240, 200)
#define C_TEXT        RGB565(230, 235, 245)
#define C_DIM         RGB565(120, 130, 150)

static const uint16_t s_brick_palette[BRICK_ROWS] = {
    RGB565(245,  90, 110),
    RGB565(255, 175,  60),
    RGB565(240, 220,  60),
    RGB565( 90, 220, 130),
    RGB565( 90, 200, 255),
};

static uint8_t s_bricks[BRICK_ROWS][BRICK_COLS];
static int     s_remaining_bricks;
static int     s_paddle_x;
static float   s_ball_x, s_ball_y;
static float   s_ball_vx, s_ball_vy;
static int     s_lives;
static int     s_score;
static int64_t s_last_us;
static bool    s_serving;
static bool    s_won;
static int64_t s_serve_at_us;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void reset_layout(void)
{
    s_remaining_bricks = BRICK_ROWS * BRICK_COLS;
    for (int r = 0; r < BRICK_ROWS; r++)
        for (int c = 0; c < BRICK_COLS; c++)
            s_bricks[r][c] = 1;
}

static void serve_ball(void)
{
    s_ball_x = s_paddle_x + PADDLE_W / 2.0f - BALL_SZ / 2.0f;
    s_ball_y = PADDLE_Y - BALL_SZ - 1;
    s_ball_vx = ((rand() % 20) - 10) * 0.005f;    /* small random tilt */
    if (s_ball_vx > -0.02f && s_ball_vx < 0.02f) s_ball_vx = 0.03f;
    s_ball_vy = -0.050f;
    s_serving = true;
    s_serve_at_us = esp_timer_get_time() + 600000;
}

static void breakout_enter(void)
{
    s_paddle_x = (LCD_WIDTH - PADDLE_W) / 2;
    s_lives = 3;
    s_score = 0;
    s_won = false;
    s_last_us = esp_timer_get_time();
    reset_layout();
    serve_ball();
}

static bool brick_collide(void)
{
    int bx = (int)s_ball_x;
    int by = (int)s_ball_y;
    int total_w = BRICK_COLS * BRICK_W + (BRICK_COLS - 1) * BRICK_GAP_X;
    int origin_x = (LCD_WIDTH - total_w) / 2;
    for (int r = 0; r < BRICK_ROWS; r++) {
        int ry = BRICK_TOP + r * (BRICK_H + BRICK_GAP_Y);
        if (by + BALL_SZ < ry || by > ry + BRICK_H) continue;
        for (int c = 0; c < BRICK_COLS; c++) {
            if (!s_bricks[r][c]) continue;
            int rx = origin_x + c * (BRICK_W + BRICK_GAP_X);
            if (bx + BALL_SZ < rx || bx > rx + BRICK_W) continue;
            /* Hit. Decide bounce axis from overlap depths. */
            float over_x = (s_ball_vx > 0) ? (bx + BALL_SZ - rx) : (rx + BRICK_W - bx);
            float over_y = (s_ball_vy > 0) ? (by + BALL_SZ - ry) : (ry + BRICK_H - by);
            if (over_x < over_y) s_ball_vx = -s_ball_vx;
            else                  s_ball_vy = -s_ball_vy;
            s_bricks[r][c] = 0;
            s_remaining_bricks--;
            s_score += 10 + (BRICK_ROWS - r) * 2;
            return true;
        }
    }
    return false;
}

static bool breakout_update(const arcade_input_t *in)
{
    int64_t now = esp_timer_get_time();
    float dt_ms = (now - s_last_us) / 1000.0f;
    if (dt_ms > 50.0f) dt_ms = 50.0f;
    s_last_us = now;

    /* Paddle follows stick X. */
    s_paddle_x += (int)(in->jx * 0.030f * dt_ms);
    s_paddle_x = clampi(s_paddle_x, 0, LCD_WIDTH - PADDLE_W);

    if (s_won) {
        /* Joystick click → reset for a fresh game. */
        if (in->jbtn_pressed) breakout_enter();
        goto draw;
    }

    if (s_serving) {
        s_ball_x = s_paddle_x + PADDLE_W / 2.0f - BALL_SZ / 2.0f;
        if (now >= s_serve_at_us) s_serving = false;
    } else {
        s_ball_x += s_ball_vx * dt_ms;
        s_ball_y += s_ball_vy * dt_ms;

        if (s_ball_x < 0)                  { s_ball_x = 0;                    s_ball_vx = -s_ball_vx; }
        if (s_ball_x > LCD_WIDTH - BALL_SZ){ s_ball_x = LCD_WIDTH - BALL_SZ;  s_ball_vx = -s_ball_vx; }
        if (s_ball_y < 0)                  { s_ball_y = 0;                    s_ball_vy = -s_ball_vy; }

        /* Paddle. */
        if (s_ball_vy > 0 &&
            s_ball_y + BALL_SZ >= PADDLE_Y &&
            s_ball_y           <= PADDLE_Y + PADDLE_H &&
            s_ball_x + BALL_SZ >= s_paddle_x &&
            s_ball_x           <= s_paddle_x + PADDLE_W) {
            s_ball_y = PADDLE_Y - BALL_SZ;
            s_ball_vy = -s_ball_vy;
            float rel = ((s_ball_x + BALL_SZ / 2.0f) - (s_paddle_x + PADDLE_W / 2.0f)) / (PADDLE_W / 2.0f);
            s_ball_vx += rel * 0.03f;
        }
        /* Bricks. */
        brick_collide();

        /* Speed cap — halved. */
        if (s_ball_vx >  0.09f) s_ball_vx =  0.09f;
        if (s_ball_vx < -0.09f) s_ball_vx = -0.09f;
        if (s_ball_vy >  0.09f) s_ball_vy =  0.09f;
        if (s_ball_vy < -0.09f) s_ball_vy = -0.09f;

        if (s_ball_y > LCD_HEIGHT) {
            s_lives--;
            if (s_lives <= 0) {
                /* Game over — re-deal a fresh game. */
                reset_layout();
                s_lives = 3;
                s_score = 0;
            }
            serve_ball();
        }
        if (s_remaining_bricks <= 0) s_won = true;
    }

draw:
    gc9107_fill_screen(C_BG);

    /* HUD */
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", s_score);
    gc9107_draw_string(2, 2, "PTS", C_DIM, C_BG, 1);
    gc9107_draw_string(22, 2, buf, C_TEXT, C_BG, 1);
    snprintf(buf, sizeof(buf), "%d", s_lives);
    gc9107_draw_string(LCD_WIDTH - 30, 2, "LIVES", C_DIM, C_BG, 1);
    gc9107_draw_string(LCD_WIDTH - 8,  2, buf,     C_TEXT, C_BG, 1);

    /* Bricks */
    int total_w = BRICK_COLS * BRICK_W + (BRICK_COLS - 1) * BRICK_GAP_X;
    int origin_x = (LCD_WIDTH - total_w) / 2;
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (!s_bricks[r][c]) continue;
            int rx = origin_x + c * (BRICK_W + BRICK_GAP_X);
            int ry = BRICK_TOP + r * (BRICK_H + BRICK_GAP_Y);
            gc9107_fill_rect(rx, ry, BRICK_W, BRICK_H, s_brick_palette[r]);
        }
    }

    /* Paddle + ball */
    gc9107_fill_rect(s_paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H, C_PADDLE);
    gc9107_fill_rect((int)s_ball_x, (int)s_ball_y, BALL_SZ, BALL_SZ, C_BALL);

    if (s_won) {
        arcade_draw_centered(LCD_HEIGHT / 2 - 4, "CLEARED!", C_TEXT, C_BG, 1);
        arcade_draw_centered(LCD_HEIGHT / 2 + 8, "click to retry", C_DIM, C_BG, 1);
    } else if (s_serving) {
        arcade_draw_centered(LCD_HEIGHT - 24, "READY", C_TEXT, C_BG, 1);
    }

    gc9107_flush();
    return true;
}

const arcade_game_t game_breakout = {
    .name     = "Breakout",
    .on_enter = breakout_enter,
    .update   = breakout_update,
    .on_exit  = NULL,
};
