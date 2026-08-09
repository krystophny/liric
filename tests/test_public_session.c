#include <liric/liric_session.h>

#include <stdio.h>
#include <string.h>

static int fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static int test_behavior(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err = {0};
    lr_session_t *session = lr_session_create(&cfg, &err);
    lr_type_t *i32;
    void *address = NULL;
    int (*function)(void);

    if (!session)
        return fail("public session creation");
    i32 = lr_type_i32_s(session);
    if (!i32)
        return fail("public i32 type creation");
    if (lr_session_func_begin(session, "public_ret_42", i32, NULL, 0, false, &err) != 0)
        return fail("public function begin");
    if (lr_session_set_block(session, lr_session_block(session), &err) != 0)
        return fail("public block selection");
    lr_emit_ret(session, LR_IMM(42, i32));
    if (lr_session_func_end(session, &address, &err) != 0)
        return fail("public function end");
    if (!address)
        return fail("public function address");

    memcpy(&function, &address, sizeof(function));
    if (function() != 42)
        return fail("public function returns 42");
    lr_session_destroy(session);
    return 0;
}

static int test_invalid_input(void) {
    lr_error_t err = {0};
    lr_session_abi_info_t abi = {0};

    if (lr_session_get_abi_info(NULL, 0) != LR_ERR_ARGUMENT)
        return fail("null ABI output is rejected");
    if (lr_session_get_abi_info(&abi, sizeof(abi) - 1) != LR_ERR_ARGUMENT)
        return fail("short ABI output is rejected");
    if (lr_session_func_end(NULL, NULL, &err) != -1)
        return fail("null session function end is rejected");
    if (err.code != LR_ERR_STATE)
        return fail("null session function end reports state error");
    return 0;
}

int main(void) {
    if (test_behavior() != 0 || test_invalid_input() != 0)
        return 1;
    puts("public C session behavior and invalid-input oracle: ok");
    return 0;
}
