/* Smoke test: exercises the window, renderer, textures, shapes, text, JSON and
 * the maths with SDL's dummy video driver, so it runs on a machine with no
 * display. Run it with `make run-tests`. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* SDL_setenv and SDL_getenv work on every target, including MinGW where the
 * CRT has no setenv at all. The engine already depends on SDL2. */
#include <SDL.h>
#include "afndle/platform/platform.h"
#include "afndle/render/render.h"
#include "afndle/core/json.h"
#include "afndle/core/time.h"
#include "afndle/core/str.h"

static int failures;
static const char *current_group = "";

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL [%s] %s:%d  %s\n", current_group, __FILE__, __LINE__, #cond); \
    failures++; } } while (0)

/* headless by default: a test run must never need a display or a GPU.
 * Set AFNDLE_TEST_NO_HEADLESS to test against a real window instead. */
static void headless(void) {
#if !defined(AFNDLE_TEST_NO_HEADLESS)
    if (!SDL_getenv("SDL_VIDEODRIVER")) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (!SDL_getenv("SDL_AUDIODRIVER")) SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
}

static void test_math(void) {
    current_group = "math";
    /* mat3 is row-major, applied as m * v */
    AfMat3 t = af_mat3_translate(af_v2(10.0f, 20.0f));
    AfVec2 p = af_mat3_xform(t, af_v2(1.0f, 2.0f));
    CHECK(af_absf(p.x - 11.0f) < 0.001f && af_absf(p.y - 22.0f) < 0.001f);
    /* xform_dir ignores the translation */
    p = af_mat3_xform_dir(t, af_v2(1.0f, 2.0f));
    CHECK(af_v2eq(p, af_v2(1.0f, 2.0f)));
    /* inverse undoes the transform */
    AfMat3 m = af_mat3_trs(af_v2(5.0f, 6.0f), 0.7f, af_v2s(2.0f));
    AfVec2 orig = af_v2(3.0f, 4.0f);
    AfVec2 there = af_mat3_xform(m, orig);
    AfVec2 back = af_mat3_xform(af_mat3_invert(m), there);
    CHECK(af_absf(back.x - orig.x) < 0.001f && af_absf(back.y - orig.y) < 0.001f);
    /* translate * rotate * scale composes left to right */
    AfVec2 sc = af_mat3_xform(af_mat3_trs(af_v2s(0.0f), 0.0f, af_v2(2.0f, 3.0f)),
                              af_v2(1.0f, 1.0f));
    CHECK(af_v2eq(sc, af_v2(2.0f, 3.0f)));
    /* det of a scale is the area factor */
    CHECK(af_absf(af_mat3_det(af_mat3_scale(af_v2(2.0f, 3.0f))) - 6.0f) < 0.001f);
    /* colour helpers */
    CHECK(af_color_eq(af_color_hex(0xFF800040), af_rgba8(255, 128, 0, 0x40)));
    CHECK(af_color_to_rgba8(af_color_hex(0x336699FF)) == 0x336699FFu);
    /* rects and bounds */
    AfBounds b = af_bounds_from_rect(af_rect(2.0f, 3.0f, 4.0f, 6.0f));
    CHECK(af_v2eq(b.min, af_v2(2.0f, 3.0f)));
    CHECK(af_v2eq(b.max, af_v2(6.0f, 9.0f)));
    CHECK(af_v2eq(af_bounds_center(b), af_v2(4.0f, 6.0f)));
    AfBounds mb = af_bounds_merge(b, af_bounds_from_rect(af_rect(-1.0f, 0.0f, 1.0f, 1.0f)));
    CHECK(af_absf(mb.min.x + 1.0f) < 0.001f);
}

