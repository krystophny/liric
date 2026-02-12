#include "../src/arena.h"
#include "../src/ir.h"
#include "../src/liric.h"
#include "../src/jit.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static inline void fn_ptr_cast(void *dst, void *src) {
    memcpy(dst, &src, sizeof(src));
}

int test_merge_two_independent_functions(void) {
    lr_arena_t *a1 = lr_arena_create(0);
    lr_arena_t *a2 = lr_arena_create(0);
    lr_module_t *m1 = lr_module_create(a1);
    lr_module_t *m2 = lr_module_create(a2);

    lr_func_t *f1 = lr_func_create(m1, "func_a", m1->type_i32, NULL, 0,
                                   false);
    lr_block_t *b1 = lr_block_create(f1, a1, "entry");
    lr_operand_t ops1[1] = { lr_op_imm_i64(10, m1->type_i32) };
    lr_block_append(b1, lr_inst_create(a1, LR_OP_RET, m1->type_i32, 0,
                                       ops1, 1));

    lr_func_t *f2 = lr_func_create(m2, "func_b", m2->type_i32, NULL, 0,
                                   false);
    lr_block_t *b2 = lr_block_create(f2, a2, "entry");
    lr_operand_t ops2[1] = { lr_op_imm_i64(20, m2->type_i32) };
    lr_block_append(b2, lr_inst_create(a2, LR_OP_RET, m2->type_i32, 0,
                                       ops2, 1));

    int rc = lr_module_merge(m1, m2);
    TEST_ASSERT_EQ(rc, 0, "merge succeeds");

    int found_a = 0, found_b = 0;
    for (lr_func_t *f = m1->first_func; f; f = f->next) {
        if (strcmp(f->name, "func_a") == 0) found_a = 1;
        if (strcmp(f->name, "func_b") == 0) found_b = 1;
    }
    TEST_ASSERT(found_a, "func_a exists in dest after merge");
    TEST_ASSERT(found_b, "func_b exists in dest after merge");

    lr_arena_destroy(a2);
    lr_arena_destroy(a1);
    return 0;
}

int test_merge_declaration_replaced_by_definition(void) {
    lr_arena_t *ad = lr_arena_create(0);
    lr_arena_t *as = lr_arena_create(0);
    lr_module_t *dest = lr_module_create(ad);
    lr_module_t *src = lr_module_create(as);

    lr_func_declare(dest, "foo", dest->type_i32, NULL, 0, false);

    lr_func_t *sf = lr_func_create(src, "foo", src->type_i32, NULL, 0,
                                   false);
    lr_block_t *sb = lr_block_create(sf, as, "entry");
    lr_operand_t ops[1] = { lr_op_imm_i64(42, src->type_i32) };
    lr_block_append(sb, lr_inst_create(as, LR_OP_RET, src->type_i32, 0,
                                       ops, 1));

    int rc = lr_module_merge(dest, src);
    TEST_ASSERT_EQ(rc, 0, "merge succeeds");

    lr_func_t *df = NULL;
    int count = 0;
    for (lr_func_t *f = dest->first_func; f; f = f->next) {
        if (strcmp(f->name, "foo") == 0) {
            df = f;
            count++;
        }
    }
    TEST_ASSERT_EQ(count, 1, "exactly one foo in dest");
    TEST_ASSERT(df != NULL, "foo found");
    TEST_ASSERT(!df->is_decl, "foo is no longer a declaration");
    TEST_ASSERT(df->first_block != NULL, "foo has blocks");

    lr_inst_t *first_inst = df->first_block->first;
    TEST_ASSERT(first_inst != NULL, "foo has instructions");
    TEST_ASSERT_EQ(first_inst->op, LR_OP_RET, "first inst is ret");
    TEST_ASSERT_EQ(first_inst->operands[0].imm_i64, 42,
                   "ret value is 42");

    lr_arena_destroy(as);
    lr_arena_destroy(ad);
    return 0;
}

int test_merge_global_definition(void) {
    lr_arena_t *ad = lr_arena_create(0);
    lr_arena_t *as = lr_arena_create(0);
    lr_module_t *dest = lr_module_create(ad);
    lr_module_t *src = lr_module_create(as);

    lr_global_t *sg = lr_global_create(src, "my_global", src->type_i32,
                                       true);
    uint8_t init_data[4] = {0x2a, 0x00, 0x00, 0x00};
    sg->init_data = lr_arena_alloc(as, 4, 1);
    memcpy(sg->init_data, init_data, 4);
    sg->init_size = 4;

    int rc = lr_module_merge(dest, src);
    TEST_ASSERT_EQ(rc, 0, "merge succeeds");

    lr_global_t *dg = NULL;
    for (lr_global_t *g = dest->first_global; g; g = g->next) {
        if (strcmp(g->name, "my_global") == 0) {
            dg = g;
            break;
        }
    }
    TEST_ASSERT(dg != NULL, "my_global exists in dest");
    TEST_ASSERT(dg->is_const, "my_global is const");
    TEST_ASSERT_EQ(dg->init_size, 4, "init size is 4");
    TEST_ASSERT(memcmp(dg->init_data, init_data, 4) == 0,
                "init data matches");

    lr_arena_destroy(as);
    lr_arena_destroy(ad);
    return 0;
}

int test_merge_jit_runs_merged_function(void) {
    static const char *src_main =
        "declare i32 @helper(i32)\n"
        "define i32 @merged_main(i32 %0) {\n"
        "entry:\n"
        "  %1 = call i32 @helper(i32 %0)\n"
        "  ret i32 %1\n"
        "}\n";
    static const char *src_helper =
        "define i32 @helper(i32 %0) {\n"
        "entry:\n"
        "  %1 = add i32 %0, 100\n"
        "  ret i32 %1\n"
        "}\n";

    char err[256] = {0};
    lr_module_t *m_main = lr_parse_ll(src_main, strlen(src_main), err,
                                      sizeof(err));
    TEST_ASSERT(m_main != NULL, "parse main module");

    lr_module_t *m_helper = lr_parse_ll(src_helper, strlen(src_helper), err,
                                        sizeof(err));
    TEST_ASSERT(m_helper != NULL, "parse helper module");

    int rc = lr_module_merge(m_main, m_helper);
    TEST_ASSERT_EQ(rc, 0, "merge succeeds");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    lr_jit_begin_update(jit);
    rc = lr_jit_add_module(jit, m_main);
    TEST_ASSERT_EQ(rc, 0, "jit add module");
    lr_jit_end_update(jit);

    void *addr = lr_jit_get_function(jit, "merged_main");
    TEST_ASSERT(addr != NULL, "merged_main resolved");

    typedef int (*fn_t)(int);
    fn_t fn;
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(5), 105, "merged_main(5) == 105");
    TEST_ASSERT_EQ(fn(-10), 90, "merged_main(-10) == 90");

    lr_jit_destroy(jit);
    lr_module_free(m_helper);
    lr_module_free(m_main);
    return 0;
}
