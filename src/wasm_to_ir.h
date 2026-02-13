#ifndef LIRIC_WASM_TO_IR_H
#define LIRIC_WASM_TO_IR_H

#include "ir.h"
#include "wasm_decode.h"
#include <liric/liric_session.h>

lr_module_t *lr_wasm_build_module(const lr_wasm_module_t *wmod,
                                  lr_arena_t *arena,
                                  char *err, size_t errlen);

int lr_wasm_to_session(const lr_wasm_module_t *wmod,
                       lr_session_t *session,
                       void **out_last_addr,
                       lr_error_t *err);

#endif
