#include "stencil_runtime.h"

#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

int test_stencil_runtime_lookup_known_entries(void) {
#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
    TEST_ASSERT(lr_stencil_lookup_for_ir(LR_OP_ADD, LR_TYPE_I32) != NULL,
                "add_i32 lookup");
    TEST_ASSERT(lr_stencil_lookup_for_ir(LR_OP_SUB, LR_TYPE_I64) != NULL,
                "sub_i64 lookup");
    TEST_ASSERT(lr_stencil_lookup_for_ir(LR_OP_FADD, LR_TYPE_DOUBLE) != NULL,
                "fadd_f64 lookup");
#else
    TEST_ASSERT(lr_stencil_lookup_for_ir(LR_OP_ADD, LR_TYPE_I32) == NULL,
                "no generated stencils on this platform");
#endif
    return 0;
}

int test_stencil_runtime_lookup_unknown_entry_returns_null(void) {
    TEST_ASSERT(lr_stencil_lookup_for_ir(LR_OP_MUL, LR_TYPE_I64) == NULL,
                "unsupported opcode lookup");
    TEST_ASSERT(lr_stencil_lookup_for_ir(LR_OP_ADD, LR_TYPE_I64) == NULL,
                "unsupported opcode/type pair lookup");
    return 0;
}

int test_stencil_runtime_emit_patches_all_holes(void) {
    uint8_t stencil_bytes[56];
    uint8_t out[80];
    uint8_t *cursor;
    uint16_t src1_u16;
    uint32_t dst_u32;
    int64_t imm_i64;
    int32_t branch_i32;
    uint64_t func_u64;
    uint64_t global_u64;
    lr_stencil_reloc_t relocs[] = {
        { 1, 1, LR_STENCIL_HOLE_SRC0_OFF },
        { 3, 2, LR_STENCIL_HOLE_SRC1_OFF },
        { 8, 4, LR_STENCIL_HOLE_DST_OFF },
        { 14, 8, LR_STENCIL_HOLE_IMM64 },
        { 24, 4, LR_STENCIL_HOLE_BRANCH_REL },
        { 28, 8, LR_STENCIL_HOLE_FUNC_ADDR },
        { 40, 8, LR_STENCIL_HOLE_GLOBAL_ADDR },
    };
    lr_stencil_t st = {
        "test_emit_holes",
        stencil_bytes,
        (uint16_t)sizeof(stencil_bytes),
        relocs,
        (uint8_t)(sizeof(relocs) / sizeof(relocs[0])),
    };
    lr_stencil_emit_args_t args = {
        .src0_off = -16,
        .src1_off = 0x1234,
        .dst_off = -32,
        .imm64 = 0x1122334455667788ll,
        .branch_rel = -12345,
        .func_addr = (uintptr_t)0x0102030405060708ull,
        .global_addr = (uintptr_t)0x1112131415161718ull,
    };

    memset(stencil_bytes, 0xCC, sizeof(stencil_bytes));
    memset(out, 0xAA, sizeof(out));
    cursor = out + 4;

    TEST_ASSERT(lr_stencil_emit(&cursor, out + sizeof(out), &st, &args, false) == 0,
                "emit succeeds");
    TEST_ASSERT((size_t)(cursor - (out + 4)) == sizeof(stencil_bytes),
                "emit size matches stencil size");

    TEST_ASSERT(out[5] == (uint8_t)args.src0_off, "src0 1-byte patch");
    memcpy(&src1_u16, out + 7, sizeof(src1_u16));
    TEST_ASSERT(src1_u16 == (uint16_t)args.src1_off, "src1 2-byte patch");
    memcpy(&dst_u32, out + 12, sizeof(dst_u32));
    TEST_ASSERT(dst_u32 == (uint32_t)args.dst_off, "dst 4-byte patch");
    memcpy(&imm_i64, out + 18, sizeof(imm_i64));
    TEST_ASSERT(imm_i64 == args.imm64, "imm64 8-byte patch");
    memcpy(&branch_i32, out + 28, sizeof(branch_i32));
    TEST_ASSERT(branch_i32 == args.branch_rel, "branch 4-byte patch");
    memcpy(&func_u64, out + 32, sizeof(func_u64));
    TEST_ASSERT(func_u64 == (uint64_t)args.func_addr, "func addr 8-byte patch");
    memcpy(&global_u64, out + 44, sizeof(global_u64));
    TEST_ASSERT(global_u64 == (uint64_t)args.global_addr, "global addr 8-byte patch");
    return 0;
}

int test_stencil_runtime_emit_strip_trailing_ret(void) {
    uint8_t stencil_bytes[] = { 0x90, 0x90, 0xC3 };
    lr_stencil_t st = {
        "strip_ret",
        stencil_bytes,
        (uint16_t)sizeof(stencil_bytes),
        NULL,
        0,
    };
    uint8_t out[8] = {0};
    uint8_t *cursor = out;

    TEST_ASSERT(lr_stencil_emit(&cursor, out + sizeof(out), &st, NULL, true) == 0,
                "emit with ret stripping");
    TEST_ASSERT((size_t)(cursor - out) == 2, "trailing ret removed");
    TEST_ASSERT(out[0] == 0x90 && out[1] == 0x90, "ret-free bytes emitted");
    return 0;
}

int test_stencil_runtime_emit_rejects_small_buffer(void) {
    uint8_t stencil_bytes[] = { 0x90, 0x90, 0x90, 0x90 };
    lr_stencil_t st = {
        "small_buffer",
        stencil_bytes,
        (uint16_t)sizeof(stencil_bytes),
        NULL,
        0,
    };
    uint8_t out[3] = {0};
    uint8_t *cursor = out;

    TEST_ASSERT(lr_stencil_emit(&cursor, out + sizeof(out), &st, NULL, false) != 0,
                "emit fails when buffer is too small");
    TEST_ASSERT(cursor == out, "cursor unchanged on failure");
    return 0;
}
