#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAZE_W   24
#define MAZE_H   24
#define CELL_PX  16      // 24*16 = 384 px world, scrolls behind the ball

// Wall bits in cell.walls — set means wall is present
#define WALL_N   0x1
#define WALL_E   0x2
#define WALL_S   0x4
#define WALL_W   0x8

typedef struct {
    uint8_t walls;
} maze_cell_t;

// Recursive-backtracker carve. `seed` from esp_random() / 0 for default.
void maze_generate(uint32_t seed);

// True if you can move from (r, c) by (dr, dc) — exactly one of dr/dc is ±1.
bool maze_can_move(int r, int c, int dr, int dc);

// Read-only access to a cell.
const maze_cell_t *maze_cell(int r, int c);
