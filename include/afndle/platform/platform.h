/* Platform abstraction: window, input, filesystem, clipboard, threads.
 *
 * Everything SDL-specific lives in src/platform. The rest of the engine only
 * ever sees this header, so a second backend (or a headless one) is a drop-in.
 */
#ifndef AFNDLE_PLATFORM_PLATFORM_H
#define AFNDLE_PLATFORM_PLATFORM_H

#include <stdarg.h>
#include <stdint.h>

#include "afndle/core/afconfig.h"
#include "afndle/core/math.h"
#include "afndle/core/mem.h"
#include "afndle/core/str.h"

/* ============================================================== key codes */
/* Layout-independent, SDL_Scancode values so bindings work on AZERTY/Dvorak.
 * af_key_name() renders the platform's local name for display. */
typedef enum {
    AF_KEY_UNKNOWN = 0,
    AF_KEY_A, AF_KEY_B, AF_KEY_C, AF_KEY_D, AF_KEY_E, AF_KEY_F, AF_KEY_G,
    AF_KEY_H, AF_KEY_I, AF_KEY_J, AF_KEY_K, AF_KEY_L, AF_KEY_M, AF_KEY_N,
    AF_KEY_O, AF_KEY_P, AF_KEY_Q, AF_KEY_R, AF_KEY_S, AF_KEY_T, AF_KEY_U,
    AF_KEY_V, AF_KEY_W, AF_KEY_X, AF_KEY_Y, AF_KEY_Z,
    AF_KEY_0, AF_KEY_1, AF_KEY_2, AF_KEY_3, AF_KEY_4,
    AF_KEY_5, AF_KEY_6, AF_KEY_7, AF_KEY_8, AF_KEY_9,
    AF_KEY_ESCAPE, AF_KEY_ENTER, AF_KEY_TAB, AF_KEY_BACKSPACE, AF_KEY_SPACE,
    AF_KEY_MINUS, AF_KEY_EQUALS, AF_KEY_LEFTBRACKET, AF_KEY_RIGHTBRACKET,
    AF_KEY_BACKSLASH, AF_KEY_SEMICOLON, AF_KEY_APOSTROPHE, AF_KEY_GRAVE,
    AF_KEY_COMMA, AF_KEY_PERIOD, AF_KEY_SLASH, AF_KEY_CAPS_LOCK,
    AF_KEY_F1, AF_KEY_F2, AF_KEY_F3, AF_KEY_F4, AF_KEY_F5, AF_KEY_F6,
    AF_KEY_F7, AF_KEY_F8, AF_KEY_F9, AF_KEY_F10, AF_KEY_F11, AF_KEY_F12,
    AF_KEY_PRINTSCREEN, AF_KEY_SCROLLLOCK, AF_KEY_PAUSE,
    AF_KEY_INSERT, AF_KEY_HOME, AF_KEY_PAGEUP, AF_KEY_DELETE, AF_KEY_END,
    AF_KEY_PAGEDOWN,
    AF_KEY_RIGHT, AF_KEY_LEFT, AF_KEY_DOWN, AF_KEY_UP,
    AF_KEY_NUMLOCKCLEAR, AF_KEY_KP_DIVIDE, AF_KEY_KP_MULTIPLY, AF_KEY_KP_MINUS,
    AF_KEY_KP_PLUS, AF_KEY_KP_ENTER, AF_KEY_KP_1, AF_KEY_KP_2, AF_KEY_KP_3,
    AF_KEY_KP_4, AF_KEY_KP_5, AF_KEY_KP_6, AF_KEY_KP_7, AF_KEY_KP_8,
    AF_KEY_KP_9, AF_KEY_KP_0, AF_KEY_KP_PERIOD,
    AF_KEY_APPLICATION, AF_KEY_POWER, AF_KEY_LCTRL, AF_KEY_LSHIFT, AF_KEY_LALT,
    AF_KEY_LGUI, AF_KEY_RCTRL, AF_KEY_RSHIFT, AF_KEY_RALT, AF_KEY_RGUI,
    AF_KEY_COUNT
} AfKey;

