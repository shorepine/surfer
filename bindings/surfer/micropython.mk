# surfer user C module for the MicroPython unix port.
# Build from the surfer repo root with:  make mpy
# (that target builds build/libsurfer.a + generated asset headers first,
#  then invokes ports/unix with USER_C_MODULES=<repo>/bindings)

SURFER_MOD_DIR := $(USERMOD_DIR)
SURFER_DIR ?= $(abspath $(SURFER_MOD_DIR)/../..)

SRC_USERMOD_C += $(SURFER_MOD_DIR)/modsurfer.c
SRC_USERMOD_C += $(SURFER_MOD_DIR)/port_sdl.c

CFLAGS_USERMOD += -I$(SURFER_DIR)/include \
                  -I$(SURFER_DIR)/src/hal/sdl \
                  -I$(SURFER_DIR)/build/gen

ifeq ($(SURFER_WEB),1)
# webassembly port (make mpy-web; SURFER_WEB comes from the web variant):
# emscripten's SDL2 replaces sdl2-config, and surfer itself comes
# emcc-compiled as libsurfer-web.a. The port builds C99 -Werror; surfer
# is gnu11 and warns differently — relax both for these files only.
SRC_USERMOD_C += $(SURFER_MOD_DIR)/web/surfer_web_run.c
SRC_USERMOD_C += $(SURFER_MOD_DIR)/web/hal_sdl_web.c
CFLAGS_USERMOD += -sUSE_SDL=2 -std=gnu11 -Wno-error
# not LDFLAGS_USERMOD: the port links `emcc $(LDFLAGS) $(OBJ) $(JSFLAGS)`,
# and an archive must come after the objects that pull symbols from it
JSFLAGS += $(SURFER_DIR)/build/libsurfer-web.a
else
CFLAGS_USERMOD += $(shell sdl2-config --cflags)
LDFLAGS_USERMOD += $(SURFER_DIR)/build/libsurfer.a $(shell sdl2-config --libs)
endif
