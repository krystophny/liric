#include <liric/liric.h>
#include <liric/liric_compat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
#define GET_FN(fn_var, jit, name) \
    do { void *_p = lr_jit_get_function((jit), (name)); \
         fn_ptr_cast(&(fn_var), _p); } while (0)

/* Build: define i32 @f() { entry: ret i32 42 } */
int test_builder_ret_42(void) {
    lr_module_t *m = lr_module_create_new();
    TEST_ASSERT(m != NULL, "module create");

    lr_type_t *i32 = lr_type_i32_get(m);
    lr_func_t *f = lr_func_define(m, "f", i32, NULL, 0, false);
    TEST_ASSERT(f != NULL, "func define");

    lr_block_t *entry = lr_block_new(f, m, "entry");
    lr_build_ret(m, entry, LR_IMM(42, i32));

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; GET_FN(fn, jit, "f");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 42, "f() == 42");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

/* Build: define i32 @add(i32 %a, i32 %b) { entry: %c = add i32 %a, %b; ret i32 %c } */
int test_builder_add_args(void) {
    lr_module_t *m = lr_module_create_new();
    lr_type_t *i32 = lr_type_i32_get(m);
    lr_type_t *params[] = { i32, i32 };
    lr_func_t *f = lr_func_define(m, "add", i32, params, 2, false);

    uint32_t va = lr_func_param_vreg(f, 0);
    uint32_t vb = lr_func_param_vreg(f, 1);

    lr_block_t *entry = lr_block_new(f, m, "entry");
    uint32_t vc = lr_build_add(m, entry, f, i32, LR_VREG(va, i32), LR_VREG(vb, i32));
    lr_build_ret(m, entry, LR_VREG(vc, i32));

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; GET_FN(fn, jit, "add");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(10, 32), 42, "add(10,32) == 42");
    TEST_ASSERT_EQ(fn(-5, 5), 0, "add(-5,5) == 0");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

/* Build arithmetic chain: (a+b)*b - a */
int test_builder_arithmetic(void) {
    lr_module_t *m = lr_module_create_new();
    lr_type_t *i32 = lr_type_i32_get(m);
    lr_type_t *params[] = { i32, i32 };
    lr_func_t *f = lr_func_define(m, "arith", i32, params, 2, false);

    uint32_t va = lr_func_param_vreg(f, 0);
    uint32_t vb = lr_func_param_vreg(f, 1);

    lr_block_t *entry = lr_block_new(f, m, "entry");
    uint32_t sum = lr_build_add(m, entry, f, i32, LR_VREG(va, i32), LR_VREG(vb, i32));
    uint32_t prod = lr_build_mul(m, entry, f, i32, LR_VREG(sum, i32), LR_VREG(vb, i32));
    uint32_t diff = lr_build_sub(m, entry, f, i32, LR_VREG(prod, i32), LR_VREG(va, i32));
    lr_build_ret(m, entry, LR_VREG(diff, i32));

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; GET_FN(fn, jit, "arith");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(3, 4), 25, "arith(3,4) == 25");
    TEST_ASSERT_EQ(fn(10, 2), 14, "arith(10,2) == 14");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

/* Build: icmp sgt + conditional branch (max function) */
int test_builder_icmp_branch(void) {
    lr_module_t *m = lr_module_create_new();
    lr_type_t *i32 = lr_type_i32_get(m);
    lr_type_t *i1 = lr_type_i1_get(m);
    lr_type_t *params[] = { i32, i32 };
    lr_func_t *f = lr_func_define(m, "max", i32, params, 2, false);

    uint32_t va = lr_func_param_vreg(f, 0);
    uint32_t vb = lr_func_param_vreg(f, 1);

    lr_block_t *entry = lr_block_new(f, m, "entry");
    lr_block_t *then_bb = lr_block_new(f, m, "then");
    lr_block_t *else_bb = lr_block_new(f, m, "else");

    uint32_t cmp = lr_build_icmp(m, entry, f, LR_CMP_SGT,
                                  LR_VREG(va, i32), LR_VREG(vb, i32));
    lr_build_condbr(m, entry, LR_VREG(cmp, i1),
                     lr_block_id(then_bb), lr_block_id(else_bb));
    lr_build_ret(m, then_bb, LR_VREG(va, i32));
    lr_build_ret(m, else_bb, LR_VREG(vb, i32));

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; GET_FN(fn, jit, "max");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(10, 5), 10, "max(10,5) == 10");
    TEST_ASSERT_EQ(fn(3, 7), 7, "max(3,7) == 7");
    TEST_ASSERT_EQ(fn(4, 4), 4, "max(4,4) == 4");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