typedef enum {
    AF_MOUSE_LEFT = 0, AF_MOUSE_MIDDLE, AF_MOUSE_RIGHT, AF_MOUSE_X1,
    AF_MOUSE_X2, AF_MOUSE_BUTTON_COUNT
} AfMouseButton;

typedef enum {
    AF_MOD_NONE   = 0,
    AF_MOD_LSHIFT = 1 << 0, AF_MOD_RSHIFT = 1 << 1, AF_MOD_SHIFT = 0x3,
    AF_MOD_LCTRL  = 1 << 2, AF_MOD_RCTRL  = 1 << 3, AF_MOD_CTRL  = 0xC,
    AF_MOD_LALT   = 1 << 4, AF_MOD_RALT  = 1 << 5, AF_MOD_ALT   = 0x30,
    AF_MOD_LGUI   = 1 << 6, AF_MOD_RGUI  = 1 << 7, AF_MOD_GUI   = 0xC0
} AfMod;

AF_API const char* af_key_name(AfKey key);
AF_API AfKey       af_key_from_name(const char* name);
/** Maps a raw SDL scancode to AfKey; exposed so tests can drive input. */
AF_API AfKey       af_key_from_scancode(int scancode);
AF_API const char* af_mouse_button_name(AfMouseButton b);

/* ================================================================== window */
typedef struct AfWindow AfWindow;
typedef struct AfRenderer AfRenderer;

typedef struct {
    const char* title;
    int         width, height;
    int         resizable;   /* 0 = fixed size, 1 = resizable */
    int         fullscreen;
    int         vsync;
    int         high_dpi;    /* allow render scale > 1 on phones/desktops */
    float       scale;       /* render scale hint, 0 = auto */
    int         max_fps;     /* 0 = uncapped */
    int         hidden;      /* start hidden (editor uses a second hidden win) */
} AfWindowDesc;

/** Fills a desc with 1280x720, resizable, vsync, high-DPI. Start here, then
 *  override what you need. */
AF_API AfWindowDesc af_window_desc_default(void);

AF_API AfWindow* af_window_create(const AfWindowDesc* desc);
AF_API void      af_window_destroy(AfWindow* win);
AF_API AfRenderer* af_window_renderer(AfWindow* win);
AF_API AfWindow* af_window_from_renderer(AfRenderer* r);

AF_API void af_window_set_title(AfWindow* win, const char* title);
AF_API void af_window_get_size(AfWindow* win, int* w, int* h);
AF_API void af_window_set_size(AfWindow* win, int w, int h);
AF_API int  af_window_get_drawable_size(AfWindow* win, int* w, int* h);
AF_API float af_window_get_render_scale(AfWindow* win);
AF_API void af_window_set_fullscreen(AfWindow* win, int fullscreen);
AF_API int  af_window_is_fullscreen(AfWindow* win);
AF_API void af_window_minimize(AfWindow* win);
AF_API void af_window_restore(AfWindow* win);
AF_API void af_window_show(AfWindow* win);
AF_API void af_window_hide(AfWindow* win);
AF_API void af_window_focus(AfWindow* win);
AF_API void af_window_set_icon_from_file(AfWindow* win, const char* path);
AF_API double af_window_get_monitor_refresh_hz(AfWindow* win);
/** Asks the WM to close. The flag is visible in af_window_should_close(). */
AF_API void af_window_request_close(AfWindow* win);
/** True once the user or the app asked to close. The main loop polls this. */
AF_API int  af_window_should_close(AfWindow* win);
AF_API void af_window_clear_close(AfWindow* win);

