#include <stdio.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "arcade.h"

/* Court: 128×128. AI paddle on top (Y=4), player paddle on bottom (Y=120).
 * Player uses stick X to slide left/right.                                  */
#define PADDLE_W       28
#define PADDLE_H        4
#define PADDLE_Y_AI     4
#define PADDLE_Y_PL   120
#define BALL_SZ         3
#define HUD_Y_TOP      14    /* score is drawn just below the AI paddle's score row */

#define C_BG          RGB565(  8,  12,  20)
#define C_NET         RGB565( 60,  70,  90)
#define C_PADDLE_PL   RGB565( 90, 200, 255)
#define C_PADDLE_AI   RGB565(245,  90, 110)
#define C_BALL        RGB565(255, 240, 200)
#define C_TEXT        RGB565(230, 235, 245)
#define C_DIM         RGB565(120, 130, 150)

static int   s_paddle_pl_x;
static int   s_paddle_ai_x;
static float s_ball_x, s_ball_y;
static float s_ball_vx, s_ball_vy;
static int   s_score_pl, s_score_ai;
static int64_t s_last_us;
static bool  s_serving = true;
static int64_t s_serve_at_us = 0;

static void reset_ball(int towards_player)
{
    s_ball_x  = LCD_WIDTH  / 2.0f - BALL_SZ / 2.0f;
    s_ball_y  = LCD_HEIGHT / 2.0f - BALL_SZ / 2.0f;
    int sign  = towards_player ? 1 : -1;
    float vx  = (rand() % 30 - 15) * 0.04f;       /* −0.6 .. +0.6 px/ms */
    s_ball_vx = vx;
    s_ball_vy = sign * 0.06f;                     /* px/ms */
    s_serving      = true;
    s_serve_at_us  = esp_timer_get_time() + 700000;     /* 0.7 s delay */
}