/* Build loop with phi: sum 1..10 = 55 */
int test_builder_loop_phi(void) {
    lr_module_t *m = lr_module_create_new();
    lr_type_t *i32 = lr_type_i32_get(m);
    lr_type_t *i1 = lr_type_i1_get(m);
    lr_func_t *f = lr_func_define(m, "sum10", i32, NULL, 0, false);

    lr_block_t *entry = lr_block_new(f, m, "entry");
    lr_block_t *loop = lr_block_new(f, m, "loop");
    lr_block_t *exit_bb = lr_block_new(f, m, "exit");

    uint32_t entry_id = lr_block_id(entry);
    uint32_t loop_id = lr_block_id(loop);
    uint32_t exit_id = lr_block_id(exit_bb);

    lr_build_br(m, entry, loop_id);

    /* PHI nodes for i and sum */
    lr_operand_desc_t i_vals[] = { LR_IMM(0, i32), LR_VREG(0, i32) };
    uint32_t i_blocks[] = { entry_id, loop_id };
    uint32_t vi = lr_build_phi(m, loop, f, i32, i_vals, i_blocks, 2);

    lr_operand_desc_t s_vals[] = { LR_IMM(0, i32), LR_VREG(0, i32) };
    uint32_t s_blocks[] = { entry_id, loop_id };
    uint32_t vs = lr_build_phi(m, loop, f, i32, s_vals, s_blocks, 2);

    uint32_t vnext = lr_build_add(m, loop, f, i32, LR_VREG(vi, i32), LR_IMM(1, i32));
    uint32_t vsum_next = lr_build_add(m, loop, f, i32, LR_VREG(vs, i32), LR_VREG(vnext, i32));

    /* Patch PHI incoming values: i_phi gets vnext from loop, s_phi gets vsum_next from loop.
       The PHI operands are interleaved [val, block, val, block, ...]. We need to
       update operand index 2 (the "from loop" value) for both PHIs.
       Since the builder API doesn't expose a PHI patch function, we set the placeholder
       vreg=0 above. We need a different approach. Let me use a forward vreg. */

    /* Actually, with SSA the PHI needs the vreg that will be produced later.
       The trick is that PHI values reference vregs that are defined in predecessor blocks.
       vnext and vsum_next are defined in the same block (loop), which IS the predecessor.
       So we need to know their vreg IDs before creating the PHIs.

       The clean approach: allocate vregs first, create PHIs with them, then build
       the instructions that define those vregs... but the builder assigns vregs automatically.

       Alternative: build the loop body first (without PHI), then add PHIs referencing the results.
       But PHIs must be at the beginning of the block.

       The real solution: PHIs reference the vreg numbers. Since we know the pattern, let's
       pre-allocate vregs and build in the right order. Actually, let's just re-create
       the block with proper ordering. */

    /* Simpler approach: use the alloc+manual pattern */
    lr_module_free(m);

    m = lr_module_create_new();
    i32 = lr_type_i32_get(m);
    i1 = lr_type_i1_get(m);
    f = lr_func_define(m, "sum10", i32, NULL, 0, false);

    entry = lr_block_new(f, m, "entry");
    loop = lr_block_new(f, m, "loop");
    exit_bb = lr_block_new(f, m, "exit");

    entry_id = lr_block_id(entry);
    loop_id = lr_block_id(loop);
    exit_id = lr_block_id(exit_bb);

    lr_build_br(m, entry, loop_id);

    /* Pre-allocate vregs for the loop body results */
    uint32_t vreg_next = lr_vreg_alloc(f);
    uint32_t vreg_sum_next = lr_vreg_alloc(f);

    /* Now build PHIs referencing the pre-allocated vregs */
    lr_operand_desc_t phi_i_vals[] = { LR_IMM(0, i32), LR_VREG(vreg_next, i32) };
    uint32_t phi_i_blocks[] = { entry_id, loop_id };
    vi = lr_build_phi(m, loop, f, i32, phi_i_vals, phi_i_blocks, 2);

    lr_operand_desc_t phi_s_vals[] = { LR_IMM(0, i32), LR_VREG(vreg_sum_next, i32) };
    uint32_t phi_s_blocks[] = { entry_id, loop_id };
    vs = lr_build_phi(m, loop, f, i32, phi_s_vals, phi_s_blocks, 2);

    /* Build loop body: next = i + 1, sum_next = sum + next */
    /* But wait - lr_build_add auto-allocates a new vreg. We need the result to be
       vreg_next and vreg_sum_next specifically. The builder API doesn't support this.
       We need to check if the auto-allocated vregs match what we pre-allocated.
       Since lr_vreg_alloc increments next_vreg, and lr_build_add also increments it,
       the vregs won't match.

       The correct approach is: lr_vreg_alloc should return the NEXT vreg that
       lr_build_* will assign. So we pre-allocate first, then the build functions
       should use those same IDs... but that's not how it works. Each build call
       allocates a fresh vreg.

       The real fix: build the instructions first (they get auto vregs), then build
       PHIs referencing those auto vregs, placing them at block start. But the builder
       API appends to block end.

       Let me just build the equivalent of the loop.ll test using a different structure. */

    lr_module_free(m);

    /* Take 3: build with correct SSA structure using the builder API.
       The trick is that PHI operands just reference vreg numbers. We can predict
       what vreg numbers the later instructions will get by checking the current
       next_vreg counter. After allocating param vregs and phi vregs, we know
       the next auto-assigned vreg number. */

    m = lr_module_create_new();
    i32 = lr_type_i32_get(m);
    i1 = lr_type_i1_get(m);
    f = lr_func_define(m, "sum10", i32, NULL, 0, false);

    entry = lr_block_new(f, m, "entry");
    loop = lr_block_new(f, m, "loop");
    exit_bb = lr_block_new(f, m, "exit");

    entry_id = lr_block_id(entry);
    loop_id = lr_block_id(loop);
    exit_id = lr_block_id(exit_bb);

    lr_build_br(m, entry, loop_id);

    /* The PHI nodes will consume vregs. After creating 2 PHIs, the add instructions
       will get the next sequential vregs. We can pre-compute them:
       - phi_i gets vreg N
       - phi_s gets vreg N+1
       - add (next) gets vreg N+2
       - add (sum_next) gets vreg N+3
       - icmp gets vreg N+4
       So phi_i references N+2 from loop, phi_s references N+3 from loop. */

    /* Build PHIs first with placeholder values, then the body. The placeholder
       vreg IDs need to be the actual IDs that the body instructions will produce.
       Since the function has 0 params, next_vreg starts at 0.
       phi_i = vreg 0, phi_s = vreg 1, next = vreg 2, sum_next = vreg 3, done = vreg 4 */

    lr_operand_desc_t phi_i_v[] = { LR_IMM(0, i32), LR_VREG(2, i32) };
    uint32_t phi_i_b[] = { entry_id, loop_id };
    vi = lr_build_phi(m, loop, f, i32, phi_i_v, phi_i_b, 2);

    lr_operand_desc_t phi_s_v[] = { LR_IMM(0, i32), LR_VREG(3, i32) };
    uint32_t phi_s_b[] = { entry_id, loop_id };
    vs = lr_build_phi(m, loop, f, i32, phi_s_v, phi_s_b, 2);

    vnext = lr_build_add(m, loop, f, i32, LR_VREG(vi, i32), LR_IMM(1, i32));
    vsum_next = lr_build_add(m, loop, f, i32, LR_VREG(vs, i32), LR_VREG(vnext, i32));

    uint32_t vdone = lr_build_icmp(m, loop, f, LR_CMP_EQ,
                                    LR_VREG(vnext, i32), LR_IMM(10, i32));
    lr_build_condbr(m, loop, LR_VREG(vdone, i1), exit_id, loop_id);

    lr_build_ret(m, exit_bb, LR_VREG(vsum_next, i32));

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; GET_FN(fn, jit, "sum10");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 55, "sum10() == 55");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

