# surfer "web" variant of micropython's webassembly port: repl mode in a
# canvas. Build via `make mpy-web` from the surfer repo root — it needs
# build/libsurfer-web.a and the generated asset headers first.
# read by bindings/surfer/micropython.mk to pick the emscripten branch —
# it can't test $(CC): the port sets CC=emcc after including py.mk
SURFER_WEB := 1

# NO ASYNCIFY, deliberately: everything in this build is synchronous
# (browser-driven frames, deferred GC, SDL sleeps disabled — see
# mpconfigvariant.h and hal_sdl_web.c). Without it, ASYNCIFY's failure
# modes (suspend inside import wedges the VM; inside a sync call it
# aborts) are structurally impossible instead of merely avoided, and
# the VM skips the unwind-bookkeeping overhead ASYNCIFY instruments
# into every function.
JSFLAGS += -s USE_SDL=2 -s ALLOW_MEMORY_GROWTH

FROZEN_MANIFEST = $(VARIANT_DIR)/manifest.py
