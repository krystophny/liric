#ifndef LIRIC_WASM_TO_IR_H
#define LIRIC_WASM_TO_IR_H

#include "ir.h"
#include "wasm_decode.h"

typedef int (*lr_wasm_inst_callback_t)(lr_func_t *func, lr_block_t *block,
                                        const lr_inst_t *inst, void *ctx);

lr_module_t *lr_wasm_to_ir_streaming(const lr_wasm_module_t *wmod,
                                     lr_arena_t *arena,
                                     lr_wasm_inst_callback_t on_inst,
                                     void *ctx,
                                     char *err, size_t errlen);

lr_module_t *lr_wasm_to_ir(const lr_wasm_module_t *wmod,
                            lr_arena_t *arena, char *err, size_t errlen);

#endif
