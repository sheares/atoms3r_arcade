#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ─── AtomS3R Grove / external I2C pins ───────────────────────────────────────
#define JOYSTICK2_I2C_SDA   2
#define JOYSTICK2_I2C_SCL   1
#define JOYSTICK2_I2C_FREQ  400000   // 400 kHz

// M5Stack Joystick2 Unit default I2C address
#define JOYSTICK2_ADDR      0x63

// Raw data register (read 5 consecutive bytes from 0x00)
#define JOYSTICK2_REG_DATA  0x00

// ─── Data type ────────────────────────────────────────────────────────────────
typedef struct {
    uint16_t x_raw;   // 0–65535, centre ~32767
    uint16_t y_raw;   // 0–65535, centre ~32767
    bool     btn;     // true = pressed
    // Normalised −100 … +100 range derived from raw values
    int16_t  x;
    int16_t  y;
} joystick2_data_t;

// ─── Public API ───────────────────────────────────────────────────────────────
esp_err_t joystick2_init(void);
esp_err_t joystick2_read(joystick2_data_t *out);

// Call once with stick at rest to set software zero-point
esp_err_t joystick2_calibrate_centre(void);
