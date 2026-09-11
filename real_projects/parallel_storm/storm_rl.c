#include "storm_rl.h"
#include "raylib.h"

static Texture2D g_tex;
static int g_fb_w;
static int g_fb_h;
static int g_captured;

void storm_rl_init(int win_w, int win_h, int fb_w, int fb_h, const char* title) {
    Image img;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(win_w, win_h, title);
    SetTargetFPS(0);
    g_fb_w = fb_w;
    g_fb_h = fb_h;
    img = GenImageColor(fb_w, fb_h, BLACK);
    g_tex = LoadTextureFromImage(img);
    UnloadImage(img);
    SetTextureFilter(g_tex, TEXTURE_FILTER_BILINEAR);
}

int storm_rl_should_close(void) {
    return WindowShouldClose();
}

void storm_rl_present(const unsigned char* rgba, const char* hud) {
    Rectangle src;
    Rectangle dst;
    Vector2 origin;
    UpdateTexture(g_tex, rgba);
    BeginDrawing();
    ClearBackground(BLACK);
    src.x = 0.0f;
    src.y = 0.0f;
    src.width = (float)g_fb_w;
    src.height = (float)g_fb_h;
    dst.x = 0.0f;
    dst.y = 0.0f;
    dst.width = (float)GetScreenWidth();
    dst.height = (float)GetScreenHeight();
    origin.x = 0.0f;
    origin.y = 0.0f;
    DrawTexturePro(g_tex, src, dst, origin, 0.0f, WHITE);
    DrawFPS(8, 8);
    if (hud && hud[0])
        DrawText(hud, 8, 32, 16, RAYWHITE);
    EndDrawing();
}

void storm_rl_shutdown(void) {
    if (g_captured)
        EnableCursor();
    UnloadTexture(g_tex);
    CloseWindow();
}

int storm_rl_key_w(void) { return IsKeyDown(KEY_W); }
int storm_rl_key_a(void) { return IsKeyDown(KEY_A); }
int storm_rl_key_s(void) { return IsKeyDown(KEY_S); }
int storm_rl_key_d(void) { return IsKeyDown(KEY_D); }
int storm_rl_key_shift(void) { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }
int storm_rl_key_q(void) { return IsKeyDown(KEY_Q); }
int storm_rl_key_esc_pressed(void) { return IsKeyPressed(KEY_ESCAPE); }
int storm_rl_click(void) { return IsMouseButtonPressed(MOUSE_BUTTON_LEFT); }

int storm_rl_captured(void) { return g_captured; }

void storm_rl_set_capture(int on) {
    if (on && !g_captured) {
        DisableCursor();
        g_captured = 1;
    } else if (!on && g_captured) {
        EnableCursor();
        g_captured = 0;
    }
}

void storm_rl_mouse_delta(float* dx, float* dy) {
    Vector2 d;
    if (!g_captured) {
        *dx = 0.0f;
        *dy = 0.0f;
        return;
    }
    d = GetMouseDelta();
    *dx = d.x;
    *dy = d.y;
}

float storm_rl_dt(void) {
    return GetFrameTime();
}
