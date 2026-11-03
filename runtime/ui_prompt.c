// Text entry for the software keyboard (runtime/hle/swkbd.c) in windowed builds: a native text box over the game's
// window, so the player can type their name (IME input included: Japanese, Chinese, Korean...).
//   macOS: an NSAlert with a text field; Windows: a small dialog; Linux: zenity or kdialog.
// The box runs on the window's thread (SDL_RunOnMainThread), which keeps pumping the window; the calling game thread
// waits. ARK_KEYBOARD_TEXT=<text> answers without asking (scripted runs); so does LARECOMP_HIDDEN (no window to show).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "rt.h"

typedef struct {
    const char *title, *prompt, *initial;
    int max_len;
    char *out;
    int cap;
    int result;  // 1 entered, 0 cancelled, -1 no text box available
} Prompt;

SDL_Window *ui_window(void);  // runtime/nvn/vulkan/device.c

#if defined(__APPLE__)
// ---- macOS: NSAlert + NSTextField through the Objective-C runtime (keeps this file plain C)
#include <objc/message.h>
#include <objc/runtime.h>

typedef struct { double x, y, w, h; } ark_rect;  // NSRect (CGFloat = double on 64-bit)
#define MSG(ret, ...) ((ret(*)(id, SEL, ##__VA_ARGS__))objc_msgSend)
static id cls(const char *n) { return (id)objc_getClass(n); }
static SEL sel(const char *n) { return sel_registerName(n); }
static id nsstr(const char *s) { return MSG(id, const char *)(cls("NSString"), sel("stringWithUTF8String:"), s ? s : ""); }
void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *);

static void prompt_native(Prompt *p) {
    void *pool = objc_autoreleasePoolPush();
    id alert = MSG(id)(MSG(id)(cls("NSAlert"), sel("alloc")), sel("init"));
    MSG(void, id)(alert, sel("setMessageText:"), nsstr(p->title));
    MSG(void, id)(alert, sel("setInformativeText:"), nsstr(p->prompt));
    // icon: a keyboard (SF Symbol, macOS 11+) instead of the generic executable / folder icon Ark gets as a bare binary
    if (MSG(BOOL, SEL)(cls("NSImage"), sel("respondsToSelector:"), sel("imageWithSystemSymbolName:accessibilityDescription:"))) {
        id img = MSG(id, id, id)(cls("NSImage"), sel("imageWithSystemSymbolName:accessibilityDescription:"), nsstr("keyboard"), nsstr("Keyboard"));
        if (img && MSG(BOOL, SEL)(cls("NSImageSymbolConfiguration"), sel("respondsToSelector:"), sel("configurationWithPointSize:weight:"))) {
            id conf = MSG(id, double, double)(cls("NSImageSymbolConfiguration"), sel("configurationWithPointSize:weight:"), 40.0, 0.0);
            id sized = conf ? MSG(id, id)(img, sel("imageWithSymbolConfiguration:"), conf) : NULL;
            if (sized) img = sized;
        }
        if (img) MSG(void, id)(alert, sel("setIcon:"), img);
    }
    MSG(id, id)(alert, sel("addButtonWithTitle:"), nsstr("OK"));
    MSG(id, id)(alert, sel("addButtonWithTitle:"), nsstr("Cancel"));
    ark_rect r = {0, 0, 260, 24};
    id field = MSG(id, ark_rect)(MSG(id)(cls("NSTextField"), sel("alloc")), sel("initWithFrame:"), r);
    MSG(void, id)(field, sel("setStringValue:"), nsstr(p->initial));
    MSG(void, id)(alert, sel("setAccessoryView:"), field);
    MSG(void, id)(MSG(id)(alert, sel("window")), sel("setInitialFirstResponder:"), field);
    MSG(void, BOOL)(MSG(id)(cls("NSApplication"), sel("sharedApplication")), sel("activateIgnoringOtherApps:"), YES);
    long button = MSG(long)(alert, sel("runModal"));  // NSAlertFirstButtonReturn = 1000
    if (button == 1000) {
        const char *s = MSG(const char *)(MSG(id)(field, sel("stringValue")), sel("UTF8String"));
        snprintf(p->out, (size_t)p->cap, "%s", s ? s : "");
        p->result = 1;
    } else {
        p->result = 0;
    }
    MSG(void)(field, sel("release"));
    MSG(void)(alert, sel("release"));
    objc_autoreleasePoolPop(pool);
}

#elif defined(_WIN32)
// ---- Windows: a dialog from an in-memory template (static text, edit box, OK, Cancel)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum { ID_TEXT = 100, ID_EDIT = 101 };
typedef struct { Prompt *p; wchar_t prompt[600], initial[260]; } DlgData;

static INT_PTR CALLBACK dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    DlgData *d = (DlgData *)GetWindowLongPtrW(h, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        d = (DlgData *)lp;
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)d);
        SetDlgItemTextW(h, ID_TEXT, d->prompt);
        SetDlgItemTextW(h, ID_EDIT, d->initial);
        HWND e = GetDlgItem(h, ID_EDIT);
        if (d->p->max_len > 0) SendMessageW(e, EM_SETLIMITTEXT, (WPARAM)d->p->max_len, 0);
        SendMessageW(e, EM_SETSEL, 0, -1);
        SetForegroundWindow(h);
        SetFocus(e);
        return FALSE;  // (focus set here)
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t w[260];
            GetDlgItemTextW(h, ID_EDIT, w, 260);
            WideCharToMultiByte(CP_UTF8, 0, w, -1, d->p->out, d->p->cap, NULL, NULL);
            EndDialog(h, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
        break;
    }
    return FALSE;
}

