#include "../src/ir.h"
#include <liric/liric.h>
#include <liric/liric_session.h>
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

int test_headers_share_opcode_and_operand_types(void) {
    lr_operand_desc_t lhs = LR_VREG(7, NULL);
    lr_operand_desc_t rhs = LR_IMM(9, NULL);
    lr_operand_desc_t ops[2] = { lhs, rhs };
    lr_operand_desc_t g = LR_GLOBAL(3, NULL);
    lr_phi_copy_desc_t phi_copy = { .dest_vreg = 11, .src_op = g };
    lr_inst_desc_t inst = {0};
    lr_op_t public_op = LR_OP_ADD;
    lr_opcode_t internal_op = (lr_opcode_t)public_op;
    lr_fcmp_pred_t pred = LR_FCMP_UEQ;

    inst.op = public_op;
    inst.operands = ops;
    inst.num_operands = 2;

    TEST_ASSERT_EQ(lhs.kind, LR_OP_KIND_VREG, "LR_VREG sets operand kind");
    TEST_ASSERT_EQ(rhs.kind, LR_OP_KIND_IMM_I64, "LR_IMM sets operand kind");
    TEST_ASSERT_EQ(lhs.global_offset, 0, "LR_VREG zero-initializes global_offset");
    TEST_ASSERT_EQ(rhs.global_offset, 0, "LR_IMM zero-initializes global_offset");
    TEST_ASSERT_EQ(g.global_offset, 0, "LR_GLOBAL zero-initializes global_offset");
    TEST_ASSERT_EQ(phi_copy.dest_vreg, 11, "phi copy desc stores destination vreg");
    TEST_ASSERT_EQ(phi_copy.src_op.kind, LR_OP_KIND_GLOBAL, "phi copy desc stores source operand");
    TEST_ASSERT_EQ(inst.op, LR_OP_ADD, "session instruction uses shared opcode enum");
    TEST_ASSERT_EQ(internal_op, LR_OP_ADD, "public lr_op_t aliases internal opcode enum");
    TEST_ASSERT(pred == LR_FCMP_UEQ, "floating predicate enum is shared");

    return 0;
}