/* Build alloca/load/store pattern */
int test_builder_alloca_load_store(void) {
    lr_module_t *m = lr_module_create_new();
    lr_type_t *i32 = lr_type_i32_get(m);
    lr_type_t *ptr = lr_type_ptr_get(m);
    lr_func_t *f = lr_func_define(m, "als", i32, NULL, 0, false);

    lr_block_t *entry = lr_block_new(f, m, "entry");
    uint32_t slot = lr_build_alloca(m, entry, f, i32);
    lr_build_store(m, entry, LR_IMM(99, i32), LR_VREG(slot, ptr));
    uint32_t val = lr_build_load(m, entry, f, i32, LR_VREG(slot, ptr));
    lr_build_ret(m, entry, LR_VREG(val, i32));

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; GET_FN(fn, jit, "als");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 99, "als() == 99");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

/* Builder should canonicalize runtime GEP index operands to i64 at construction. */
int test_builder_gep_runtime_index_canonicalized_i64(void) {
    lr_module_t *m = lr_module_create_new();
    lr_type_t *vty = lr_type_void_get(m);
    lr_type_t *i32 = lr_type_i32_get(m);
    lr_type_t *ptr = lr_type_ptr_get(m);
    lr_type_t *params[] = { ptr, i32 };
    lr_func_t *f = lr_func_define(m, "g", vty, params, 2, false);
    TEST_ASSERT(f != NULL, "func define");

    uint32_t base = lr_func_param_vreg(f, 0);
    uint32_t idx = lr_func_param_vreg(f, 1);
    lr_block_t *entry = lr_block_new(f, m, "entry");
    lr_operand_desc_t indices[] = { LR_VREG(idx, i32) };

    (void)lr_build_gep(m, entry, f, i32, LR_VREG(base, ptr), indices, 1);
    lr_build_ret_void(m, entry);

    FILE *tmp = tmpfile();
    TEST_ASSERT(tmp != NULL, "tmpfile");
    lr_module_dump_to(m, tmp);
    fseek(tmp, 0, SEEK_END);
    long len = ftell(tmp);
    TEST_ASSERT(len > 0, "dump produced output");
    fseek(tmp, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    TEST_ASSERT(buf != NULL, "alloc dump buffer");
    size_t nread = fread(buf, 1, (size_t)len, tmp);
    TEST_ASSERT(nread == (size_t)len, "read full dump");
    buf[len] = '\0';
    fclose(tmp);

    TEST_ASSERT(strstr(buf, "sext i32 %v1 to i64") != NULL,
                "builder inserts sext i32->i64 for runtime gep index");
    TEST_ASSERT(strstr(buf, "getelementptr i32, ptr") != NULL,
                "builder emits gep");
    TEST_ASSERT(strstr(buf, ", i64 %v") != NULL,
                "gep uses canonical i64 runtime index");
    free(buf);

    lr_module_free(m);
    return 0;
}

/* Build a function that calls another (forward typed call) */
int test_builder_call(void) {
    lr_module_t *m = lr_module_create_new();
    lr_type_t *i32 = lr_type_i32_get(m);
    lr_type_t *ptr = lr_type_ptr_get(m);

    /* define i32 @helper(i32 %x) { ret i32 %x+10 } */
    lr_type_t *h_params[] = { i32 };
    lr_func_t *helper = lr_func_define(m, "helper", i32, h_params, 1, false);
    uint32_t hx = lr_func_param_vreg(helper, 0);
    lr_block_t *hentry = lr_block_new(helper, m, "entry");
    uint32_t hr = lr_build_add(m, hentry, helper, i32, LR_VREG(hx, i32), LR_IMM(10, i32));
    lr_build_ret(m, hentry, LR_VREG(hr, i32));

    /* define i32 @caller(i32 %a) { %r = call i32 @helper(i32 %a); ret i32 %r } */
    lr_type_t *c_params[] = { i32 };
    lr_func_t *caller = lr_func_define(m, "caller", i32, c_params, 1, false);
    uint32_t ca = lr_func_param_vreg(caller, 0);
    lr_block_t *centry = lr_block_new(caller, m, "entry");

    uint32_t helper_sym = lr_symbol_intern(m, "helper");
    lr_operand_desc_t args[] = { LR_VREG(ca, i32) };
    uint32_t cr = lr_build_call(m, centry, caller, i32,
                                 LR_GLOBAL(helper_sym, ptr), args, 1);
    lr_build_ret(m, centry, LR_VREG(cr, i32));

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int);
    fn_t fn; GET_FN(fn, jit, "caller");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(32), 42, "caller(32) == 42");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