/* ================================================================== events */
typedef enum {
    AF_EVENT_NONE = 0,
    AF_EVENT_QUIT,
    AF_EVENT_WINDOW_CLOSE, AF_EVENT_WINDOW_RESIZED, AF_EVENT_WINDOW_MOVED,
    AF_EVENT_WINDOW_FOCUS_GAINED, AF_EVENT_WINDOW_FOCUS_LOST,
    AF_EVENT_WINDOW_MINIMIZED, AF_EVENT_WINDOW_RESTORED,
    AF_EVENT_WINDOW_RESIZE_PIXEL, /* HiDPI scale changed */
    AF_EVENT_KEY_DOWN, AF_EVENT_KEY_UP, AF_EVENT_TEXT_INPUT,
    AF_EVENT_MOUSE_MOVE, AF_EVENT_MOUSE_DOWN, AF_EVENT_MOUSE_UP,
    AF_EVENT_MOUSE_WHEEL,
    AF_EVENT_GAMEPAD_ADDED, AF_EVENT_GAMEPAD_REMOVED,
    AF_EVENT_GAMEPAD_BUTTON_DOWN, AF_EVENT_GAMEPAD_BUTTON_UP,
    AF_EVENT_GAMEPAD_AXIS,
    AF_EVENT_TOUCH_DOWN, AF_EVENT_TOUCH_UP, AF_EVENT_TOUCH_MOVE,
    AF_EVENT_COUNT
} AfEventType;

typedef struct {
    AfEventType type;
    uint32_t    window_id;
    /* key */
    int         key;         /* AfKey */
    int         scancode;
    int         mods;        /* AfMod bits */
    int         repeat;
    char        text[16];    /* AF_EVENT_TEXT_INPUT, UTF-8 */
    /* mouse / touch (window pixels) */
    float       x, y;
    float       dx, dy;
    float       wheel_x, wheel_y;
    int         button;      /* AfMouseButton */
    float       pressure;
    int         finger_id;
    /* gamepad */
    int         gamepad_id;
    int         gp_button;
    int         gp_axis;
    float       gp_value;
} AfEvent;

#define AF_MAX_GAMEPADS 8
#define AF_GP_BUTTON_COUNT 15
#define AF_GP_AXIS_COUNT 6
/* Text typed in one frame. Longer pastes are truncated on purpose: the editor
 * reads character by character and does not want a 4KB burst. */
#define AF_TEXT_INPUT_MAX 32

/** A frame's input snapshot. Public so a game can keep one on the stack or
 *  copy it per frame without going through the window. */
struct AfInputState {
    /* key edges */
    uint8_t  key_down[AF_KEY_COUNT];
    uint8_t  key_pressed[AF_KEY_COUNT];
    uint8_t  key_released[AF_KEY_COUNT];
    uint16_t key_frames[AF_KEY_COUNT];
    int      mods;                 /* AfMod bits */
    /* mouse */
    float    mouse_x, mouse_y;
    float    mouse_dx, mouse_dy;
    float    scroll_x, scroll_y;
    uint8_t  mouse_down[AF_MOUSE_BUTTON_COUNT];
    uint8_t  mouse_pressed[AF_MOUSE_BUTTON_COUNT];
    uint8_t  mouse_released[AF_MOUSE_BUTTON_COUNT];
    int      mouse_inside;
    /* text typed this frame, UTF-8 */
    char     text[AF_TEXT_INPUT_MAX];
    /* touch: the first finger also drives the mouse */
    int      touch_count;
    int      has_touch;
    int      touch_used_as_mouse;
    /* gamepads */
    uint8_t  gp_down[AF_MAX_GAMEPADS][AF_GP_BUTTON_COUNT];
    uint8_t  gp_pressed[AF_MAX_GAMEPADS][AF_GP_BUTTON_COUNT];
    float    gp_axis[AF_MAX_GAMEPADS][AF_GP_AXIS_COUNT];
    float    gp_axis_prev[AF_MAX_GAMEPADS][AF_GP_AXIS_COUNT];
    int      gamepad_count;
};
typedef struct AfInputState AfInputState;

