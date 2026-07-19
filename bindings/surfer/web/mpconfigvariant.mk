# surfer "web" variant of micropython's webassembly port: tulip mode in a
# canvas. Build via `make mpy-web` from the surfer repo root — it needs
# build/libsurfer-web.a and the generated asset headers first.
# read by bindings/surfer/micropython.mk to pick the emscripten branch —
# it can't test $(CC): the port sets CC=emcc after including py.mk
SURFER_WEB := 1

JSFLAGS += -s ASYNCIFY -s USE_SDL=2 -s ALLOW_MEMORY_GROWTH
# The VM suspends (emscripten_sleep in surf_hal_sdl_pump) with the whole
# interpreter on the C stack; the default 4KB asyncify stack overflows.
JSFLAGS += -s ASYNCIFY_STACK_SIZE=65536

FROZEN_MANIFEST = $(VARIANT_DIR)/manifest.py
JSFLAGS += -s ASSERTIONS=2