/* Build select instruction */
int test_builder_select(void) {
    lr_module_t *m = lr_module_create_new();
    lr_type_t *i32 = lr_type_i32_get(m);
    lr_type_t *i1 = lr_type_i1_get(m);
    lr_type_t *params[] = { i32, i32 };
    lr_func_t *f = lr_func_define(m, "sel_max", i32, params, 2, false);

    uint32_t va = lr_func_param_vreg(f, 0);
    uint32_t vb = lr_func_param_vreg(f, 1);

    lr_block_t *entry = lr_block_new(f, m, "entry");
    uint32_t cmp = lr_build_icmp(m, entry, f, LR_CMP_SGT,
                                  LR_VREG(va, i32), LR_VREG(vb, i32));
    uint32_t sel = lr_build_select(m, entry, f, i32, LR_VREG(cmp, i1),
                                    LR_VREG(va, i32), LR_VREG(vb, i32));
    lr_build_ret(m, entry, LR_VREG(sel, i32));

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; GET_FN(fn, jit, "sel_max");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(10, 5), 10, "sel_max(10,5) == 10");
    TEST_ASSERT_EQ(fn(3, 7), 7, "sel_max(3,7) == 7");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

/* Build roundtrip: build via API, dump to text, compare with text-parsed version */
int test_builder_roundtrip(void) {
    lr_module_t *m = lr_module_create_new();
    lr_type_t *i32 = lr_type_i32_get(m);
    lr_type_t *params[] = { i32, i32 };
    lr_func_t *f = lr_func_define(m, "add", i32, params, 2, false);
    uint32_t va = lr_func_param_vreg(f, 0);
    uint32_t vb = lr_func_param_vreg(f, 1);
    lr_block_t *entry = lr_block_new(f, m, "entry");
    uint32_t vc = lr_build_add(m, entry, f, i32, LR_VREG(va, i32), LR_VREG(vb, i32));
    lr_build_ret(m, entry, LR_VREG(vc, i32));

    /* Dump to buffer via tmpfile */
    FILE *tmp = tmpfile();
    TEST_ASSERT(tmp != NULL, "tmpfile");
    lr_module_dump_to(m, tmp);
    fseek(tmp, 0, SEEK_END);
    long len = ftell(tmp);
    TEST_ASSERT(len > 0, "dump produced output");
    fseek(tmp, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    size_t nread = fread(buf, 1, (size_t)len, tmp);
    TEST_ASSERT(nread == (size_t)len, "read full dumped module");
    buf[len] = '\0';
    fclose(tmp);

    /* Parse the dumped text and JIT it */
    char err[256] = {0};
    lr_module_t *m2 = lr_parse_ll(buf, (size_t)len, err, sizeof(err));
    TEST_ASSERT(m2 != NULL, "re-parse dumped IR");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m2);
    TEST_ASSERT_EQ(rc, 0, "jit add re-parsed module");

    typedef int (*fn_t)(int, int);
    fn_t fn; GET_FN(fn, jit, "add");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(10, 32), 42, "roundtrip add(10,32) == 42");

    lr_jit_destroy(jit);
    lr_module_free(m2);
    lr_module_free(m);
    free(buf);
    return 0;
}