static void pong_enter(void)
{
    s_paddle_pl_x = (LCD_WIDTH - PADDLE_W) / 2;
    s_paddle_ai_x = (LCD_WIDTH - PADDLE_W) / 2;
    s_score_pl = s_score_ai = 0;
    s_last_us = esp_timer_get_time();
    reset_ball(1);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static bool pong_update(const arcade_input_t *in)
{
    int64_t now = esp_timer_get_time();
    float dt_ms = (now - s_last_us) / 1000.0f;
    if (dt_ms > 50.0f) dt_ms = 50.0f;
    s_last_us = now;

    /* Player paddle follows stick X. Speed scales with deflection magnitude. */
    float pl_v = in->jx * 0.025f;                 /* full deflection ≈ 2.5 px/ms */
    s_paddle_pl_x += (int)(pl_v * dt_ms);
    s_paddle_pl_x = clampi(s_paddle_pl_x, 0, LCD_WIDTH - PADDLE_W);

    /* AI: aim at the ball with a capped speed and a small lag for fairness. */
    int ai_target = (int)s_ball_x + BALL_SZ / 2 - PADDLE_W / 2;
    float ai_speed = 0.07f;                       /* px/ms */
    if (s_paddle_ai_x < ai_target) {
        s_paddle_ai_x += (int)(ai_speed * dt_ms);
        if (s_paddle_ai_x > ai_target) s_paddle_ai_x = ai_target;
    } else if (s_paddle_ai_x > ai_target) {
        s_paddle_ai_x -= (int)(ai_speed * dt_ms);
        if (s_paddle_ai_x < ai_target) s_paddle_ai_x = ai_target;
    }
    s_paddle_ai_x = clampi(s_paddle_ai_x, 0, LCD_WIDTH - PADDLE_W);

    /* Ball motion. */
    if (s_serving) {
        if (now >= s_serve_at_us) s_serving = false;
    } else {
        s_ball_x += s_ball_vx * dt_ms;
        s_ball_y += s_ball_vy * dt_ms;

        if (s_ball_x < 0)                    { s_ball_x = 0;                       s_ball_vx = -s_ball_vx; }
        if (s_ball_x > LCD_WIDTH - BALL_SZ)  { s_ball_x = LCD_WIDTH  - BALL_SZ;    s_ball_vx = -s_ball_vx; }

        /* AI paddle collision (ball moving up). */
        if (s_ball_vy < 0 &&
            s_ball_y <= PADDLE_Y_AI + PADDLE_H &&
            s_ball_y + BALL_SZ >= PADDLE_Y_AI &&
            s_ball_x + BALL_SZ >= s_paddle_ai_x &&
            s_ball_x          <= s_paddle_ai_x + PADDLE_W) {
            s_ball_y = PADDLE_Y_AI + PADDLE_H;
            s_ball_vy = -s_ball_vy;
            float rel = ((s_ball_x + BALL_SZ / 2.0f) - (s_paddle_ai_x + PADDLE_W / 2.0f)) / (PADDLE_W / 2.0f);
            s_ball_vx += rel * 0.05f;
        }
        /* Player paddle collision (ball moving down). */
        if (s_ball_vy > 0 &&
            s_ball_y + BALL_SZ >= PADDLE_Y_PL &&
            s_ball_y           <= PADDLE_Y_PL + PADDLE_H &&
            s_ball_x + BALL_SZ >= s_paddle_pl_x &&
            s_ball_x           <= s_paddle_pl_x + PADDLE_W) {
            s_ball_y = PADDLE_Y_PL - BALL_SZ;
            s_ball_vy = -s_ball_vy;
            float rel = ((s_ball_x + BALL_SZ / 2.0f) - (s_paddle_pl_x + PADDLE_W / 2.0f)) / (PADDLE_W / 2.0f);
            s_ball_vx += rel * 0.05f;
        }
        /* Speed cap. */
        if (s_ball_vx >  0.20f) s_ball_vx =  0.20f;
        if (s_ball_vx < -0.20f) s_ball_vx = -0.20f;
        if (s_ball_vy >  0.18f) s_ball_vy =  0.18f;
        if (s_ball_vy < -0.18f) s_ball_vy = -0.18f;

        /* Out of bounds — score and reserve. */
        if (s_ball_y + BALL_SZ < 0)             { s_score_pl++; reset_ball(1); }
        else if (s_ball_y > LCD_HEIGHT)         { s_score_ai++; reset_ball(0); }
    }

    /* ── Draw ───────────────────────────────────────────────────────── */
    gc9107_fill_screen(C_BG);
    /* Dashed midline */
    for (int x = 4; x < LCD_WIDTH; x += 8) gc9107_fill_rect(x, LCD_HEIGHT / 2, 4, 1, C_NET);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", s_score_ai);
    gc9107_draw_string(4, LCD_HEIGHT / 2 - 12, buf, C_PADDLE_AI, C_BG, 1);
    snprintf(buf, sizeof(buf), "%d", s_score_pl);
    gc9107_draw_string(4, LCD_HEIGHT / 2 + 5,  buf, C_PADDLE_PL, C_BG, 1);

    gc9107_fill_rect(s_paddle_ai_x, PADDLE_Y_AI, PADDLE_W, PADDLE_H, C_PADDLE_AI);
    gc9107_fill_rect(s_paddle_pl_x, PADDLE_Y_PL, PADDLE_W, PADDLE_H, C_PADDLE_PL);
    gc9107_fill_rect((int)s_ball_x, (int)s_ball_y, BALL_SZ, BALL_SZ, C_BALL);

    if (s_serving) {
        arcade_draw_centered(LCD_HEIGHT / 2 - 4, "READY", C_TEXT, C_BG, 1);
    }

    gc9107_flush();
    return true;
}

const arcade_game_t game_pong = {
    .name     = "Pong",
    .on_enter = pong_enter,
    .update   = pong_update,
    .on_exit  = NULL,
};
