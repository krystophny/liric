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

    lr_target_prescan_static_alloca_offsets(func, arena, &capture, capture_static_alloca);

    TEST_ASSERT_EQ(capture.count, 3, "only static allocas are prescanned");
    TEST_ASSERT_EQ(capture.dests[0], static_dest0, "first static alloca visited");
    TEST_ASSERT_EQ(capture.dests[1], static_dest1, "second static alloca visited");
    TEST_ASSERT_EQ(capture.dests[2], static_dest2, "third static alloca visited");

    lr_arena_destroy(arena);
    return 0;
}

int test_ir_finalize_builds_dense_arrays(void) {
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *mod = lr_module_create(arena);
    lr_func_t *func = lr_func_create(mod, "f", mod->type_i32, NULL, 0, false);
    lr_block_t *entry = lr_block_create(func, arena, "entry");
    lr_block_t *exit = lr_block_create(func, arena, "exit");

    lr_operand_t add_ops[2] = {
        lr_op_imm_i64(4, mod->type_i32),
        lr_op_imm_i64(5, mod->type_i32)
    };
    uint32_t sum_vreg = lr_vreg_new(func);
    lr_inst_t *add_inst = lr_inst_create(arena, LR_OP_ADD, mod->type_i32, sum_vreg,
                                         add_ops, 2);
    lr_operand_t br_ops[1] = { lr_op_block(exit->id) };
    lr_inst_t *br_inst = lr_inst_create(arena, LR_OP_BR, mod->type_void, 0, br_ops, 1);
    lr_operand_t ret_ops[1] = { lr_op_vreg(sum_vreg, mod->type_i32) };
    lr_inst_t *ret_inst = lr_inst_create(arena, LR_OP_RET, mod->type_i32, 0, ret_ops, 1);

    TEST_ASSERT(func->block_array == NULL, "block array starts null");
    TEST_ASSERT(entry->inst_array == NULL, "inst array starts null");

    lr_block_append(entry, add_inst);
    lr_block_append(entry, br_inst);
    lr_block_append(exit, ret_inst);

    TEST_ASSERT_EQ(lr_func_finalize(func, arena), 0, "finalize succeeds");
    TEST_ASSERT(func->block_array != NULL, "block array populated");
    TEST_ASSERT(func->block_array[entry->id] == entry, "entry indexed by block id");
    TEST_ASSERT(func->block_array[exit->id] == exit, "exit indexed by block id");
    TEST_ASSERT_EQ(entry->num_insts, 2, "entry has two instructions");
    TEST_ASSERT(entry->inst_array[0] == add_inst, "entry[0] points to first inst");
    TEST_ASSERT(entry->inst_array[1] == br_inst, "entry[1] points to second inst");
    TEST_ASSERT_EQ(exit->num_insts, 1, "exit has one instruction");
    TEST_ASSERT(exit->inst_array[0] == ret_inst, "exit[0] points to ret inst");

    lr_operand_t tail_ops[1] = { lr_op_imm_i64(0, mod->type_i32) };
    lr_inst_t *tail_ret = lr_inst_create(arena, LR_OP_RET, mod->type_i32, 0, tail_ops, 1);
    lr_block_append(exit, tail_ret);
    TEST_ASSERT(exit->inst_array == NULL, "append invalidates inst array");
    TEST_ASSERT_EQ(exit->num_insts, 0, "append resets cached inst count");

    TEST_ASSERT_EQ(lr_func_finalize(func, arena), 0, "re-finalize succeeds");
    TEST_ASSERT_EQ(exit->num_insts, 2, "re-finalize updates instruction count");
    TEST_ASSERT(exit->inst_array[1] == tail_ret, "new instruction appears in rebuilt cache");

    (void)lr_block_create(func, arena, "tail");
    TEST_ASSERT(func->block_array == NULL, "new block invalidates block array");
    TEST_ASSERT_EQ(lr_func_finalize(func, arena), 0, "finalize rebuilds block array");
    TEST_ASSERT(func->block_array != NULL, "rebuilt block array is present");
    TEST_ASSERT_EQ(func->num_blocks, 3, "function now has three blocks");

    lr_arena_destroy(arena);
    return 0;
}

int test_ir_inst_create_packs_operands_in_single_allocation(void) {
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *mod = lr_module_create(arena);
    lr_operand_t ops[3] = {
        lr_op_imm_i64(10, mod->type_i64),
        lr_op_imm_i64(20, mod->type_i64),
        lr_op_imm_i64(30, mod->type_i64),
    };

    lr_inst_t *inst = lr_inst_create(arena, LR_OP_ADD, mod->type_i64, 7, ops, 3);
    TEST_ASSERT(inst != NULL, "instruction allocation succeeds");
    TEST_ASSERT(inst->operands != NULL, "operand storage is present");
    TEST_ASSERT_EQ(inst->num_operands, 3, "operand count preserved");
    TEST_ASSERT_EQ(inst->operands[0].imm_i64, 10, "operand[0] copied");
    TEST_ASSERT_EQ(inst->operands[1].imm_i64, 20, "operand[1] copied");
    TEST_ASSERT_EQ(inst->operands[2].imm_i64, 30, "operand[2] copied");

    ops[0].imm_i64 = 999;
    TEST_ASSERT_EQ(inst->operands[0].imm_i64, 10, "operands are not aliased to input array");

    {
        size_t expected_offset = (sizeof(lr_inst_t) + _Alignof(lr_operand_t) - 1u)
                                 & ~((size_t)_Alignof(lr_operand_t) - 1u);
        TEST_ASSERT((uint8_t *)inst->operands == (uint8_t *)inst + expected_offset,
                    "operands are packed immediately after instruction header");
    }

    lr_arena_destroy(arena);
    return 0;
}

