#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gc9107.h"

/* ── Input state passed to each game's update() each frame ────────────── */
typedef struct {
    int16_t jx, jy;            /* joystick analog, −100..+100 (deadzoned upstream) */
    bool    jbtn_down;         /* joystick stick click held this frame */
    bool    jbtn_pressed;      /* edge: just went down */
    bool    jbtn_released;     /* edge: just went up */
    bool    atom_btn_down;     /* AtomS3R button held */
    bool    atom_btn_pressed;  /* edge */
    bool    atom_btn_released; /* edge */
} arcade_input_t;

/* ── Game-module interface ────────────────────────────────────────────── */
/*  Each game implements four hooks plus a name. The shell drives the
 *  lifecycle: on_enter() → update() repeatedly → on_exit().
 *  update() returns true to keep running, false to request a return to
 *  the menu (e.g., on Asteroids' "QUIT" gesture). The shell also forces
 *  an exit on long-press of the AtomS3R button, so games don't have to
 *  poll for that themselves.                                              */
typedef struct {
    const char *name;
    void  (*on_enter)(void);
    bool  (*update)(const arcade_input_t *in);   /* returns false → quit */
    void  (*on_exit)(void);
} arcade_game_t;

/* ── Shared palette ───────────────────────────────────────────────────── */
#define A_C_BG          RGB565( 12,  14,  20)
#define A_C_PANEL       RGB565( 26,  30,  44)
#define A_C_PANEL_LO    RGB565( 18,  20,  30)
#define A_C_BORDER      RGB565( 70,  80, 110)
#define A_C_TEXT        RGB565(240, 244, 252)
#define A_C_DIM         RGB565(130, 140, 160)
#define A_C_ACCENT      RGB565( 90, 200, 255)
#define A_C_GOOD        RGB565( 80, 220, 130)
#define A_C_BAD         RGB565(245,  90, 110)
#define A_C_WARN        RGB565(255, 175,  60)

/* ── Helpers usable by all games ──────────────────────────────────────── */
void arcade_draw_centered(int y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);
void arcade_draw_text_overlay(int x, int y, const char *str, uint16_t fg, uint8_t scale);
