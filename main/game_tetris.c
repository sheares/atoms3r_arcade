#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "arcade.h"

/* ── Geometry ─────────────────────────────────────────────────────────── */
#define GRID_W       10
#define GRID_H       16                     /* shorter than canonical (20) but bigger cells */
#define CELL          8
#define FIELD_X       2
#define FIELD_Y       0
#define FIELD_PX_W   (GRID_W * CELL)        /* 80 */
#define FIELD_PX_H   (GRID_H * CELL)        /* 128 — fills the screen height */
#define HUD_X       (FIELD_X + FIELD_PX_W + 4)

#define C_BG         RGB565(  8,  12,  20)
#define C_FIELD      RGB565( 14,  18,  28)
#define C_GRID       RGB565( 22,  26,  38)
#define C_GHOST      RGB565( 60,  72, 100)
#define C_BORDER     RGB565( 70,  82, 110)
#define C_TEXT       RGB565(230, 235, 245)
#define C_DIM        RGB565(120, 130, 150)
#define C_ACCENT     RGB565( 90, 200, 255)
#define C_GAMEOVER_BG RGB565(40, 10, 20)

/* ── Pieces ────────────────────────────────────────────────────────────
 *  Each piece has 4 rotations encoded as a 16-bit bitmap of a 4×4 box.
 *  Row 0 sits in bits 15..12 (top nibble), and within each nibble bit 3
 *  is column 0. Helper: piece_cell(pat, r, c).                            */
static const uint16_t s_pieces[7][4] = {
    /* I */ { 0x0F00, 0x2222, 0x0F00, 0x2222 },
    /* O */ { 0x0660, 0x0660, 0x0660, 0x0660 },
    /* T */ { 0x0E40, 0x04C4, 0x04E0, 0x0464 },
    /* S */ { 0x06C0, 0x0462, 0x06C0, 0x0462 },
    /* Z */ { 0x0C60, 0x0264, 0x0C60, 0x0264 },
    /* J */ { 0x08E0, 0x6440, 0x0E20, 0x44C0 },
    /* L */ { 0x02E0, 0x4460, 0x0E80, 0xC440 },
};

static const uint16_t s_palette[7] = {
    RGB565(  0, 240, 240),    /* I cyan   */
    RGB565(240, 240,   0),    /* O yellow */
    RGB565(170,  60, 230),    /* T purple */
    RGB565( 50, 220,  90),    /* S green  */
    RGB565(245,  90,  90),    /* Z red    */
    RGB565( 60, 120, 245),    /* J blue   */
    RGB565(245, 170,  40),    /* L orange */
};

static inline bool piece_cell(uint16_t pat, int r, int c)
{
    int bit = 15 - r * 4 - c;
    return (pat >> bit) & 1;
}

/* ── State ────────────────────────────────────────────────────────────── */
static uint8_t s_grid[GRID_H][GRID_W];     /* 0 = empty, else piece+1 */
static int     s_cur_type;
static int     s_cur_rot;
static int     s_cur_x, s_cur_y;           /* top-left of the 4×4 bbox in grid coords */
static int     s_next_type;
static int     s_score;
static int     s_lines;
static int     s_level;
static bool    s_over;

static int64_t s_last_fall_us;
static int     s_stick_dx;                 /* current held horizontal dir */
static int64_t s_stick_dx_since_us;
static int64_t s_next_repeat_us;
static bool    s_prev_up;                  /* edge detect for rotate */
static bool    s_prev_jbtn;                /* edge detect for hard drop */

#define DAS_FIRST_US   200000              /* delay before auto-repeat begins */
#define DAS_REPEAT_US   55000              /* time between auto-repeat steps */

/* ── Level / gravity ─────────────────────────────────────────────────── */
static int fall_period_ms(void)
{
    int p = 720 - (s_level - 1) * 70;
    if (p < 90) p = 90;
    return p;
}

/* ── Collision check ─────────────────────────────────────────────────── */
static bool fits(int type, int rot, int x, int y)
{
    uint16_t pat = s_pieces[type][rot & 3];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!piece_cell(pat, r, c)) continue;
            int gx = x + c;
            int gy = y + r;
            if (gx < 0 || gx >= GRID_W) return false;
            if (gy >= GRID_H)            return false;
            if (gy >= 0 && s_grid[gy][gx]) return false;
        }
    }
    return true;
}

/* ── Spawn / lock / line clear ───────────────────────────────────────── */
static void spawn_next(void)
{
    s_cur_type = s_next_type;
    s_cur_rot  = 0;
    s_cur_x    = (GRID_W / 2) - 2;          /* roughly centred */
    s_cur_y    = -1;                        /* a hair above the top */
    s_next_type = rand() % 7;
    if (!fits(s_cur_type, s_cur_rot, s_cur_x, s_cur_y + 1)) {
        s_over = true;
    }
}

