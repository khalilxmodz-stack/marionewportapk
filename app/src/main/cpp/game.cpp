// Plumber Bros - a tiny original side-scrolling platformer in plain C++.
// Original level layouts and programmer-art graphics (colored rectangles).
// No ROM data, no emulation, no external assets.
#include "game.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

const int TS = 16;            // tile size
const int LH = 15;            // level height in tiles
const int MAXW = 200;         // max level width in tiles
const int MAXE = 64;          // max entities
const int GROUND_ROW = 13;    // first ground row (rows 13-14)
const int NUM_LEVELS = 3;

enum : uint8_t { T_EMPTY, T_GROUND, T_BRICK, T_QBLOCK, T_USED, T_HARD,
                 T_PIPE_TL, T_PIPE_TR, T_PIPE_BL, T_PIPE_BR, T_COIN, T_POLE };
enum : uint8_t { C_COIN, C_POWER };
enum { E_NONE, E_GOOMBA, E_KOOPA, E_SHELL, E_MUSH, E_FLOWER, E_FIREBALL, E_COINPOP, E_DEBRIS };
enum { G_TITLE, G_INTRO, G_PLAY, G_GAMEOVER, G_WIN };
enum { MS_NORMAL, MS_DEAD, MS_SLIDE, MS_WALK, MS_COUNT };

struct Spawn { int type, x, y; bool done; };
struct Level {
    uint8_t t[LH][MAXW];
    uint8_t c[LH][MAXW];
    int w, theme, poleX, nsp;
    Spawn sp[96];
};
struct Ent { int type; float x, y, vx, vy; int w, h, state, timer, dir; };
struct Bump { int tx, ty, t; };
struct Mario {
    float x, y, vx, vy, anim;
    int h, power, facing, invuln, state, timer;
    bool ground, visible;
};
struct Theme { uint32_t sky, ground, groundDk, brick, brickLn, hard, hardDk; };

const Theme THEMES[3] = {
    {0x5C94FC, 0xC87832, 0x704010, 0xC05028, 0x402010, 0xA08060, 0x504030},
    {0x000000, 0x2850C8, 0x102060, 0x3868E8, 0x102060, 0x3050A0, 0x182860},
    {0x20205C, 0x808080, 0x404040, 0x909090, 0x303030, 0x707080, 0x383848},
};

Level L;
Ent ents[MAXE];
Bump bumps[8];
Mario M;
int gstate = G_TITLE, levelNo = 0, score = 0, coinCount = 0, lives = 3;
int timeLeft = 400, timeTick = 0, stateTimer = 0;
float cam = 0;
unsigned frame = 0;
bool paused = false;

// ---------------------------------------------------------------- helpers
inline int fl(float v) { return (int)floorf(v / TS); }

inline bool isSolid(uint8_t t) {
    return t == T_GROUND || t == T_BRICK || t == T_QBLOCK || t == T_USED || t == T_HARD ||
           (t >= T_PIPE_TL && t <= T_PIPE_BR);
}
bool solidAt(int tx, int ty) {
    if (ty < 0) return false;
    if (tx < 0 || tx >= L.w) return true;   // invisible walls at both ends
    if (ty >= LH) return false;
    return isSolid(L.t[ty][tx]);
}
bool overlap(float ax, float ay, int aw, int ah, float bx, float by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

struct MoveRes { bool ground = false, wall = false, head = false; int htx = 0, hty = 0; };

// Axis-separated AABB move against the tile map.
MoveRes moveBox(float& x, float& y, float& vx, float& vy, int w, int h) {
    MoveRes r;
    x += vx;
    if (vx > 0) {
        int tx = fl(x + w - 1);
        for (int ty = fl(y); ty <= fl(y + h - 1); ty++)
            if (solidAt(tx, ty)) { x = (float)(tx * TS - w); vx = 0; r.wall = true; break; }
    } else if (vx < 0) {
        int tx = fl(x);
        for (int ty = fl(y); ty <= fl(y + h - 1); ty++)
            if (solidAt(tx, ty)) { x = (float)((tx + 1) * TS); vx = 0; r.wall = true; break; }
    }
    y += vy;
    if (vy > 0) {
        int ty = fl(y + h - 1);
        for (int tx = fl(x); tx <= fl(x + w - 1); tx++)
            if (solidAt(tx, ty)) { y = (float)(ty * TS - h); vy = 0; r.ground = true; break; }
    } else if (vy < 0) {
        int ty = fl(y);
        int cx = fl(x + w * 0.5f), pick = -1;
        if (solidAt(cx, ty)) pick = cx;
        else for (int tx = fl(x); tx <= fl(x + w - 1); tx++) if (solidAt(tx, ty)) { pick = tx; break; }
        if (pick >= 0) { y = (float)((ty + 1) * TS); vy = 0; r.head = true; r.htx = pick; r.hty = ty; }
    }
    return r;
}

// ---------------------------------------------------------------- level building
void setT(int x, int y, uint8_t t) { if (x >= 0 && x < MAXW && y >= 0 && y < LH) L.t[y][x] = t; }
void fill(int x0, int y0, int x1, int y1, uint8_t t) {
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) setT(x, y, t);
}
void ground(int x0, int x1) { fill(x0, GROUND_ROW, x1, LH - 1, T_GROUND); }
void brick(int x, int y, int n) { for (int i = 0; i < n; i++) setT(x + i, y, T_BRICK); }
void hard(int x, int y, int n) { for (int i = 0; i < n; i++) setT(x + i, y, T_HARD); }
void coins(int x, int y, int n) { for (int i = 0; i < n; i++) setT(x + i, y, T_COIN); }
void q(int x, int y, uint8_t content) { setT(x, y, T_QBLOCK); if (x < MAXW && y < LH) L.c[y][x] = content; }
void pipe(int x, int h) {
    int top = GROUND_ROW - h;
    setT(x, top, T_PIPE_TL); setT(x + 1, top, T_PIPE_TR);
    for (int r = top + 1; r < GROUND_ROW; r++) { setT(x, r, T_PIPE_BL); setT(x + 1, r, T_PIPE_BR); }
}
void stairsUp(int x, int n) { for (int i = 0; i < n; i++) fill(x + i, GROUND_ROW - 1 - i, x + i, GROUND_ROW - 1, T_HARD); }
void flag(int x) {
    for (int r = 3; r <= 11; r++) setT(x, r, T_POLE);
    setT(x, 12, T_HARD);
    L.poleX = x;
}
void spawnE(int type, int x, int row) {
    if (L.nsp < 96) { L.sp[L.nsp].type = type; L.sp[L.nsp].x = x; L.sp[L.nsp].y = row; L.sp[L.nsp].done = false; L.nsp++; }
}
void goomba(int x, int row = 12) { spawnE(E_GOOMBA, x, row); }
void koopa(int x, int row = 12) { spawnE(E_KOOPA, x, row); }

