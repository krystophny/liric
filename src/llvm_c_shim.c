#include <liric/liric_compat.h>
#include <liric/liric_legacy.h>

#include <stdlib.h>

struct LLVMLiricSessionState {
    lr_jit_t *jit;
};

LLVMLiricSessionStateRef LLVMLiricSessionCreate(void) {
    const char *runtime_lib = getenv("LIRIC_RUNTIME_LIB");
    LLVMLiricSessionStateRef state =
        (LLVMLiricSessionStateRef)calloc(1, sizeof(LLVMLiricSessionState));
    if (!state)
        return NULL;
    state->jit = lr_jit_create();
    if (!state->jit) {
        free(state);
        return NULL;
    }
    if (runtime_lib && runtime_lib[0]) {
        if (lr_jit_load_library(state->jit, runtime_lib) != 0) {
            lr_jit_destroy(state->jit);
            free(state);
            return NULL;
        }
    }
    return state;
}

void LLVMLiricSessionDispose(LLVMLiricSessionStateRef state) {
    if (!state)
        return;
    if (state->jit)
        lr_jit_destroy(state->jit);
    free(state);
}

int LLVMLiricSessionAddCompatModule(LLVMLiricSessionStateRef state,
                                    lc_module_compat_t *mod) {
    if (!state || !mod)
        return -1;
    if (!state->jit)
        return -1;
    if (lc_module_finalize_for_execution(mod) != 0)
        return -1;
    return lc_module_add_to_jit(mod, state->jit);
}

void LLVMLiricSessionAddSymbol(LLVMLiricSessionStateRef state,
                               const char *name, void *addr) {
    if (!state || !state->jit || !name || !name[0])
        return;
    lr_jit_add_symbol(state->jit, name, addr);
}

void *LLVMLiricSessionLookup(LLVMLiricSessionStateRef state, const char *name) {
    if (!state || !state->jit || !name || !name[0])
        return NULL;
    return lr_jit_get_function(state->jit, name);
}

const char *LLVMLiricHostTargetName(void) {
    return lr_jit_host_target_name();
}
