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

static uint32_t count_block_opcode(const lr_block_t *block, lr_opcode_t op) {
    uint32_t count = 0;
    if (!block || !block->inst_array)
        return 0;
    for (uint32_t i = 0; i < block->num_insts; i++) {
        if (block->inst_array[i]->op == op)
            count++;
    }
    return count;
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
    lr_type_t *params[1] = { mod->type_i32 };
    lr_func_t *func = lr_func_create(mod, "f", mod->type_i32, params, 1, false);
    lr_block_t *entry = lr_block_create(func, arena, "entry");
    lr_block_t *exit = lr_block_create(func, arena, "exit");

    lr_operand_t add_ops[2] = {
        lr_op_vreg(func->param_vregs[0], mod->type_i32),
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
    TEST_ASSERT(func->linear_inst_array == NULL, "linear inst array starts null");
    TEST_ASSERT(func->block_inst_offsets == NULL, "block offsets start null");
    TEST_ASSERT(entry->inst_array == NULL, "inst array starts null");
    TEST_ASSERT(!lr_func_is_finalized(func), "fresh function is not finalized");

    lr_block_append(entry, add_inst);
    lr_block_append(entry, br_inst);
    lr_block_append(exit, ret_inst);

    TEST_ASSERT_EQ(lr_func_finalize(func, arena), 0, "finalize succeeds");
    TEST_ASSERT(lr_func_is_finalized(func), "function reports finalized after finalize");
    TEST_ASSERT(func->block_array != NULL, "block array populated");
    TEST_ASSERT(func->block_array[entry->id] == entry, "entry indexed by block id");
    TEST_ASSERT(func->block_array[exit->id] == exit, "exit indexed by block id");
    TEST_ASSERT_EQ(entry->num_insts, 2, "entry has two instructions");
    TEST_ASSERT(entry->inst_array[0] == add_inst, "entry[0] points to first inst");
    TEST_ASSERT(entry->inst_array[1] == br_inst, "entry[1] points to second inst");
    TEST_ASSERT_EQ(exit->num_insts, 1, "exit has one instruction");
    TEST_ASSERT(exit->inst_array[0] == ret_inst, "exit[0] points to ret inst");
    TEST_ASSERT(func->linear_inst_array != NULL, "linear inst array populated");
    TEST_ASSERT(func->block_inst_offsets != NULL, "block offsets populated");
    TEST_ASSERT_EQ(func->num_linear_insts, 3, "linear inst array has three entries");
    TEST_ASSERT(func->linear_inst_array[0] == add_inst, "linear[0] points to add");
    TEST_ASSERT(func->linear_inst_array[1] == br_inst, "linear[1] points to br");
    TEST_ASSERT(func->linear_inst_array[2] == ret_inst, "linear[2] points to ret");
    TEST_ASSERT_EQ(func->block_inst_offsets[entry->id], 0, "entry starts at linear index 0");
    TEST_ASSERT_EQ(func->block_inst_offsets[exit->id], 2, "exit starts at linear index 2");
    TEST_ASSERT_EQ(func->block_inst_offsets[func->num_blocks], 3, "linear sentinel matches count");

    lr_operand_t tail_ops[1] = { lr_op_imm_i64(0, mod->type_i32) };
    lr_inst_t *tail_ret = lr_inst_create(arena, LR_OP_RET, mod->type_i32, 0, tail_ops, 1);
    lr_block_append(exit, tail_ret);
    TEST_ASSERT(exit->inst_array == NULL, "append invalidates inst array");
    TEST_ASSERT_EQ(exit->num_insts, 0, "append resets cached inst count");
    TEST_ASSERT(func->linear_inst_array == NULL, "append invalidates linear inst array");
    TEST_ASSERT(func->block_inst_offsets == NULL, "append invalidates block offsets");
    TEST_ASSERT_EQ(func->num_linear_insts, 0, "append resets linear inst count");
    TEST_ASSERT(!lr_func_is_finalized(func), "append invalidates finalized state");

    TEST_ASSERT_EQ(lr_func_finalize(func, arena), 0, "re-finalize succeeds");
    TEST_ASSERT(lr_func_is_finalized(func), "re-finalize restores finalized state");
    TEST_ASSERT_EQ(exit->num_insts, 2, "re-finalize updates instruction count");
    TEST_ASSERT(exit->inst_array[1] == tail_ret, "new instruction appears in rebuilt cache");
    TEST_ASSERT_EQ(func->num_linear_insts, 4, "re-finalize updates linear inst count");
    TEST_ASSERT(func->linear_inst_array[3] == tail_ret, "new instruction appears in linear cache");
    TEST_ASSERT_EQ(func->block_inst_offsets[func->num_blocks], 4, "linear sentinel updates");

    lr_block_t *tail = lr_block_create(func, arena, "tail");
    TEST_ASSERT(tail != NULL, "tail block is created");
    TEST_ASSERT(func->block_array == NULL, "new block invalidates block array");
    TEST_ASSERT(func->linear_inst_array == NULL, "new block invalidates linear inst array");
    TEST_ASSERT(func->block_inst_offsets == NULL, "new block invalidates block offsets");
    TEST_ASSERT(!lr_func_is_finalized(func), "new block invalidates finalized state");
    TEST_ASSERT_EQ(lr_func_finalize(func, arena), 0, "finalize rebuilds block array");
    TEST_ASSERT(lr_func_is_finalized(func), "function is finalized after rebuild");
    TEST_ASSERT(func->block_array != NULL, "rebuilt block array is present");
    TEST_ASSERT_EQ(func->num_blocks, 3, "function now has three blocks");
    TEST_ASSERT_EQ(func->num_linear_insts, 4, "empty block does not change linear inst count");
    TEST_ASSERT_EQ(func->block_inst_offsets[tail->id], 4, "empty block starts at final linear index");
    TEST_ASSERT_EQ(func->block_inst_offsets[func->num_blocks], 4, "linear sentinel stays unchanged");

    lr_arena_destroy(arena);
    return 0;
}

int test_ir_finalize_peephole_constant_identity_and_branch(void) {
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *mod = lr_module_create(arena);
    lr_func_t *func = lr_func_create(mod, "peephole_fold", mod->type_i32, NULL, 0, false);
    lr_block_t *entry = lr_block_create(func, arena, "entry");
    lr_block_t *thenb = lr_block_create(func, arena, "then");
    lr_block_t *elseb = lr_block_create(func, arena, "else");

    uint32_t v0 = lr_vreg_new(func);
    uint32_t v1 = lr_vreg_new(func);
    uint32_t v2 = lr_vreg_new(func);
    uint32_t v3 = lr_vreg_new(func);
    uint32_t dead = lr_vreg_new(func);

    lr_operand_t add_ops[2] = {
        lr_op_imm_i64(4, mod->type_i32),
        lr_op_imm_i64(5, mod->type_i32),
    };
    lr_operand_t add_zero_ops[2] = {
        lr_op_vreg(v0, mod->type_i32),
        lr_op_imm_i64(0, mod->type_i32),
    };
    lr_operand_t mul_one_ops[2] = {
        lr_op_vreg(v1, mod->type_i32),
        lr_op_imm_i64(1, mod->type_i32),
    };
    lr_operand_t add_one_ops[2] = {
        lr_op_vreg(v2, mod->type_i32),
        lr_op_imm_i64(1, mod->type_i32),
    };
    lr_operand_t dead_ops[2] = {
        lr_op_imm_i64(7, mod->type_i32),
        lr_op_imm_i64(8, mod->type_i32),
    };
    lr_operand_t condbr_ops[3] = {
        lr_op_imm_i64(1, mod->type_i1),
        lr_op_block(thenb->id),
        lr_op_block(elseb->id),
    };
    lr_operand_t ret_then_ops[1] = { lr_op_vreg(v3, mod->type_i32) };
    lr_operand_t ret_else_ops[1] = { lr_op_imm_i64(0, mod->type_i32) };

    lr_block_append(entry, lr_inst_create(arena, LR_OP_ADD, mod->type_i32, v0, add_ops, 2));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_ADD, mod->type_i32, v1, add_zero_ops, 2));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_MUL, mod->type_i32, v2, mul_one_ops, 2));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_ADD, mod->type_i32, v3, add_one_ops, 2));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_ADD, mod->type_i32, dead, dead_ops, 2));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_CONDBR, mod->type_void, 0, condbr_ops, 3));
    lr_block_append(thenb, lr_inst_create(arena, LR_OP_RET, mod->type_i32, 0, ret_then_ops, 1));
    lr_block_append(elseb, lr_inst_create(arena, LR_OP_RET, mod->type_i32, 0, ret_else_ops, 1));

    TEST_ASSERT_EQ(lr_func_finalize(func, arena), 0, "finalize succeeds");
    TEST_ASSERT_EQ(entry->num_insts, 1, "entry arithmetic chain is eliminated");
    TEST_ASSERT_EQ(entry->inst_array[0]->op, LR_OP_BR, "constant condbr is simplified to br");
    TEST_ASSERT_EQ(entry->inst_array[0]->operands[0].kind, LR_VAL_BLOCK,
                   "simplified branch keeps block target");
    TEST_ASSERT_EQ(entry->inst_array[0]->operands[0].block_id, thenb->id,
                   "simplified branch targets true block");
    TEST_ASSERT(thenb->inst_array != NULL, "then block cache is present");
    TEST_ASSERT_EQ(thenb->num_insts, 1, "then block keeps return");
    TEST_ASSERT_EQ(thenb->inst_array[0]->operands[0].kind, LR_VAL_IMM_I64,
                   "ret operand is folded to immediate");
    TEST_ASSERT_EQ(thenb->inst_array[0]->operands[0].imm_i64, 10,
                   "constant chain folds to final value");
    TEST_ASSERT_EQ(func->num_linear_insts, 3, "only one branch and two returns remain");

    lr_arena_destroy(arena);
    return 0;
}

