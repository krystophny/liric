#ifndef LIRIC_BC_DECODE_H
#define LIRIC_BC_DECODE_H

#include "arena.h"
#include "ir.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Recognizes raw LLVM bitcode and the LLVM bitcode wrapper container. */
bool lr_bc_is_bitcode(const uint8_t *data, size_t len);

/* Native LLVM bitcode parser — always available (no LLVM dependency). */
bool lr_bc_parser_available(void);

typedef int (*lr_bc_inst_callback_t)(lr_func_t *func, lr_block_t *block,
                                     const lr_inst_t *inst, void *ctx);

/* Parses LLVM bitcode while invoking `on_inst` for each decoded instruction. */
lr_module_t *lr_parse_bc_data_streaming(const uint8_t *data, size_t len,
                                        lr_arena_t *arena,
                                        lr_bc_inst_callback_t on_inst, void *ctx,
                                        char *err, size_t errlen);

/* Parses LLVM bitcode into an lr_module_t (native reader, zero dependencies). */
lr_module_t *lr_parse_bc_data(const uint8_t *data, size_t len,
                               lr_arena_t *arena, char *err, size_t errlen);

#endif
