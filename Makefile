# surfer — desktop build & test loop (see CLAUDE.md)

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Iinclude

CORE_SRCS   := $(wildcard src/core/*.c)
WIDGET_SRCS := $(wildcard src/widgets/*.c)
SDL_SRCS    := $(wildcard src/hal/sdl/*.c)
HDRS        := include/surfer.h src/core/surf_internal.h src/hal/sdl/hal_sdl.h

SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

GEN_DIR := build/gen

.PHONY: sdl test clean

sdl: build/surfer_demo build/surfer_bounce

test: build/surfer_test
	./build/surfer_test

$(GEN_DIR)/bounce_assets.h: tools/gen_demo_assets.py
	@mkdir -p $(GEN_DIR)
	python3 tools/gen_demo_assets.py > $@

$(GEN_DIR)/widget_assets.h: tools/gen_widget_assets.py
	@mkdir -p $(GEN_DIR)
	python3 tools/gen_widget_assets.py > $@

# the "desktop demo" tracks the current milestone: M1 mixer
build/surfer_demo: $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/mixer.c \
		$(GEN_DIR)/widget_assets.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/mixer.c $(SDL_LIBS) -lm

build/surfer_bounce: $(CORE_SRCS) $(SDL_SRCS) demos/bounce.c \
		$(GEN_DIR)/bounce_assets.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(SDL_SRCS) demos/bounce.c $(SDL_LIBS)

build/surfer_test: $(CORE_SRCS) $(WIDGET_SRCS) tests/test_core.c tests/test_widgets.c \
		tests/mock_hal.c tests/mock_hal.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc/core -Itests \
		-o $@ $(CORE_SRCS) $(WIDGET_SRCS) tests/test_core.c tests/test_widgets.c \
		tests/mock_hal.c -lm

clean:
	rm -rf build
