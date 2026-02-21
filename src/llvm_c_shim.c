#include <liric/liric_compat.h>
#include <liric/liric_legacy.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct LLVMLiricSessionState {
    lr_jit_t *jit;
};

static int read_file_bytes(const char *path, uint8_t **out_data, size_t *out_len) {
    FILE *f = NULL;
    long len = 0;
    uint8_t *buf = NULL;
    size_t nread = 0;
    if (!path || !path[0] || !out_data || !out_len)
        return -1;
    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    len = ftell(f);
    if (len <= 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return -1;
    }
    nread = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (nread != (size_t)len) {
        free(buf);
        return -1;
    }
    *out_data = buf;
    *out_len = nread;
    return 0;
}

LLVMLiricSessionStateRef LLVMLiricSessionCreate(void) {
    const char *runtime_bc = getenv("LIRIC_RUNTIME_BC");
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
    if (runtime_bc && runtime_bc[0]) {
        uint8_t *bc = NULL;
        size_t bc_len = 0;
        int rc = read_file_bytes(runtime_bc, &bc, &bc_len);
        if (rc != 0 || lr_jit_set_runtime_bc(state->jit, bc, bc_len) != 0) {
            free(bc);
            lr_jit_destroy(state->jit);
            free(state);
            return NULL;
        }
        free(bc);
    } else if (runtime_lib && runtime_lib[0]) {
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

int LLVMLiricSessionLoadLibrary(LLVMLiricSessionStateRef state,
                                const char *path) {
    if (!state || !state->jit || !path || !path[0])
        return -1;
    return lr_jit_load_library(state->jit, path);
}

void *LLVMLiricSessionLookup(LLVMLiricSessionStateRef state, const char *name) {
    if (!state || !state->jit || !name || !name[0])
        return NULL;
    return lr_jit_get_function(state->jit, name);
}

const char *LLVMLiricHostTargetName(void) {
    return lr_jit_host_target_name();
}
