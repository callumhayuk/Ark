// Ark: the in-game overlay menu (F1). Dear ImGui (third_party/imgui, v1.91.9) drawn by its Vulkan backend into the
// window image after the game's picture (nvn/vulkan/device.c backend_present -> menu_render, on the render thread).
//
// Threads: events arrive on the window's thread (device.c ui_run -> menu_handle_event), frames are built on the
// render thread; ImGui's state is shared between them under one lock. The menu doesn't flip the run-time switches
// itself: it queues the change for the window's thread (menu_apply_window_requests), where the F keys apply theirs,
// so the switches only ever change on that thread.
//
// While the menu is open the game gets no buttons or sticks (hle/input.c menu_input_captured); keys and pad events
// still pass through so held-key state stays right, and F2..F8 keep working. Escape or F1 closes it. Mouse, keyboard
// (arrows, Space / Enter) and controller (D-pad, south button) all drive it.
//
// Sections: Visual (render resolution, frame rate counter, the F2..F7 switches), Gameplay (F8 game speed, the shiny and
// alpha rate sliders; more toggles go here). The frame rate counter (top left, on by default, g_settings.show_fps) is drawn even when the menu is closed.
//
// Render resolution: every 16:9 render target the game sizes for 1920x1080 (the 3D passes, their half / quarter-size
// buffers and the final image the HUD is drawn into) is allocated at a scale of that size (nvn/output_scale.c,
// vulkan/textures.c tex_scalable), so the HUD renders at the chosen resolution too. Changing it reallocates those
// targets on their next use (a short hitch, as Dynamic does); the choice is saved in ark.ini.
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "menu.h"

extern "C" {
#include "../settings.h"            // g_settings.res_mode (nvn/output_scale.c reads it every presented frame)
void renderer_get_scale(float *sx, float *sy);  // nvn/vulkan/textures.c: the render targets' scale now
int ark_switch_get(int which);       // runtime/patches.c (0 60 fps, 1 full detail, 2 grass, 3 text pace, 4 table2)
void ark_switch_toggle(int which);   // ... and 5: terrain range (cycles)
float ark_terrain_factor(void);
void ark_terrain_set(float f);
int ark_speed(void);
void ark_speed_set(int s);
uint32_t ark_shiny_last_rolls(void);  // runtime/patches.c: shiny and alpha rates
float ark_alpha_last_share(void);
void hotkeys_update_title(void);     // runtime/hotkeys.c
void vk_menu_device(VkInstance *inst, VkPhysicalDevice *phys, VkDevice *dev, uint32_t *qfam, VkQueue *queue,
                    VkPipelineCache *pcache, VkFormat *format, uint32_t *images);  // nvn/vulkan/device.c
}
#define LOG(...) fprintf(stderr, __VA_ARGS__)  // (as runtime/rt.h)

static std::mutex lock;                    // ImGui's context: events (window thread) vs. frames (render thread)
static std::atomic<bool> open_;
static std::atomic<float> density{1};      // window pixels per window point (Retina: 2)
static bool context_made, backend_ready;
static uint64_t last_ns;

// Requests for the window's thread (menu_apply_window_requests)
static std::atomic<int> req_toggle;        // bit i: ark_switch_toggle(i), i = 0..4
static std::atomic<int> req_terrain{-1};   // index into terrain_steps
static std::atomic<int> req_speed{-1};     // index into speed_steps

static const float terrain_steps[] = {1, 2, 4, 6};
static const char *terrain_names[] = {"x1 (40 m)", "x2 (80 m)", "x4 (160 m)", "x6 (240 m)"};
static const int speed_steps[] = {1, 2, 4, 0};
static const char *speed_names[] = {"x1", "x2", "x4", "Unlimited"};
static const int res_modes[] = {RES_MATCH, RES_Q75, RES_Q50, RES_Q25, 1, 2, 3, 4, RES_DYNAMIC};
static const char *res_names[] = {"Match window", "75% of window", "50% of window", "25% of window", "1x (1080p)",
                                  "2x (2160p)", "3x (3240p)", "4x (4320p)", "Dynamic (holds 60 fps)"};
static std::atomic<int> req_res{-1};       // index into res_modes
static std::atomic<int> req_fps{-1};       // frame rate counter: 0 off, 1 on
static std::atomic<int> req_shiny{-1};     // shiny roll multiplier, 0..500
static std::atomic<int> req_alpha{-1};     // alpha slot picks, 0..100
static std::atomic<bool> req_save_rates;   // a rate slider was released: save ark.ini