int test_builder_compat_add_to_jit(void) {
    lc_context_t *ctx = lc_context_create();
    TEST_ASSERT(ctx != NULL, "context create");

    lc_module_compat_t *mod = lc_module_create(ctx, "compat_jit");
    TEST_ASSERT(mod != NULL, "compat module create");

    lr_module_t *ir = lc_module_get_ir(mod);
    TEST_ASSERT(ir != NULL, "compat module ir");

    lr_type_t *i32 = lc_get_int_type(mod, 32);
    TEST_ASSERT(i32 != NULL, "i32 type");

    lr_func_t *f = lr_func_define(ir, "compat_ret42", i32, NULL, 0, false);
    TEST_ASSERT(f != NULL, "function define");
    lr_block_t *entry = lr_block_new(f, ir, "entry");
    TEST_ASSERT(entry != NULL, "entry block");
    lr_build_ret(ir, entry, LR_IMM(42, i32));

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lc_module_add_to_jit(mod, jit);
    TEST_ASSERT_EQ(rc, 0, "lc_module_add_to_jit");

    typedef int (*fn_t)(void);
    fn_t fn;
    GET_FN(fn, jit, "compat_ret42");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 42, "compat_ret42() == 42");

    lr_jit_destroy(jit);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}

int test_builder_compat_add_to_jit_null_args(void) {
    lc_context_t *ctx = lc_context_create();
    TEST_ASSERT(ctx != NULL, "context create");
    lc_module_compat_t *mod = lc_module_create(ctx, "compat_null_args");
    TEST_ASSERT(mod != NULL, "compat module create");
    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    TEST_ASSERT_EQ(lc_module_add_to_jit(NULL, jit), -1, "null module rejected");
    TEST_ASSERT_EQ(lc_module_add_to_jit(mod, NULL), -1, "null jit rejected");

    lr_jit_destroy(jit);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}

