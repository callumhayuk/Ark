// Ark: run-time switches for testing, on function keys in the game window. The window title shows their state.
//   F2  60 fps on / off (the frame time table, the present interval and the 1/30 s code sites follow at once)
//   F3  full-detail models (no level of detail)        F4  grass and ground detail to their generation limit
//   F5  dialogue text at the 30 fps pace (at 60 fps)   F6  second frame-time table halved (old behaviour, to compare)
//   F7  terrain detail range (x1, x2, x4, x6 of the game's 40 m)
//   F8  game speed x1 -> x2 -> x4 -> unlimited (runtime/patches.c ark_speed)
//   F9  frame dump (build/pass_*.png)                   F10 timing scan (runtime/timescan.c)
// (F1 and F11 stay with the window: menu, fullscreen.)
#include <SDL3/SDL.h>
#include <stdio.h>

#include "rt.h"

extern int ark_switch_get(int which);
extern void ark_switch_toggle(int which);
extern void ui_set_title(const char *title);

void hotkeys_update_title(void) {
    extern float ark_terrain_factor(void);
    extern int ark_speed(void);
    char t[340], sp[16];
    int s = ark_speed();
    if (s) snprintf(sp, sizeof sp, "x%d", s); else snprintf(sp, sizeof sp, "max");
    snprintf(t, sizeof t, "Ark  |  F2 60 fps: %s  F3 full detail: %s  F4 grass: %s  F5 text pace: %s  F6 table2: %s  F7 terrain: x%g  F8 speed: %s",
             ark_switch_get(0) ? "ON" : "off", ark_switch_get(1) ? "ON" : "off", ark_switch_get(2) ? "ON" : "off",
             ark_switch_get(3) ? "ON" : "off", ark_switch_get(4) ? "ON" : "off", ark_terrain_factor(), sp);
    ui_set_title(t);
}

// true when the key was one of the switches (the game doesn't get it)
bool hotkeys_handle(const SDL_Event *e) {
    if (e->type != SDL_EVENT_KEY_DOWN || e->key.repeat) return false;
    int which = -1;
    switch (e->key.scancode) {
    case SDL_SCANCODE_F2: which = 0; break;
    case SDL_SCANCODE_F3: which = 1; break;
    case SDL_SCANCODE_F4: which = 2; break;
    case SDL_SCANCODE_F5: which = 3; break;
    case SDL_SCANCODE_F6: which = 4; break;
    case SDL_SCANCODE_F7: which = 5; break;
    case SDL_SCANCODE_F8: {
        extern void ark_speed_cycle(void);
        ark_speed_cycle();
        hotkeys_update_title();
        return true;
    }
    default: return false;
    }
    ark_switch_toggle(which);
    hotkeys_update_title();
    return true;
}