void buildLevel(int n) {
    memset(&L, 0, sizeof(L));
    switch (n) {
    case 0:   // grassland-style opener
        L.w = 140; L.theme = 0;
        ground(0, 45); ground(49, 80); ground(84, 139);
        q(12, 9, C_COIN); brick(13, 9, 1); q(14, 9, C_POWER); brick(15, 9, 1); q(16, 9, C_COIN);
        q(14, 5, C_COIN);
        pipe(24, 2); pipe(32, 3); pipe(40, 2);
        goomba(20); goomba(28); goomba(37);
        coins(46, 9, 3);
        goomba(55); goomba(58); koopa(66);
        brick(60, 9, 1); q(61, 9, C_POWER); brick(62, 9, 1);
        brick(70, 9, 5); coins(70, 8, 5);
        pipe(76, 2);
        coins(81, 10, 3);
        q(96, 9, C_COIN); q(98, 9, C_COIN);
        goomba(90); goomba(92); koopa(102);
        goomba(108); goomba(110);
        stairsUp(114, 5);
        flag(130);
        break;
    case 1:   // underground-style
        L.w = 150; L.theme = 1;
        ground(0, 28); ground(33, 70); ground(75, 100); ground(104, 149);
        q(8, 9, C_COIN); brick(9, 9, 3); q(12, 9, C_POWER);
        goomba(14); goomba(20); koopa(24);
        coins(29, 9, 4); hard(30, 11, 1);
        pipe(38, 2); pipe(46, 3); pipe(54, 3);
        goomba(35); goomba(42); koopa(50); goomba(58); goomba(60);
        brick(62, 9, 2); q(64, 9, C_COIN); brick(65, 9, 2);
        coins(71, 9, 4);
        brick(85, 9, 1); q(86, 9, C_POWER); brick(87, 9, 1);
        goomba(80); koopa(84); goomba(90); goomba(92);
        coins(101, 10, 3);
        goomba(112); koopa(118); goomba(122); goomba(124);
        stairsUp(128, 6);
        flag(138);
        break;
    default:  // dusk stage, gaps and more enemies
        L.w = 170; L.theme = 2;
        ground(0, 20); ground(25, 34); ground(39, 48); ground(53, 70);
        ground(75, 90); ground(95, 120); ground(124, 169);
        q(6, 9, C_COIN); brick(7, 9, 1); q(8, 9, C_POWER);
        goomba(10); goomba(14); koopa(17);
        coins(21, 9, 4);
        goomba(32); koopa(33);
        brick(29, 9, 1); q(30, 9, C_POWER); brick(31, 9, 1);
        hard(36, 11, 1);
        goomba(41); goomba(43); pipe(45, 2); koopa(47);
        coins(49, 9, 4);
        goomba(57); goomba(62); koopa(64);
        brick(58, 9, 1); q(59, 9, C_COIN); brick(60, 9, 1);
        pipe(66, 2);
        coins(71, 9, 4);
        brick(78, 9, 6); coins(78, 8, 6);
        goomba(83); goomba(86); koopa(88);
        coins(91, 9, 4);
        brick(98, 9, 1); q(99, 9, C_POWER); brick(100, 9, 1);
        goomba(104); koopa(110); goomba(116);
        hard(122, 11, 1);
        goomba(128); koopa(132); goomba(136); goomba(138); pipe(142, 3); goomba(146); koopa(147);
        stairsUp(150, 6);
        flag(160);
        break;
    }
}

