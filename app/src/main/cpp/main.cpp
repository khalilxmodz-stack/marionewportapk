// Native Android frontend: NativeActivity + EGL + GLES2 batch renderer,
// on-screen touch controls, keyboard / gamepad input. 24 FPS render cap,
// game logic runs on a fixed 60 Hz timestep.
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <chrono>
#include <vector>
#include "game.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "plumber", __VA_ARGS__)

namespace {

using Clock = std::chrono::steady_clock;
const int TARGET_FPS = 24;            // render rate; change here if you want smoother
const double TICK = 1.0 / 60.0;       // logic rate

struct Vtx { float x, y, r, g, b, a; };
struct Pad { bool left = false, right = false, up = false, down = false, a = false, b = false, start = false; };
struct Btn { float x, y, w, h; int id; };   // id: 0 left, 1 right, 2 jump, 3 run/fire, 4 start

std::vector<Vtx> g_batch;
EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLSurface g_surf = EGL_NO_SURFACE;
EGLContext g_ctx = EGL_NO_CONTEXT;
GLuint g_prog = 0;
GLint g_locRes = -1;
int g_w = 0, g_h = 0;
float g_scale = 1, g_offX = 0, g_offY = 0;
bool g_ready = false;
std::vector<Btn> g_btns;

Pad g_touch, g_key, g_joy;
Input g_in;

// ------------------------------------------------------------------ input
void refresh() {
    g_in.left  = g_touch.left  || g_key.left  || g_joy.left;
    g_in.right = g_touch.right || g_key.right || g_joy.right;
    g_in.up    = g_touch.up    || g_key.up    || g_joy.up;
    g_in.down  = g_touch.down  || g_key.down  || g_joy.down;
    g_in.a     = g_touch.a     || g_key.a     || g_joy.a;
    g_in.b     = g_touch.b     || g_key.b     || g_joy.b;
    g_in.start = g_touch.start || g_key.start || g_joy.start;
}
void applyPad(Pad& old, const Pad& n) {
    if (!old.a && n.a) g_in.a_edge = true;
    if (!old.b && n.b) g_in.b_edge = true;
    if (!old.start && n.start) g_in.start_edge = true;
    old = n;
    refresh();
}

void handleTouch(AInputEvent* ev) {
    int action = AMotionEvent_getAction(ev);
    int act = action & AMOTION_EVENT_ACTION_MASK;
    int idx = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    Pad n;
    if (act != AMOTION_EVENT_ACTION_CANCEL) {
        size_t cnt = AMotionEvent_getPointerCount(ev);
        for (size_t i = 0; i < cnt; i++) {
            if ((act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_POINTER_UP) && (int)i == idx) continue;
            float px = AMotionEvent_getX(ev, i), py = AMotionEvent_getY(ev, i);
            for (const Btn& b : g_btns) {
                if (px < b.x || px > b.x + b.w || py < b.y || py > b.y + b.h) continue;
                switch (b.id) {
                case 0: n.left = true; break;
                case 1: n.right = true; break;
                case 2: n.a = true; break;
                case 3: n.b = true; break;
                case 4: n.start = true; break;
                }
            }
        }
    }
    applyPad(g_touch, n);
}

void handleJoystick(AInputEvent* ev) {
    float ax = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0);
    float ay = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0);
    float hx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_X, 0);
    float hy = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_Y, 0);
    Pad n = g_joy;
    n.left = ax < -0.4f || hx < -0.5f;
    n.right = ax > 0.4f || hx > 0.5f;
    n.up = ay < -0.5f || hy < -0.5f;
    n.down = ay > 0.5f || hy > 0.5f;
    applyPad(g_joy, n);
}

bool handleKey(AInputEvent* ev) {
    int code = AKeyEvent_getKeyCode(ev);
    bool down = AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN;
    Pad n = g_key;
    switch (code) {
    case AKEYCODE_DPAD_LEFT:  n.left = down; break;
    case AKEYCODE_DPAD_RIGHT: n.right = down; break;
    case AKEYCODE_DPAD_UP:    n.up = down; break;
    case AKEYCODE_DPAD_DOWN:  n.down = down; break;
    case AKEYCODE_BUTTON_A: case AKEYCODE_SPACE: case AKEYCODE_Z: case AKEYCODE_DPAD_CENTER:
        n.a = down; break;
    case AKEYCODE_BUTTON_B: case AKEYCODE_BUTTON_X: case AKEYCODE_X: case AKEYCODE_SHIFT_LEFT:
        n.b = down; break;
    case AKEYCODE_BUTTON_START: case AKEYCODE_ENTER: case AKEYCODE_BUTTON_SELECT:
        n.start = down; break;
    default: return false;      // let the system handle Back, volume, etc.
    }
    applyPad(g_key, n);
    return true;
}

