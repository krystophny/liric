#include <llvm-c/LiricSession.h>
#include <liric/liric_legacy.h>

#include <stdlib.h>
#include <string.h>

typedef struct liric_symbol_entry {
    char *name;
    void *addr;
} liric_symbol_entry_t;

struct LLVMLiricSessionState {
    lc_module_compat_t **modules;
    size_t num_modules;
    size_t cap_modules;
    liric_symbol_entry_t *symbols;
    size_t num_symbols;
    size_t cap_symbols;
};

static int ensure_module_cap(LLVMLiricSessionStateRef state, size_t need) {
    size_t new_cap;
    lc_module_compat_t **new_modules;
    if (!state)
        return -1;
    if (need <= state->cap_modules)
        return 0;
    new_cap = state->cap_modules == 0 ? 8u : state->cap_modules;
    while (new_cap < need)
        new_cap *= 2u;
    new_modules = (lc_module_compat_t **)realloc(
        state->modules, sizeof(*new_modules) * new_cap);
    if (!new_modules)
        return -1;
    state->modules = new_modules;
    state->cap_modules = new_cap;
    return 0;
}

static int ensure_symbol_cap(LLVMLiricSessionStateRef state, size_t need) {
    size_t new_cap;
    liric_symbol_entry_t *new_symbols;
    if (!state)
        return -1;
    if (need <= state->cap_symbols)
        return 0;
    new_cap = state->cap_symbols == 0 ? 16u : state->cap_symbols;
    while (new_cap < need)
        new_cap *= 2u;
    new_symbols = (liric_symbol_entry_t *)realloc(
        state->symbols, sizeof(*new_symbols) * new_cap);
    if (!new_symbols)
        return -1;
    state->symbols = new_symbols;
    state->cap_symbols = new_cap;
    return 0;
}

LLVMLiricSessionStateRef LLVMLiricSessionCreate(void) {
    return (LLVMLiricSessionStateRef)calloc(1, sizeof(LLVMLiricSessionState));
}

void LLVMLiricSessionDispose(LLVMLiricSessionStateRef state) {
    size_t i;
    if (!state)
        return;
    for (i = 0; i < state->num_symbols; i++)
        free(state->symbols[i].name);
    free(state->symbols);
    free(state->modules);
    free(state);
}

int LLVMLiricSessionAddCompatModule(LLVMLiricSessionStateRef state,
                                    lc_module_compat_t *mod) {
    size_t i;
    if (!state || !mod)
        return -1;
    for (i = 0; i < state->num_symbols; i++) {
        lc_module_add_external_symbol(mod, state->symbols[i].name,
                                      state->symbols[i].addr);
    }
    if (lc_module_finalize_for_execution(mod) != 0)
        return -1;
    if (ensure_module_cap(state, state->num_modules + 1u) != 0)
        return -1;
    state->modules[state->num_modules++] = mod;
    return 0;
}

void LLVMLiricSessionAddSymbol(LLVMLiricSessionStateRef state,
                               const char *name, void *addr) {
    size_t i;
    char *copy;
    if (!state || !name || !name[0])
        return;
    for (i = 0; i < state->num_modules; i++)
        lc_module_add_external_symbol(state->modules[i], name, addr);
    if (ensure_symbol_cap(state, state->num_symbols + 1u) != 0)
        return;
    copy = strdup(name);
    if (!copy)
        return;
    state->symbols[state->num_symbols].name = copy;
    state->symbols[state->num_symbols].addr = addr;
    state->num_symbols++;
}

void *LLVMLiricSessionLookup(LLVMLiricSessionStateRef state, const char *name) {
    size_t i;
    if (!state || !name || !name[0])
        return NULL;
    for (i = state->num_symbols; i > 0; i--) {
        liric_symbol_entry_t *sym = &state->symbols[i - 1u];
        if (sym->name && strcmp(sym->name, name) == 0)
            return sym->addr;
    }
    for (i = state->num_modules; i > 0; i--) {
        void *addr = lc_module_lookup_in_session(state->modules[i - 1u], name);
        if (addr)
            return addr;
    }
    return NULL;
}

const char *LLVMLiricHostTargetName(void) {
    return lr_jit_host_target_name();
}
