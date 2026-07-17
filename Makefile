# surfer — desktop build & test loop (see CLAUDE.md)

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Iinclude

CORE_SRCS := $(wildcard src/core/*.c)
SDL_SRCS  := $(wildcard src/hal/sdl/*.c)

SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

GEN_DIR := build/gen

.PHONY: sdl test clean

sdl: build/surfer_demo

test: build/surfer_test
	./build/surfer_test

$(GEN_DIR)/bounce_assets.h: tools/gen_demo_assets.py
	@mkdir -p $(GEN_DIR)
	python3 tools/gen_demo_assets.py > $@

build/surfer_demo: $(CORE_SRCS) $(SDL_SRCS) demos/bounce.c $(GEN_DIR)/bounce_assets.h \
		include/surfer.h src/core/surf_internal.h src/hal/sdl/hal_sdl.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(SDL_SRCS) demos/bounce.c $(SDL_LIBS)

build/surfer_test: $(CORE_SRCS) tests/test_core.c include/surfer.h src/core/surf_internal.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc/core -o $@ $(CORE_SRCS) tests/test_core.c

clean:
	rm -rf build