int test_ir_finalize_redundant_load_elimination(void) {
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *mod = lr_module_create(arena);
    lr_func_t *func = lr_func_create(mod, "redundant_load", mod->type_i32, NULL, 0, false);
    lr_block_t *entry = lr_block_create(func, arena, "entry");

    uint32_t ptr = lr_vreg_new(func);
    uint32_t load0 = lr_vreg_new(func);
    uint32_t load1 = lr_vreg_new(func);
    uint32_t sum = lr_vreg_new(func);

    lr_operand_t store_ops[2] = {
        lr_op_imm_i64(7, mod->type_i32),
        lr_op_vreg(ptr, mod->type_ptr),
    };
    lr_operand_t load_ptr_ops[1] = { lr_op_vreg(ptr, mod->type_ptr) };
    lr_operand_t add_ops[2] = {
        lr_op_vreg(load0, mod->type_i32),
        lr_op_vreg(load1, mod->type_i32),
    };
    lr_operand_t ret_ops[1] = { lr_op_vreg(sum, mod->type_i32) };

    lr_block_append(entry, lr_inst_create(arena, LR_OP_ALLOCA, mod->type_i32, ptr, NULL, 0));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_STORE, mod->type_void, 0, store_ops, 2));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_LOAD, mod->type_i32, load0, load_ptr_ops, 1));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_LOAD, mod->type_i32, load1, load_ptr_ops, 1));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_ADD, mod->type_i32, sum, add_ops, 2));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_RET, mod->type_i32, 0, ret_ops, 1));

    TEST_ASSERT_EQ(lr_func_finalize(func, arena), 0, "finalize succeeds");
    TEST_ASSERT_EQ(count_block_opcode(entry, LR_OP_LOAD), 1,
                   "second load from same address is eliminated");
    TEST_ASSERT_EQ(entry->inst_array[3]->op, LR_OP_ADD, "add stays in expected slot");
    TEST_ASSERT_EQ(entry->inst_array[3]->operands[0].kind, LR_VAL_VREG, "add lhs remains vreg");
    TEST_ASSERT_EQ(entry->inst_array[3]->operands[1].kind, LR_VAL_VREG, "add rhs remains vreg");
    TEST_ASSERT_EQ(entry->inst_array[3]->operands[0].vreg, load0,
                   "add lhs uses first load result");
    TEST_ASSERT_EQ(entry->inst_array[3]->operands[1].vreg, load0,
                   "add rhs reuses first load result");

    lr_arena_destroy(arena);
    return 0;
}

