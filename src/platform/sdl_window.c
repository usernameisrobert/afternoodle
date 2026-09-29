/* SDL2 backend: window, renderer handle, input snapshot, clipboard, threads.
 *
 * Touch is unified into the mouse: the first active finger drives
 * af_mouse_pos()/af_mouse_down(LEFT) so a game with no touch-specific code
 * still works on a phone. That is what makes "plug a keyboard and mouse into
 * your phone" and "pure touch" the same binary.
 */
#include <SDL.h>
#if defined(AF_OS_ANDROID)
#  include <SDL_haptic.h>
#endif
#include <SDL_image.h>
#include <SDL_syswm.h>

#include "afndle/core/log.h"
#include "afndle/core/mem.h"
#include "afndle/core/str.h"
#include "afndle/core/time.h"
#include "afndle/platform/platform.h"

/* The window owns the engine renderer, which is a thin wrapper around the SDL
 * one. Private header: not part of the published API. */
#include "../render/render_internal.h"

#if defined(AF_OS_WINDOWS)
#  include <io.h>
#  include <windows.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#endif

#if defined(AF_OS_ANDROID)
#  include <SDL_system.h>
#endif

/* ============================================================== input state */

/* AfInputState is declared in platform.h: callers allocate snapshots. */

/* ============================================================== key table */

static const char* const k_key_names[AF_KEY_COUNT] = {
    "?",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Escape", "Enter", "Tab", "Backspace", "Space", "Minus", "Equals",
    "LeftBracket", "RightBracket", "Backslash", "Semicolon", "Apostrophe",
    "Grave", "Comma", "Period", "Slash", "CapsLock",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "PrintScreen", "ScrollLock", "Pause", "Insert", "Home", "PageUp", "Delete",
    "End", "PageDown", "Right", "Left", "Down", "Up",
    "NumLock", "KpDivide", "KpMultiply", "KpMinus", "KpPlus", "KpEnter",
    "Kp1", "Kp2", "Kp3", "Kp4", "Kp5", "Kp6", "Kp7", "Kp8", "Kp9", "Kp0",
    "KpPeriod", "Application", "Power", "LCtrl", "LShift", "LAlt", "LGui",
    "RCtrl", "RShift", "RAlt", "RGui"};

const char *af_key_name(AfKey key) {
    if ((int)key <= 0 || (int)key >= AF_KEY_COUNT) return "?";
    return k_key_names[key];
}

AfKey af_key_from_name(const char *name) {
    if (!name) return AF_KEY_UNKNOWN;
    for (int i = 1; i < AF_KEY_COUNT; i++)
        if (af_str_casecmp(af_str(name), k_key_names[i]) == 0) return (AfKey)i;
    /* Aliases people actually type. */
    if (af_str_casecmp(af_str(name), "Return") == 0) return AF_KEY_ENTER;
    if (af_str_casecmp(af_str(name), "Esc") == 0) return AF_KEY_ESCAPE;
    if (af_str_casecmp(af_str(name), "Del") == 0) return AF_KEY_DELETE;
    if (af_str_casecmp(af_str(name), "Control") == 0) return AF_KEY_LCTRL;
    if (af_str_casecmp(af_str(name), "Shift") == 0) return AF_KEY_LSHIFT;
    if (af_str_casecmp(af_str(name), "Alt") == 0) return AF_KEY_LALT;
    if (af_str_casecmp(af_str(name), "Spacebar") == 0) return AF_KEY_SPACE;
    return AF_KEY_UNKNOWN;
}

/* AfKey enum was laid out to match SDL_Scancode order exactly, so the mapping
 * is a bounds check away. Verify once at startup instead of a 250-entry table. */
AF_INLINE AfKey scancode_to_key(int sc) {
    if (sc <= 0 || sc >= AF_KEY_COUNT) return AF_KEY_UNKNOWN;
    return (AfKey)sc;
}

AfKey af_key_from_scancode(int scancode) { return scancode_to_key(scancode); }

const char *af_mouse_button_name(AfMouseButton b) {
    switch (b) {
        case AF_MOUSE_LEFT: return "Left";
        case AF_MOUSE_MIDDLE: return "Middle";
        case AF_MOUSE_RIGHT: return "Right";
        case AF_MOUSE_X1: return "X1";
        case AF_MOUSE_X2: return "X2";
        default: return "?";
    }
}

/* ================================================================= window */

struct AfWindow {
    SDL_Window*   sdl;
    SDL_Renderer* sdl_renderer;
    AfRenderer*   renderer;      /* engine wrapper, created at window_create */
    uint32_t      sdl_id;
    char          title[256];
    int           width, height;
    float         scale;
    int           drawable_w, drawable_h;
    int           fullscreen;
    int           closing;
    AfInputState  input;
    struct AfWindow* next;   /* registry chain */
};

/* Windows created this process, keyed by SDL window id. Small list: a game
 * has one window, the editor has two. */
#define AF_MAX_WINDOWS 8
static AfWindow* g_windows[AF_MAX_WINDOWS];
static int g_window_count;