static WORD *put_wstr(WORD *w, const wchar_t *s) { do *w++ = (WORD)*s; while (*s++); return w; }
static WORD *dlg_item(WORD *w, DWORD style, short x, short y, short cx, short cy, WORD id, WORD atom, const wchar_t *text) {
    w = (WORD *)(((uintptr_t)w + 3) & ~(uintptr_t)3);  // DLGITEMTEMPLATE is DWORD aligned
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)w;
    it->style = style | WS_CHILD | WS_VISIBLE, it->dwExtendedStyle = 0;
    it->x = x, it->y = y, it->cx = cx, it->cy = cy, it->id = id;
    w = (WORD *)(it + 1);
    *w++ = 0xffff, *w++ = atom;  // predefined class: 0x80 button, 0x81 edit, 0x82 static
    w = put_wstr(w, text);
    *w++ = 0;  // no creation data
    return w;
}

static void prompt_native(Prompt *p) {
    static DlgData d;
    memset(&d, 0, sizeof d);
    d.p = p;
    char both[900];
    snprintf(both, sizeof both, "%s%s%s", p->title, p->prompt[0] ? "\n" : "", p->prompt);
    MultiByteToWideChar(CP_UTF8, 0, both, -1, d.prompt, 600);
    MultiByteToWideChar(CP_UTF8, 0, p->initial, -1, d.initial, 260);

    static WORD buf[1024];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE *t = (DLGTEMPLATE *)buf;
    t->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | DS_SETFOREGROUND | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t->cdit = 4, t->x = 0, t->y = 0, t->cx = 220, t->cy = 82;
    WORD *w = (WORD *)(t + 1);
    *w++ = 0, *w++ = 0;  // no menu, default class
    w = put_wstr(w, L"Ark");
    *w++ = 9;  // font size
    w = put_wstr(w, L"Segoe UI");
    w = dlg_item(w, SS_LEFT, 8, 6, 204, 30, ID_TEXT, 0x82, L"");
    w = dlg_item(w, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 40, 204, 14, ID_EDIT, 0x81, L"");
    w = dlg_item(w, BS_DEFPUSHBUTTON | WS_TABSTOP, 106, 62, 50, 14, IDOK, 0x80, L"OK");
    dlg_item(w, BS_PUSHBUTTON | WS_TABSTOP, 162, 62, 50, 14, IDCANCEL, 0x80, L"Cancel");

    HWND owner = NULL;
    SDL_Window *win = ui_window();
    if (win) owner = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), t, owner, dlg_proc, (LPARAM)&d);
    p->result = r == 1 ? 1 : r == 0 ? 0 : -1;
}

#else
// ---- Linux: zenity, else kdialog
static void shell_quote(char *o, size_t cap, const char *s) {
    size_t n = 0;
    if (n + 1 < cap) o[n++] = '\'';
    for (; *s && n + 5 < cap; s++) {
        if (*s == '\'') memcpy(o + n, "'\\''", 4), n += 4;
        else o[n++] = *s;
    }
    if (n + 1 < cap) o[n++] = '\'';
    o[n] = 0;
}

static void prompt_native(Prompt *p) {
    char t[400], m[1200], i[600], cmd[2600];
    shell_quote(t, sizeof t, p->title);
    char both[900];
    snprintf(both, sizeof both, "%s%s%s", p->title, p->prompt[0] ? "\n" : "", p->prompt);
    shell_quote(m, sizeof m, both);
    shell_quote(i, sizeof i, p->initial);
    if (!system("command -v zenity >/dev/null 2>&1"))
        snprintf(cmd, sizeof cmd, "zenity --entry --title=%s --text=%s --entry-text=%s 2>/dev/null", t, m, i);
    else if (!system("command -v kdialog >/dev/null 2>&1"))
        snprintf(cmd, sizeof cmd, "kdialog --title %s --inputbox %s %s 2>/dev/null", t, m, i);
    else { p->result = -1; return; }
    FILE *f = popen(cmd, "r");
    if (!f) { p->result = -1; return; }
    char line[600] = "";
    bool got = fgets(line, sizeof line, f) != NULL;
    int status = pclose(f);
    line[strcspn(line, "\r\n")] = 0;
    if (status != 0 || !got) { p->result = 0; return; }
    snprintf(p->out, (size_t)p->cap, "%s", line);
    p->result = 1;
}
#endif

static void SDLCALL prompt_on_main(void *ud) {
    prompt_native((Prompt *)ud);
    // give the keyboard back to the game: the box's window had it, and closing it left no window focused (the
    // game ignored keys until clicked)
    SDL_Window *w = ui_window();
    if (w) SDL_RaiseWindow(w);
}

int menu_keyboard_run(const char *title, const char *prompt, const char *initial, int max_len, char *out, int cap) {
    const char *t = getenv("ARK_KEYBOARD_TEXT");
    if (t || getenv("LARECOMP_HIDDEN") || !ui_window()) {
        snprintf(out, (size_t)cap, "%s", t ? t : initial && initial[0] ? initial : "Ark");
        return 1;
    }
    Prompt p = {title ? title : "", prompt ? prompt : "", initial ? initial : "", max_len, out, cap, -1};
    out[0] = 0;
    if (!SDL_RunOnMainThread(prompt_on_main, &p, true)) {
        LOG("[swkbd] couldn't show the text box: %s\n", SDL_GetError());
        p.result = -1;
    }
    extern void input_reset(void);  // keys held when the box opened (Enter, A) would stay down: runtime/hle/input.c
    input_reset();
    if (p.result < 0) {  // no text box here (Linux without zenity / kdialog)
        LOG("[swkbd] no text box available: answering \"%s\" (set ARK_KEYBOARD_TEXT to choose)\n", initial && initial[0] ? initial : "Ark");
        snprintf(out, (size_t)cap, "%s", initial && initial[0] ? initial : "Ark");
        return 1;
    }
    return p.result;
}