/** Pumps the OS event queue into `events` and refreshes the input snapshot.
 *  `events` may be NULL to only refresh `input`. */
AF_API void af_pump_events(AfWindow* win, AfEvent* events, int max_events,
                           int* out_count, AfInputState* input);
/** Input snapshot valid after af_pump_events. */
AF_API const AfInputState* af_input(AfWindow* win);
AF_API int         af_key_down(AfInputState* in, AfKey k);
AF_API int         af_key_pressed(AfInputState* in, AfKey k); /* this frame */
AF_API int         af_key_released(AfInputState* in, AfKey k);
AF_API int         af_key_held_frames(AfInputState* in, AfKey k);
AF_API int         af_mouse_down(AfInputState* in, AfMouseButton b);
AF_API int         af_mouse_pressed(AfInputState* in, AfMouseButton b);
AF_API int         af_mouse_released(AfInputState* in, AfMouseButton b);
AF_API AfVec2      af_mouse_pos(AfInputState* in);
AF_API AfVec2      af_mouse_delta(AfInputState* in);
AF_API AfVec2      af_mouse_scroll(AfInputState* in);
AF_API int         af_mods(AfInputState* in);
AF_API int         af_key_mod(AfInputState* in, AfMod m);
/** Text typed this frame, UTF-8. */
AF_API const char* af_text_input(AfInputState* in);
/* Gestures the editor and the on-screen joystick share. */
AF_API int         af_input_has_touch(AfInputState* in);
AF_API int         af_touch_count(AfInputState* in);
/* Clears per-frame edges. Call at the end of a frame if you pump twice. */
AF_API void        af_input_end_frame(AfInputState* in);

/* Any-window pump (no window handle needed). */
AF_API void af_pump_events_global(AfEvent* events, int max_events, int* out_count,
                                  AfInputState* input);
/* Requests the soft keyboard (Android/iOS). No-op elsewhere. */
AF_API void af_platform_show_soft_keyboard(int show);
/** True if running on a phone/tablet form factor. */
AF_API int  af_platform_is_mobile(void);
AF_API const char* af_platform_name(void);
AF_API const char* af_platform_cpu_name(void);
int         af_platform_stdout_is_tty(void);
/** Opens a URL or file in the OS handler. */
AF_API int  af_platform_open_url(const char* url);
/** Vibration where supported (no-op elsewhere). */
AF_API void af_platform_vibrate(int milliseconds);

/* =============================================================== gamepads */
typedef enum {
    AF_GP_A = 0, AF_GP_B, AF_GP_X, AF_GP_Y,
    AF_GP_BACK, AF_GP_GUIDE, AF_GP_START,
    AF_GP_LEFTSTICK, AF_GP_RIGHTSTICK, AF_GP_LEFTSHOULDER, AF_GP_RIGHTSHOULDER,
    AF_GP_DPAD_UP, AF_GP_DPAD_DOWN, AF_GP_DPAD_LEFT, AF_GP_DPAD_RIGHT
} AfGamepadButton;
typedef enum {
    AF_GP_AXIS_LEFTX = 0, AF_GP_AXIS_LEFTY, AF_GP_AXIS_RIGHTX, AF_GP_AXIS_RIGHTY,
    AF_GP_AXIS_TRIGGERLEFT, AF_GP_AXIS_TRIGGERRIGHT
} AfGamepadAxis;

AF_API int         af_gamepad_count(void);
AF_API const char* af_gamepad_name(int index);
AF_API int         af_gamepad_button(AfInputState* in, int pad, AfGamepadButton b);
AF_API int         af_gamepad_button_pressed(AfInputState* in, int pad, AfGamepadButton b);
AF_API float       af_gamepad_axis(AfInputState* in, int pad, AfGamepadAxis a);
AF_API float       af_gamepad_axis_dir(AfInputState* in, int pad, AfVec2* out); /* left stick, -1..1 */

