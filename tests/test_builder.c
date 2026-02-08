#include <liric/liric.h>
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
    TEST_ASSERT(strstr(buf, "getelementptr i32, ptr %v0, i64 %v2") != NULL,
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
