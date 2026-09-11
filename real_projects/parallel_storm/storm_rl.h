#ifndef STORM_RL_H
#define STORM_RL_H

#ifdef __cplusplus
extern "C" {
#endif

void storm_rl_init(int win_w, int win_h, int fb_w, int fb_h, const char* title);
int storm_rl_should_close(void);
void storm_rl_present(const unsigned char* rgba, const char* hud);
void storm_rl_shutdown(void);

int storm_rl_key_w(void);
int storm_rl_key_a(void);
int storm_rl_key_s(void);
int storm_rl_key_d(void);
int storm_rl_key_shift(void);
int storm_rl_key_q(void);
int storm_rl_key_esc_pressed(void);
int storm_rl_click(void);

int storm_rl_captured(void);
void storm_rl_set_capture(int on);
void storm_rl_mouse_delta(float* dx, float* dy);
float storm_rl_dt(void);

#ifdef __cplusplus
}
#endif

#endif