int test_builder_compat_memory_and_call_path(void) {
    lc_context_t *ctx = lc_context_create();
    TEST_ASSERT(ctx != NULL, "context create");
    lc_module_compat_t *mod = lc_module_create(ctx, "compat_mem_call");
    TEST_ASSERT(mod != NULL, "compat module create");

    lr_module_t *ir = lc_module_get_ir(mod);
    TEST_ASSERT(ir != NULL, "compat module ir");
    lr_type_t *i32 = lc_get_int_type(mod, 32);
    TEST_ASSERT(i32 != NULL, "i32 type");

    lr_type_t *fn_params[1] = { i32 };
    lr_type_t *fn_ty = lr_type_func_new(ir, i32, fn_params, 1, false);
    TEST_ASSERT(fn_ty != NULL, "function type");

    lc_value_t *callee_val = lc_func_create(mod, "compat_inc5", fn_ty);
    TEST_ASSERT(callee_val != NULL, "callee function value");
    lr_func_t *callee = lc_value_get_func(callee_val);
    TEST_ASSERT(callee != NULL, "callee function");
    lc_value_t *callee_bb_val = lc_block_create(mod, callee, "entry");
    lr_block_t *callee_bb = lc_value_get_block(callee_bb_val);
    TEST_ASSERT(callee_bb != NULL, "callee entry block");
    lc_value_t *callee_arg0 = lc_func_get_arg(mod, callee_val, 0);
    lc_value_t *c5 = lc_value_const_int(mod, i32, 5, 32);
    lc_value_t *sum = lc_create_add(mod, callee_bb, callee, callee_arg0, c5, "sum");
    TEST_ASSERT(sum != NULL, "compat add in callee");
    lc_create_ret(mod, callee_bb, sum);

    lc_value_t *caller_val = lc_func_create(mod, "compat_mem_call", fn_ty);
    TEST_ASSERT(caller_val != NULL, "caller function value");
    lr_func_t *caller = lc_value_get_func(caller_val);
    TEST_ASSERT(caller != NULL, "caller function");
    lc_value_t *caller_bb_val = lc_block_create(mod, caller, "entry");
    lr_block_t *caller_bb = lc_value_get_block(caller_bb_val);
    TEST_ASSERT(caller_bb != NULL, "caller entry block");
    lc_value_t *caller_arg0 = lc_func_get_arg(mod, caller_val, 0);

    lr_type_t *arr2_i32 = lr_type_array_new(ir, i32, 2);
    TEST_ASSERT(arr2_i32 != NULL, "array type");
    lc_alloca_inst_t *alloca_arr = lc_create_alloca(
        mod, caller_bb, caller, arr2_i32, NULL, "arr");
    TEST_ASSERT(alloca_arr != NULL, "alloca array");
    TEST_ASSERT(alloca_arr->result != NULL, "alloca result");

    lc_value_t *idx0 = lc_value_const_int(mod, i32, 0, 32);
    lc_value_t *idx1 = lc_value_const_int(mod, i32, 1, 32);
    lc_value_t *indices[2] = { idx0, idx1 };
    lc_value_t *elem_ptr = lc_create_gep(mod, caller_bb, caller, arr2_i32,
        alloca_arr->result, indices, 2, "elem_ptr");
    TEST_ASSERT(elem_ptr != NULL, "gep element pointer");

    lc_create_store(mod, caller_bb, caller_arg0, elem_ptr);
    lc_value_t *loaded = lc_create_load(mod, caller_bb, caller, i32, elem_ptr, "loaded");
    TEST_ASSERT(loaded != NULL, "load element");

    lc_value_t *call_args[1] = { loaded };
    lc_value_t *call_res = lc_create_call(mod, caller_bb, caller, fn_ty,
        callee_val, call_args, 1, "inc5_call");
    TEST_ASSERT(call_res != NULL, "compat call result");
    lc_create_ret(mod, caller_bb, call_res);

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lc_module_add_to_jit(mod, jit);
    TEST_ASSERT_EQ(rc, 0, "lc_module_add_to_jit");

    typedef int (*fn_t)(int);
    fn_t fn;
    GET_FN(fn, jit, "compat_mem_call");
    TEST_ASSERT(fn != NULL, "compat_mem_call lookup");
    TEST_ASSERT_EQ(fn(37), 42, "compat_mem_call(37) == 42");
    TEST_ASSERT_EQ(fn(0), 5, "compat_mem_call(0) == 5");

    lr_jit_destroy(jit);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}

