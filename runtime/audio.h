#pragma once
#include <stdint.h>

typedef struct {
    unsigned underruns;       /* game audio ran dry */
    int fill_frames;          /* buffered game audio */
    int target_frames;
    int device_rate, device_samples;
    int playing;
} AudioStats;

int  audio_init(int latency_mode);              /* 0 ok, <0 no audio device */
void audio_shutdown(void);
int  audio_rate(void);
int  audio_ok(void);

/* ---- game audio (producer: emulation thread) ---- */
void audio_game_begin(void);
void audio_game_end(void);
void audio_game_push(const int16_t *stereo, int frames);
int  audio_game_target(void);                   /* how many frames to keep buffered */
void audio_game_wait(int target_frames);        /* block until buffered <= target (or aborted) */
void audio_game_abort(void);
void audio_game_set_paused(int p);

/* ---- menu music and UI sounds (main thread) ---- */
enum { SFX_HOVER, SFX_CLICK, SFX_CONFIRM, SFX_BACK, SFX_TOGGLE };
void audio_menu_apply(void);                    /* pick up settings (volumes, custom files) */
void audio_menu_music(int on);
void audio_sfx(int which);
void audio_sfx_preview(int which);              /* plays even if UI sounds are switched off */
void audio_music_preview(void);

void audio_get_stats(AudioStats *s);

/* ---- controller speaker / haptics (DualSense over USB) ---- */
int  audio_pad_open(void);                      /* returns channel count or 0 */
void audio_pad_close(void);
int  audio_pad_available(void);
void audio_pad_set_gain(float speaker, float haptics);
void audio_pad_push(const int16_t *stereo, int frames);
void audio_pad_play_beep(int kind);             /* short built-in sound for the test button */
