# ============================================================================
#  afternoodle engine  --  POSIX build (Linux x86_64, macOS arm64, BSD)
# ============================================================================
#  make              build the static library
#  make tests        build the test binary
#  make run-tests    build and run the tests (headless, no display needed)
#  make install      install headers and library into PREFIX
#  make clean
# ============================================================================
#  Dependencies: SDL2, SDL2_image, SDL2_ttf, SDL2_mixer
#    Debian/Ubuntu: sudo apt install libsdl2-dev libsdl2-image-dev \
#                              libsdl2-ttf-dev libsdl2-mixer-dev
#    Fedora:        sudo dnf install SDL2-devel SDL2_image-devel \
#                              SDL2_ttf-devel SDL2_mixer-devel
#    Arch:          sudo pacman -S sdl2 sdl2_image sdl2_ttf sdl2_mixer
#    macOS:         brew install sdl2 sdl2_image sdl2_ttf sdl2_mixer
#    Android:       pkg install sdl2 sdl2_image sdl2_ttf sdl2_mixer
# ============================================================================

NAME     = afndle
PREFIX  ?= /usr/local

CC      ?= cc
AR      ?= ar
INSTALL ?= install
BUILD   ?= build
LIB      = $(BUILD)/lib$(NAME).a
TEST_BIN = $(BUILD)/$(NAME)-tests

PKGS     = sdl2 SDL2_image SDL2_ttf SDL2_mixer
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs   $(PKGS) 2>/dev/null)
ifeq ($(strip $(PKG_LIBS)),)
  PKG_LIBS := -lSDL2 -lSDL2_image -lSDL2_ttf -lSDL2_mixer
endif

UNAME_S := $(shell uname -s)

OPT     ?= -O2
CFLAGS  ?= $(OPT) -g
CFLAGS  += -std=c11 -fno-strict-aliasing
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS  += -Iinclude -Isrc $(PKG_CFLAGS)
CFLAGS  += -DAFNDLE_BUILDING
LDFLAGS ?=

# macOS has no -lpthread and wants -framework for the window server
ifeq ($(UNAME_S),Darwin)
  LDLIBS := $(PKG_LIBS) -lm
else
  LDLIBS := $(PKG_LIBS) -lm -lpthread
endif

SRC_DIRS = src/core src/platform src/render src/ecs src/physics src/audio \
           src/script src/vsl src/app
CORE_SRC = $(foreach d,$(SRC_DIRS),$(wildcard $(d)/*.c))
CORE_OBJ = $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRC))

# The test binary forces the dummy video/audio drivers at startup, so CI and
# headless machines need no display server.
TEST_SRC = $(wildcard tests/*.c)
TEST_OBJ = $(patsubst %.c,$(BUILD)/%.o,$(TEST_SRC))

.PHONY: all lib tests run-tests install clean

all: lib

lib: $(LIB)

$(LIB): $(CORE_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  AR   $@"

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
	@echo "  CC   $<"

tests: $(TEST_BIN)

$(TEST_BIN): $(TEST_OBJ) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TEST_OBJ) $(LIB) -o $@ $(LDLIBS)
	@echo "  LINK $@"

# The engine looks for a system TTF and falls back to the built-in bitmap font,
# so these tests pass with no fonts installed. AFNDLE_FONT pins one if present.
AFNDLE_FONT ?= /system/fonts/Roboto-Regular.ttf
run-tests: $(TEST_BIN)
	@AFNDLE_FONT="$(AFNDLE_FONT)" $(TEST_BIN)

install: $(LIB)
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 644 $(LIB) $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include/afndle
	@for d in core platform render ecs physics audio script vsl app; do \
	  if [ -d include/afndle/$$d ]; then \
	    $(INSTALL) -d $(DESTDIR)$(PREFIX)/include/afndle/$$d; \
	    $(INSTALL) -m 644 include/afndle/$$d/*.h $(DESTDIR)$(PREFIX)/include/afndle/$$d/; \
	  fi; \
	done
	@echo "  INSTALL $(DESTDIR)$(PREFIX)"

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
