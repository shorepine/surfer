/* The sdl hal compiled as a usermod TU for the webassembly port, with
 * the ASYNCIFY frame yield compiled OUT: an ASYNCIFY suspend inside
 * MP's import machinery wedges the VM (`import tulip` never comes
 * back), so on the MP web build the browser drives frames from JS
 * (index.html's rAF loop → tulip.frame()) and every call into the VM
 * stays synchronous. Also keeps hal_sdl.c out of libsurfer-web.a for
 * this build — emscripten drops EM_JS bodies that arrive via a static
 * archive. */
#define SURF_HAL_SDL_NO_YIELD 1
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  /* posix_memalign under emscripten's strict libc */
#endif
#include "../../../src/hal/sdl/hal_sdl.c"
