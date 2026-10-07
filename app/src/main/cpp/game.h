// Platform-independent game logic. The frontend (main.cpp) supplies input and
// implements gfx_rect(); the game never touches Android or OpenGL directly.
#pragma once
#include <stdint.h>

constexpr int VIEW_W = 256;   // virtual screen size (16 x 15 tiles of 16px)
constexpr int VIEW_H = 240;

struct Input {
    bool left = false, right = false, up = false, down = false;
    bool a = false;       // jump (held)
    bool b = false;       // run / fire (held)
    bool start = false;
    // One-shot "just pressed" latches, set by the frontend, cleared by the game.
    bool a_edge = false, b_edge = false, start_edge = false;
};

void game_init();
void game_update(Input& in);   // advance exactly one 60 Hz logic tick
void game_draw();              // emit rectangles via gfx_rect
void game_pause();             // called when the app goes to background

// Implemented by the frontend. Coordinates are in the 256x240 virtual screen.
void gfx_rect(float x, float y, float w, float h, uint32_t rgb);
