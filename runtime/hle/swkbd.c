// Software keyboard (the player's name, nicknames, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; Ark has no applets, so ShowKeyboard is replaced here. The text comes from menu_keyboard_run: a
// native text box on the window's thread in windowed builds (runtime/ui_prompt.c), ARK_KEYBOARD_TEXT otherwise
// (runtime/stubs.c). This (game) thread waits for the answer; the window stays responsive meanwhile.
#include <stdio.h>
#include <string.h>

#include "../hle.h"

// title / prompt: UTF-8 (may be empty). Returns 1 with the text in out (UTF-8), 0 if the player cancelled.
int menu_keyboard_run(const char *title, const char *prompt, const char *initial, int max_len, char *out, int cap);

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}. ShowKeyboardArg starts with the SDK's KeyboardConfig:
//   +0x024 headerText[65]  +0x0a6 subText[129]  +0x1a8 guideText[257]   (UTF-16)
//   +0x3ac textMaxLength   +0x3b0 textMinLength
//   +0x3bd isUseUtf8: the answer is UTF-8 when set, UTF-16 otherwise (Legends: Arceus: UTF-16)
//   +0x3c0 / +0x3c4 initial text offset (bytes) / length (UTF-16 units) in the work buffer (+0x4d0)
// Writing UTF-8 into a UTF-16 answer was the "name is one kanji" bug: "Ark" read as u16 is U+7241 'k'.
#define KC_HEADER 0x024
#define KC_SUB 0x0a6
#define KC_GUIDE 0x1a8
#define KC_MAX_LEN 0x3ac
#define KC_MIN_LEN 0x3b0
#define KC_UTF8 0x3bd
#define KC_INIT_OFF 0x3c0
#define KC_INIT_LEN 0x3c4
#define ARG_WORK_BUF 0x4d0

// UTF-16 (n units, or up to a 0 when n < 0) -> UTF-8, always terminated.
static void u16_to_utf8(const uint16_t *s, int n, char *out, size_t cap) {
    size_t o = 0;
    for (int i = 0; (n < 0 || i < n) && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && (n < 0 || i + 1 < n) && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        char b[4];
        int k;
        if (c < 0x80) b[0] = (char)c, k = 1;
        else if (c < 0x800) b[0] = (char)(0xc0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3f)), k = 2;
        else if (c < 0x10000) b[0] = (char)(0xe0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 0x3f)), b[2] = (char)(0x80 | (c & 0x3f)), k = 3;
        else b[0] = (char)(0xf0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 0x3f)), b[2] = (char)(0x80 | (c >> 6 & 0x3f)), b[3] = (char)(0x80 | (c & 0x3f)), k = 4;
        if (o + k >= cap) break;
        memcpy(out + o, b, k), o += k;
    }
    out[o] = 0;
}

// Next code point of a UTF-8 string (invalid bytes -> U+FFFD); advances *p.
static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = *s++;
    int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (c >= 0x80 && !extra) c = 0xfffd;
    else if (extra) c &= 0x3f >> extra;
    for (; extra; extra--, s++) {
        if ((*s & 0xc0) != 0x80) { c = 0xfffd; break; }
        c = c << 6 | (*s & 0x3f);
    }
    *p = (const char *)s;
    return c;
}

// The answer, at most max_units UTF-16 units (whole characters), into the game's buffer.
static void write_answer(uint64_t ptr, uint64_t size, const char *text, int utf8, int max_units) {
    if (!ptr || !size) return;
    memset((void *)(uintptr_t)ptr, 0, size);
    int units = 0;
    if (utf8) {
        char *o = (char *)(uintptr_t)ptr;
        size_t n = 0;
        for (const char *p = text; *p;) {
            const char *start = p;
            uint32_t c = utf8_next(&p);
            int u = c >= 0x10000 ? 2 : 1;
            size_t len = (size_t)(p - start);
            if ((max_units > 0 && units + u > max_units) || n + len + 1 > size) break;
            memcpy(o + n, start, len), n += len, units += u;
        }
        return;
    }
    uint16_t *o = (uint16_t *)(uintptr_t)ptr;
    size_t cap = size / 2, n = 0;
    for (const char *p = text; *p;) {
        uint32_t c = utf8_next(&p);
        int u = c >= 0x10000 ? 2 : 1;
        if ((max_units > 0 && units + u > max_units) || n + u + 1 > cap) break;
        if (u == 2) o[n++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10)), o[n++] = (uint16_t)(0xdc00 + ((c - 0x10000) & 0x3ff));
        else o[n++] = (uint16_t)c;
        units += u;
    }
}

HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out), size = LD64(out + 8);
    const uint8_t *cfg = (const uint8_t *)(uintptr_t)arg;
    int max_len = *(const int32_t *)(cfg + KC_MAX_LEN), min_len = *(const int32_t *)(cfg + KC_MIN_LEN);
    int utf8 = cfg[KC_UTF8] != 0;
    uint32_t off = *(const uint32_t *)(cfg + KC_INIT_OFF), n = *(const uint32_t *)(cfg + KC_INIT_LEN);
    uint64_t wb = LD64(arg + ARG_WORK_BUF);

    char initial[256] = "", header[256], sub[384], guide[768];
    if (wb && n && n < 256) u16_to_utf8((const uint16_t *)(uintptr_t)(wb + off), (int)n, initial, sizeof initial);
    u16_to_utf8((const uint16_t *)(cfg + KC_HEADER), 65, header, sizeof header);
    u16_to_utf8((const uint16_t *)(cfg + KC_SUB), 129, sub, sizeof sub);
    u16_to_utf8((const uint16_t *)(cfg + KC_GUIDE), 257, guide, sizeof guide);
    // the box's message: the game's header line, else its guide text ("Enter your name" and the like)
    const char *title = header[0] ? header : guide[0] ? guide : "Enter text";
    char prompt[512];
    snprintf(prompt, sizeof prompt, "%s%s%s", sub, sub[0] && guide[0] && title != guide ? "\n" : "", title != guide ? guide : "");

    char text[256];
    int ok = menu_keyboard_run(title, prompt, initial, max_len, text, sizeof text);
    if (!ok || !text[0]) snprintf(text, sizeof text, "%s", initial[0] ? initial : "Ark");  // cancelled / empty: keep a name
    LOG("[swkbd] text entry \"%s\" (max %d, min %d, %s, initial \"%s\"): %s \"%s\"\n", title, max_len, min_len,
        utf8 ? "UTF-8" : "UTF-16", initial, ok ? "entered" : "cancelled", text);
    write_answer(ptr, size, text, utf8, max_len);
    c->x[0] = 0;
}