static void test_strings(void) {
    current_group = "str";
    /* AfStrBuf owns its own heap, so it is a good stand-in for a scratch pad */
    AfStrBuf sb = AF_STRBUF_INIT;
    af_strbuf_appendf(&sb, "n=%d s=%s", 42, "x");
    CHECK(af_str_eq_cstr(af_strbuf_view(&sb), "n=42 s=x"));
    /* always NUL-terminated, so the raw pointer is usable as a C string */
    CHECK(af_str_eq_cstr(af_str(af_strbuf_cstr(&sb)), "n=42 s=x"));
    /* slices clamp instead of running off the end */
    AfStr all = af_strbuf_view(&sb);
    CHECK(af_str_eq_cstr(af_str_slice(all, 0, 1), "n"));
    CHECK(af_str_empty(af_str_slice(all, 100, 200)));
    CHECK(af_str_empty(af_str_slice(all, 5, 2)));   /* end before start */
    /* a negative start clamps to 0 rather than reading behind the buffer */
    CHECK(af_str_eq_cstr(af_str_slice(all, -5, 2), "n="));
    /* erase then append */
    af_strbuf_erase(&sb, 0, 2);
    CHECK(af_str_eq_cstr(af_strbuf_view(&sb), "42 s=x"));
    af_strbuf_append_char(&sb, '!');
    CHECK(af_str_eq_cstr(af_strbuf_view(&sb), "42 s=x!"));
    /* parse helpers */
    int32_t iv = 0;
    CHECK(af_str_to_i32(af_str("123"), &iv) && iv == 123);
    CHECK(!af_str_to_i32(af_str("nope"), &iv));
    float fv = 0.0f;
    CHECK(af_str_to_f32(af_str("1.5"), &fv) && af_absf(fv - 1.5f) < 0.001f);
    /* a sub-string is found */
    CHECK(af_str_find(af_str("hello world"), af_str("world")) == 6);
    CHECK(af_str_find(af_str("hello"), af_str("zzz")) == -1);
    /* trim hands back a view, not a copy */
    CHECK(af_str_eq_cstr(af_str_trim(af_str("  pad  ")), "pad"));
    af_strbuf_free(&sb);
}

static void test_json(void) {
    current_group = "json";
    AfArena *a = af_arena_create(64 * 1024);
    const char *err = NULL;
    int32_t pos = 0;
    AfJson *doc = af_json_parse(
        a, "{\"name\":\"pong\",\"players\":[1,2],\"on\":true,\"n\":-3.5}", -1,
        &err, &pos);
    CHECK(doc != NULL);
    if (doc) {
        CHECK(af_str_eq_cstr(af_json_get_str(doc, "name", ""), "pong"));
        CHECK(af_json_get_i32(doc, "missing", 7) == 7);
        CHECK(af_json_count(af_json_get_arr(doc, "players")) == 2);
        /* array elements are bare nodes, so read the union directly */
        AfJson *arr = af_json_get_arr(doc, "players");
        CHECK(af_json_is(af_json_at(arr, 0), AF_JSON_NUMBER));
        CHECK((int32_t)af_json_at(arr, 0)->u.number == 1);
        CHECK((int32_t)af_json_at(arr, 1)->u.number == 2);
        CHECK(af_json_at(arr, 9) == NULL);   /* out of range, not a crash */
        CHECK(af_json_get_bool(doc, "on", 0) == 1);
        CHECK(af_absf(af_json_get_f64(doc, "n", 0.0) + 3.5) < 0.0001);
        /* round trip */
        char *txt = af_json_to_string(a, doc, 0);
        CHECK(txt && strstr(txt, "\"pong\"") != NULL);
        /* duplicate keys are rejected, not silently overwritten */
        AfArena *a2 = af_arena_create(4096);
        const char *e2 = NULL;
        int32_t p2 = 0;
        CHECK(af_json_parse(a2, "{\"a\":1,\"a\":2}", -1, &e2, &p2) == NULL);
        CHECK(e2 != NULL);
        af_arena_destroy(a2);
        /* builder API round trips through the parser */
        AfArena *a3 = af_arena_create(4096);
        AfJson *obj = af_json_new_obj(a3);
        af_json_set_int(a3, obj, "hp", 5);
        AfJson *tags = af_json_new_arr(a3);
        af_json_push(a3, tags, af_json_new_str(a3, "hi"));
        af_json_set(a3, obj, "tags", tags);
        char *s3 = af_json_to_string(a3, obj, 0);
        AfJson *back = af_json_parse(a3, s3, -1, &e2, &p2);
        CHECK(back != NULL);
        CHECK(af_json_get_i32(back, "hp", 0) == 5);
        CHECK(af_json_count(af_json_get_arr(back, "tags")) == 1);
        af_arena_destroy(a3);
    }
    af_arena_destroy(a);
}

