#ifndef LIRIC_BC_DECODE_H
#define LIRIC_BC_DECODE_H

#include "arena.h"
#include "ir.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct lr_session lr_session_t;

/* Recognizes raw LLVM bitcode and the LLVM bitcode wrapper container. */
bool lr_bc_is_bitcode(const uint8_t *data, size_t len);

/* Native LLVM bitcode parser — always available (no LLVM dependency). */
bool lr_bc_parser_available(void);

typedef struct lr_bc_inst_desc {
    lr_opcode_t op;
    lr_type_t *type;
    uint32_t dest;
    const lr_operand_desc_t *operands;
    uint32_t num_operands;
    const uint32_t *indices;
    uint32_t num_indices;
    int icmp_pred;
    int fcmp_pred;
    bool call_external_abi;
    bool call_vararg;
    uint32_t call_fixed_args;
} lr_bc_inst_desc_t;

typedef int (*lr_bc_stream_callback_t)(lr_func_t *func, lr_block_t *block,
                                       const lr_bc_inst_desc_t *inst, void *ctx);

/* Parses LLVM bitcode while invoking `on_inst` for each decoded instruction. */
lr_module_t *lr_parse_bc_streaming(const uint8_t *data, size_t len,
                                   lr_arena_t *arena,
                                   lr_bc_stream_callback_t on_inst, void *ctx,
                                   char *err, size_t errlen);

/* Parses LLVM bitcode into an lr_module_t (native reader, zero dependencies). */
lr_module_t *lr_parse_bc_with_arena(const uint8_t *data, size_t len,
                                    lr_arena_t *arena, char *err, size_t errlen);

/* Parses LLVM bitcode and streams through the session API. */
int lr_parse_bc_to_session(const uint8_t *data, size_t len,
                           lr_session_t *session,
                           char *err, size_t errlen);

#endif