// ---------------------------------------------------------------- entities
Ent* addEnt(int type, float x, float y) {
    for (Ent& e : ents) {
        if (e.type != E_NONE) continue;
        memset(&e, 0, sizeof(e));
        e.type = type; e.x = x; e.y = y; e.dir = -1;
        switch (type) {
        case E_GOOMBA: e.w = 14; e.h = 15; break;
        case E_KOOPA: e.w = 14; e.h = 22; break;
        case E_SHELL: e.w = 14; e.h = 14; break;
        case E_MUSH: case E_FLOWER: e.w = 14; e.h = 16; break;
        default: e.w = 8; e.h = 8; break;
        }
        return &e;
    }
    return nullptr;
}
bool isWalker(const Ent& e) { return e.type == E_GOOMBA || e.type == E_KOOPA || e.type == E_SHELL; }
// A "live" enemy can hurt Mario / be hurt.
bool isLive(const Ent& e) { return isWalker(e) && e.state == 0; }

void killEnt(Ent& e, int dir) {
    e.state = 2; e.vy = -3.5f; e.vx = dir * 1.0f; e.timer = 0;
    score += 100;
}

void addBump(int tx, int ty) {
    int s = 0;
    for (int i = 0; i < 8; i++) if (bumps[i].t == 0) { s = i; break; }
    bumps[s].tx = tx; bumps[s].ty = ty; bumps[s].t = 1;
}
void addCoin() {
    score += 200;
    if (++coinCount >= 100) { coinCount = 0; lives++; }
}
void setPower(int p) {
    int nh = (p == 0) ? 16 : 32;
    if (nh != M.h) { M.y += (float)(M.h - nh); M.h = nh; }
    M.power = p;
}

void bumpTile(int tx, int ty) {
    if (tx < 0 || tx >= L.w || ty < 0 || ty >= LH) return;
    uint8_t t = L.t[ty][tx];
    if (t == T_QBLOCK) {
        addBump(tx, ty);
        L.t[ty][tx] = T_USED;
        if (L.c[ty][tx] == C_COIN) {
            Ent* e = addEnt(E_COINPOP, (float)(tx * TS + 4), (float)(ty * TS - 14));
            if (e) { e->vy = -5.0f; e->timer = 0; }
            addCoin();
        } else {
            Ent* e = addEnt(M.power == 0 ? E_MUSH : E_FLOWER, (float)(tx * TS + 1), (float)(ty * TS));
            if (e) e->dir = 1;
        }
    } else if (t == T_BRICK) {
        if (M.power > 0) {
            L.t[ty][tx] = T_EMPTY;
            score += 50;
            for (int i = 0; i < 4; i++) {
                Ent* d = addEnt(E_DEBRIS, (float)(tx * TS + (i & 1) * 8), (float)(ty * TS + (i >> 1) * 8));
                if (d) { d->vx = (i & 1) ? 1.5f : -1.5f; d->vy = (i >> 1) ? -2.5f : -4.5f; }
            }
        } else addBump(tx, ty);
    } else return;
    // anything walking on top of the bumped block gets flipped
    for (Ent& e : ents) {
        if (!isWalker(e) || e.state == 2 || (e.type == E_GOOMBA && e.state == 1)) continue;
        if (fabsf(e.y + e.h - ty * TS) < 3 && e.x + e.w > tx * TS && e.x < tx * TS + TS)
            killEnt(e, (e.x + e.w * 0.5f < tx * TS + 8) ? -1 : 1);
    }
}

int countFireballs() { int n = 0; for (Ent& e : ents) if (e.type == E_FIREBALL) n++; return n; }

void updateEnts() {
    for (Ent& e : ents) {
        if (e.type == E_NONE) continue;
        if (e.x + e.w < cam - 48 || e.y > 270) { e.type = E_NONE; continue; }
        if (e.x > cam + VIEW_W + 48) continue;
        switch (e.type) {
        case E_GOOMBA: case E_KOOPA: case E_SHELL: {
            if (e.state == 2) { e.vy += 0.3f; e.x += e.vx; e.y += e.vy; break; }
            if (e.type == E_GOOMBA && e.state == 1) { if (--e.timer <= 0) e.type = E_NONE; break; }
            if (e.type == E_SHELL && e.timer > 0) e.timer--;
            if (e.type != E_SHELL) e.vx = e.dir * 0.5f;
            float ovx = e.vx;
            e.vy += 0.3f; if (e.vy > 4) e.vy = 4;
            MoveRes r = moveBox(e.x, e.y, e.vx, e.vy, e.w, e.h);
            if (r.wall) { if (e.type == E_SHELL) e.vx = -ovx; else e.dir = -e.dir; }
            if (e.type == E_SHELL && fabsf(e.vx) > 0.1f) {
                for (Ent& o : ents)
                    if (&o != &e && (o.type == E_GOOMBA || o.type == E_KOOPA) && o.state == 0 &&
                        overlap(e.x, e.y, e.w, e.h, o.x, o.y, o.w, o.h))
                        killEnt(o, e.vx > 0 ? 1 : -1);
            }
            break;
        }
        case E_MUSH: case E_FLOWER:
            if (e.state == 0) {
                e.y -= 0.5f;
                if (++e.timer >= 32) { e.state = 1; e.dir = 1; }
            } else if (e.type == E_MUSH) {
                e.vx = e.dir * 1.0f;
                e.vy += 0.3f; if (e.vy > 4) e.vy = 4;
                MoveRes r = moveBox(e.x, e.y, e.vx, e.vy, e.w, e.h);
                if (r.wall) e.dir = -e.dir;
            }
            break;
        case E_FIREBALL: {
            e.vx = e.dir * 3.2f;
            e.vy += 0.35f; if (e.vy > 4) e.vy = 4;
            MoveRes r = moveBox(e.x, e.y, e.vx, e.vy, e.w, e.h);
            if (r.ground) e.vy = -2.6f;
            if (r.wall) { e.type = E_NONE; break; }
            for (Ent& o : ents)
                if (isLive(o) && overlap(e.x, e.y, e.w, e.h, o.x, o.y, o.w, o.h)) {
                    killEnt(o, e.dir); e.type = E_NONE; break;
                }
            break;
        }
        case E_COINPOP:
            e.vy += 0.3f; e.y += e.vy;
            if (++e.timer >= 30) e.type = E_NONE;
            break;
        case E_DEBRIS:
            e.vy += 0.35f; e.x += e.vx; e.y += e.vy;
            break;
        }
    }
}

