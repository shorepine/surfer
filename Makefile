# surfer — desktop build & test loop (see CLAUDE.md)

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Iinclude

CORE_SRCS   := $(wildcard src/core/*.c) $(wildcard src/text/*.c)
WIDGET_SRCS := $(wildcard src/widgets/*.c)
SDL_SRCS    := $(wildcard src/hal/sdl/*.c)
HDRS        := include/surfer.h src/core/surf_internal.h src/hal/sdl/hal_sdl.h

SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

GEN_DIR := build/gen

.PHONY: sdl test clean

sdl: build/surfer_demo build/surfer_settings build/surfer_type build/surfer_editor \
	build/surfer_bounce

test: build/surfer_test
	./build/surfer_test

$(GEN_DIR)/bounce_assets.h: tools/gen_demo_assets.py
	@mkdir -p $(GEN_DIR)
	python3 tools/gen_demo_assets.py > $@

$(GEN_DIR)/widget_assets.h: tools/gen_widget_assets.py
	@mkdir -p $(GEN_DIR)
	python3 tools/gen_widget_assets.py > $@

build/tools/fontbake: tools/fontbake.c tools/stb/stb_truetype.h
	@mkdir -p build/tools
	$(CC) -O2 -Itools -o $@ tools/fontbake.c -lm

$(GEN_DIR)/font_ui16.h: build/tools/fontbake assets/fonts/Roboto-Regular.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake ui16 16 assets/fonts/Roboto-Regular.ttf $@

$(GEN_DIR)/font_ui28.h: build/tools/fontbake assets/fonts/Roboto-Regular.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake ui28 28 assets/fonts/Roboto-Regular.ttf $@

$(GEN_DIR)/font_mono16.h: build/tools/fontbake assets/fonts/JetBrainsMono-Regular.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake mono16 16 assets/fonts/JetBrainsMono-Regular.ttf $@ \
		"32-126,167,181,8211,8212,8230"

# the "desktop demo" tracks the current milestone: mixer (M1) + text labels (M3)
build/surfer_demo: $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/mixer.c \
		$(GEN_DIR)/widget_assets.h $(GEN_DIR)/font_ui16.h $(GEN_DIR)/font_ui28.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/mixer.c $(SDL_LIBS) -lm

build/surfer_type: $(CORE_SRCS) $(SDL_SRCS) demos/type.c \
		$(GEN_DIR)/font_ui16.h $(GEN_DIR)/font_ui28.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(SDL_SRCS) demos/type.c $(SDL_LIBS) -lm

build/surfer_settings: $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/settings.c \
		$(GEN_DIR)/widget_assets.h $(GEN_DIR)/font_ui16.h $(GEN_DIR)/font_ui28.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/settings.c $(SDL_LIBS) -lm

build/surfer_editor: $(CORE_SRCS) $(SDL_SRCS) demos/editor.c \
		$(GEN_DIR)/font_mono16.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(SDL_SRCS) demos/editor.c $(SDL_LIBS) -lm

build/surfer_bounce: $(CORE_SRCS) $(SDL_SRCS) demos/bounce.c \
		$(GEN_DIR)/bounce_assets.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(SDL_SRCS) demos/bounce.c $(SDL_LIBS)

TEST_SRCS := $(wildcard tests/*.c)

build/surfer_test: $(CORE_SRCS) $(WIDGET_SRCS) $(TEST_SRCS) tests/mock_hal.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc/core -Itests \
		-o $@ $(CORE_SRCS) $(WIDGET_SRCS) $(TEST_SRCS) -lm

# static lib + generated headers for the MicroPython binding
LIB_SRCS := $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS)
LIB_OBJS := $(patsubst %.c,build/obj/%.o,$(LIB_SRCS))

build/obj/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -c $< -o $@

build/libsurfer.a: $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

gen: $(GEN_DIR)/widget_assets.h $(GEN_DIR)/font_ui16.h $(GEN_DIR)/font_ui28.h \
	$(GEN_DIR)/font_mono16.h

MPY_DIR ?= $(HOME)/micropython

mpy: build/libsurfer.a gen
	@# MP only knows libsurfer.a as a linker flag, not a dependency —
	@# drop the binary so a changed lib always relinks
	rm -f $(MPY_DIR)/ports/unix/build-standard/micropython
	$(MAKE) -C $(MPY_DIR)/ports/unix USER_C_MODULES=$(abspath bindings) \
		SURFER_DIR=$(abspath .) \
		CFLAGS_EXTRA="-Wno-gnu-folding-constant"  # newer clang vs MP v1.26
	@echo "→ $(MPY_DIR)/ports/unix/build-standard/micropython"

# MicroPython esp32 port for the P4 (tulip mode on device).
# Needs micropython v1.28.x (has ESP32-P4 support) + IDF v5.5.1 (MP's
# recommended version; its P4 code expects 5.5 APIs). The surfer-native
# firmware in ports/esp32p4/ stays on 5.4.1 independently.
MPY_P4_DIR ?= $(HOME)/micropython-1.28
IDF_EXPORT ?= $(HOME)/esp/esp-idf-v5.5.1/export.sh

mpy-p4: gen
	cp bindings/surfer/boards/SURFER_P4/partitions-surfer-16MiB.csv \
		$(MPY_P4_DIR)/ports/esp32/
	bash -c "source $(IDF_EXPORT) >/dev/null 2>&1 && \
		$(MAKE) -C $(MPY_P4_DIR)/ports/esp32 \
		BOARD=SURFER_P4 \
		BOARD_DIR=$(abspath bindings/surfer/boards/SURFER_P4) \
		USER_C_MODULES=$(abspath bindings/surfer/micropython.cmake)"
	@echo "→ $(MPY_P4_DIR)/ports/esp32/build-SURFER_P4/  (make mpy-p4-flash PORT=...)"

mpy-p4-flash:
	bash -c "source $(IDF_EXPORT) >/dev/null 2>&1 && \
		$(MAKE) -C $(MPY_P4_DIR)/ports/esp32 \
		BOARD=SURFER_P4 \
		BOARD_DIR=$(abspath bindings/surfer/boards/SURFER_P4) \
		USER_C_MODULES=$(abspath bindings/surfer/micropython.cmake) \
		PORT=$(PORT) deploy"

.PHONY: gen mpy mpy-p4 mpy-p4-flash

clean:
	rm -rf build
