/* Web entry point: exec a python string from JS, synchronously. The
 * port's mp_js_do_exec additionally converts its result through the JS
 * proxy layer, which we don't initialize — tulip mode lives on the
 * canvas, not in JS objects. Participates in the port's deferred-GC
 * protocol: with MICROPY_GC_SPLIT_HEAP_AUTO the collector only sets a
 * pending flag, and the real collection runs when the external call
 * depth returns to zero (no wasm frames to scan — see the port's
 * main.c). */
#ifdef __EMSCRIPTEN__

#include <string.h>

#include <emscripten.h>

#include "py/compile.h"
#include "py/runtime.h"

// from the port's main.c
void external_call_depth_inc(void);
void external_call_depth_dec(void);

EMSCRIPTEN_KEEPALIVE void surfer_web_run(const char *src)
{
    external_call_depth_inc();
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(
            MP_QSTR__lt_stdin_gt_, src, strlen(src), 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t module_fun = mp_compile(&parse_tree, source_name, false);
        mp_call_function_0(module_fun);
        nlr_pop();
    } else {
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
    }
    external_call_depth_dec();
}

#endif /* __EMSCRIPTEN__ */