static void lock_piece(void)
{
    uint16_t pat = s_pieces[s_cur_type][s_cur_rot];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!piece_cell(pat, r, c)) continue;
            int gx = s_cur_x + c;
            int gy = s_cur_y + r;
            if (gy >= 0 && gy < GRID_H && gx >= 0 && gx < GRID_W) {
                s_grid[gy][gx] = (uint8_t)(s_cur_type + 1);
            }
        }
    }
    /* Find full rows. */
    int cleared = 0;
    for (int r = GRID_H - 1; r >= 0; r--) {
        bool full = true;
        for (int c = 0; c < GRID_W; c++) {
            if (!s_grid[r][c]) { full = false; break; }
        }
        if (full) {
            /* Drop everything above one row. */
            for (int rr = r; rr > 0; rr--) {
                memcpy(s_grid[rr], s_grid[rr - 1], sizeof(s_grid[0]));
            }
            memset(s_grid[0], 0, sizeof(s_grid[0]));
            cleared++;
            r++;                            /* recheck the same row index */
        }
    }
    if (cleared > 0) {
        static const int kBase[] = { 0, 100, 300, 500, 800 };
        s_score += kBase[cleared] * s_level;
        s_lines += cleared;
        int new_level = 1 + s_lines / 10;
        if (new_level > s_level) s_level = new_level;
    }
    spawn_next();
}

static int ghost_y(void)
{
    int gy = s_cur_y;
    while (fits(s_cur_type, s_cur_rot, s_cur_x, gy + 1)) gy++;
    return gy;
}

/* ── Rendering ───────────────────────────────────────────────────────── */
static void draw_cell(int gx, int gy, uint16_t color, bool ghost)
{
    int px = FIELD_X + gx * CELL;
    int py = FIELD_Y + gy * CELL;
    if (ghost) {
        gc9107_draw_rect(px, py, CELL, CELL, color);
    } else {
        gc9107_fill_rect(px,     py,     CELL,     CELL,     color);
        gc9107_fill_rect(px + 1, py + 1, CELL - 2, CELL - 2,
                         /* slight darker inner */
                         RGB565( ((color >> 11) & 0x1F) * 24 / 31 << 3,
                                 ((color >>  5) & 0x3F) * 24 / 31 << 2,
                                 ( color        & 0x1F) * 24 / 31 << 3 ) );
    }
}

static void draw_field(void)
{
    /* Field border + background. */
    gc9107_draw_rect(FIELD_X - 1, FIELD_Y - 1, FIELD_PX_W + 2, FIELD_PX_H + 2, C_BORDER);
    gc9107_fill_rect(FIELD_X, FIELD_Y, FIELD_PX_W, FIELD_PX_H, C_FIELD);

    /* Locked blocks. */
    for (int r = 0; r < GRID_H; r++) {
        for (int c = 0; c < GRID_W; c++) {
            if (s_grid[r][c]) {
                draw_cell(c, r, s_palette[s_grid[r][c] - 1], false);
            }
        }
    }
}

static void draw_piece(int type, int rot, int x, int y, bool ghost)
{
    uint16_t pat = s_pieces[type][rot & 3];
    uint16_t col = ghost ? C_GHOST : s_palette[type];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!piece_cell(pat, r, c)) continue;
            int gy = y + r;
            int gx = x + c;
            if (gy < 0 || gy >= GRID_H) continue;
            if (gx < 0 || gx >= GRID_W) continue;
            draw_cell(gx, gy, col, ghost);
        }
    }
}

static void draw_preview(int type, int x, int y)
{
    /* 4×4 cells × CELL_SMALL=5px = 20px box for the next-piece preview. */
    const int sc = 4;
    uint16_t pat = s_pieces[type][0];
    /* Backdrop to clear stale pixels. */
    gc9107_fill_rect(x - 2, y - 2, 4 * sc + 4, 4 * sc + 4, C_FIELD);
    gc9107_draw_rect(x - 2, y - 2, 4 * sc + 4, 4 * sc + 4, C_GRID);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!piece_cell(pat, r, c)) continue;
            gc9107_fill_rect(x + c * sc, y + r * sc, sc, sc, s_palette[type]);
        }
    }
}

static void draw_hud(void)
{
    /* Clear right column. */
    gc9107_fill_rect(HUD_X - 2, 0, LCD_WIDTH - (HUD_X - 2), LCD_HEIGHT, C_BG);

    gc9107_draw_string(HUD_X, 4, "SCORE", C_DIM, C_BG, 1);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_score);
    gc9107_draw_string(HUD_X, 14, buf, C_TEXT, C_BG, 1);

    gc9107_draw_string(HUD_X, 30, "LV", C_DIM, C_BG, 1);
    snprintf(buf, sizeof(buf), "%d", s_level);
    gc9107_draw_string(HUD_X + 16, 30, buf, C_ACCENT, C_BG, 1);

    gc9107_draw_string(HUD_X, 42, "LINES", C_DIM, C_BG, 1);
    snprintf(buf, sizeof(buf), "%d", s_lines);
    gc9107_draw_string(HUD_X, 52, buf, C_TEXT, C_BG, 1);

    gc9107_draw_string(HUD_X, 70, "NEXT", C_DIM, C_BG, 1);
    draw_preview(s_next_type, HUD_X + 4, 80);
}