// ---------------------------------------------------------------- game flow
void startLevel(int n) {
    levelNo = n;
    buildLevel(n);
    memset(ents, 0, sizeof(ents));
    memset(bumps, 0, sizeof(bumps));
    cam = 0; timeLeft = 400; timeTick = 0; paused = false;
    M.vx = M.vy = M.anim = 0;
    M.h = (M.power == 0) ? 16 : 32;
    M.x = 40; M.y = (float)(GROUND_ROW * TS - M.h);
    M.facing = 1; M.ground = true; M.invuln = 0; M.state = MS_NORMAL; M.timer = 0; M.visible = true;
    gstate = G_INTRO; stateTimer = 0;
}
void newGame() { lives = 3; score = 0; coinCount = 0; M.power = 0; startLevel(0); }
void nextLevel() {
    if (levelNo + 1 >= NUM_LEVELS) { gstate = G_WIN; stateTimer = 0; }
    else startLevel(levelNo + 1);
}
void marioDie() {
    if (M.state != MS_NORMAL) return;
    setPower(0);
    M.state = MS_DEAD; M.vy = -4.5f; M.vx = 0; M.timer = 0;
}
void finishDeath() {
    lives--;
    M.power = 0;
    if (lives <= 0) { gstate = G_GAMEOVER; stateTimer = 0; }
    else startLevel(levelNo);
}
void marioHurt() {
    if (M.invuln > 0 || M.state != MS_NORMAL) return;
    if (M.power > 0) { setPower(0); M.invuln = 120; }
    else marioDie();
}
void startFlag() {
    M.state = MS_SLIDE; M.vx = M.vy = 0; M.facing = 1; M.timer = 0;
    M.x = (float)(L.poleX * TS + 2);
    int hgt = (int)((GROUND_ROW - 1) * TS - (M.y + M.h)) / TS;   // tiles above the base block
    score += hgt <= 1 ? 100 : hgt <= 3 ? 400 : hgt <= 5 ? 800 : hgt <= 7 ? 2000 : 5000;
}

void stompEnemy(Ent& e) {
    score += 100;
    if (e.type == E_GOOMBA) { e.state = 1; e.timer = 30; }
    else if (e.type == E_KOOPA) { e.type = E_SHELL; e.y += 22 - 14; e.h = 14; e.vx = 0; e.timer = 0; e.state = 0; }
}