int test_ir_phi_copies_flat_arrays_preserve_emission_order(void) {
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *mod = lr_module_create(arena);
    lr_func_t *func = lr_func_create(mod, "phi_copies", mod->type_i32, NULL, 0, false);
    lr_block_t *entry = lr_block_create(func, arena, "entry");
    lr_block_t *left = lr_block_create(func, arena, "left");
    lr_block_t *right = lr_block_create(func, arena, "right");
    lr_block_t *merge = lr_block_create(func, arena, "merge");

    lr_operand_t condbr_ops[3] = {
        lr_op_imm_i64(1, mod->type_i1),
        lr_op_block(left->id),
        lr_op_block(right->id),
    };
    lr_block_append(entry, lr_inst_create(arena, LR_OP_CONDBR, mod->type_void, 0, condbr_ops, 3));

    lr_operand_t left_br_ops[1] = { lr_op_block(merge->id) };
    lr_block_append(left, lr_inst_create(arena, LR_OP_BR, mod->type_void, 0, left_br_ops, 1));

    lr_operand_t right_br_ops[1] = { lr_op_block(merge->id) };
    lr_block_append(right, lr_inst_create(arena, LR_OP_BR, mod->type_void, 0, right_br_ops, 1));

    uint32_t phi0_dest = lr_vreg_new(func);
    lr_operand_t phi0_ops[4] = {
        lr_op_imm_i64(11, mod->type_i32), lr_op_block(left->id),
        lr_op_imm_i64(21, mod->type_i32), lr_op_block(right->id),
    };
    lr_block_append(merge, lr_inst_create(arena, LR_OP_PHI, mod->type_i32, phi0_dest, phi0_ops, 4));

    uint32_t phi1_dest = lr_vreg_new(func);
    lr_operand_t phi1_ops[4] = {
        lr_op_imm_i64(12, mod->type_i32), lr_op_block(left->id),
        lr_op_imm_i64(22, mod->type_i32), lr_op_block(right->id),
    };
    lr_block_append(merge, lr_inst_create(arena, LR_OP_PHI, mod->type_i32, phi1_dest, phi1_ops, 4));

    lr_operand_t ret_ops[1] = { lr_op_vreg(phi1_dest, mod->type_i32) };
    lr_block_append(merge, lr_inst_create(arena, LR_OP_RET, mod->type_i32, 0, ret_ops, 1));

    TEST_ASSERT_EQ(lr_func_finalize(func, arena), 0, "finalize succeeds");

    lr_block_phi_copies_t *copies = lr_build_phi_copies(arena, func);
    TEST_ASSERT(copies != NULL, "phi copies built");
    TEST_ASSERT_EQ(copies[entry->id].count, 0, "entry has no incoming phi copies");
    TEST_ASSERT_EQ(copies[merge->id].count, 0, "merge has no outgoing phi copies");
    TEST_ASSERT_EQ(copies[left->id].count, 2, "left predecessor has two phi copies");
    TEST_ASSERT_EQ(copies[right->id].count, 2, "right predecessor has two phi copies");

    TEST_ASSERT_EQ(copies[left->id].copies[0].dest_vreg, phi1_dest,
                   "left copy order matches previous linked-list emission");
    TEST_ASSERT_EQ(copies[left->id].copies[1].dest_vreg, phi0_dest,
                   "left copy second element matches previous order");
    TEST_ASSERT_EQ(copies[left->id].copies[0].src_op.kind, LR_VAL_IMM_I64,
                   "left first src is immediate");
    TEST_ASSERT_EQ(copies[left->id].copies[1].src_op.kind, LR_VAL_IMM_I64,
                   "left second src is immediate");
    TEST_ASSERT_EQ(copies[left->id].copies[0].src_op.imm_i64, 12,
                   "left first src value preserved");
    TEST_ASSERT_EQ(copies[left->id].copies[1].src_op.imm_i64, 11,
                   "left second src value preserved");

    TEST_ASSERT_EQ(copies[right->id].copies[0].dest_vreg, phi1_dest,
                   "right copy order matches previous linked-list emission");
    TEST_ASSERT_EQ(copies[right->id].copies[1].dest_vreg, phi0_dest,
                   "right copy second element matches previous order");
    TEST_ASSERT_EQ(copies[right->id].copies[0].src_op.kind, LR_VAL_IMM_I64,
                   "right first src is immediate");
    TEST_ASSERT_EQ(copies[right->id].copies[1].src_op.kind, LR_VAL_IMM_I64,
                   "right second src is immediate");
    TEST_ASSERT_EQ(copies[right->id].copies[0].src_op.imm_i64, 22,
                   "right first src value preserved");
    TEST_ASSERT_EQ(copies[right->id].copies[1].src_op.imm_i64, 21,
                   "right second src value preserved");

    lr_arena_destroy(arena);
    return 0;
}