AfWindow* af_window_lookup(uint32_t sdl_window_id) {
    for (int i = 0; i < g_window_count; i++)
        if (g_windows[i]->sdl_id == sdl_window_id) return g_windows[i];
    return NULL;
}

static void window_register(AfWindow* w) {
    if (g_window_count < AF_MAX_WINDOWS) g_windows[g_window_count++] = w;
}

static void window_unregister(AfWindow* w) {
    for (int i = 0; i < g_window_count; i++) {
        if (g_windows[i] != w) continue;
        g_windows[i] = g_windows[--g_window_count];
        return;
    }
}


/* --------------------------------------------------------------- platform */

static struct {
    int   initialised;
    int   video_up;
    int   audio_up;
    int   game_event_filter;
    char  app_name[64];
    char  last_clipboard[2048];
    char  data_dir[1024];
} g_plat;

void af_platform_set_data_dir(const char *dir) {
    g_plat.data_dir[0] = '\0';
    if (dir && *dir) {
        af_str_cpy_max(af_str(dir), g_plat.data_dir, (int)sizeof(g_plat.data_dir));
        /* Create it up front so later saves never race on missing parents. */
        af_fs_mkdirs(g_plat.data_dir);
    }
}

const char *af_platform_data_dir(void) { return g_plat.data_dir; }

