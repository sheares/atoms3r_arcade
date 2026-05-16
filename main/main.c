#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "arcade.h"
#include "gc9107.h"
#include "lp5562.h"
#include "joystick2.h"
#include "font.h"

static const char *TAG = "arcade";

#define BTN_PIN             41         /* AtomS3R built-in button */
#define STICK_DEAD          30
#define BACK_HOLD_US        700000     /* 0.7 s long-press AtomS3R btn = back to menu */
#define MENU_REPEAT_FIRST   400000     /* first repeat after holding stick this long */
#define MENU_REPEAT_NEXT    160000     /* subsequent repeats */

/* ── External game modules ────────────────────────────────────────────── */
extern const arcade_game_t game_snake;
extern const arcade_game_t game_maze;
extern const arcade_game_t game_pong;
extern const arcade_game_t game_breakout;
extern const arcade_game_t game_asteroids;
extern const arcade_game_t game_tetris;

static const arcade_game_t *const s_games[] = {
    &game_snake,
    &game_tetris,
    &game_maze,
    &game_pong,
    &game_breakout,
    &game_asteroids,
};
#define GAME_COUNT ((int)(sizeof(s_games) / sizeof(s_games[0])))

/* ── Helpers exposed via arcade.h ─────────────────────────────────────── */
void arcade_draw_centered(int y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    int w = (int)strlen(s) * 6 * scale;
    int x = (LCD_WIDTH - w) / 2;
    if (x < 0) x = 0;
    gc9107_draw_string(x, y, s, fg, bg, scale);
}

/* Render text without painting glyph backgrounds — letters show against whatever
 * was already drawn underneath. Used for rainbow / busy backdrops. */
void arcade_draw_text_overlay(int x, int y, const char *str, uint16_t fg, uint8_t scale)
{
    while (*str) {
        char c = *str++;
        if (c < FONT_FIRST || c > FONT_LAST) c = '?';
        const uint8_t *glyph = font5x8[c - FONT_FIRST];
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint8_t col_data = glyph[col];
            for (int row = 0; row < FONT_HEIGHT; row++) {
                if (col_data & (1 << row)) {
                    if (scale == 1) gc9107_draw_pixel(x + col, y + row, fg);
                    else gc9107_fill_rect(x + col * scale, y + row * scale, scale, scale, fg);
                }
            }
        }
        x += (FONT_WIDTH + 1) * scale;
    }
}

/* ── Input state machine ──────────────────────────────────────────────── */
static int  s_selected = 0;

static void read_input(arcade_input_t *out)
{
    static bool prev_jbtn = false;
    static bool prev_atombtn = false;

    joystick2_data_t j = {0};
    bool js_ok = (joystick2_read(&j) == ESP_OK);

    out->jx = js_ok ? j.x : 0;
    out->jy = js_ok ? j.y : 0;
    /* Apply deadzone here so games get clean "no input" zeros. */
    if (out->jx > -STICK_DEAD && out->jx < STICK_DEAD) out->jx = 0;
    if (out->jy > -STICK_DEAD && out->jy < STICK_DEAD) out->jy = 0;

    bool jbtn = js_ok && j.btn;
    out->jbtn_down     = jbtn;
    out->jbtn_pressed  = jbtn && !prev_jbtn;
    out->jbtn_released = !jbtn && prev_jbtn;
    prev_jbtn = jbtn;

    bool abtn = (gpio_get_level(BTN_PIN) == 0);
    out->atom_btn_down     = abtn;
    out->atom_btn_pressed  = abtn && !prev_atombtn;
    out->atom_btn_released = !abtn && prev_atombtn;
    prev_atombtn = abtn;
}

/* ── Menu rendering ───────────────────────────────────────────────────── */
static void render_menu(void)
{
    gc9107_fill_screen(A_C_BG);

    /* Title bar */
    gc9107_fill_rect(0, 0, LCD_WIDTH, 20, A_C_PANEL);
    gc9107_fill_rect(0, 19, LCD_WIDTH, 1, A_C_BORDER);
    arcade_draw_centered(6, "ARCADE", A_C_ACCENT, A_C_PANEL, 1);

    /* Game list — fits up to ~7 rows below the title bar. */
    int row_h = 17;
    int top   = 22;
    for (int i = 0; i < GAME_COUNT; i++) {
        int y = top + i * row_h;
        bool is_sel = (i == s_selected);
        uint16_t bg = is_sel ? A_C_PANEL : A_C_BG;
        uint16_t fg = is_sel ? A_C_ACCENT : A_C_TEXT;
        gc9107_fill_rect(4, y, LCD_WIDTH - 8, row_h - 2, bg);
        if (is_sel) {
            gc9107_fill_rect(4, y, 3, row_h - 2, A_C_ACCENT);
        }
        gc9107_draw_string(12, y + 4, s_games[i]->name, fg, bg, 1);
    }
}

