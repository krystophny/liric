#include "../src/arena.h"
#include "../src/ir.h"
#include "../src/target_shared.h"
#include <stdio.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s: got %lld, expected %lld (line %d)\n", \
                msg, _a, _b, __LINE__); \
        return 1; \
    } \
} while (0)

typedef struct {
    uint32_t count;
    uint32_t dests[8];
} prescan_capture_t;

static int32_t capture_static_alloca(void *ctx, const lr_inst_t *inst) {
    prescan_capture_t *capture = (prescan_capture_t *)ctx;
    if (capture->count < 8) {
        capture->dests[capture->count] = inst->dest;
    }
    capture->count++;
    return -(int32_t)capture->count;
}

int test_target_shared_static_alloca_table(void) {
    lr_arena_t *arena = lr_arena_create(0);
    int32_t *offsets = NULL;
    uint32_t num_offsets = 0;

    TEST_ASSERT_EQ(lr_target_lookup_static_alloca_offset(offsets, num_offsets, 0),
                   0, "missing entry defaults to zero");

    lr_target_set_static_alloca_offset(arena, &offsets, &num_offsets, 70, -144);
    TEST_ASSERT(num_offsets > 70, "table grows to requested vreg");
    TEST_ASSERT_EQ(lr_target_lookup_static_alloca_offset(offsets, num_offsets, 70),
                   -144, "stored offset is retrievable");
    TEST_ASSERT_EQ(lr_target_lookup_static_alloca_offset(offsets, num_offsets, 69),
                   0, "untouched entries stay zero");

    lr_target_set_static_alloca_offset(arena, &offsets, &num_offsets, 2, -32);
    TEST_ASSERT_EQ(lr_target_lookup_static_alloca_offset(offsets, num_offsets, 2),
                   -32, "smaller vreg can be set after growth");

    lr_target_set_static_alloca_offset(arena, &offsets, &num_offsets, 70, -256);
    TEST_ASSERT_EQ(lr_target_lookup_static_alloca_offset(offsets, num_offsets, 70),
                   -256, "existing vreg offset can be updated");

    lr_arena_destroy(arena);
    return 0;
}

int test_target_shared_prescan_filters_dynamic_alloca(void) {
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *mod = lr_module_create(arena);
    lr_func_t *func = lr_func_create(mod, "f", mod->type_void, NULL, 0, false);
    lr_block_t *entry = lr_block_create(func, arena, "entry");
    lr_block_t *next = lr_block_create(func, arena, "next");
    prescan_capture_t capture = {0};

    lr_operand_t one = lr_op_imm_i64(1, mod->type_i64);
    lr_operand_t two = lr_op_imm_i64(2, mod->type_i64);
    lr_operand_t dyn_n = lr_op_vreg(lr_vreg_new(func), mod->type_i64);

    uint32_t static_dest0 = lr_vreg_new(func);
    uint32_t dynamic_dest0 = lr_vreg_new(func);
    uint32_t static_dest1 = lr_vreg_new(func);
    uint32_t dynamic_dest1 = lr_vreg_new(func);
    uint32_t static_dest2 = lr_vreg_new(func);

    lr_block_append(entry, lr_inst_create(arena, LR_OP_ALLOCA, mod->type_i64,
                                          static_dest0, NULL, 0));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_ALLOCA, mod->type_i64,
                                          dynamic_dest0, &two, 1));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_ALLOCA, mod->type_i64,
                                          static_dest1, &one, 1));
    lr_block_append(next, lr_inst_create(arena, LR_OP_ALLOCA, mod->type_i64,
                                         dynamic_dest1, &dyn_n, 1));
    lr_block_append(next, lr_inst_create(arena, LR_OP_ALLOCA, mod->type_i64,
                                         static_dest2, &one, 1));

    lr_target_prescan_static_alloca_offsets(func, &capture, capture_static_alloca);

    TEST_ASSERT_EQ(capture.count, 3, "only static allocas are prescanned");
    TEST_ASSERT_EQ(capture.dests[0], static_dest0, "first static alloca visited");
    TEST_ASSERT_EQ(capture.dests[1], static_dest1, "second static alloca visited");
    TEST_ASSERT_EQ(capture.dests[2], static_dest2, "third static alloca visited");

    lr_arena_destroy(arena);
    return 0;
}