int af_platform_stdout_is_tty(void) {
#if defined(AF_OS_WINDOWS)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

int af_platform_is_mobile(void) {
#if defined(AF_OS_ANDROID) || defined(AF_OS_IOS)
    return 1;
#else
    return 0;
#endif
}

const char *af_platform_name(void) {
#if defined(AF_OS_WINDOWS)
    return "Windows";
#elif defined(AF_OS_ANDROID)
    return "Android";
#elif defined(AF_OS_LINUX)
    return "Linux";
#elif defined(AF_OS_MACOS)
    return "macOS";
#else
    return "Unknown";
#endif
}

const char *af_platform_cpu_name(void) {
#if defined(__aarch64__)
    return "arm64";
#elif defined(__arm__)
    return "arm32";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

const char *af_platform_clipboard_mime(void) { return "text/plain"; }

void af_platform_init(const char *app_name) {
    if (g_plat.initialised) return;
    af_str_cpy_max(af_str(app_name ? app_name : "afternoodle"), g_plat.app_name,
                   (int)sizeof(g_plat.app_name));
    if (SDL_Init(0) != 0) AF_ERROR("SDL_Init failed: %s", SDL_GetError());
    g_plat.initialised = 1;
}

void af_platform_shutdown(void) {
    if (!g_plat.initialised) return;
    if (g_plat.audio_up) af_platform_audio_shutdown();
    if (g_plat.video_up) af_platform_video_shutdown();
    SDL_Quit();
    g_plat.initialised = 0;
}

int af_platform_video_init(void) {
    if (g_plat.video_up) return 1;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        AF_ERROR("SDL video init failed: %s", SDL_GetError());
        return 0;
    }
    /* The engine draws its own text, so hide the OS cursor only when asked. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
    g_plat.video_up = 1;
    return 1;
}

void af_platform_video_shutdown(void) {
    if (!g_plat.video_up) return;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    g_plat.video_up = 0;
}

int af_platform_audio_init(void) {
    if (g_plat.audio_up) return 1;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        AF_WARN("audio unavailable: %s", SDL_GetError());
        return 0;
    }
    g_plat.audio_up = 1;
    return 1;
}

void af_platform_audio_shutdown(void) {
    if (!g_plat.audio_up) return;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g_plat.audio_up = 0;
}

int af_platform_has_audio(void) { return g_plat.audio_up; }

int af_platform_open_url(const char *url) {
    if (!url) return 0;
    int r = SDL_OpenURL(url);
    if (r != 0) AF_WARN("could not open '%s': %s", url, SDL_GetError());
    return r == 0;
}

void af_platform_vibrate(int milliseconds) {
    /* No cross-platform haptics without another dependency; the SDL build
     * some targets ship does not expose SDL_Vibrate. Hook a platform backend
     * in here if you need it. */
    AF_UNUSED(milliseconds);
}

void af_platform_show_soft_keyboard(int show) {
#if defined(AF_OS_ANDROID)
    /* Opening an invisible text field is the only reliable way to raise the
     * IME on Android. Done lazily so it never costs anything on desktop. */
    static SDL_Texture *dummy = NULL;
    AF_UNUSED(dummy);
    SDL_StartTextInput();
    if (!show) SDL_StopTextInput();
#else
    AF_UNUSED(show);
#endif
}

/* ---------------------------------------------------------------- window */

AfWindowDesc af_window_desc_default(void) {
    AfWindowDesc d;
    memset(&d, 0, sizeof(d));
    d.width = 1280;
    d.height = 720;
    d.resizable = 1;
    d.vsync = 1;
    d.high_dpi = 1;
    return d;
}

/* Keeps the engine renderer's screen size in step with the drawable. Called on
 * every resize, so a HiDPI change or a rotation lands in the same frame. */
static void window_sync_renderer(AfWindow *w) {
    if (!w || !w->sdl_renderer || !w->renderer) return;
    int dw = 0, dh = 0;
    if (SDL_GetRendererOutputSize(w->sdl_renderer, &dw, &dh) != 0) return;
    if (dw <= 0 || dh <= 0) return;
    if (dw == w->drawable_w && dh == w->drawable_h) return;

    int ww = 0, wh = 0;
    af_window_get_size(w, &ww, &wh);
    float scale = 1.0f;
    if (ww > 0 && wh > 0) scale = af_minf((float)dw / (float)ww, (float)dh / (float)wh);

    w->drawable_w = dw;
    w->drawable_h = dh;
    w->scale = scale;
    w->width = ww;
    w->height = wh;
    af_r2d_set_size(w->renderer, dw, dh, scale);
}

AfWindow *af_window_create(const AfWindowDesc *desc) {
    AfWindowDesc d;
    if (desc) d = *desc;
    else d = af_window_desc_default();
    if (d.width <= 0) d.width = 1280;
    if (d.height <= 0) d.height = 720;
    if (!d.title) d.title = g_plat.app_name;

    if (!af_platform_video_init()) return NULL;

    Uint32 flags = SDL_WINDOW_SHOWN;
    if (d.high_dpi) flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    if (d.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (d.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (d.hidden) flags &= ~SDL_WINDOW_SHOWN;
#if defined(AF_OS_ANDROID) || defined(AF_OS_IOS)
    flags |= SDL_WINDOW_FULLSCREEN;
#endif

    SDL_Window *win = SDL_CreateWindow(d.title, SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, d.width, d.height,
                                       flags);
    if (!win) {
        AF_ERROR("could not create window: %s", SDL_GetError());
        return NULL;
    }
    /* On a phone the framebuffer is fixed: no title bar, no resize. */
#if defined(AF_OS_ANDROID) || defined(AF_OS_IOS)
    SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_SetWindowBordered(win, SDL_FALSE);
#endif

    Uint32 rflags = SDL_RENDERER_ACCELERATED;
    if (d.vsync) rflags |= SDL_RENDERER_PRESENTVSYNC;
    SDL_Renderer *rend = SDL_CreateRenderer(win, -1, rflags);
    if (!rend) {
        AF_WARN("accelerated renderer unavailable (%s); trying software",
                SDL_GetError());
        rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!rend) {
        AF_ERROR("no renderer available: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        return NULL;
    }

    AfWindow *w = (AfWindow *)af_calloc(1, sizeof(AfWindow));
    w->sdl = win;
    w->sdl_renderer = rend;
    w->sdl_id = (uint32_t)SDL_GetWindowID(win);
    w->fullscreen = d.fullscreen ? 1 : 0;
    af_str_cpy_max(af_str(d.title), w->title, (int)sizeof(w->title));
    af_window_get_size(w, &w->width, &w->height);

    int dw = w->width > 0 ? w->width : d.width;
    int dh = w->height > 0 ? w->height : d.height;
    SDL_GetRendererOutputSize(rend, &dw, &dh);
    float scale = 1.0f;
    if (w->width > 0 && w->height > 0)
        scale = af_minf((float)dw / (float)w->width, (float)dh / (float)w->height);
    if (d.scale > 0.0f) scale = d.scale;
    w->drawable_w = dw;
    w->drawable_h = dh;
    w->scale = scale;
    w->renderer = af_r2d_create(rend, dw, dh, scale);

    window_register(w);
    SDL_StartTextInput();
    return w;
}

void af_window_destroy(AfWindow *win) {
    if (!win) return;
    SDL_StopTextInput();
    window_unregister(win);
    if (win->renderer) af_r2d_destroy(win->renderer);
    if (win->sdl_renderer) SDL_DestroyRenderer(win->sdl_renderer);
    if (win->sdl) SDL_DestroyWindow(win->sdl);
    af_free(win);
}

AfRenderer *af_window_renderer(AfWindow *win) {
    if (!win) return NULL;
    /* Defensive: covers resizes the OS never sent us an event for. */
    window_sync_renderer(win);
    return win->renderer;
}

AfWindow *af_window_from_renderer(AfRenderer *r) {
    if (!r) return NULL;
    SDL_Window *w = SDL_RenderGetWindow(af_r2d_sdl(r));
    if (!w) return NULL;
    return af_window_lookup((uint32_t)SDL_GetWindowID(w));
}

void af_window_set_title(AfWindow *win, const char *title) {
    if (!win || !title) return;
    af_str_cpy_max(af_str(title), win->title, (int)sizeof(win->title));
    SDL_SetWindowTitle(win->sdl, title);
}

void af_window_get_size(AfWindow *win, int *w, int *h) {
    if (!win) return;
    int ww = 0, hh = 0;
    SDL_GetWindowSize(win->sdl, &ww, &hh);
    if (w) *w = ww;
    if (h) *h = hh;
}

void af_window_set_size(AfWindow *win, int w, int h) {
    if (!win || w <= 0 || h <= 0) return;
    SDL_SetWindowSize(win->sdl, w, h);
    window_sync_renderer(win);
}

int af_window_get_drawable_size(AfWindow *win, int *w, int *h) {
    if (!win) return 0;
    int ww = 0, hh = 0;
    SDL_GetRendererOutputSize(win->sdl_renderer, &ww, &hh);
    if (w) *w = ww;
    if (h) *h = hh;
    return 1;
}

float af_window_get_render_scale(AfWindow *win) {
    if (!win) return 1.0f;
    window_sync_renderer(win);
    return win->scale;
}

void af_window_set_fullscreen(AfWindow *win, int fullscreen) {
    if (!win) return;
    win->fullscreen = fullscreen ? 1 : 0;
    SDL_SetWindowFullscreen(win->sdl,
                            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

int af_window_is_fullscreen(AfWindow *win) {
    return win ? (SDL_GetWindowFlags(win->sdl) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0
               : 0;
}

void af_window_minimize(AfWindow *win) { if (win) SDL_MinimizeWindow(win->sdl); }
void af_window_restore(AfWindow *win)  { if (win) SDL_RestoreWindow(win->sdl); }
void af_window_show(AfWindow *win)     { if (win) SDL_ShowWindow(win->sdl); }
void af_window_hide(AfWindow *win)     { if (win) SDL_HideWindow(win->sdl); }
void af_window_focus(AfWindow *win)    { if (win) SDL_RaiseWindow(win->sdl); }
void af_window_request_close(AfWindow *win) { if (win) win->closing = 1; }
int af_window_should_close(AfWindow *win) { return win ? win->closing : 1; }
void af_window_clear_close(AfWindow *win) { if (win) win->closing = 0; }

double af_window_get_monitor_refresh_hz(AfWindow *win) {
    if (!win) return 60.0;
    int did = SDL_GetWindowDisplayIndex(win->sdl);
    if (did < 0) return 60.0;
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(did, &mode) != 0) return 60.0;
    if (mode.refresh_rate <= 0) return 60.0;
    return (double)mode.refresh_rate;
}

void af_window_set_icon_from_file(AfWindow *win, const char *path) {
    if (!win || !path) return;
    SDL_Surface *s = IMG_Load(path);
    if (!s) return;
    SDL_SetWindowIcon(win->sdl, s);
    SDL_FreeSurface(s);
}

/* ================================================================ input */

static int mods_to_af(Uint16 m) {
    int out = AF_MOD_NONE;
    if (m & KMOD_LSHIFT) out |= AF_MOD_LSHIFT;
    if (m & KMOD_RSHIFT) out |= AF_MOD_RSHIFT;
    if (m & KMOD_LCTRL)  out |= AF_MOD_LCTRL;
    if (m & KMOD_RCTRL)  out |= AF_MOD_RCTRL;
    if (m & KMOD_LALT)   out |= AF_MOD_LALT;
    if (m & KMOD_RALT)   out |= AF_MOD_RALT;
    if (m & KMOD_LGUI)   out |= AF_MOD_LGUI;
    if (m & KMOD_RGUI)   out |= AF_MOD_RGUI;
    return out;
}

static void push_text(AfInputState *in, const char *utf8) {
    if (!utf8 || !*utf8) return;
    size_t n = strlen(utf8);
    size_t have = strlen(in->text);
    if (have + n + 1 >= sizeof(in->text)) return;
    memcpy(in->text + have, utf8, n + 1);
}

static void update_mouse_pos(AfInputState *in, float x, float y, int window_w,
                             int window_h) {
    in->mouse_dx += x - in->mouse_x;
    in->mouse_dy += y - in->mouse_y;
    in->mouse_x = x;
    in->mouse_y = y;
    in->mouse_inside = (x >= 0 && y >= 0 && x < (float)window_w &&
                        y < (float)window_h)
                           ? 1
                           : 0;
}

/* Translates one SDL event into engine state plus (optionally) an AfEvent.
 * `out` points at the free tail of the caller's array; `room` is how much of it
 * is left. Returns the number of AfEvents written. */
static int handle_event(AfWindow *win, SDL_Event *e, AfEvent *out, int room) {
    AfInputState *in = &win->input;
    int wrote = 0;
    int win_w = 0, win_h = 0;
    af_window_get_size(win, &win_w, &win_h);

/* Emits one AfEvent of the given type; `wrote` is bumped only if there was
 * room, so a short buffer degrades to "dropped events" and never overruns. */
#define PUSH(ev)                                                \
    do {                                                        \
        if (wrote < room) {                                     \
            memset(&out[wrote], 0, sizeof(AfEvent));           \
            out[wrote].type = (ev);                             \
            wrote++;                                            \
        }                                                       \
    } while (0)

    switch (e->type) {
        case SDL_QUIT:
            PUSH(AF_EVENT_QUIT);
            break;
        case SDL_WINDOWEVENT: {
            AfEventType t = AF_EVENT_NONE;
            switch (e->window.event) {
                case SDL_WINDOWEVENT_CLOSE:   t = AF_EVENT_WINDOW_CLOSE; break;
                case SDL_WINDOWEVENT_RESIZED:
                case SDL_WINDOWEVENT_SIZE_CHANGED: t = AF_EVENT_WINDOW_RESIZED; break;
                case SDL_WINDOWEVENT_MOVED:   t = AF_EVENT_WINDOW_MOVED; break;
                case SDL_WINDOWEVENT_FOCUS_GAINED: t = AF_EVENT_WINDOW_FOCUS_GAINED; break;
                case SDL_WINDOWEVENT_FOCUS_LOST:   t = AF_EVENT_WINDOW_FOCUS_LOST; break;
                case SDL_WINDOWEVENT_MINIMIZED:   t = AF_EVENT_WINDOW_MINIMIZED; break;
                case SDL_WINDOWEVENT_RESTORED:    t = AF_EVENT_WINDOW_RESTORED; break;
                default: break;
            }
            if (t != AF_EVENT_NONE) {
                /* A resize changes the drawable size, so the renderer's
                 * screen size has to follow before anything draws. */
                if (t == AF_EVENT_WINDOW_RESIZED) window_sync_renderer(win);
                PUSH(t);
                if (wrote > 0) {
                    out[wrote - 1].x = (float)e->window.data1;
                    out[wrote - 1].y = (float)e->window.data2;
                    out[wrote - 1].window_id = (uint32_t)SDL_GetWindowID(win->sdl);
                }
            }
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            int down = e->type == SDL_KEYDOWN;
            AfKey k = scancode_to_key(e->key.keysym.scancode);
            in->mods = mods_to_af(e->key.keysym.mod);
            if (k != AF_KEY_UNKNOWN) {
                if (down) {
                    /* Ignore auto-repeat for the "pressed" edge. */
                    if (!in->key_down[k]) {
                        in->key_pressed[k] = 1;
                        in->key_frames[k] = 0;
                    }
                    in->key_down[k] = 1;
                } else {
                    if (in->key_down[k]) in->key_released[k] = 1;
                    in->key_down[k] = 0;
                }
            }
            if (out && wrote < room) {
                memset(&out[wrote], 0, sizeof(AfEvent));
                out[wrote].type = down ? AF_EVENT_KEY_DOWN : AF_EVENT_KEY_UP;
                out[wrote].key = k;
                out[wrote].scancode = e->key.keysym.scancode;
                out[wrote].mods = in->mods;
                out[wrote].repeat = e->key.repeat;
                out[wrote].window_id = (uint32_t)SDL_GetWindowID(win->sdl);
                wrote++;
            }
            break;
        }
        case SDL_TEXTINPUT:
            push_text(in, e->text.text);
            if (out && wrote < room) {
                memset(&out[wrote], 0, sizeof(AfEvent));
                out[wrote].type = AF_EVENT_TEXT_INPUT;
                af_str_cpy_max(af_str(e->text.text), out[wrote].text,
                               (int)sizeof(out[wrote].text));
                wrote++;
            }
            break;
        case SDL_MOUSEMOTION: {
            float x = e->motion.x, y = e->motion.y;
            if (e->motion.state & SDL_BUTTON_X1) in->mouse_down[AF_MOUSE_X1] = 1;
            update_mouse_pos(in, x, y, win_w, win_h);
            in->has_touch = 0;
            if (out && wrote < room) {
                memset(&out[wrote], 0, sizeof(AfEvent));
                out[wrote].type = AF_EVENT_MOUSE_MOVE;
                out[wrote].x = x;
                out[wrote].y = y;
                out[wrote].dx = e->motion.xrel;
                out[wrote].dy = e->motion.yrel;
                wrote++;
            }
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int down = e->type == SDL_MOUSEBUTTONDOWN;
            int b = -1;
            switch (e->button.button) {
                case SDL_BUTTON_LEFT:   b = AF_MOUSE_LEFT; break;
                case SDL_BUTTON_MIDDLE: b = AF_MOUSE_MIDDLE; break;
                case SDL_BUTTON_RIGHT:  b = AF_MOUSE_RIGHT; break;
                case SDL_BUTTON_X1:     b = AF_MOUSE_X1; break;
                case SDL_BUTTON_X2:     b = AF_MOUSE_X2; break;
                default: break;
            }
            if (b >= 0) {
                if (down) {
                    if (!in->mouse_down[b]) in->mouse_pressed[b] = 1;
                    in->mouse_down[b] = 1;
                } else {
                    if (in->mouse_down[b]) in->mouse_released[b] = 1;
                    in->mouse_down[b] = 0;
                }
            }
            if (out && wrote < room) {
                memset(&out[wrote], 0, sizeof(AfEvent));
                out[wrote].type = down ? AF_EVENT_MOUSE_DOWN : AF_EVENT_MOUSE_UP;
                out[wrote].button = b;
                out[wrote].x = e->button.x;
                out[wrote].y = e->button.y;
                out[wrote].dx = e->button.x;
                out[wrote].dy = e->button.y;
                wrote++;
            }
            break;
        }
        case SDL_MOUSEWHEEL: {
            float dy = (float)e->wheel.y;
            float dx = (float)e->wheel.x;
            in->scroll_x += dx;
            in->scroll_y += dy;
            if (out && wrote < room) {
                memset(&out[wrote], 0, sizeof(AfEvent));
                out[wrote].type = AF_EVENT_MOUSE_WHEEL;
                out[wrote].wheel_x = dx;
                out[wrote].wheel_y = dy;
                out[wrote].x = in->mouse_x;
                out[wrote].y = in->mouse_y;
                wrote++;
            }
            break;
        }
        case SDL_CONTROLLERDEVICEADDED: {
            if (e->cdevice.which < 8) in->gamepad_count = af_gamepad_count();
            PUSH(AF_EVENT_GAMEPAD_ADDED);
            if (wrote) out[wrote - 1].gamepad_id = e->cdevice.which;
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED:
            PUSH(AF_EVENT_GAMEPAD_REMOVED);
            if (wrote) out[wrote - 1].gamepad_id = e->cdevice.which;
            in->gamepad_count = af_gamepad_count();
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            int down = e->type == SDL_CONTROLLERBUTTONDOWN;
            int pad = e->cbutton.which, b = e->cbutton.button;
            if (pad >= 0 && pad < 8 && b >= 0 && b < AF_GP_BUTTON_COUNT) {
                if (down && !in->gp_down[pad][b]) in->gp_pressed[pad][b] = 1;
                in->gp_down[pad][b] = (uint8_t)(down ? 1 : 0);
            }
            if (out && wrote < room) {
                memset(&out[wrote], 0, sizeof(AfEvent));
                out[wrote].type =
                    down ? AF_EVENT_GAMEPAD_BUTTON_DOWN : AF_EVENT_GAMEPAD_BUTTON_UP;
                out[wrote].gamepad_id = pad;
                out[wrote].gp_button = b;
                wrote++;
            }
            break;
        }
        case SDL_CONTROLLERAXISMOTION: {
            int pad = e->caxis.which, ax = e->caxis.axis;
            float v = (float)e->caxis.value / 32767.0f;
            if (af_absf(v) < 0.08f) v = 0.0f;
            if (pad >= 0 && pad < 8 && ax >= 0 && ax < 6) {
                in->gp_axis_prev[pad][ax] = in->gp_axis[pad][ax];
                in->gp_axis[pad][ax] = v;
            }
            if (out && wrote < room) {
                memset(&out[wrote], 0, sizeof(AfEvent));
                out[wrote].type = AF_EVENT_GAMEPAD_AXIS;
                out[wrote].gamepad_id = pad;
                out[wrote].gp_axis = ax;
                out[wrote].gp_value = v;
                wrote++;
            }
            break;
        }
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION: {
            int down = e->type == SDL_FINGERDOWN;
            float x = e->tfinger.x * (float)win_w;
            float y = e->tfinger.y * (float)win_h;
            in->has_touch = 1;
            in->touch_count = down ? 1 : (in->touch_count ? 0 : 0);
            if (down) {
                in->touch_used_as_mouse = 1;
                if (!in->mouse_down[AF_MOUSE_LEFT]) in->mouse_pressed[AF_MOUSE_LEFT] = 1;
                in->mouse_down[AF_MOUSE_LEFT] = 1;
            } else {
                if (in->mouse_down[AF_MOUSE_LEFT]) in->mouse_released[AF_MOUSE_LEFT] = 1;
                in->mouse_down[AF_MOUSE_LEFT] = 0;
            }
            update_mouse_pos(in, x, y, win_w, win_h);
            if (out && wrote < room) {
                memset(&out[wrote], 0, sizeof(AfEvent));
                out[wrote].type = down ? AF_EVENT_TOUCH_DOWN
                                       : (e->type == SDL_FINGERMOTION ? AF_EVENT_TOUCH_MOVE
                                                                      : AF_EVENT_TOUCH_UP);
                out[wrote].x = x;
                out[wrote].y = y;
                out[wrote].finger_id = e->tfinger.fingerId;
                out[wrote].pressure = e->tfinger.pressure;
                out[wrote].dx = e->tfinger.dx * (float)win_w;
                out[wrote].dy = e->tfinger.dy * (float)win_h;
                wrote++;
            }
            break;
        }
        default:
            break;
    }
#undef PUSH
    return wrote;
}

void af_pump_events(AfWindow *win, AfEvent *events, int max_events,
                    int *out_count, AfInputState *input) {
    if (out_count) *out_count = 0;
    if (!win) return;

    /* Clear per-frame edges before we look for new ones. */
    memset(win->input.key_pressed, 0, sizeof(win->input.key_pressed));
    memset(win->input.key_released, 0, sizeof(win->input.key_released));
    memset(win->input.mouse_pressed, 0, sizeof(win->input.mouse_pressed));
    memset(win->input.mouse_released, 0, sizeof(win->input.mouse_released));
    memset(win->input.gp_pressed, 0, sizeof(win->input.gp_pressed));
    win->input.mouse_dx = 0.0f;
    win->input.mouse_dy = 0.0f;
    win->input.scroll_x = 0.0f;
    win->input.scroll_y = 0.0f;
    win->input.text[0] = '\0';

    /* Wipe the caller's event buffer. */
    if (events && max_events > 0) memset(events, 0, sizeof(AfEvent) * (size_t)max_events);

    /* Drain the OS queue. Window events are routed to the window that owns
     * them so a second window (the editor's game view) behaves correctly. */
    int total = 0;
    int room = (events && max_events > 0) ? max_events : 0;
    /* When the caller only wants input state we still need somewhere to put
     * the translated events, so give it a one-slot scratch buffer. */
    AfEvent scratch;
    AfEvent *dst = events ? events : &scratch;
    if (room <= 0) room = 1;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        /* Window events carry no window handle in SDL2, so attribute them by
         * the id the OS reported. Quits go to the window being pumped. */
        AfWindow *target = win;
        if (e.type == SDL_WINDOWEVENT) {
            AfWindow *w = af_window_lookup((uint32_t)e.window.windowID);
            if (w) target = w;
        }
        if (!target) continue;

        if (e.type == SDL_WINDOWEVENT &&
            e.window.event == SDL_WINDOWEVENT_CLOSE) {
            target->closing = 1;
        }
        int w0 = total;
        int wrote = handle_event(target, &e, dst + w0, room - w0);
        total = w0 + wrote;
    }
    if (!events) total = 0;

    if (out_count) *out_count = total;
    if (input) *input = win->input;

    /* Hold-time counters for tap-vs-hold decisions. */
    for (int k = 0; k < AF_KEY_COUNT; k++)
        if (win->input.key_down[k] && win->input.key_frames[k] < 65535)
            win->input.key_frames[k]++;
}

void af_pump_events_global(AfEvent *events, int max_events, int *out_count,
                           AfInputState *input) {
    /* Drain events without a specific window (used by the CLI/headless tools). */
    if (out_count) *out_count = 0;
    SDL_Event e;
    int total = 0;
    while (SDL_PollEvent(&e)) {
        if (total >= max_events) break;
        events[total].type = AF_EVENT_NONE;
        if (e.type == SDL_QUIT) events[total++].type = AF_EVENT_QUIT;
    }
    if (out_count) *out_count = total;
    AF_UNUSED(input);
}

const AfInputState *af_input(AfWindow *win) { return win ? &win->input : NULL; }

int af_key_down(AfInputState *in, AfKey k) {
    return (in && k > 0 && k < AF_KEY_COUNT) ? in->key_down[k] : 0;
}
int af_key_pressed(AfInputState *in, AfKey k) {
    return (in && k > 0 && k < AF_KEY_COUNT) ? in->key_pressed[k] : 0;
}
int af_key_released(AfInputState *in, AfKey k) {
    return (in && k > 0 && k < AF_KEY_COUNT) ? in->key_released[k] : 0;
}
int af_key_held_frames(AfInputState *in, AfKey k) {
    return (in && k > 0 && k < AF_KEY_COUNT) ? in->key_frames[k] : 0;
}
int af_mouse_down(AfInputState *in, AfMouseButton b) {
    return (in && b >= 0 && b < AF_MOUSE_BUTTON_COUNT) ? in->mouse_down[b] : 0;
}
int af_mouse_pressed(AfInputState *in, AfMouseButton b) {
    return (in && b >= 0 && b < AF_MOUSE_BUTTON_COUNT) ? in->mouse_pressed[b] : 0;
}
int af_mouse_released(AfInputState *in, AfMouseButton b) {
    return (in && b >= 0 && b < AF_MOUSE_BUTTON_COUNT) ? in->mouse_released[b] : 0;
}
AfVec2 af_mouse_pos(AfInputState *in) { return in ? af_v2(in->mouse_x, in->mouse_y) : af_v2s(0); }
AfVec2 af_mouse_delta(AfInputState *in) { return in ? af_v2(in->mouse_dx, in->mouse_dy) : af_v2s(0); }
AfVec2 af_mouse_scroll(AfInputState *in) { return in ? af_v2(in->scroll_x, in->scroll_y) : af_v2s(0); }
int af_mods(AfInputState *in) { return in ? in->mods : 0; }
int af_key_mod(AfInputState *in, AfMod m) { return (af_mods(in) & m) != 0; }
const char *af_text_input(AfInputState *in) { return in ? in->text : ""; }
int af_input_has_touch(AfInputState *in) { return in ? in->has_touch : 0; }
int af_touch_count(AfInputState *in) { return in ? in->touch_count : 0; }

void af_input_end_frame(AfInputState *in) {
    if (!in) return;
    memset(in->key_pressed, 0, sizeof(in->key_pressed));
    memset(in->key_released, 0, sizeof(in->key_released));
    memset(in->mouse_pressed, 0, sizeof(in->mouse_pressed));
    memset(in->mouse_released, 0, sizeof(in->mouse_released));
    in->mouse_dx = in->mouse_dy = 0.0f;
    in->scroll_x = in->scroll_y = 0.0f;
    in->text[0] = '\0';
}

/* ============================================================== gamepads */

int af_gamepad_count(void) {
    int n = 0, i;
    for (i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) n++;
    return n;
}

const char *af_gamepad_name(int index) {
    int seen = 0, i;
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        if (seen == index) {
            const char *n = SDL_GameControllerNameForIndex(i);
            return n ? n : "Gamepad";
        }
        seen++;
    }
    return "";
}

int af_gamepad_button(AfInputState *in, int pad, AfGamepadButton b) {
    if (!in || pad < 0 || pad >= 8 || b < 0 || b >= AF_GP_BUTTON_COUNT) return 0;
    return in->gp_down[pad][b];
}
int af_gamepad_button_pressed(AfInputState *in, int pad, AfGamepadButton b) {
    if (!in || pad < 0 || pad >= 8 || b < 0 || b >= AF_GP_BUTTON_COUNT) return 0;
    return in->gp_pressed[pad][b];
}
float af_gamepad_axis(AfInputState *in, int pad, AfGamepadAxis a) {
    if (!in || pad < 0 || pad >= 8 || a < 0 || a >= 6) return 0.0f;
    return in->gp_axis[pad][a];
}
float af_gamepad_axis_dir(AfInputState *in, int pad, AfVec2 *out) {
    float x = af_gamepad_axis(in, pad, AF_GP_AXIS_LEFTX);
    float y = af_gamepad_axis(in, pad, AF_GP_AXIS_LEFTY);
    if (out) *out = af_v2(x, y);
    return af_v2len(af_v2(x, y));
}

/* ============================================================== clipboard */

void af_clipboard_set(AfStr text) {
    if (text.len <= 0) { SDL_SetClipboardText(""); return; }
    char tmp[4096];
    af_str_cpy_max(text, tmp, (int)sizeof(tmp));
    SDL_SetClipboardText(tmp);
}

AfStrBuf af_clipboard_get(void) {
    AfStrBuf b = AF_STRBUF_INIT;
    char* s = SDL_GetClipboardText();
    if (s) {
        af_strbuf_append(&b, s);
        SDL_free(s);
    }
    return b;
}

/* ================================================================ threads */

struct AfMutex {
#if defined(AF_OS_WINDOWS)
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t m;
#endif
};

struct AfThread {
#if defined(AF_OS_WINDOWS)
    HANDLE h;
#else
    pthread_t t;
#endif
    AfThreadFn fn;
    void* user;
};

AfMutex *af_mutex_create(void) {
    AfMutex *m = (AfMutex *)af_calloc(1, sizeof(AfMutex));
#if defined(AF_OS_WINDOWS)
    InitializeCriticalSection(&m->cs);
#else
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
    return m;
}
void af_mutex_destroy(AfMutex *m) {
    if (!m) return;
#if defined(AF_OS_WINDOWS)
    DeleteCriticalSection(&m->cs);
#else
    pthread_mutex_destroy(&m->m);
#endif
    af_free(m);
}
void af_mutex_lock(AfMutex *m) {
    if (!m) return;
#if defined(AF_OS_WINDOWS)
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->m);
#endif
}
int af_mutex_try_lock(AfMutex *m) {
    if (!m) return 0;
#if defined(AF_OS_WINDOWS)
    return TryEnterCriticalSection(&m->cs) ? 1 : 0;
#else
    return pthread_mutex_trylock(&m->m) == 0;
#endif
}
void af_mutex_unlock(AfMutex *m) {
    if (!m) return;
#if defined(AF_OS_WINDOWS)
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->m);
#endif
}

#if !defined(AF_OS_WINDOWS)
static void *thread_trampoline(void *p) {
    AfThread *t = (AfThread *)p;
    t->fn(t->user);
    return NULL;
}
#else
static DWORD WINAPI thread_trampoline(LPVOID p) {
    AfThread *t = (AfThread *)p;
    t->fn(t->user);
    return 0;
}
#endif

AfThread *af_thread_start(AfThreadFn fn, void *user, const char *name) {
    AF_UNUSED(name);
    if (!fn) return NULL;
    AfThread *t = (AfThread *)af_calloc(1, sizeof(AfThread));
    t->fn = fn;
    t->user = user;
#if defined(AF_OS_WINDOWS)
    t->h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)thread_trampoline, t, 0, NULL);
    if (!t->h) { af_free(t); return NULL; }
#else
    if (pthread_create(&t->t, NULL, thread_trampoline, t) != 0) {
        af_free(t);
        return NULL;
    }
#endif
    return t;
}

void af_thread_join(AfThread *t) {
    if (!t) return;
#if defined(AF_OS_WINDOWS)
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
#else
    pthread_join(t->t, NULL);
#endif
    af_free(t);
}

void af_sleep_ms(int ms) { af_sleep((double)ms * 0.001); }