void updateMarioNormal(Input& in) {
    float maxs = in.b ? 2.5f : 1.5625f;
    int dir = (in.right ? 1 : 0) - (in.left ? 1 : 0);
    float acc = M.ground ? 0.07f : 0.05f;
    if (dir != 0) {
        M.facing = dir;
        if (M.vx * dir < 0) M.vx += dir * (M.ground ? 0.15f : 0.08f);          // skid / turn
        else if (fabsf(M.vx) < maxs) { M.vx += dir * acc; if (fabsf(M.vx) > maxs) M.vx = dir * maxs; }
        else if (M.ground) M.vx -= dir * 0.04f;                                // eased back from run
    } else if (M.ground) {
        if (fabsf(M.vx) < 0.06f) M.vx = 0; else M.vx -= (M.vx > 0 ? 0.06f : -0.06f);
    }
    if (in.a_edge && M.ground) { M.vy = -4.0f - 0.35f * fabsf(M.vx) / 2.5f; M.ground = false; }
    in.a_edge = false;
    float g = (M.vy < 0 && in.a) ? 0.125f : 0.4375f;
    M.vy += g; if (M.vy > 4.0f) M.vy = 4.0f;

    MoveRes r = moveBox(M.x, M.y, M.vx, M.vy, 12, M.h);
    M.ground = r.ground;
    if (r.head) bumpTile(r.htx, r.hty);
    if (M.x < cam) { M.x = cam; if (M.vx < 0) M.vx = 0; }
    M.anim += fabsf(M.vx) * 0.12f;
    if (M.invuln > 0) M.invuln--;

    if (M.power == 2 && in.b_edge && countFireballs() < 2) {
        Ent* f = addEnt(E_FIREBALL, M.x + (M.facing > 0 ? 12 : -8), M.y + (M.h == 32 ? 10 : 4));
        if (f) { f->dir = M.facing; f->vy = 0; }
    }
    in.b_edge = false;

    // coin / flag pickups (non-solid tiles)
    for (int ty = fl(M.y); ty <= fl(M.y + M.h - 1); ty++)
        for (int tx = fl(M.x); tx <= fl(M.x + 11); tx++) {
            if (tx < 0 || tx >= L.w || ty < 0 || ty >= LH) continue;
            uint8_t t = L.t[ty][tx];
            if (t == T_COIN) { L.t[ty][tx] = T_EMPTY; addCoin(); }
            else if (t == T_POLE && M.state == MS_NORMAL) { startFlag(); return; }
        }

    // enemies and power-ups
    for (Ent& e : ents) {
        if (M.state != MS_NORMAL) break;
        if (e.type == E_NONE) continue;
        if (!overlap(M.x, M.y, 12, M.h, e.x, e.y, e.w, e.h)) continue;
        bool stomp = M.vy > 0 && (M.y + M.h) - e.y < 12;
        switch (e.type) {
        case E_GOOMBA: case E_KOOPA:
            if (e.state != 0) break;
            if (stomp) { stompEnemy(e); M.vy = in.a ? -4.2f : -3.0f; M.ground = false; }
            else marioHurt();
            break;
        case E_SHELL:
            if (e.state != 0 || e.timer > 0) break;
            if (e.vx == 0) {                                   // kick a resting shell
                e.vx = (M.x + 6 < e.x + e.w * 0.5f) ? 4.0f : -4.0f;
                e.timer = 12; score += 100;
                if (stomp) M.vy = -3.0f;
            } else if (stomp) { e.vx = 0; e.timer = 12; M.vy = -3.0f; score += 100; }
            else marioHurt();
            break;
        case E_MUSH:
            if (e.state != 1) break;
            e.type = E_NONE; score += 1000;
            if (M.power == 0) { setPower(1); M.invuln = 30; }
            break;
        case E_FLOWER:
            if (e.state != 1) break;
            e.type = E_NONE; score += 1000;
            setPower(M.power == 0 ? 1 : 2); M.invuln = 30;
            break;
        }
    }

    if (M.state == MS_NORMAL && M.y > VIEW_H + 20) marioDie();    // fell into a pit
}

void updatePlay(Input& in) {
    if (in.start_edge) paused = !paused;
    if (paused) return;

    switch (M.state) {
    case MS_NORMAL:
        updateMarioNormal(in);
        if (M.state == MS_NORMAL && ++timeTick >= 24) {
            timeTick = 0;
            if (--timeLeft <= 0) { timeLeft = 0; marioDie(); }
        }
        break;
    case MS_DEAD:
        M.vy += 0.25f; M.y += M.vy;
        if (++M.timer > 150) { finishDeath(); return; }
        break;
    case MS_SLIDE:
        M.y += 2.0f;
        if (M.y + M.h >= (GROUND_ROW - 1) * TS) {
            M.y = (float)((GROUND_ROW - 1) * TS - M.h);
            M.state = MS_WALK; M.timer = 0; M.vx = 0; M.vy = 0;
        }
        break;
    case MS_WALK: {
        M.vx = 1.2f; M.vy += 0.4375f; if (M.vy > 4) M.vy = 4;
        MoveRes r = moveBox(M.x, M.y, M.vx, M.vy, 12, M.h);
        M.ground = r.ground;
        M.anim += 0.15f;
        if (++M.timer >= 60) M.visible = false;
        if (M.timer >= 90) { M.state = MS_COUNT; M.timer = 0; }
        break;
    }
    case MS_COUNT:
        if (timeLeft > 0) { int d = timeLeft >= 2 ? 2 : timeLeft; timeLeft -= d; score += 50 * d; }
        else if (++M.timer > 60) { nextLevel(); return; }
        break;
    }

    if (M.state != MS_DEAD) {
        // activate enemies as they come into range
        for (int i = 0; i < L.nsp; i++) {
            Spawn& s = L.sp[i];
            if (s.done || s.x * TS > cam + VIEW_W + 16) continue;
            s.done = true;
            Ent* e = addEnt(s.type, (float)(s.x * TS + 1), 0);
            if (e) e->y = (float)((s.y + 1) * TS - e->h);
        }
        updateEnts();
        float target = M.x - 112;
        if (target > cam) cam = target;
        float maxc = (float)(L.w * TS - VIEW_W);
        if (cam > maxc) cam = maxc;
        if (cam < 0) cam = 0;
    }
    for (int i = 0; i < 8; i++) if (bumps[i].t > 0 && ++bumps[i].t > 10) bumps[i].t = 0;
}

// ---------------------------------------------------------------- drawing
inline void R(float x, float y, float w, float h, uint32_t c) { gfx_rect(x, y, w, h, c); }

