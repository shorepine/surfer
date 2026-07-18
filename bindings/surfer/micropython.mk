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
                  -I$(SURFER_DIR)/build/gen \
                  $(shell sdl2-config --cflags)

LDFLAGS_USERMOD += $(SURFER_DIR)/build/libsurfer.a $(shell sdl2-config --libs)