// Frame rate: presented frames counted over half-second windows (render thread)
static struct { uint64_t t0; int n; double max_ms, fps, ms, worst; uint64_t last; } fr;
static void frame_rate_tick(uint64_t now) {
    if (fr.last) {
        double dt = (now - fr.last) / 1e6;
        if (dt > fr.max_ms) fr.max_ms = dt;
    }
    fr.last = now;
    if (!fr.t0) fr.t0 = now;
    fr.n++;
    double span = (now - fr.t0) / 1e9;
    if (span >= 0.5) {
        fr.fps = fr.n / span;
        fr.ms = span * 1000 / fr.n;
        fr.worst = fr.max_ms;
        fr.t0 = now, fr.n = 0, fr.max_ms = 0;
    }
}

static bool shown_fps(void) { return req_fps.load() >= 0 ? req_fps.load() == 1 : g_settings.show_fps; }

static void draw_frame_rate(void) {
    float s = density.load();
    ImGui::SetNextWindowPos(ImVec2(8 * s, 8 * s), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * s, 4 * s));
    if (ImGui::Begin("##fps", nullptr, f)) {
        if (fr.fps > 0) ImGui::Text("%.0f fps  %.1f ms  (max %.0f ms)", fr.fps, fr.ms, fr.worst);
        else ImGui::Text("-- fps");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

static void ensure_context(void) {  // (under the lock)
    if (context_made) return;
    context_made = true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;  // (no imgui.ini in the game folder)
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "ark";
}

static void check_vk(VkResult r) {
    if (r != VK_SUCCESS) LOG("[menu] Vulkan call failed: %d\n", (int)r);
}

// A system font if there is one (sharper than ImGui's built-in bitmap font when scaled for Retina)
static void load_font(float scale) {
    ImGuiIO &io = ImGui::GetIO();
    static const char *paths[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    for (const char *p : paths) {
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fclose(f);
        if (io.Fonts->AddFontFromFileTTF(p, 16.0f * scale)) return;
    }
    ImFontConfig cfg;
    cfg.SizePixels = 13.0f * std::round(scale < 1 ? 1 : scale);
    io.Fonts->AddFontDefault(&cfg);
}

static bool init_backend(void) {  // (render thread, under the lock)
    ImGui_ImplVulkan_InitInfo ii = {};
    VkFormat format;
    uint32_t images = 0;
    vk_menu_device(&ii.Instance, &ii.PhysicalDevice, &ii.Device, &ii.QueueFamily, &ii.Queue, &ii.PipelineCache, &format,
                   &images);
    static VkFormat fmt;  // (the backend keeps the pointer)
    fmt = format;
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.MinImageCount = 2;
    ii.ImageCount = images < 2 ? 2 : images;
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.DescriptorPoolSize = 8;
    ii.UseDynamicRendering = true;  // (the window image is drawn with dynamic rendering: device.c vk_frame_begin)
    ii.PipelineRenderingCreateInfo = {};
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &fmt;
    ii.CheckVkResultFn = check_vk;
    float s = density.load();
    load_font(s);
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 6;
    st.FrameRounding = 4;
    st.WindowPadding = ImVec2(14, 12);
    st.ItemSpacing = ImVec2(10, 7);
    st.Colors[ImGuiCol_WindowBg].w = 0.92f;
    st.ScaleAllSizes(s);
    if (!ImGui_ImplVulkan_Init(&ii)) {
        LOG("[menu] ImGui's Vulkan backend failed to start\n");
        return false;
    }
    LOG("[menu] overlay ready (format %d, %u images, scale %g)\n", (int)format, ii.ImageCount, s);
    return true;
}

// ---- the menu

// A setting's key hint in the right-hand column
static void key_hint(const char *key) {
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", key);
    ImGui::TableNextColumn();
}

static void tip(const char *text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetItemTooltip("%s", text);
}

// A switch's state as shown: its value, flipped if a toggle is queued and not applied yet
static bool shown_switch(int i) { return (ark_switch_get(i) != 0) != ((req_toggle.load() >> i) & 1); }

static void switch_row(int i, const char *label, const char *key, const char *help) {
    bool v = shown_switch(i);
    if (ImGui::Checkbox(label, &v)) req_toggle.fetch_xor(1 << i);
    tip(help);
    key_hint(key);
}

static int terrain_index(void) {
    if (req_terrain.load() >= 0) return req_terrain.load();
    float f = ark_terrain_factor();
    for (int i = 0; i < 4; i++)
        if (std::fabs(terrain_steps[i] - f) < 0.01f) return i;
    return -1;  // (an ARK_TERRAIN_RANGE value between the steps)
}

static int speed_index(void) {
    if (req_speed.load() >= 0) return req_speed.load();
    int s = ark_speed();
    for (int i = 0; i < 4; i++)
        if (speed_steps[i] == s) return i;
    return -1;
}

static int res_index(void) {
    if (req_res.load() >= 0) return req_res.load();
    for (int i = 0; i < 9; i++)
        if (res_modes[i] == g_settings.res_mode) return i;
    return -1;  // (5x..8x from ark.ini)
}

// "1 in N" for a chance p (0 < p <= 1)
static void one_in(char *buf, size_t n, double p) {
    if (p <= 0) std::snprintf(buf, n, "never");
    else if (p >= 0.995) std::snprintf(buf, n, "every time");
    else std::snprintf(buf, n, "1 in %.0f", 1 / p);
}

// Chance of at least one hit in n tries of chance p each
static double at_least_one(double p, double n) { return n <= 0 ? 0 : 1 - std::pow(1 - p, n); }

static int shown_rate(const std::atomic<int> &req, int v) { return req.load() >= 0 ? req.load() : v; }

static void rate_slider(const char *label, std::atomic<int> &req, int v, int max, const char *help) {
    int m = shown_rate(req, v);
    if (ImGui::SliderInt(label, &m, 0, max, m == 0 ? "Off" : "x%d", ImGuiSliderFlags_AlwaysClamp)) req = m;
    if (ImGui::IsItemDeactivatedAfterEdit()) req_save_rates = true;
    tip(help);
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
}

static void rate_note(const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::Indent();
    ImGui::TextDisabled("%s", buf);
    ImGui::Unindent();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
}

static void draw_rates(void) {
    const double shiny_p = 1.0 / 4096;  // one roll
    int sm = shown_rate(req_shiny, g_settings.shiny_rolls), am = shown_rate(req_alpha, g_settings.alpha_rolls);
    char a[32], b[32];
    rate_slider("Shiny rate", req_shiny, g_settings.shiny_rolls, 500,
                "Multiplies the game's shiny rolls. Each roll is 1 in 4096; the game gives 1, more with the Shiny Charm, "
                "a complete research entry and outbreaks. Off: no random shinies. Applies to Pokémon generated after "
                "the change; ones already out keep theirs.");
    if (sm == 0) {
        rate_note("No random shinies");
    } else {
        one_in(a, sizeof a, at_least_one(shiny_p, sm));
        one_in(b, sizeof b, at_least_one(shiny_p, 7.0 * sm));
        rate_note("%s, %s with Shiny Charm + perfect entry", a, b);
        uint32_t last = ark_shiny_last_rolls();
        if (last) {
            one_in(a, sizeof a, at_least_one(shiny_p, (double)last * sm));
            rate_note("Last Pokémon: %u roll%s x %d, %s", last, last == 1 ? "" : "s", sm, a);
        }
    }
    rate_slider("Alpha rate", req_alpha, g_settings.alpha_rolls, 100,
                "Repeats a spawn's pick of what appears up to this many times until it lands on an alpha, so an area "
                "where 1 in N spawns is an alpha gives about this many times as many. Off: alpha slots are avoided. "
                "Alpha spawn points (always an alpha) are not affected. Applies to Pokémon that spawn after the change.");
    float share = ark_alpha_last_share();
    if (share < 0) {
        rate_note(am == 0 ? "Alpha slots avoided" : "Odds show after the next spawn");
    } else if (share <= 0) {
        rate_note("Last spawn table has no alpha slots");
    } else {
        one_in(a, sizeof a, share);
        if (am == 0) std::snprintf(b, sizeof b, "avoided");
        else one_in(b, sizeof b, at_least_one(share, am));
        rate_note("Last spawn table: %s, now %s", a, b);
    }
}

static bool begin_settings_table(const char *id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableNextColumn();
    return true;
}

static void draw_menu(void) {
    float s = density.load();
    ImGui::SetNextWindowPos(ImVec2(32 * s, 56 * s), ImGuiCond_FirstUseEver);  // (below the frame rate counter)
    bool keep = true;
    if (!ImGui::Begin("Ark", &keep, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!keep) open_ = false;
        return;
    }
    ImGui::PushItemWidth(170 * s);

    ImGui::SeparatorText("Visual");
    if (begin_settings_table("visual")) {
        int ri = res_index();
        if (ImGui::Combo("Render resolution", &ri, res_names, 9)) req_res = ri;
        tip("Resolution the game renders at, HUD included. \"Match window\" renders at the window's own pixel size; "
            "1x is the Switch's docked 1080p. Changing it causes a short hitch.");
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        {
            float sx = 1, sy = 1;
            renderer_get_scale(&sx, &sy);
            ImGui::Indent();
            ImGui::TextDisabled("Rendering at %d x %d", (int)std::lround(1920 * sx), (int)std::lround(1080 * sy));
            ImGui::Unindent();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
        }
        bool fps = shown_fps();
        if (ImGui::Checkbox("Frame rate counter", &fps)) req_fps = fps ? 1 : 0;
        tip("Frames presented per second, the average frame time and the slowest frame, top left.");
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        switch_row(0, "60 fps", "F2",
                   "Runs the game at 60 frames per second (normal game speed). Version 1.1.1 only.");
        bool fps60 = ark_switch_get(0) != 0;
        ImGui::BeginDisabled(!fps60);
        ImGui::Indent();
        switch_row(3, "Dialogue text at 30 fps pace", "F5",
                   "At 60 fps, prints dialogue text at the speed it has at 30 fps.");
        switch_row(4, "Second frame-time table halved", "F6",
                   "At 60 fps: the old behaviour, for comparison (animations, flames, water and text boxes run at "
                   "double speed).");
        ImGui::Unindent();
        ImGui::EndDisabled();
        switch_row(1, "Full-detail models", "F3", "Draws every model at its highest level of detail.");
        switch_row(2, "Grass and ground detail to the limit", "F4",
                   "Generates grass and small ground detail out to the game's generation limit.");
        int ti = terrain_index();
        if (ImGui::Combo("Terrain detail range", &ti, terrain_names, 4)) req_terrain = ti;
        tip("Distance within which the terrain is drawn at full detail, as a multiple of the game's 40 m.");
        key_hint("F7");
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Gameplay");
    if (begin_settings_table("gameplay")) {
        int si = speed_index();
        if (ImGui::Combo("Game speed", &si, speed_names, 4)) req_speed = si;
        tip("Runs the whole game faster, to get through long sections quickly. Audio may stutter above x1.");
        key_hint("F8");
        draw_rates();
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("F1 or Esc closes the menu. The game is paused for input while it is open.");
    ImGui::PopItemWidth();
    ImGui::End();
    if (!keep) open_ = false;
}

// ---- the hooks (runtime/ui/menu.h)

extern "C" bool menu_input_captured(void) { return open_.load(); }
extern "C" void menu_test_open(void) { open_ = true; }  // (LARECOMP_MENU_AT=<frame>: screenshot tests)

static ImGuiKey imgui_key(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
    case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

static ImGuiKey imgui_pad_button(int b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;   // activate
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;   // cancel
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// Window thread. true: the event was the menu's (the game doesn't get it).
extern "C" bool menu_handle_event(const SDL_Event *e) {
    bool key_down = e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat;
    if (key_down && e->key.scancode == SDL_SCANCODE_F1) {
        if (SDL_Window *w = SDL_GetWindowFromID(e->key.windowID)) density = SDL_GetWindowPixelDensity(w);
        open_ = !open_.load();
        return true;
    }
    if (!open_.load()) return false;
    if (key_down && e->key.scancode == SDL_SCANCODE_ESCAPE) {
        open_ = false;
        return true;
    }
    std::lock_guard<std::mutex> g(lock);
    ensure_context();
    ImGuiIO &io = ImGui::GetIO();
    float d = density.load();
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        ImGuiKey k = imgui_key(e->key.scancode);
        io.AddKeyEvent(ImGuiMod_Shift, (e->key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Ctrl, (e->key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (e->key.mod & SDL_KMOD_ALT) != 0);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, e->type == SDL_EVENT_KEY_DOWN);
        return false;  // (hle/input.c keeps track of held keys; the game is masked while the menu is open)
    }
    case SDL_EVENT_MOUSE_MOTION:
        io.AddMousePosEvent(e->motion.x * d, e->motion.y * d);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1
              : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        io.AddMousePosEvent(e->button.x * d, e->button.y * d);
        if (b >= 0) io.AddMouseButtonEvent(b, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        io.AddMouseWheelEvent(e->wheel.x, e->wheel.y);
        return true;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        ImGuiKey k = imgui_pad_button(e->gbutton.button);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        return false;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY || e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            float v = e->gaxis.value / 32767.0f;
            float neg = v < -0.3f ? -v : 0, pos = v > 0.3f ? v : 0;
            bool y = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY;
            io.AddKeyAnalogEvent(y ? ImGuiKey_GamepadLStickUp : ImGuiKey_GamepadLStickLeft, neg > 0, neg);
            io.AddKeyAnalogEvent(y ? ImGuiKey_GamepadLStickDown : ImGuiKey_GamepadLStickRight, pos > 0, pos);
        }
        return false;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        io.AddFocusEvent(false);
        return false;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        io.AddFocusEvent(true);
        return false;
    default:
        return false;
    }
}

// Window thread, after each batch of events: apply what the menu asked for.
extern "C" void menu_apply_window_requests(void *sdl_window) {
    SDL_Window *w = (SDL_Window *)sdl_window;
    if (w) density = SDL_GetWindowPixelDensity(w);
    bool changed = false;
    int t = req_toggle.exchange(0);
    for (int i = 0; i < 5; i++)
        if (t & (1 << i)) ark_switch_toggle(i), changed = true;
    int ti = req_terrain.exchange(-1);
    if (ti >= 0 && ti < 4) ark_terrain_set(terrain_steps[ti]), changed = true;
    int ri = req_res.exchange(-1);
    if (ri >= 0 && ri < 9 && g_settings.res_mode != res_modes[ri]) {
        g_settings.res_mode = res_modes[ri];
        settings_save();
        LOG("[menu] render resolution: %s\n", res_names[ri]);
    }
    int fi = req_fps.exchange(-1);
    if (fi >= 0 && g_settings.show_fps != (fi == 1)) {
        g_settings.show_fps = fi == 1;
        settings_save();
    }
    int si = req_speed.exchange(-1);
    if (si >= 0 && si < 4) ark_speed_set(speed_steps[si]), changed = true;
    int sh = req_shiny.load(), al = req_alpha.load();  // (cleared once saved, so the sliders show them until then)
    if (sh >= 0) __atomic_store_n(&g_settings.shiny_rolls, sh, __ATOMIC_RELAXED);
    if (al >= 0) __atomic_store_n(&g_settings.alpha_rolls, al, __ATOMIC_RELAXED);
    if (req_save_rates.exchange(false)) {
        req_shiny.compare_exchange_strong(sh, -1);
        req_alpha.compare_exchange_strong(al, -1);
        settings_save();
        LOG("[menu] shiny rate x%d, alpha rate x%d\n", g_settings.shiny_rolls, g_settings.alpha_rolls);
    }
    if (changed) hotkeys_update_title();
}

// Render thread, inside the window image's rendering (after the game's picture, before present).
extern "C" void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    uint64_t now_tick = SDL_GetTicksNS();
    frame_rate_tick(now_tick);  // (every presented frame, menu open or not)
    bool fps = shown_fps(), open = open_.load();
    if ((!open && !fps) || !cmd || !target_w || !target_h) return;
    std::lock_guard<std::mutex> g(lock);
    ensure_context();
    if (!backend_ready) {
        static bool failed;
        if (failed) return;
        if (!init_backend()) { failed = true; return; }
        backend_ready = true;
    }
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)target_w, (float)target_h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    uint64_t now = SDL_GetTicksNS();
    io.DeltaTime = last_ns && now > last_ns ? (float)((now - last_ns) / 1e9) : 1.0f / 60;
    if (io.DeltaTime > 0.25f) io.DeltaTime = 0.25f;  // (first frame after reopening)
    last_ns = now;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (fps) draw_frame_rate();
    if (open) draw_menu();
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);
}