const uint16_t FONT_DIGIT[10] = {
    0b111'101'101'101'111, 0b010'110'010'010'111, 0b110'001'010'100'111, 0b110'001'010'001'110,
    0b101'101'111'001'001, 0b111'100'110'001'110, 0b011'100'111'101'111, 0b111'001'010'010'010,
    0b111'101'111'101'111, 0b111'101'111'001'110};
const uint16_t FONT_ALPHA[26] = {
    0b010'101'111'101'101, 0b110'101'110'101'110, 0b011'100'100'100'011, 0b110'101'101'101'110,
    0b111'100'110'100'111, 0b111'100'110'100'100, 0b011'100'101'101'011, 0b101'101'111'101'101,
    0b111'010'010'010'111, 0b001'001'001'101'010, 0b101'101'110'101'101, 0b100'100'100'100'111,
    0b101'111'111'101'101, 0b110'101'101'101'101, 0b010'101'101'101'010, 0b110'101'110'100'100,
    0b010'101'101'111'011, 0b110'101'110'101'101, 0b011'100'010'001'110, 0b111'010'010'010'010,
    0b101'101'101'101'111, 0b101'101'101'101'010, 0b101'101'111'111'101, 0b101'101'010'101'101,
    0b101'101'010'010'010, 0b111'001'010'100'111};

uint16_t glyph(char c) {
    c = (char)toupper((unsigned char)c);
    if (c >= '0' && c <= '9') return FONT_DIGIT[c - '0'];
    if (c >= 'A' && c <= 'Z') return FONT_ALPHA[c - 'A'];
    if (c == '-') return 0b000'000'111'000'000;
    if (c == '?') return 0b110'001'010'000'010;
    if (c == '!') return 0b010'010'010'000'010;
    return 0;
}
void drawChar(char c, float x, float y, int s, uint32_t col) {
    uint16_t g = glyph(c);
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 3; k++)
            if (g & (1 << (14 - (r * 3 + k)))) R(x + k * s, y + r * s, (float)s, (float)s, col);
}
void text(const char* s, float x, float y, int sc, uint32_t col) {
    for (; *s; s++, x += 4 * sc) drawChar(*s, x, y, sc, col);
}
void ctext(const char* s, float y, int sc, uint32_t col) {
    float w = (float)strlen(s) * 4 * sc - sc;
    text(s, (VIEW_W - w) * 0.5f, y, sc, col);
}

void drawTile(uint8_t t, float x, float y, const Theme& th) {
    switch (t) {
    case T_GROUND:
        R(x, y, 16, 16, th.ground);
        R(x, y + 15, 16, 1, th.groundDk); R(x + 15, y, 1, 16, th.groundDk);
        R(x, y + 7, 16, 1, th.groundDk); R(x + 7, y, 1, 8, th.groundDk);
        break;
    case T_BRICK:
        R(x, y, 16, 16, th.brick);
        for (int i = 0; i < 4; i++) R(x, y + i * 4 + 3, 16, 1, th.brickLn);
        R(x + 7, y, 1, 4, th.brickLn); R(x + 3, y + 4, 1, 4, th.brickLn); R(x + 11, y + 4, 1, 4, th.brickLn);
        R(x + 7, y + 8, 1, 4, th.brickLn); R(x + 3, y + 12, 1, 4, th.brickLn); R(x + 11, y + 12, 1, 4, th.brickLn);
        break;
    case T_QBLOCK:
        R(x, y, 16, 16, 0xE8A020);
        R(x, y, 16, 1, 0xF8D870); R(x, y, 1, 16, 0xF8D870);
        R(x, y + 15, 16, 1, 0x805010); R(x + 15, y, 1, 16, 0x805010);
        drawChar('?', x + 5, y + 3, 2, 0x805010);
        break;
    case T_USED:
        R(x, y, 16, 16, 0x886644);
        R(x, y + 15, 16, 1, 0x443322); R(x + 15, y, 1, 16, 0x443322);
        R(x + 2, y + 2, 2, 2, 0x443322); R(x + 12, y + 2, 2, 2, 0x443322);
        break;
    case T_HARD:
        R(x, y, 16, 16, th.hard);
        R(x, y, 16, 1, 0xC0A080); R(x, y, 1, 16, 0xC0A080);
        R(x, y + 15, 16, 1, th.hardDk); R(x + 15, y, 1, 16, th.hardDk);
        break;
    case T_PIPE_TL: case T_PIPE_TR: case T_PIPE_BL: case T_PIPE_BR: {
        bool left = (t == T_PIPE_TL || t == T_PIPE_BL), top = (t == T_PIPE_TL || t == T_PIPE_TR);
        R(x, y, 16, 16, 0x28B028);
        if (left) { R(x, y, 1, 16, 0x084008); R(x + 3, top ? y + 2 : y, 3, top ? 12 : 16, 0x80F080); }
        else { R(x + 15, y, 1, 16, 0x084008); R(x + 10, top ? y + 2 : y, 4, top ? 12 : 16, 0x108010); }
        if (top) { R(x, y, 16, 1, 0x084008); R(x, y + 15, 16, 1, 0x084008); }
        break;
    }
    case T_COIN: {
        bool narrow = (frame / 10) & 1;
        R(x + (narrow ? 6 : 5), y + 2, narrow ? 4 : 6, 12, 0xF8D820);
        if (!narrow) { R(x + 4, y + 4, 8, 8, 0xF8D820); R(x + 7, y + 4, 2, 8, 0xC09000); }
        break;
    }
    case T_POLE:
        R(x + 7, y, 2, 16, 0x30C030);
        break;
    default: break;
    }
}

