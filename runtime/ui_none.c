// Ark: no-ops for LAS Recomp features Ark doesn't have (save editor, window icon, level names). The overlay menu's
// hooks are in runtime/ui/menu.cpp.
#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL_events.h>

void save_editor_frame(uint64_t frame) { (void)frame; }

typedef struct SDL_Window SDL_Window;
void rt_window_icon(SDL_Window *w) { (void)w; }
const char *game_level_name(void) { return "?"; }  // (LAS logged the current map with its FPS line)
