// surfer web variant.
#define MICROPY_VARIANT_ENABLE_JS_HOOK (1)

// The pyscript-style GC: defer collection to the top level (no wasm
// frames live, so no stack/register scan). The standard variant's
// gc_collect uses emscripten_scan_registers, an ASYNCIFY suspend — and
// a suspend inside a synchronous VM call aborts, while one inside
// import machinery wedges the VM. Everything here is driven by
// synchronous calls from the browser's rAF loop, so the GC must never
// suspend.
#define MICROPY_GC_SPLIT_HEAP      (1)
#define MICROPY_GC_SPLIT_HEAP_AUTO (1)