int test_ir_finalize_redundant_load_kept_after_store(void) {
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *mod = lr_module_create(arena);
    lr_func_t *func = lr_func_create(mod, "redundant_load_store_barrier",
                                     mod->type_i32, NULL, 0, false);
    lr_block_t *entry = lr_block_create(func, arena, "entry");

    uint32_t ptr = lr_vreg_new(func);
    uint32_t load0 = lr_vreg_new(func);
    uint32_t load1 = lr_vreg_new(func);
    uint32_t sum = lr_vreg_new(func);

    lr_operand_t store0_ops[2] = {
        lr_op_imm_i64(7, mod->type_i32),
        lr_op_vreg(ptr, mod->type_ptr),
    };
    lr_operand_t store1_ops[2] = {
        lr_op_imm_i64(8, mod->type_i32),
        lr_op_vreg(ptr, mod->type_ptr),
    };
    lr_operand_t load_ptr_ops[1] = { lr_op_vreg(ptr, mod->type_ptr) };
    lr_operand_t add_ops[2] = {
        lr_op_vreg(load0, mod->type_i32),
        lr_op_vreg(load1, mod->type_i32),
    };
    lr_operand_t ret_ops[1] = { lr_op_vreg(sum, mod->type_i32) };

    lr_block_append(entry, lr_inst_create(arena, LR_OP_ALLOCA, mod->type_i32, ptr, NULL, 0));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_STORE, mod->type_void, 0, store0_ops, 2));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_LOAD, mod->type_i32, load0, load_ptr_ops, 1));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_STORE, mod->type_void, 0, store1_ops, 2));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_LOAD, mod->type_i32, load1, load_ptr_ops, 1));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_ADD, mod->type_i32, sum, add_ops, 2));
    lr_block_append(entry, lr_inst_create(arena, LR_OP_RET, mod->type_i32, 0, ret_ops, 1));

    TEST_ASSERT_EQ(lr_func_finalize(func, arena), 0, "finalize succeeds");
    TEST_ASSERT_EQ(count_block_opcode(entry, LR_OP_LOAD), 2,
                   "store invalidates load cache and keeps second load");
    TEST_ASSERT_EQ(entry->inst_array[5]->op, LR_OP_ADD, "add stays in expected slot");
    TEST_ASSERT_EQ(entry->inst_array[5]->operands[0].vreg, load0,
                   "add lhs remains first load");
    TEST_ASSERT_EQ(entry->inst_array[5]->operands[1].vreg, load1,
                   "add rhs keeps second load");

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
    lr_type_t *params[1] = { mod->type_i1 };
    lr_func_t *func = lr_func_create(mod, "phi_copies", mod->type_i32, params, 1, false);
    lr_block_t *entry = lr_block_create(func, arena, "entry");
    lr_block_t *left = lr_block_create(func, arena, "left");
    lr_block_t *right = lr_block_create(func, arena, "right");
    lr_block_t *merge = lr_block_create(func, arena, "merge");

    lr_operand_t condbr_ops[3] = {
        lr_op_vreg(func->param_vregs[0], mod->type_i1),
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

    uint32_t phi_sum_dest = lr_vreg_new(func);
    lr_operand_t phi_sum_ops[2] = {
        lr_op_vreg(phi0_dest, mod->type_i32),
        lr_op_vreg(phi1_dest, mod->type_i32),
    };
    lr_block_append(merge, lr_inst_create(arena, LR_OP_ADD, mod->type_i32, phi_sum_dest, phi_sum_ops, 2));

    lr_operand_t ret_ops[1] = { lr_op_vreg(phi_sum_dest, mod->type_i32) };
    lr_block_append(merge, lr_inst_create(arena, LR_OP_RET, mod->type_i32, 0, ret_ops, 1));

    TEST_ASSERT(func->block_array == NULL, "phi copy build works without pre-finalize");

    lr_block_phi_copies_t *copies = lr_build_phi_copies(arena, func);
    TEST_ASSERT(copies != NULL, "phi copies built");
    TEST_ASSERT(func->block_array != NULL, "phi copy build finalizes block array");
    TEST_ASSERT(func->linear_inst_array != NULL, "phi copy build finalizes linear inst array");
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

    {
        uint32_t phi2_dest = lr_vreg_new(func);
        lr_operand_t phi2_ops[4] = {
            lr_op_imm_i64(13, mod->type_i32), lr_op_block(left->id),
            lr_op_imm_i64(23, mod->type_i32), lr_op_block(right->id),
        };
        lr_block_append(merge, lr_inst_create(arena, LR_OP_PHI, mod->type_i32, phi2_dest, phi2_ops, 4));
        TEST_ASSERT(func->linear_inst_array == NULL, "append invalidates linear cache before phi rebuild");

        lr_block_phi_copies_t *copies2 = lr_build_phi_copies(arena, func);
        TEST_ASSERT(copies2 != NULL, "phi copies rebuild after mutation");
        TEST_ASSERT_EQ(copies2[left->id].count, 2,
                       "unused phi remains eliminated after rebuild");
        TEST_ASSERT_EQ(copies2[right->id].count, 2,
                       "unused phi remains eliminated on right predecessor");
        TEST_ASSERT_EQ(copies2[left->id].copies[0].dest_vreg, phi1_dest,
                       "left copy order for live phis is preserved");
        TEST_ASSERT_EQ(copies2[right->id].copies[0].dest_vreg, phi1_dest,
                       "right copy order for live phis is preserved");
        (void)phi2_dest;
    }

    lr_arena_destroy(arena);
    return 0;
}