int test_builder_compat_phi_finalize_add_incoming_after_finalize_noop(void) {
    lc_context_t *ctx = lc_context_create();
    TEST_ASSERT(ctx != NULL, "context create");
    lc_module_compat_t *mod = lc_module_create(ctx, "compat_phi_finalize");
    TEST_ASSERT(mod != NULL, "compat module create");

    lr_module_t *ir = lc_module_get_ir(mod);
    TEST_ASSERT(ir != NULL, "compat module ir");
    lr_type_t *i32 = lc_get_int_type(mod, 32);
    TEST_ASSERT(i32 != NULL, "i32 type");

    lr_type_t *params[1] = { i32 };
    lr_type_t *fn_ty = lr_type_func_new(ir, i32, params, 1, false);
    TEST_ASSERT(fn_ty != NULL, "function type");

    lc_value_t *fn_val = lc_func_create(mod, "compat_abs_phi_finalize", fn_ty);
    TEST_ASSERT(fn_val != NULL, "function create");
    lr_func_t *fn = lc_value_get_func(fn_val);
    TEST_ASSERT(fn != NULL, "function unwrap");
    lc_value_t *arg = lc_func_get_arg(mod, fn_val, 0);
    TEST_ASSERT(arg != NULL, "function arg");

    lr_block_t *entry = lc_value_get_block(lc_block_create(mod, fn, "entry"));
    lr_block_t *then_bb = lc_value_get_block(lc_block_create(mod, fn, "then"));
    lr_block_t *else_bb = lc_value_get_block(lc_block_create(mod, fn, "else"));
    lr_block_t *merge_bb = lc_value_get_block(lc_block_create(mod, fn, "merge"));
    TEST_ASSERT(entry && then_bb && else_bb && merge_bb, "block create");

    lc_value_t *c0 = lc_value_const_int(mod, i32, 0, 32);
    TEST_ASSERT(c0 != NULL, "const zero");
    lc_value_t *is_neg = lc_create_icmp_slt(mod, entry, fn, arg, c0, "is_neg");
    TEST_ASSERT(is_neg != NULL, "icmp slt");
    lc_create_cond_br(mod, entry, is_neg, then_bb, else_bb);

    lc_value_t *neg = lc_create_sub(mod, then_bb, fn, c0, arg, "neg");
    TEST_ASSERT(neg != NULL, "neg value");
    lc_create_br(mod, then_bb, merge_bb);
    lc_create_br(mod, else_bb, merge_bb);

    lc_phi_node_t *phi = lc_create_phi(mod, merge_bb, fn, i32, "result");
    TEST_ASSERT(phi != NULL, "phi create");
    lc_phi_add_incoming(phi, neg, then_bb);
    lc_phi_add_incoming(phi, arg, else_bb);
    lc_phi_finalize(phi);
    /* Regression guard: this must remain a no-op after finalize. */
    lc_phi_add_incoming(phi, c0, entry);
    lc_phi_finalize(phi);
    TEST_ASSERT(phi->result != NULL, "phi result");
    lc_create_ret(mod, merge_bb, phi->result);

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lc_module_add_to_jit(mod, jit);
    TEST_ASSERT_EQ(rc, 0, "lc_module_add_to_jit");

    typedef int (*fn_t)(int);
    fn_t fp;
    GET_FN(fp, jit, "compat_abs_phi_finalize");
    TEST_ASSERT(fp != NULL, "compat_abs_phi_finalize lookup");
    TEST_ASSERT_EQ(fp(5), 5, "compat_abs_phi_finalize(5) == 5");
    TEST_ASSERT_EQ(fp(-7), 7, "compat_abs_phi_finalize(-7) == 7");
    TEST_ASSERT_EQ(fp(0), 0, "compat_abs_phi_finalize(0) == 0");

    lr_jit_destroy(jit);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}
