#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "esp_timer.h"
#include "arcade.h"

#define MAX_ROCKS    12
#define MAX_BULLETS   4
#define ROCK_R_BIG    8
#define ROCK_R_MED    5
#define ROCK_R_SML    3
#define SHIP_R        4
#define BULLET_TTL   900       /* ms */
#define FIRE_COOLDOWN 200      /* ms */

#define C_BG     RGB565(  6,   8,  16)
#define C_STAR   RGB565( 80,  90, 120)
#define C_SHIP   RGB565( 90, 200, 255)
#define C_FLAME  RGB565(255, 175,  60)
#define C_ROCK   RGB565(200, 200, 220)
#define C_BULLET RGB565(255, 240, 200)
#define C_TEXT   RGB565(230, 235, 245)
#define C_DIM    RGB565(120, 130, 150)

typedef struct {
    bool   alive;
    float  x, y, vx, vy;
    uint8_t r;
} body_t;

typedef struct {
    bool   alive;
    float  x, y, vx, vy;
    int    ttl_ms;
} bullet_t;

static float    s_ship_x, s_ship_y;
static float    s_ship_vx, s_ship_vy;
static float    s_ship_a;     /* facing, radians, 0 = pointing up */
static bool     s_thrusting;
static int      s_score;
static int      s_lives;
static int64_t  s_last_us;
static int64_t  s_fire_ready_at_us;
static body_t   s_rocks[MAX_ROCKS];
static bullet_t s_bullets[MAX_BULLETS];

static float frand(void)             { return (rand() / (float)RAND_MAX); }
static float frand_pm(float a)       { return (frand() * 2.0f - 1.0f) * a; }
static float wrapf(float v, float lim){ while (v < 0) v += lim; while (v >= lim) v -= lim; return v; }

static void spawn_rock(int r_px, float speed)
{
    for (int i = 0; i < MAX_ROCKS; i++) {
        if (s_rocks[i].alive) continue;
        s_rocks[i].alive = true;
        /* Spawn at a random screen edge so it drifts in. */
        if (rand() & 1) {
            s_rocks[i].x = (rand() & 1) ? 0 : LCD_WIDTH - 1;
            s_rocks[i].y = frand() * LCD_HEIGHT;
        } else {
            s_rocks[i].x = frand() * LCD_WIDTH;
            s_rocks[i].y = (rand() & 1) ? 0 : LCD_HEIGHT - 1;
        }
        float a = frand() * 6.2831853f;
        s_rocks[i].vx = cosf(a) * speed;
        s_rocks[i].vy = sinf(a) * speed;
        s_rocks[i].r  = r_px;
        return;
    }
}

static void asteroids_enter(void)
{
    s_ship_x = LCD_WIDTH  / 2.0f;
    s_ship_y = LCD_HEIGHT / 2.0f;
    s_ship_vx = s_ship_vy = 0;
    s_ship_a  = 0;
    s_thrusting = false;
    s_score = 0;
    s_lives = 3;
    s_last_us = esp_timer_get_time();
    s_fire_ready_at_us = 0;
    for (int i = 0; i < MAX_ROCKS; i++)  s_rocks[i].alive = false;
    for (int i = 0; i < MAX_BULLETS; i++) s_bullets[i].alive = false;
    for (int i = 0; i < 4; i++) spawn_rock(ROCK_R_BIG, 0.014f + frand() * 0.010f);
}

static void split_rock(body_t *src)
{
    /* Big → two mediums. Medium → nothing (mediums are the final tier). */
    if (src->r != ROCK_R_BIG) return;
    int   newr = ROCK_R_MED;
    float spd  = 0.028f;
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < MAX_ROCKS; i++) {
            if (s_rocks[i].alive) continue;
            s_rocks[i].alive = true;
            s_rocks[i].x = src->x;
            s_rocks[i].y = src->y;
            float a = frand() * 6.2831853f;
            s_rocks[i].vx = cosf(a) * spd;
            s_rocks[i].vy = sinf(a) * spd;
            s_rocks[i].r  = newr;
            break;
        }
    }
}

