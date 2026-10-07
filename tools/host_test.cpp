// Headless smoke test: runs the real game logic on the desktop with a simple
// bot to check that it compiles, doesn't crash, and that levels are completable.
//   g++ -std=c++17 -O1 -Wall -I../app/src/main/cpp host_test.cpp -o host_test && ./host_test
#include "../app/src/main/cpp/game.cpp"
#include <stdio.h>

static long g_rects = 0;
void gfx_rect(float, float, float, float, uint32_t) { g_rects++; }

int main() {
    game_init();
    Input in;
    in.start_edge = true;
    game_update(in);                        // title -> new game
    int lastLevel = -1;
    int deaths = 0, lastLives = lives;
    long ticks = 0;
    for (; ticks < 600000 && gstate != G_WIN && gstate != G_GAMEOVER && gstate != G_TITLE; ticks++) {
        if (levelNo != lastLevel) { lastLevel = levelNo; printf("tick %ld: entered level %d (lives %d, score %d)\n", ticks, levelNo + 1, lives, score); }
        if (lives != lastLives) { deaths++; printf("  died at x=%.0f (level %d)\n", M.x, levelNo + 1); lastLives = lives; }
        if (M.state == MS_DEAD && M.timer == 1) printf("  death begins at x=%.0f y=%.0f tile=%d (level %d) power=%d\n", M.x, M.y, (int)(M.x/16), levelNo + 1, M.power);
        in = Input();
        if (gstate == G_PLAY && M.state == MS_NORMAL) {
            in.right = true; in.b = true; in.a = true;
            float front = M.x + 12;
            bool pit = false, wall = false, enemy = false;
            for (int d = 4; d <= 16; d += 4) {
                int col = fl(front + d);
                if (M.ground && !solidAt(col, GROUND_ROW)) pit = true;
            }
            if (solidAt(fl(front + 6), fl(M.y + M.h - 2))) wall = true;
            for (Ent& e : ents)
                if (isLive(e) && e.x > M.x && e.x - M.x < 50 && fabsf(e.y - M.y) < 30) enemy = true;
            if (M.ground && (pit || wall || enemy)) in.a_edge = true;
            if (!M.ground && M.vy > 0) in.a = false;
            // fire at things when we can
            if (M.power == 2 && (ticks % 40 == 0)) in.b_edge = true;
        }
        game_update(in);
        if (ticks % 7 == 0) game_draw();   // exercise the renderer too
    }
    printf("finished: gstate=%d level=%d lives=%d score=%d deaths=%d ticks=%ld rects=%ld\n",
           gstate, levelNo + 1, lives, score, deaths, ticks, g_rects);
    return 0;
}