/* Returns true when a game has been picked. */
static bool menu_loop(void)
{
    int64_t last_step_us = 0;
    int     step_dir     = 0;        /* -1, 0, +1 — direction currently held */

    for (;;) {
        arcade_input_t in;
        read_input(&in);

        /* Stick up/down navigates with repeat. */
        int now_dir = 0;
        if (in.jy < -STICK_DEAD) now_dir = -1;
        else if (in.jy > STICK_DEAD) now_dir = +1;

        int64_t now = esp_timer_get_time();
        if (now_dir != step_dir) {
            /* Direction changed (or released) — fire immediately on new push. */
            step_dir = now_dir;
            if (now_dir != 0) {
                s_selected += now_dir;
                if (s_selected < 0) s_selected = GAME_COUNT - 1;
                if (s_selected >= GAME_COUNT) s_selected = 0;
                last_step_us = now + MENU_REPEAT_FIRST;
            }
        } else if (now_dir != 0 && now > last_step_us) {
            s_selected += now_dir;
            if (s_selected < 0) s_selected = GAME_COUNT - 1;
            if (s_selected >= GAME_COUNT) s_selected = 0;
            last_step_us = now + MENU_REPEAT_NEXT;
        }

        /* Pick: joystick click OR AtomS3R button. */
        if (in.jbtn_pressed || in.atom_btn_pressed) {
            return true;
        }

        render_menu();
        gc9107_flush();
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

/* Drives a single game until the user holds the AtomS3R button or the game
 * returns false from its update(). */
static void run_game(const arcade_game_t *g)
{
    ESP_LOGI(TAG, "Entering '%s'", g->name);
    if (g->on_enter) g->on_enter();

    int64_t btn_down_us = 0;
    bool    btn_was_down = false;
    bool    requested_exit = false;

    for (;;) {
        arcade_input_t in;
        read_input(&in);

        /* Long-press AtomS3R button = quit to menu, regardless of game state. */
        int64_t now = esp_timer_get_time();
        if (in.atom_btn_down) {
            if (!btn_was_down) btn_down_us = now;
            btn_was_down = true;
            if ((now - btn_down_us) > BACK_HOLD_US) {
                ESP_LOGI(TAG, "Long-press → back to menu");
                requested_exit = true;
            }
        } else {
            btn_was_down = false;
            btn_down_us = 0;
        }

        if (requested_exit) break;
        if (!g->update(&in)) break;

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (g->on_exit) g->on_exit();
    ESP_LOGI(TAG, "Exited '%s'", g->name);
}

/* ── Hardware bring-up ────────────────────────────────────────────────── */
static void hw_init(void)
{
    gc9107_init();
    gc9107_set_rotation(2);              /* matches atoms3r_maze convention */

    i2c_master_bus_handle_t sys_bus = NULL;
    if (lp5562_init(&sys_bus)) lp5562_set_backlight(sys_bus, 200);

    if (joystick2_init() != ESP_OK) {
        ESP_LOGE(TAG, "Joystick2 not found");
        gc9107_fill_screen(A_C_BG);
        arcade_draw_centered(40, "NO",       A_C_BAD,  A_C_BG, 3);
        arcade_draw_centered(72, "JOYSTICK", A_C_TEXT, A_C_BG, 1);
        arcade_draw_centered(92, "plug in & reset", A_C_DIM, A_C_BG, 1);
        gc9107_flush();
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    joystick2_calibrate_centre();

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BTN_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);
}

void app_main(void)
{
    hw_init();
    srand((unsigned)esp_random());

    /* Splash */
    gc9107_fill_screen(A_C_BG);
    arcade_draw_centered(44, "AtomS3R", A_C_ACCENT, A_C_BG, 2);
    arcade_draw_centered(68, "ARCADE",  A_C_TEXT,   A_C_BG, 2);
    arcade_draw_centered(110, "v1", A_C_DIM, A_C_BG, 1);
    gc9107_flush();
    vTaskDelay(pdMS_TO_TICKS(800));

    for (;;) {
        if (menu_loop()) {
            run_game(s_games[s_selected]);
        }
        /* Brief pause between menu and game so the click doesn't immediately
         * re-trigger the launch in a tight loop. */
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
