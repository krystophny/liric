#ifndef LIRIC_WASM_TO_IR_H
#define LIRIC_WASM_TO_IR_H

#include "ir.h"
#include "wasm_decode.h"

lr_module_t *lr_wasm_to_ir(const lr_wasm_module_t *wmod,
                            lr_arena_t *arena, char *err, size_t errlen);

#endif
