# afternoodle

A 2D game engine in C11, built on SDL2. Everything is C: the engine, the visual
node editor, the toolchain, and the game format.

```c
#include "afndle/render/render.h"

af_platform_init("my game");
af_platform_video_init();

AfWindowDesc d = af_window_desc_default();
d.title = "my game";
AfWindow *win = af_window_create(&d);
AfRenderer *r = af_window_renderer(win);

AfFont *font = af_font_default();

while (running) {
    AfEvent evs[64];
    int n = 0;
    AfInputState input;
    af_pump_events(win, evs, 64, &n, &input);

    af_r2d_begin(r, af_color_hex(0x1A1A24));
    af_r2d_sprite_simple(r, af_texture_load("hero.png"),
                         af_rect(32, 32, 64, 64), af_color_white(), 0);
    af_r2d_ui_text(r, font, af_v2(10, 10), "hello", af_color_white(), 16.0f);
    af_r2d_end(r);
}
```

## Status

Early. The rendering and platform layers are implemented, tested, and used by
the engine's own test suite. The ECS, physics, scripting, audio, and editor
layers are designed but not written yet, so the version here is a renderer and a
window, not a finished engine.

Built and passing:

- **core** — math (vectors, rects, bounds, 2x3 transforms), strings, arenas,
  logging, time, JSON parse/serialize
- **platform** — window and renderer lifetime, resize and DPI sync, input
  state, event pump, filesystem
- **render** — batched sprite drawing via `SDL_RenderGeometry`, cameras, nested
  clipping, shape primitives, TTF and bitmap fonts, text layout, procedural
  textures, GPU readback, PNG export

Not written yet: `src/ecs`, `src/physics`, `src/audio`, `src/script`,
`src/vsl`, `src/app`, `src/editor`, `tools`.

## Building

Needs SDL2, SDL2_image, SDL2_ttf, and SDL2_mixer.

```sh
# Linux
sudo apt install libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev libsdl2-mixer-dev
make
make run-tests

# macOS
brew install sdl2 sdl2_image sdl2_ttf sdl2_mixer
make
make run-tests

# Windows (MSYS2 / Git Bash with MinGW)
pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-SDL2_image mingw-w64-x86_64-SDL2_ttf mingw-w64-x86_64-SDL2_mixer
make -f Makefile.win
```

`make` produces `build/libafndle.a` and `build/afndle-tests`. The test binary
runs headless with SDL's dummy video driver, so it works over SSH and in CI.

On macOS and Linux, release artifacts are built by CI. See
`.github/workflows/build.yml`.

## Layout

```
include/afndle/   public headers, one directory per layer
src/              implementation, mirroring include/
tests/            test binaries, built by `make run-tests`
scripts/          code generators (the bitmap font is generated)
```

## Conventions

- `Af` for types, `af_` for functions, `AF_` for constants and macros.
- Public functions take a pointer first and a plain value after.
- Returning a pointer means "owned by the thing you called it on"; returning
  `NULL` means failure and never leaves the caller to guess about cleanup.
- Textures are always ABGR8888. Use `af_tex_rgba8()` to build pixel words.
- 2x3 transforms are row-major and applied as `m * v`.

## License

MIT. See LICENSE.
