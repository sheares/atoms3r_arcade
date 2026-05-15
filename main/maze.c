#include <string.h>
#include "esp_random.h"
#include "maze.h"

static maze_cell_t s_grid[MAZE_H][MAZE_W];
static uint8_t     s_visited[MAZE_H][MAZE_W];

const maze_cell_t *maze_cell(int r, int c)
{
    if (r < 0 || r >= MAZE_H || c < 0 || c >= MAZE_W) return NULL;
    return &s_grid[r][c];
}

bool maze_can_move(int r, int c, int dr, int dc)
{
    if (r < 0 || r >= MAZE_H || c < 0 || c >= MAZE_W) return false;
    int nr = r + dr, nc = c + dc;
    if (nr < 0 || nr >= MAZE_H || nc < 0 || nc >= MAZE_W) return false;

    if (dr == -1) return !(s_grid[r][c].walls  & WALL_N);
    if (dr ==  1) return !(s_grid[r][c].walls  & WALL_S);
    if (dc ==  1) return !(s_grid[r][c].walls  & WALL_E);
    if (dc == -1) return !(s_grid[r][c].walls  & WALL_W);
    return false;
}

// Iterative DFS using a fixed-size stack — easier on the FreeRTOS stack than
// recursion when MAZE_H*MAZE_W gets large.
void maze_generate(uint32_t seed)
{
    // (Re)seed using esp_random(); `seed` is unused for now but kept for API.
    (void)seed;

    for (int r = 0; r < MAZE_H; r++) {
        for (int c = 0; c < MAZE_W; c++) {
            s_grid[r][c].walls = WALL_N | WALL_E | WALL_S | WALL_W;
            s_visited[r][c]    = 0;
        }
    }

    typedef struct { int8_t r, c; } pt_t;
    static pt_t stack[MAZE_W * MAZE_H];
    int top = 0;

    int sr = esp_random() % MAZE_H;
    int sc = esp_random() % MAZE_W;
    s_visited[sr][sc] = 1;
    stack[top++] = (pt_t){ sr, sc };

    while (top > 0) {
        pt_t cur = stack[top - 1];

        // Collect unvisited neighbours
        struct { int8_t dr, dc, my_wall, neigh_wall; } moves[] = {
            { -1, 0, WALL_N, WALL_S },
            {  1, 0, WALL_S, WALL_N },
            { 0,  1, WALL_E, WALL_W },
            { 0, -1, WALL_W, WALL_E },
        };
        int candidates[4];
        int n = 0;
        for (int i = 0; i < 4; i++) {
            int nr = cur.r + moves[i].dr;
            int nc = cur.c + moves[i].dc;
            if (nr >= 0 && nr < MAZE_H && nc >= 0 && nc < MAZE_W &&
                !s_visited[nr][nc]) {
                candidates[n++] = i;
            }
        }

        if (n == 0) {
            top--;
            continue;
        }
        int pick = candidates[esp_random() % n];
        int nr   = cur.r + moves[pick].dr;
        int nc   = cur.c + moves[pick].dc;
        s_grid[cur.r][cur.c].walls &= ~moves[pick].my_wall;
        s_grid[nr][nc].walls       &= ~moves[pick].neigh_wall;
        s_visited[nr][nc] = 1;
        stack[top++] = (pt_t){ nr, nc };
    }
}