void drawEnt(const Ent& e, int camX) {
    float x = (float)((int)e.x - camX), y = (float)(int)e.y;
    bool flip = (e.state == 2);
    auto F = [&](float dx, float dy, float w, float h, uint32_t c) {
        R(x + dx, flip ? y + e.h - dy - h : y + dy, w, h, c);
    };
    int step = (frame / 8) & 1;
    switch (e.type) {
    case E_GOOMBA:
        if (e.state == 1) { R(x, y + 9, 14, 6, 0xA85A20); R(x + 2, y + 11, 3, 2, 0xFFFFFF); R(x + 9, y + 11, 3, 2, 0xFFFFFF); break; }
        F(3, 0, 8, 3, 0xA85A20); F(1, 3, 12, 7, 0xA85A20); F(3, 8, 8, 3, 0xF0C890);
        F(2, 4, 4, 4, 0xFFFFFF); F(8, 4, 4, 4, 0xFFFFFF); F(4, 5, 2, 3, 0x000000); F(8, 5, 2, 3, 0x000000);
        F(step ? 0 : 1, 11, 6, 4, 0x301808); F(step ? 8 : 7, 11, 6, 4, 0x301808);
        break;
    case E_KOOPA: {
        float hx = e.dir > 0 ? 7.f : 1.f;
        F(hx, 0, 6, 8, 0xF8E0A0); F(hx + (e.dir > 0 ? 3 : 1), 2, 2, 2, 0x000000);
        F(1, 8, 12, 11, 0x30A030); F(3, 10, 8, 7, 0x80E080);
        F(step ? 0 : 1, 19, 5, 3, 0x603010); F(step ? 8 : 7, 19, 5, 3, 0x603010);
        break;
    }
    case E_SHELL:
        F(0, 2, 14, 10, 0x30A030); F(2, 4, 10, 6, 0x80E080); F(1, 10, 12, 4, 0xF8F0D0);
        break;
    case E_MUSH:
        R(x + 1, y, 12, 8, 0xE02020); R(x + 3, y + 1, 3, 3, 0xFFFFFF); R(x + 9, y + 2, 3, 3, 0xFFFFFF);
        R(x + 3, y + 8, 8, 8, 0xF8E0B8); R(x + 4, y + 10, 2, 3, 0); R(x + 8, y + 10, 2, 3, 0);
        break;
    case E_FLOWER:
        R(x + 6, y + 8, 2, 8, 0x20A020); R(x + 2, y + 11, 4, 2, 0x20A020); R(x + 8, y + 12, 4, 2, 0x20A020);
        R(x + 1, y + 1, 12, 8, 0xF86018); R(x + 4, y + 3, 6, 5, 0xF8D820); R(x + 5, y + 4, 4, 3, 0xE02020);
        break;
    case E_FIREBALL:
        R(x, y, 8, 8, 0xF86018); R(x + 2, y + 2, 4, 4, 0xF8E040);
        break;
    case E_COINPOP: {
        float w = ((frame / 3) & 1) ? 6.f : 2.f;
        R(x + (8 - w) * 0.5f, y, w, 14, 0xF8D820);
        break;
    }
    case E_DEBRIS:
        R(x, y, 6, 6, 0xC05028);
        break;
    }
}

void drawMario(int camX) {
    if (!M.visible) return;
    if (M.invuln > 0 && ((frame / 3) & 1)) return;
    float x = (float)((int)M.x - camX), y = (float)(int)M.y, h = (float)M.h;
    int f = M.facing;
    uint32_t shirt = M.power == 2 ? 0xFFFFFF : 0xE02020;
    uint32_t over = M.power == 2 ? 0xE02020 : 0x2040D0;
    const uint32_t skin = 0xF8B878, cap = 0xE02020, boots = 0x603010;
    auto P = [&](float fy0, float fy1, float x0, float x1, uint32_t c) {
        R(x + x0, y + fy0 * h, x1 - x0, (fy1 - fy0) * h, c);
    };
    P(0.00f, 0.20f, 2, 10, cap);
    P(0.12f, 0.20f, f > 0 ? 4.f : 0.f, f > 0 ? 12.f : 8.f, cap);
    P(0.20f, 0.42f, 2, 10, skin);
    P(0.26f, 0.34f, f > 0 ? 7.f : 3.f, f > 0 ? 9.f : 5.f, 0x000000);
    P(0.42f, 0.60f, 0, 12, shirt);
    P(0.60f, 0.84f, 2, 10, over);
    bool stepping = M.ground && fabsf(M.vx) > 0.1f && (((int)M.anim) & 1);
    float lift = stepping ? 2.f : 0.f;
    R(x + 0, y + 0.84f * h - lift, 5, 0.16f * h, boots);
    R(x + 7, y + 0.84f * h, 5, 0.16f * h - lift * 0.0f, boots);
}

