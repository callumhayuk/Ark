// Ark user settings (ark.ini in the working directory). Based on LAS Recomp's settings.h (MIT); the Link's Awakening
// options are gone, the display / audio ones keep LAS's meaning so its renderer code works unchanged.
#pragma once
#include <stdbool.h>

enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1 };
enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12 };
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* AMD FSR 1 */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1 };

typedef struct {
    int aspect;          // ASPECT_*: unlocked = fill the window, original = 16:9 with bars
    bool fullscreen;
    int res_mode;        // RES_MATCH, 1..8 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int upscaler;        // UPSCALE_*
    int antialias;       // AA_*
    float volume;        // master volume, 1 = 100%
    bool reverb;         // the audio renderer's reverb effect
    bool movie_audio;
    bool show_fps;
    int shiny_rolls;     // shiny roll multiplier, 0..500 (0: no random shinies; runtime/patches.c ark_shiny_rolls)
    int alpha_rolls;     // alpha slot picks, 0..100 (0: avoid alpha slots; runtime/patches.c ark_alpha_pick)
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
