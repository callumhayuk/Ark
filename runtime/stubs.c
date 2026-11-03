// Ark: host-side pieces LAS Recomp's services expect that Ark doesn't have (yet).
#include <stdint.h>
#include "settings.h"


// LAS save-file features (save editor, extended flags): no-ops.
void save_editor_file_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)path; (void)addr; (void)off; (void)size;
}
void flags_ext_save_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)path; (void)addr; (void)off; (void)size;
}
void flags_ext_save_written(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)path; (void)addr; (void)off; (void)size;
}

#include <stdio.h>
#include <stdlib.h>

// LAS's romfs read hook (randomizer bookkeeping): nothing to do.
void romfs_file_read_hook(const char *path) { (void)path; }

#ifdef ARK_HEADLESS
// Software keyboard text entry without a window (runtime/hle/swkbd.c): ARK_KEYBOARD_TEXT, else the game's initial
// text, else "Ark". Windowed builds ask with a native text box (runtime/ui_prompt.c).
int menu_keyboard_run(const char *title, const char *prompt, const char *initial, int max_len, char *out, int cap) {
    (void)title; (void)prompt; (void)max_len;
    const char *t = getenv("ARK_KEYBOARD_TEXT");
    snprintf(out, (size_t)cap, "%s", t ? t : initial && initial[0] ? initial : "Ark");
    return 1;
}
#endif

#ifdef ARK_HEADLESS
// Headless build (no NVN renderer): the frame counter and 60 fps switch that runtime/patches.c asks the renderer for.
// No frames are presented, so the counter stays 0 and the present-interval switch has nothing to change.
#include <stdint.h>
uint64_t renderer_present_count(void) { return 0; }
void nvn_apply_60fps_interval(int on) { (void)on; }
#endif