/* ============================================================== filesystem */
AF_API int    af_fs_exists(const char* path);
AF_API int    af_fs_is_dir(const char* path);
AF_API int    af_fs_is_file(const char* path);
/** Reads the whole file. Caller frees with af_free. *len may be NULL. */
AF_API char*  af_fs_read_file(const char* path, int64_t* out_len);
AF_API int    af_fs_write_file(const char* path, const void* data, int64_t len);
AF_API int    af_fs_append_file(const char* path, const void* data, int64_t len);
/** Creates one directory. Returns 1 on success or if it already existed. */
AF_API int    af_fs_mkdir(const char* path);
/** Creates every missing component of `path`. */
AF_API int    af_fs_mkdirs(const char* path);
AF_API int    af_fs_remove(const char* path);
AF_API int    af_fs_rename(const char* from, const char* to);
AF_API int64_t af_file_size(const char* path);
/** Modification time as unix seconds, 0 if unknown. */
AF_API int64_t af_fs_modified_time(const char* path);

typedef struct {
    char name[256];
    int  is_dir;
    int64_t size;
    int64_t modified;
} AfDirEntry;

/** Lists a directory, sorted dirs-first then case-insensitive by name.
 * Pass *out_count == 0. Caller frees with af_free. */
AF_API AfDirEntry* af_fs_list_dir(const char* path, int* out_count);
/** Directory name of `path` (writes at most cap bytes). */
AF_API void   af_fs_dirname(const char* path, char* out, int cap);
/** File name of `path`. */
AF_API void   af_fs_basename(const char* path, char* out, int cap);
AF_API void   af_fs_strip_extension(const char* path, char* out, int cap);
AF_API const char* af_fs_extension(const char* path); /* "" or ".png" */
/** Joins with the platform separator, collapsing duplicates. */
AF_API void   af_fs_path_join(const char* a, const char* b, char* out, int cap);
AF_API void   af_fs_normalize(const char* in, char* out, int cap);
AF_API int    af_fs_is_absolute(const char* path);
AF_API void   af_fs_absolute(const char* path, char* out, int cap);
/** Directory holding the running executable. */
AF_API void   af_fs_exe_dir(char* out, int cap);
/** Changes the process working directory. Returns 0 on failure. */
AF_API int    af_fs_set_cwd(const char* path);
AF_API void   af_fs_get_cwd(char* out, int cap);
/** Path to a user-writable config dir for this app, created on demand. */
AF_API void   af_fs_user_config_dir(const char* app, char* out, int cap);

/* ================================================================= threads */
typedef struct AfMutex AfMutex;
typedef struct AfThread AfThread;
AF_API AfMutex* af_mutex_create(void);
AF_API void     af_mutex_destroy(AfMutex* m);
AF_API void     af_mutex_lock(AfMutex* m);
AF_API int      af_mutex_try_lock(AfMutex* m);
AF_API void     af_mutex_unlock(AfMutex* m);
typedef void (*AfThreadFn)(void* user);
AF_API AfThread* af_thread_start(AfThreadFn fn, void* user, const char* name);
AF_API void      af_thread_join(AfThread* t);
AF_API void      af_sleep_ms(int ms);

/* ============================================================== clipboard */
AF_API void        af_clipboard_set(AfStr text);
AF_API AfStrBuf    af_clipboard_get(void); /* caller frees with af_strbuf_free */

/* ============================================================ subsystems */
AF_API void af_platform_init(const char* app_name);
AF_API void af_platform_shutdown(void);
AF_API int  af_platform_video_init(void);
AF_API void af_platform_video_shutdown(void);
AF_API int  af_platform_audio_init(void);
AF_API void af_platform_audio_shutdown(void);
AF_API int  af_platform_has_audio(void);
AF_API const char* af_platform_clipboard_mime(void);

#endif /* AFNDLE_PLATFORM_PLATFORM_H */