/* The argc/argv form is required, not decorative: on Windows SDL2main's
 * headers redirect main to SDL_main(int, char**), and a main(void) would
 * not match that declaration. */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    headless();
    test_math();
    test_strings();
    test_json();

    current_group = "platform";
    af_platform_init("afndle-tests");
    CHECK(af_platform_video_init());

    AfWindowDesc d = af_window_desc_default();
    d.width = 320;
    d.height = 240;
    d.vsync = 0;
    AfWindow *win = af_window_create(&d);
    CHECK(win != NULL);
    if (!win) {
        printf("no window, aborting\n");
        return 1;
    }

    AfRenderer *r = af_window_renderer(win);
    CHECK(r != NULL);
    CHECK(af_r2d_of_window(win) == r);
    CHECK(af_window_from_renderer(r) == win);

    AfVec2 size = af_r2d_screen_size(r);
    CHECK(size.x > 0.0f && size.y > 0.0f);
    printf("screen %.0fx%.0f scale %.2f\n", size.x, size.y, af_r2d_render_scale(r));

    /* A resize must leave the renderer and the window agreeing on the
     * drawable size. The exact numbers are the driver's business: the dummy
     * video driver ignores the requested size entirely, so assert the
     * invariant rather than the value. */
    {
        int dw = 0, dh = 0;
        af_window_set_size(win, 480, 320);
        CHECK(af_window_get_drawable_size(win, &dw, &dh));
        CHECK(dw > 0 && dh > 0);
        CHECK(af_absf(af_r2d_screen_size(r).x - (float)dw) < 0.5f);
        CHECK(af_absf(af_r2d_screen_size(r).y - (float)dh) < 0.5f);
        CHECK(af_window_get_render_scale(win) > 0.0f);
    }

    current_group = "camera";
    /* the renderer fills in the view size, so read back the camera the frame
     * will actually use */
    af_r2d_set_camera(r, af_camera_at(af_v2(100.0f, 50.0f), 2.0f));
    AfCamera2D cam = af_r2d_get_camera(r);
    CHECK(af_v2eq(cam.size, af_r2d_screen_size(r)));
    AfVec2 c = af_camera_to_screen(cam, af_v2(100.0f, 50.0f));
    CHECK(af_absf(c.x - cam.size.x * 0.5f) < 0.01f);
    CHECK(af_absf(c.y - cam.size.y * 0.5f) < 0.01f);
    AfVec2 back = af_camera_to_world(cam, c);
    CHECK(af_absf(back.x - 100.0f) < 0.01f && af_absf(back.y - 50.0f) < 0.01f);
    /* at 2x zoom one world unit is two pixels */
    AfVec2 r1 = af_camera_to_screen(cam, af_v2(101.0f, 50.0f));
    CHECK(af_absf(r1.x - c.x - 2.0f) < 0.01f);
    /* the visible world rect shrinks as zoom rises */
    AfRect vis = af_r2d_visible_world_rect(r);
    CHECK(af_absf(vis.size.x - cam.size.x * 0.5f) < 0.5f);
    CHECK(af_absf(vis.size.y - cam.size.y * 0.5f) < 0.5f);
    CHECK(af_absf(af_rect_center_x(vis) - 100.0f) < 0.5f);
    /* clamping keeps the level on screen */
    AfCamera2D cl = af_camera_at(af_v2(-9999.0f, 0.0f), 1.0f);
    cl.size = af_r2d_screen_size(r);
    cl = af_camera_clamp(cl, af_rect(0, 0, 800, 600));
    CHECK(cl.position.x >= 0.0f);
    af_r2d_set_camera(r, af_camera_default());

    current_group = "texture";
    AfTexture *checker =
        af_texture_checker(8, af_color_hex(0x334455), af_color_hex(0x8899AA));
    CHECK(af_texture_valid(checker));
    CHECK(af_texture_aspect(checker) == 1.0f);
    AfTexture *dot = af_texture_circle(16, af_color_hex(0xFF5533), af_color_white());
    CHECK(af_texture_valid(dot));
    AfTexture *solid = af_texture_solid(4, 4, af_color_hex(0x123456));
    CHECK(af_texture_valid(solid));
    CHECK(af_texture_update(solid, NULL, 0, 0, 0) == 0);  /* refuses NULL */
    /* procedural textures are owned by the caller and never enter the path
     * cache, so this only counts what af_texture_load() pulled in */
    CHECK(af_texture_cache_count() == 0);
    {
        /* write a real file, then load it twice: the second load is the cache */
        const char *path = "afndle-cache-test.png";
        CHECK(af_texture_save_png(solid, path));
        int before = af_texture_cache_count();
        AfTexture *l1 = af_texture_load(path);
        AfTexture *l2 = af_texture_load(path);
        CHECK(l1 != NULL);
        CHECK(l1 == l2);                       /* same object, not reloaded */
        CHECK(af_texture_cache_count() == before + 1);
        CHECK(af_texture_width(l1) == 4 && af_texture_height(l1) == 4);
        CHECK(af_str_eq_cstr(af_str(af_texture_path(l1)), path));
        CHECK(af_texture_load("afndle-no-such-file.png") == NULL);
        /* a reload drops the whole cache without touching live textures */
        af_texture_cache_clear();
        CHECK(af_texture_cache_count() == 0);
        remove(path);
    }

    current_group = "font";
    AfFont *font = af_font_default();
    CHECK(font != NULL);
    printf("font: %s (builtin=%d)\n", af_font_path(font), af_font_is_builtin(font));
    float w1 = af_font_measure(font, 16.0f, "Hello", 0);
    float w2 = af_font_measure(font, 16.0f, "Hello world", 0);
    CHECK(w2 > w1);
    /* two sizes on one font must not contaminate each other */
    float a11 = af_font_measure(font, 11.0f, "mmmwww", 0);
    float b37 = af_font_measure(font, 37.0f, "mmmwww", 0);
    CHECK(b37 > a11 * 2.0f);
    CHECK(af_font_measure(font, 11.0f, "mmmwww", 0) == a11);
    /* max_chars caps the count */
    CHECK(af_font_measure(font, 16.0f, "Hello world", 5) <= w1 + 0.01f);
    /* ellipsize fits and stays valid UTF-8 */
    char buf[64];
    af_font_ellipsize(font, 16.0f, "a very long label indeed", 40.0f, buf,
                      (int)sizeof(buf));
    CHECK(af_font_measure(font, 16.0f, buf, 0) <= 41.0f);
    CHECK(strlen(buf) < sizeof(buf));
    printf("ellipsized: \"%s\"\n", buf);
    /* char_at finds the index under a pixel column */
    int byte_off = -1;
    int ci = af_font_char_at(font, 16.0f, "Hello world", w1 * 0.5f, &byte_off);
    CHECK(ci >= 0 && ci < 11);
    CHECK(byte_off >= 0 && byte_off <= 11);

    current_group = "utf8";
    CHECK(af_utf8_length("a\xc3\xa9z") == 3);
    int i = 0;
    CHECK(af_utf8_next("\xc3\xa9", &i) == 0xE9);
    char enc[4];
    CHECK(af_utf8_encode(0xE9, enc) == 2);
    CHECK((unsigned char)enc[0] == 0xC3 && (unsigned char)enc[1] == 0xA9);
    CHECK(af_utf8_encode(0x1F600, enc) == 4);
    /* a full walk over a mixed string lands on the same count as af_utf8_length */
    {
        const char *mixed = "a\xc3\xa9z\xe2\x9c\x93\xf0\x9f\x98\x80";
        int j = 0, count = 0;
        uint32_t last = 0;
        while (mixed[j]) { last = af_utf8_next(mixed, &j); count++; }
        CHECK(count == af_utf8_length(mixed));
        CHECK(last == 0x1F600);
    }

    current_group = "frame";
    /* one full frame: sprites, shapes, clip, text */
    AfEvent evs[64];
    int n = 0;
    AfInputState input;
    af_pump_events(win, evs, 64, &n, &input);

    af_r2d_begin(r, af_color_hex(0x1A1A24));
    af_r2d_fill(r, af_rect(0, 0, size.x, size.y * 0.4f), af_color_hex(0x2A2A3A));
    for (int k = 0; k < 200; k++) {
        float x = (float)(k % 20) * 16.0f;
        float y = (float)(k / 20) * 16.0f + 60.0f;
        af_r2d_sprite_simple(r, checker, af_rect(x, y, 16, 16), af_color_white(), 0);
    }
    af_r2d_push_clip(r, af_rect(8, 8, 200, 100));
    for (int k = 0; k < 50; k++)
        af_r2d_circle(r, af_v2(20.0f + k * 6.0f, 30.0f), 5.0f,
                      af_color_hex(0x44FF88), 8);
    af_r2d_pop_clip(r);
    af_r2d_line(r, af_v2(0, 0), af_v2(size.x, size.y), 2.0f, af_color_hex(0xFF4444));
    af_r2d_rect_outline(r, af_rect(4, 4, size.x - 8, size.y - 8), 1.0f,
                        af_color_white());
    af_r2d_ui_text(r, font, af_v2(10, 10), "afternoodle", af_color_white(), 16.0f);
    af_r2d_text_outline(r, font, af_v2(10, 200), "line one\nline two",
                        af_color_hex(0xFFE066), af_color_black(), 14.0f, 1.0f);
    af_r2d_end(r);
    /* stats are complete only after the frame ends: sprites flush there */
    AfRenderStats st = af_r2d_stats(r);
    CHECK(st.draw_calls > 0);
    CHECK(st.quads >= 200);
    CHECK(st.sprite_count == 200);
    CHECK(st.batches < st.quads); /* batching actually happened */
    CHECK(st.vertices > 0);
    printf("frame: %d draw calls, %d quads, %d tris, %d batches, %d verts\n",
           st.draw_calls, st.quads, st.triangles, st.batches, st.vertices);

    /* a fully transparent draw is dropped, not batched */
    af_r2d_begin(r, af_color_clear());
    af_r2d_fill(r, af_rect(0, 0, 10, 10), af_color_clear());
    af_r2d_end(r);
    CHECK(af_r2d_stats(r).quads == 0);

    current_group = "readback";
    {
        /* write known pixels, read them back, write a PNG */
        static uint32_t px[16 * 16];
        for (int k = 0; k < 16 * 16; k++)
            px[k] = (k & 1) ? af_tex_rgba8(0, 0, 255, 255)
                            : af_tex_rgba8(255, 0, 0, 255);
        AfTexture *t = af_texture_create(px, 16, 16, 16 * 4, AF_FILTER_NEAREST);
        CHECK(t != NULL);
        AfColor c0 = af_texture_get_pixel(t, 0, 0);
        AfColor c1 = af_texture_get_pixel(t, 1, 0);
        CHECK(af_color_eq(c0, af_rgba8(255, 0, 0, 255)));
        CHECK(af_color_eq(c1, af_rgba8(0, 0, 255, 255)));
        /* out of range reads are refused, not clamped */
        CHECK(af_color_eq(af_texture_get_pixel(t, -1, 0), af_color_clear()));
        CHECK(af_color_eq(af_texture_get_pixel(t, 999, 0), af_color_clear()));
        /* the PNG is written somewhere we can delete afterwards */
        const char *png = "afndle-readback-test.png";
        CHECK(af_texture_save_png(t, png));
        remove(png);
        af_texture_destroy(t);
    }

    current_group = "teardown";
    af_window_destroy(win);
    af_platform_shutdown();

    if (failures)
        printf("\n%d CHECK(s) FAILED\n", failures);
    else
        printf("\nall checks passed\n");
    return failures ? 1 : 0;
}