static void fire_bullet(void)
{
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (s_bullets[i].alive) continue;
        s_bullets[i].alive = true;
        float dx = sinf(s_ship_a);
        float dy = -cosf(s_ship_a);
        s_bullets[i].x = s_ship_x + dx * SHIP_R;
        s_bullets[i].y = s_ship_y + dy * SHIP_R;
        s_bullets[i].vx = dx * 0.15f + s_ship_vx * 0.5f;
        s_bullets[i].vy = dy * 0.15f + s_ship_vy * 0.5f;
        s_bullets[i].ttl_ms = BULLET_TTL;
        return;
    }
}

static void draw_ship(int cx, int cy, float a)
{
    /* Tiny triangle with the nose along the heading vector. */
    float c = cosf(a), s = sinf(a);
    int nx = cx + (int)(s * 6);
    int ny = cy - (int)(c * 6);
    int lx = cx + (int)((-c - s) * 4);
    int ly = cy + (int)((-s + c) * 4);
    int rx = cx + (int)(( c - s) * 4);
    int ry = cy + (int)(( s + c) * 4);
    /* Bresenham lines using draw_pixel — quick and dirty. */
    for (int t = 0; t <= 16; t++) {
        int x = nx + (lx - nx) * t / 16;
        int y = ny + (ly - ny) * t / 16;
        gc9107_draw_pixel(x, y, C_SHIP);
        x = nx + (rx - nx) * t / 16;
        y = ny + (ry - ny) * t / 16;
        gc9107_draw_pixel(x, y, C_SHIP);
        x = lx + (rx - lx) * t / 16;
        y = ly + (ry - ly) * t / 16;
        gc9107_draw_pixel(x, y, C_SHIP);
    }
    if (s_thrusting) {
        int fx = cx - (int)(s * 5);
        int fy = cy + (int)(c * 5);
        gc9107_fill_rect(fx - 1, fy - 1, 3, 3, C_FLAME);
    }
}

