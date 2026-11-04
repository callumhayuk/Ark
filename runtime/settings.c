// Ark settings: ark.ini (key = value lines) in the working directory. Unknown keys are ignored, missing ones keep
// their defaults. Based on LAS Recomp's settings.c (MIT).
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

Settings g_settings = {
    .aspect = ASPECT_ORIGINAL,
    .res_mode = 1,  // 1x the Switch's 1080p
    .upscaler = UPSCALE_SHARP,
    .antialias = AA_OFF,
    .volume = 1.0f,
    .reverb = true,
    .movie_audio = true,
    .show_fps = true,  // (frame rate counter, top left: runtime/ui/menu.cpp)
    .shiny_rolls = 1,
    .alpha_rolls = 1,
};

#define INI "ark.ini"

static bool parse_bool(const char *v) { return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on"); }
static int clamp_rolls(int n, int max) { return n < 0 ? 0 : n > max ? max : n; }

void settings_load(void) {
    FILE *f = fopen(INI, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64], val[128];
        if (sscanf(line, " %63[^= ] = %127[^\r\n]", key, val) != 2 || key[0] == '#') continue;
        if (!strcmp(key, "aspect")) g_settings.aspect = !strcmp(val, "original") ? ASPECT_ORIGINAL : ASPECT_UNLOCKED;
        else if (!strcmp(key, "fullscreen")) g_settings.fullscreen = parse_bool(val);
        else if (!strcmp(key, "resolution")) {
            if (!strcmp(val, "match")) g_settings.res_mode = RES_MATCH;
            else if (!strcmp(val, "dynamic")) g_settings.res_mode = RES_DYNAMIC;
            else if (!strcmp(val, "75%")) g_settings.res_mode = RES_Q75;
            else if (!strcmp(val, "50%")) g_settings.res_mode = RES_Q50;
            else if (!strcmp(val, "25%")) g_settings.res_mode = RES_Q25;
            else { int n = atoi(val); if (n >= 1 && n <= 8) g_settings.res_mode = n; }
        } else if (!strcmp(key, "upscaler")) g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : UPSCALE_SHARP;
        else if (!strcmp(key, "antialias")) g_settings.antialias = !strcmp(val, "smaa") ? AA_SMAA : AA_OFF;
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val) / 100.0f;
        else if (!strcmp(key, "reverb")) g_settings.reverb = parse_bool(val);
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = parse_bool(val);
        else if (!strcmp(key, "show_fps")) g_settings.show_fps = parse_bool(val);
        else if (!strcmp(key, "shiny_rolls")) g_settings.shiny_rolls = clamp_rolls(atoi(val), 500);
        else if (!strcmp(key, "alpha_rolls")) g_settings.alpha_rolls = clamp_rolls(atoi(val), 100);
    }
    fclose(f);
}

void settings_save(void) {
    FILE *f = fopen(INI, "w");
    if (!f) return;
    static const char *res[] = {"match", "1", "2", "3", "4", "5", "6", "7", "8", "dynamic", "75%", "25%", "50%"};
    fprintf(f, "aspect = %s\n", g_settings.aspect == ASPECT_ORIGINAL ? "original" : "unlocked");
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    fprintf(f, "resolution = %s\n", g_settings.res_mode >= 0 && g_settings.res_mode <= 12 ? res[g_settings.res_mode] : "1");
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_BILINEAR ? "bilinear" : "sharp");
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : "off");
    fprintf(f, "volume = %d\n", (int)(g_settings.volume * 100.0f + 0.5f));
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    fprintf(f, "shiny_rolls = %d\n", g_settings.shiny_rolls);
    fprintf(f, "alpha_rolls = %d\n", g_settings.alpha_rolls);
    fclose(f);
}