void drawHud() {
    char b[24];
    const uint32_t W = 0xFFFFFF;
    text("PLUMBER", 8, 8, 2, W);
    snprintf(b, sizeof b, "%06d", score);    text(b, 8, 24, 2, W);
    text("COINS", 72, 8, 2, W);
    snprintf(b, sizeof b, "X%02d", coinCount); text(b, 72, 24, 2, W);
    text("WORLD", 136, 8, 2, W);
    snprintf(b, sizeof b, "1-%d", levelNo + 1); text(b, 136, 24, 2, W);
    text("TIME", 200, 8, 2, W);
    snprintf(b, sizeof b, "%03d", timeLeft);  text(b, 200, 24, 2, W);
}

void drawWorld() {
    const Theme& th = THEMES[L.theme];
    int camX = (int)cam;
    R(0, 0, VIEW_W, VIEW_H, th.sky);
    for (const Ent& e : ents) if (e.type != E_NONE) drawEnt(e, camX);
    int x0 = camX / TS, x1 = (camX + VIEW_W) / TS;
    for (int tx = x0; tx <= x1 && tx < L.w; tx++)
        for (int ty = 0; ty < LH; ty++) {
            uint8_t t = L.t[ty][tx];
            if (!t) continue;
            float oy = 0;
            for (int i = 0; i < 8; i++)
                if (bumps[i].t > 0 && bumps[i].tx == tx && bumps[i].ty == ty)
                    oy = -(float)(bumps[i].t <= 5 ? bumps[i].t : 10 - bumps[i].t) * 1.2f;
            float px = (float)(tx * TS - camX), py = (float)(ty * TS) + oy;
            drawTile(t, px, py, th);
            if (t == T_POLE && (ty == 0 || L.t[ty - 1][tx] != T_POLE)) {
                R(px + 5, py, 6, 4, 0x30C030);
                R(px - 9, py + 3, 9, 7, 0xFFFFFF); R(px - 6, py + 5, 3, 3, 0xE02020);
            }
        }
    drawMario(camX);
    drawHud();
    if (paused) ctext("PAUSED", 110, 3, 0xFFFFFF);
}

}  // namespace

// ---------------------------------------------------------------- public API
void game_init() {
    memset(&L, 0, sizeof(L));
    memset(ents, 0, sizeof(ents));
    memset(bumps, 0, sizeof(bumps));
    memset(&M, 0, sizeof(M));
    M.h = 16; M.visible = true; M.facing = 1;
    gstate = G_TITLE; frame = 0; paused = false;
    buildLevel(0);
}

void game_pause() { if (gstate == G_PLAY) paused = true; }

void game_update(Input& in) {
    frame++;
    switch (gstate) {
    case G_TITLE:
        if (in.start_edge || in.a_edge) newGame();
        break;
    case G_INTRO:
        if (++stateTimer >= 100) gstate = G_PLAY;
        break;
    case G_PLAY:
        updatePlay(in);
        break;
    case G_GAMEOVER:
        if (++stateTimer >= 200) gstate = G_TITLE;
        break;
    case G_WIN:
        if (++stateTimer > 120 && (in.start_edge || in.a_edge)) gstate = G_TITLE;
        break;
    }
    in.a_edge = in.b_edge = in.start_edge = false;
}

void game_draw() {
    char b[32];
    const uint32_t W = 0xFFFFFF;
    switch (gstate) {
    case G_TITLE: {
        const Theme& th = THEMES[0];
        R(0, 0, VIEW_W, VIEW_H, th.sky);
        for (int tx = 0; tx < 16; tx++) { drawTile(T_GROUND, (float)(tx * TS), 13 * TS, th); drawTile(T_GROUND, (float)(tx * TS), 14 * TS, th); }
        drawTile(T_QBLOCK, 7 * TS, 9 * TS, th);
        ctext("PLUMBER BROS", 52, 4, 0x402010);
        ctext("PLUMBER BROS", 50, 4, W);
        if ((frame / 30) & 1) ctext("PRESS START", 130, 2, W);
        ctext("A JUMP   B RUN FIRE", 160, 1, W);
        break;
    }
    case G_INTRO:
        R(0, 0, VIEW_W, VIEW_H, 0);
        snprintf(b, sizeof b, "WORLD 1-%d", levelNo + 1); ctext(b, 90, 3, W);
        snprintf(b, sizeof b, "LIVES X %d", lives);       ctext(b, 135, 2, W);
        break;
    case G_PLAY:
        drawWorld();
        break;
    case G_GAMEOVER:
        R(0, 0, VIEW_W, VIEW_H, 0);
        ctext("GAME OVER", 105, 3, W);
        break;
    case G_WIN:
        R(0, 0, VIEW_W, VIEW_H, 0);
        ctext("YOU WIN!", 80, 4, W);
        snprintf(b, sizeof b, "SCORE %06d", score); ctext(b, 130, 2, W);
        if (stateTimer > 120 && ((frame / 30) & 1)) ctext("PRESS START", 170, 2, W);
        break;
    }
}