static bool asteroids_update(const arcade_input_t *in)
{
    int64_t now = esp_timer_get_time();
    float dt_ms = (now - s_last_us) / 1000.0f;
    if (dt_ms > 50.0f) dt_ms = 50.0f;
    s_last_us = now;

    /* Stick X = rotate, stick Y forward = thrust. */
    float rot_speed = 0.005f;             /* rad/ms at full deflection */
    s_ship_a += (in->jx / 100.0f) * rot_speed * dt_ms;

    s_thrusting = (in->jy < -40);
    if (s_thrusting) {
        s_ship_vx +=  sinf(s_ship_a) * 0.0007f * dt_ms;
        s_ship_vy += -cosf(s_ship_a) * 0.0007f * dt_ms;
    }
    /* Friction so it eventually slows. */
    s_ship_vx *= 0.997f;
    s_ship_vy *= 0.997f;

    s_ship_x = wrapf(s_ship_x + s_ship_vx * dt_ms, LCD_WIDTH);
    s_ship_y = wrapf(s_ship_y + s_ship_vy * dt_ms, LCD_HEIGHT);

    /* Three ways to fire — all gated by the cooldown:
     *   joystick click (single shot edge)
     *   AtomS3R button tap (single shot edge — long-press still exits via shell)
     *   stick-down held (rapid fire)                                         */
    bool want_fire = in->jbtn_pressed || in->atom_btn_pressed || (in->jy > 40);
    if (want_fire && now >= s_fire_ready_at_us) {
        fire_bullet();
        s_fire_ready_at_us = now + FIRE_COOLDOWN * 1000;
    }

    /* Move bullets. */
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!s_bullets[i].alive) continue;
        s_bullets[i].x = wrapf(s_bullets[i].x + s_bullets[i].vx * dt_ms, LCD_WIDTH);
        s_bullets[i].y = wrapf(s_bullets[i].y + s_bullets[i].vy * dt_ms, LCD_HEIGHT);
        s_bullets[i].ttl_ms -= (int)dt_ms;
        if (s_bullets[i].ttl_ms <= 0) s_bullets[i].alive = false;
    }

    /* Move rocks. */
    int alive_rocks = 0;
    for (int i = 0; i < MAX_ROCKS; i++) {
        if (!s_rocks[i].alive) continue;
        alive_rocks++;
        s_rocks[i].x = wrapf(s_rocks[i].x + s_rocks[i].vx * dt_ms, LCD_WIDTH);
        s_rocks[i].y = wrapf(s_rocks[i].y + s_rocks[i].vy * dt_ms, LCD_HEIGHT);
    }
    /* Spawn fresh wave if cleared. */
    if (alive_rocks == 0) {
        for (int i = 0; i < 4; i++) spawn_rock(ROCK_R_BIG, 0.014f + frand() * 0.012f);
    }

    /* Bullet/rock collisions. */
    for (int b = 0; b < MAX_BULLETS; b++) {
        if (!s_bullets[b].alive) continue;
        for (int r = 0; r < MAX_ROCKS; r++) {
            if (!s_rocks[r].alive) continue;
            float dx = s_bullets[b].x - s_rocks[r].x;
            float dy = s_bullets[b].y - s_rocks[r].y;
            float rad = s_rocks[r].r + 1;
            if (dx*dx + dy*dy <= rad*rad) {
                s_bullets[b].alive = false;
                s_score += (s_rocks[r].r == ROCK_R_BIG) ? 25 : 75;
                split_rock(&s_rocks[r]);
                s_rocks[r].alive = false;
                break;
            }
        }
    }

    /* Ship/rock collision. */
    for (int r = 0; r < MAX_ROCKS; r++) {
        if (!s_rocks[r].alive) continue;
        float dx = s_ship_x - s_rocks[r].x;
        float dy = s_ship_y - s_rocks[r].y;
        float rad = s_rocks[r].r + SHIP_R - 1;
        if (dx*dx + dy*dy <= rad*rad) {
            s_lives--;
            if (s_lives <= 0) {
                /* Quick restart. */
                asteroids_enter();
                return true;
            }
            s_ship_x = LCD_WIDTH / 2.0f;
            s_ship_y = LCD_HEIGHT / 2.0f;
            s_ship_vx = s_ship_vy = 0;
            break;
        }
    }

    /* ── Draw ───────────────────────────────────────────────────────── */
    gc9107_fill_screen(C_BG);
    /* A few static "stars" */
    for (int i = 0; i < 12; i++) {
        int sx = (i * 17 + 7) % LCD_WIDTH;
        int sy = (i * 41 + 13) % LCD_HEIGHT;
        gc9107_draw_pixel(sx, sy, C_STAR);
    }

    for (int r = 0; r < MAX_ROCKS; r++) {
        if (!s_rocks[r].alive) continue;
        gc9107_fill_circle((int)s_rocks[r].x, (int)s_rocks[r].y, s_rocks[r].r, C_ROCK);
        gc9107_fill_circle((int)s_rocks[r].x, (int)s_rocks[r].y, s_rocks[r].r - 2, C_BG);
    }
    for (int b = 0; b < MAX_BULLETS; b++) {
        if (!s_bullets[b].alive) continue;
        gc9107_fill_rect((int)s_bullets[b].x - 1, (int)s_bullets[b].y - 1, 2, 2, C_BULLET);
    }
    draw_ship((int)s_ship_x, (int)s_ship_y, s_ship_a);

    /* HUD */
    char buf[16];
    snprintf(buf, sizeof(buf), "PTS %d", s_score);
    gc9107_draw_string(2, 2, buf, C_TEXT, C_BG, 1);
    snprintf(buf, sizeof(buf), "x%d", s_lives);
    gc9107_draw_string(LCD_WIDTH - 16, 2, buf, C_TEXT, C_BG, 1);

    gc9107_flush();
    return true;
}

const arcade_game_t game_asteroids = {
    .name     = "Asteroids",
    .on_enter = asteroids_enter,
    .update   = asteroids_update,
    .on_exit  = NULL,
};