static void draw_game_over(void)
{
    int bw = 80, bh = 48;
    int bx = FIELD_X + (FIELD_PX_W - bw) / 2;
    int by = FIELD_Y + (FIELD_PX_H - bh) / 2;
    gc9107_fill_rect(bx, by, bw, bh, C_GAMEOVER_BG);
    gc9107_draw_rect(bx, by, bw, bh, C_BORDER);
    arcade_draw_centered(by +  6, "GAME", C_TEXT, C_GAMEOVER_BG, 1);
    arcade_draw_centered(by + 16, "OVER", C_TEXT, C_GAMEOVER_BG, 1);
    arcade_draw_centered(by + 32, "click", C_DIM, C_GAMEOVER_BG, 1);
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */
static void tetris_reset(void)
{
    memset(s_grid, 0, sizeof(s_grid));
    s_score = 0;
    s_lines = 0;
    s_level = 1;
    s_over  = false;
    s_next_type = rand() % 7;
    spawn_next();
    s_last_fall_us = esp_timer_get_time();
    s_stick_dx = 0;
    s_prev_up = false;
    s_prev_jbtn = false;
}

static void tetris_enter(void)
{
    tetris_reset();
}

static bool tetris_update(const arcade_input_t *in)
{
    int64_t now = esp_timer_get_time();

    if (s_over) {
        if (in->jbtn_pressed || in->atom_btn_pressed) tetris_reset();
        goto draw;
    }

    /* ── Horizontal: DAS/ARR ─────────────────────────────────────────── */
    int want_dx = 0;
    if (in->jx >  30) want_dx =  1;
    if (in->jx < -30) want_dx = -1;

    if (want_dx != s_stick_dx) {
        s_stick_dx = want_dx;
        if (want_dx != 0) {
            if (fits(s_cur_type, s_cur_rot, s_cur_x + want_dx, s_cur_y))
                s_cur_x += want_dx;
            s_next_repeat_us = now + DAS_FIRST_US;
        }
    } else if (s_stick_dx != 0 && now >= s_next_repeat_us) {
        if (fits(s_cur_type, s_cur_rot, s_cur_x + s_stick_dx, s_cur_y))
            s_cur_x += s_stick_dx;
        s_next_repeat_us = now + DAS_REPEAT_US;
    }

    /* ── Rotate: stick up edge ──────────────────────────────────────── */
    bool up_now = (in->jy < -30);
    if (up_now && !s_prev_up) {
        int new_rot = (s_cur_rot + 1) & 3;
        if (fits(s_cur_type, new_rot, s_cur_x, s_cur_y))            s_cur_rot = new_rot;
        else if (fits(s_cur_type, new_rot, s_cur_x - 1, s_cur_y)) { s_cur_rot = new_rot; s_cur_x -= 1; }
        else if (fits(s_cur_type, new_rot, s_cur_x + 1, s_cur_y)) { s_cur_rot = new_rot; s_cur_x += 1; }
        /* poor man's wall kick */
    }
    s_prev_up = up_now;

    /* ── Hard drop: joystick click ──────────────────────────────────── */
    if (in->jbtn_pressed) {
        int gy = ghost_y();
        s_score += 2 * (gy - s_cur_y);
        s_cur_y = gy;
        lock_piece();
        s_last_fall_us = now;
    } else {
        /* ── Gravity (with soft-drop while stick down) ────────────── */
        bool soft = (in->jy > 30);
        int  period_us = (soft ? 60 : fall_period_ms()) * 1000;
        if ((now - s_last_fall_us) >= period_us) {
            s_last_fall_us = now;
            if (fits(s_cur_type, s_cur_rot, s_cur_x, s_cur_y + 1)) {
                s_cur_y++;
                if (soft) s_score += 1;
            } else {
                lock_piece();
            }
        }
    }

draw:
    gc9107_fill_screen(C_BG);
    draw_field();
    if (!s_over) {
        draw_piece(s_cur_type, s_cur_rot, s_cur_x, ghost_y(), true);
        draw_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y,   false);
    }
    draw_hud();
    if (s_over) draw_game_over();
    gc9107_flush();
    return true;
}

const arcade_game_t game_tetris = {
    .name     = "Tetris",
    .on_enter = tetris_enter,
    .update   = tetris_update,
    .on_exit  = NULL,
};