int32_t onInput(android_app*, AInputEvent* ev) {
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) return handleKey(ev) ? 1 : 0;
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
        int src = AInputEvent_getSource(ev);
        if ((src & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) { handleTouch(ev); return 1; }
        if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) { handleJoystick(ev); return 1; }
    }
    return 0;
}

// ------------------------------------------------------------------ rendering
void pushQuad(float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    if (g_batch.size() > 60000) return;
    g_batch.push_back({x0, y0, r, g, b, a}); g_batch.push_back({x1, y0, r, g, b, a}); g_batch.push_back({x0, y1, r, g, b, a});
    g_batch.push_back({x1, y0, r, g, b, a}); g_batch.push_back({x1, y1, r, g, b, a}); g_batch.push_back({x0, y1, r, g, b, a});
}
void pushTri(float x0, float y0, float x1, float y1, float x2, float y2, float a) {
    g_batch.push_back({x0, y0, 1, 1, 1, a}); g_batch.push_back({x1, y1, 1, 1, 1, a}); g_batch.push_back({x2, y2, 1, 1, 1, a});
}

void flush() {
    if (g_batch.empty()) return;
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), &g_batch[0].x);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), &g_batch[0].r);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_batch.size());
    g_batch.clear();
}

}  // namespace

// Called by the game; coordinates are in the 256x240 virtual screen.
void gfx_rect(float x, float y, float w, float h, uint32_t rgb) {
    float x0 = roundf(g_offX + x * g_scale), x1 = roundf(g_offX + (x + w) * g_scale);
    float y0 = roundf(g_offY + y * g_scale), y1 = roundf(g_offY + (y + h) * g_scale);
    if (x1 <= x0 || y1 <= y0) return;
    pushQuad(x0, y0, x1, y1, ((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1.f);
}

namespace {

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof log, nullptr, log); LOGE("shader: %s", log); }
    return s;
}

void layout() {
    float m = fminf((float)g_w, (float)g_h);
    g_scale = fminf(g_w / (float)VIEW_W, g_h / (float)VIEW_H);
    g_offX = (g_w - VIEW_W * g_scale) * 0.5f;
    g_offY = (g_h > g_w) ? g_h * 0.06f : (g_h - VIEW_H * g_scale) * 0.5f;

    float s = m * 0.19f, gap = s * 0.2f, by = g_h - s - gap;
    g_btns.clear();
    g_btns.push_back({gap, by, s, s, 0});
    g_btns.push_back({gap + s + gap, by, s, s, 1});
    g_btns.push_back({g_w - s - gap, by, s, s, 2});
    g_btns.push_back({g_w - 2 * s - 2 * gap, by, s, s, 3});
    g_btns.push_back({g_w * 0.5f - s * 0.4f, gap, s * 0.8f, s * 0.4f, 4});
}

bool initDisplay(android_app* app) {
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g_dpy, nullptr, nullptr);
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(g_dpy, attrs, &cfg, 1, &n) || n < 1) { LOGE("no EGL config"); return false; }
    EGLint fmt;
    eglGetConfigAttrib(g_dpy, cfg, EGL_NATIVE_VISUAL_ID, &fmt);
    ANativeWindow_setBuffersGeometry(app->window, 0, 0, fmt);
    g_surf = eglCreateWindowSurface(g_dpy, cfg, app->window, nullptr);
    const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx) == EGL_FALSE) { LOGE("eglMakeCurrent failed"); return false; }

    const char* vs =
        "attribute vec2 aPos; attribute vec4 aCol; uniform vec2 uRes; varying vec4 vCol;\n"
        "void main(){ vec2 p = aPos / uRes * 2.0 - 1.0; gl_Position = vec4(p.x, -p.y, 0.0, 1.0); vCol = aCol; }\n";
    const char* fs =
        "precision mediump float; varying vec4 vCol;\n"
        "void main(){ gl_FragColor = vCol; }\n";
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(g_prog, 0, "aPos");
    glBindAttribLocation(g_prog, 1, "aCol");
    glLinkProgram(g_prog);
    g_locRes = glGetUniformLocation(g_prog, "uRes");
    glUseProgram(g_prog);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    eglSwapInterval(g_dpy, 1);
    return true;
}

void termDisplay() {
    if (g_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_ctx != EGL_NO_CONTEXT) eglDestroyContext(g_dpy, g_ctx);
        if (g_surf != EGL_NO_SURFACE) eglDestroySurface(g_dpy, g_surf);
        eglTerminate(g_dpy);
    }
    g_dpy = EGL_NO_DISPLAY; g_ctx = EGL_NO_CONTEXT; g_surf = EGL_NO_SURFACE;
    g_prog = 0; g_ready = false;
}

