// Ark: the in-game overlay menu (runtime/ui/menu.cpp, Dear ImGui; F1). The Vulkan backend and input call these.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL_events.h>
#ifdef __cplusplus
extern "C" {
#endif
bool menu_handle_event(const SDL_Event *e);             // window thread; true = the menu's event (F1, Esc, mouse)
void menu_apply_window_requests(void *sdl_window);      // window thread, after each batch of events
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h);  // render thread, inside the window image's pass
bool menu_input_captured(void);                         // open: the game gets no buttons / sticks (hle/input.c)
void menu_test_open(void);                              // LARECOMP_MENU_AT=<frame>
#ifdef __cplusplus
}
#endif