void drawButton(const Btn& b) {
    bool pressed = false;
    switch (b.id) {
    case 0: pressed = g_in.left; break;
    case 1: pressed = g_in.right; break;
    case 2: pressed = g_in.a; break;
    case 3: pressed = g_in.b; break;
    case 4: pressed = g_in.start; break;
    }
    float a = pressed ? 0.5f : 0.22f;
    pushQuad(b.x, b.y, b.x + b.w, b.y + b.h, 1, 1, 1, a * 0.6f);
    float t = fmaxf(2.f, b.w * 0.03f);   // border thickness
    pushQuad(b.x, b.y, b.x + b.w, b.y + t, 1, 1, 1, a);
    pushQuad(b.x, b.y + b.h - t, b.x + b.w, b.y + b.h, 1, 1, 1, a);
    pushQuad(b.x, b.y, b.x + t, b.y + b.h, 1, 1, 1, a);
    pushQuad(b.x + b.w - t, b.y, b.x + b.w, b.y + b.h, 1, 1, 1, a);
    float ga = pressed ? 0.95f : 0.6f;
    float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f;
    if (b.id == 0) pushTri(cx + b.w * 0.18f, cy - b.h * 0.25f, cx + b.w * 0.18f, cy + b.h * 0.25f, cx - b.w * 0.2f, cy, ga);
    else if (b.id == 1) pushTri(cx - b.w * 0.18f, cy - b.h * 0.25f, cx - b.w * 0.18f, cy + b.h * 0.25f, cx + b.w * 0.2f, cy, ga);
    else if (b.id == 4) {
        pushQuad(cx - b.w * 0.15f, cy - b.h * 0.25f, cx - b.w * 0.05f, cy + b.h * 0.25f, 1, 1, 1, ga);
        pushQuad(cx + b.w * 0.05f, cy - b.h * 0.25f, cx + b.w * 0.15f, cy + b.h * 0.25f, 1, 1, 1, ga);
    } else {
        // tiny 3x5 "A" / "B" glyph
        const unsigned g = (b.id == 2) ? 0b010101111101101u : 0b110101110101110u;
        float c = b.w * 0.11f, gx = cx - 1.5f * c, gy = cy - 2.5f * c;
        for (int r = 0; r < 5; r++)
            for (int k = 0; k < 3; k++)
                if (g & (1u << (14 - (r * 3 + k)))) pushQuad(gx + k * c, gy + r * c, gx + (k + 1) * c, gy + (r + 1) * c, 1, 1, 1, ga);
    }
}

void render() {
    EGLint w = 0, h = 0;
    eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &w);
    eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &h);
    if (w != g_w || h != g_h) { g_w = w; g_h = h; layout(); }

    glViewport(0, 0, g_w, g_h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g_prog);
    glUniform2f(g_locRes, (float)g_w, (float)g_h);

    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)roundf(g_offX), (GLint)roundf(g_h - (g_offY + VIEW_H * g_scale)),
              (GLsizei)roundf(VIEW_W * g_scale), (GLsizei)roundf(VIEW_H * g_scale));
    game_draw();
    flush();
    glDisable(GL_SCISSOR_TEST);

    for (const Btn& b : g_btns) drawButton(b);
    flush();
    eglSwapBuffers(g_dpy, g_surf);
}

void onCmd(android_app* app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window && initDisplay(app)) { g_w = g_h = 0; g_ready = true; }
        break;
    case APP_CMD_TERM_WINDOW:
        termDisplay();
        break;
    case APP_CMD_PAUSE:
    case APP_CMD_LOST_FOCUS:
        game_pause();
        g_touch = g_key = g_joy = Pad();
        refresh();
        break;
    default: break;
    }
}

}  // namespace

void android_main(android_app* app) {
    app->onAppCmd = onCmd;
    app->onInputEvent = onInput;
    //android_app_set_motion_event_filter(app, nullptr);
    // also deliver gamepad axis events
    game_init();

    const auto period = std::chrono::microseconds(1000000 / TARGET_FPS);
    auto nextFrame = Clock::now();
    auto last = Clock::now();
    double acc = 0;

    for (;;) {
        int timeout = -1;
        if (g_ready) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(nextFrame - Clock::now()).count();
            timeout = ms > 0 ? (int)ms : 0;
        }
        int events;
        android_poll_source* src;
        while (ALooper_pollOnce(timeout, nullptr, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) { termDisplay(); return; }
            timeout = 0;                       // drain everything that is pending
        }
        if (!g_ready) { last = Clock::now(); nextFrame = last; continue; }

        auto now = Clock::now();
        if (now < nextFrame) continue;

        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (dt > 0.25) dt = 0.25;
        acc += dt;
        int n = 0;
        while (acc >= TICK && n < 8) { game_update(g_in); acc -= TICK; n++; }
        if (n == 8) acc = 0;

        render();
        nextFrame += period;
        if (nextFrame < Clock::now()) nextFrame = Clock::now() + period;
    }
}
