#include <liric/llvm_compat_c.h>

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lr_ptr_lc_value_node {
    const void *key;
    lc_value_t *value;
    struct lr_ptr_lc_value_node *next;
} lr_ptr_lc_value_node_t;

typedef struct lr_ptr_void_node {
    const void *key;
    void *value;
    struct lr_ptr_void_node *next;
} lr_ptr_void_node_t;

typedef struct lr_type_context_node {
    const lr_type_t *type;
    const void *context;
    struct lr_type_context_node *next;
} lr_type_context_node_t;

typedef struct lr_vector_type_node {
    const lr_type_t *type;
    lr_llvm_compat_vector_type_info_t info;
    struct lr_vector_type_node *next;
} lr_vector_type_node_t;

typedef struct lr_global_alias_node {
    const lc_module_compat_t *module;
    char *logical_name;
    char *actual_name;
    struct lr_global_alias_node *next;
} lr_global_alias_node_t;

typedef struct lr_global_value_state_node {
    const void *obj;
    int linkage;
    int visibility;
    int unnamed_addr;
    int has_linkage;
    int has_visibility;
    int has_unnamed_addr;
    struct lr_global_value_state_node *next;
} lr_global_value_state_node_t;

static _Thread_local lr_ptr_lc_value_node_t *g_value_wrappers;
static _Thread_local lr_ptr_void_node_t *g_function_wrappers;
static _Thread_local lr_ptr_void_node_t *g_block_parents;
static _Thread_local lr_type_context_node_t *g_type_contexts;
static _Thread_local lr_vector_type_node_t *g_vector_types;
static _Thread_local lr_global_alias_node_t *g_global_aliases;
static _Thread_local lr_global_value_state_node_t *g_global_value_states;

static void lr_copy_out_string(const char *src,
                               char *out,
                               size_t out_cap,
                               size_t *out_len) {
    size_t n = src ? strlen(src) : 0;
    if (out_len)
        *out_len = n;
    if (!out || out_cap == 0)
        return;
    if (!src) {
        out[0] = '\0';
        return;
    }
    if (n >= out_cap)
        n = out_cap - 1;
    memcpy(out, src, n);
    out[n] = '\0';
}

static char *lr_strdup_safe(const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *dup = (char *)malloc(n + 1);
    if (!dup)
        return NULL;
    if (n)
        memcpy(dup, s, n);
    dup[n] = '\0';
    return dup;
}

static lr_ptr_lc_value_node_t *lr_find_lc_value_node(lr_ptr_lc_value_node_t *head,
                                                      const void *key) {
    lr_ptr_lc_value_node_t *it = head;
    while (it) {
        if (it->key == key)
            return it;
        it = it->next;
    }
    return NULL;
}

static lr_ptr_void_node_t *lr_find_void_node(lr_ptr_void_node_t *head,
                                             const void *key) {
    lr_ptr_void_node_t *it = head;
    while (it) {
        if (it->key == key)
            return it;
        it = it->next;
    }
    return NULL;
}

static lr_global_value_state_node_t *lr_find_global_value_state_node(const void *obj) {
    lr_global_value_state_node_t *it = g_global_value_states;
    while (it) {
        if (it->obj == obj)
            return it;
        it = it->next;
    }
    return NULL;
}

static lr_global_value_state_node_t *lr_ensure_global_value_state_node(const void *obj) {
    lr_global_value_state_node_t *node;
    if (!obj)
        return NULL;
    node = lr_find_global_value_state_node(obj);
    if (node)
        return node;
    node = (lr_global_value_state_node_t *)calloc(1, sizeof(*node));
    if (!node)
        return NULL;
    node->obj = obj;
    node->next = g_global_value_states;
    g_global_value_states = node;
    return node;
}

void lr_llvm_compat_register_value_wrapper(const void *obj, lc_value_t *value) {
    lr_ptr_lc_value_node_t *node;
    if (!obj || !value)
        return;
    node = lr_find_lc_value_node(g_value_wrappers, obj);
    if (node) {
        node->value = value;
        return;
    }
    node = (lr_ptr_lc_value_node_t *)calloc(1, sizeof(*node));
    if (!node)
        return;
    node->key = obj;
    node->value = value;
    node->next = g_value_wrappers;
    g_value_wrappers = node;
}

lc_value_t *lr_llvm_compat_lookup_value_wrapper(const void *obj) {
    lr_ptr_lc_value_node_t *node;
    if (!obj)
        return NULL;
    node = lr_find_lc_value_node(g_value_wrappers, obj);
    return node ? node->value : NULL;
}

void lr_llvm_compat_unregister_value_wrapper(const void *obj) {
    lr_ptr_lc_value_node_t *it = g_value_wrappers;
    lr_ptr_lc_value_node_t *prev = NULL;
    if (!obj)
        return;
    while (it) {
        if (it->key == obj) {
            if (prev)
                prev->next = it->next;
            else
                g_value_wrappers = it->next;
            free(it);
            return;
        }
        prev = it;
        it = it->next;
    }
}

void lr_llvm_compat_register_function_wrapper(const lr_func_t *func,
                                              void *fn_wrapper) {
    lr_ptr_void_node_t *node;
    if (!func || !fn_wrapper)
        return;
    node = lr_find_void_node(g_function_wrappers, func);
    if (node) {
        node->value = fn_wrapper;
        return;
    }
    node = (lr_ptr_void_node_t *)calloc(1, sizeof(*node));
    if (!node)
        return;
    node->key = func;
    node->value = fn_wrapper;
    node->next = g_function_wrappers;
    g_function_wrappers = node;
}

void *lr_llvm_compat_lookup_function_wrapper(const lr_func_t *func) {
    lr_ptr_void_node_t *node;
    if (!func)
        return NULL;
    node = lr_find_void_node(g_function_wrappers, func);
    return node ? node->value : NULL;
}

void lr_llvm_compat_unregister_function_wrapper(const lr_func_t *func) {
    lr_ptr_void_node_t *it = g_function_wrappers;
    lr_ptr_void_node_t *prev = NULL;
    if (!func)
        return;
    while (it) {
        if (it->key == func) {
            if (prev)
                prev->next = it->next;
            else
                g_function_wrappers = it->next;
            free(it);
            return;
        }
        prev = it;
        it = it->next;
    }
}

void lr_llvm_compat_register_block_parent(const lr_block_t *block,
                                          void *fn_wrapper) {
    lr_ptr_void_node_t *node;
    if (!block || !fn_wrapper)
        return;
    node = lr_find_void_node(g_block_parents, block);
    if (node) {
        node->value = fn_wrapper;
        return;
    }
    node = (lr_ptr_void_node_t *)calloc(1, sizeof(*node));
    if (!node)
        return;
    node->key = block;
    node->value = fn_wrapper;
    node->next = g_block_parents;
    g_block_parents = node;
}

void *lr_llvm_compat_lookup_block_parent(const lr_block_t *block) {
    lr_ptr_void_node_t *node;
    if (!block)
        return NULL;
    node = lr_find_void_node(g_block_parents, block);
    return node ? node->value : NULL;
}

void lr_llvm_compat_unregister_block_parent(const lr_block_t *block) {
    lr_ptr_void_node_t *it = g_block_parents;
    lr_ptr_void_node_t *prev = NULL;
    if (!block)
        return;
    while (it) {
        if (it->key == block) {
            if (prev)
                prev->next = it->next;
            else
                g_block_parents = it->next;
            free(it);
            return;
        }
        prev = it;
        it = it->next;
    }
}

void lr_llvm_compat_unregister_blocks_for_function(void *fn_wrapper) {
    lr_ptr_void_node_t *it = g_block_parents;
    lr_ptr_void_node_t *prev = NULL;
    if (!fn_wrapper)
        return;
    while (it) {
        if (it->value == fn_wrapper) {
            lr_ptr_void_node_t *dead = it;
            if (prev)
                prev->next = it->next;
            else
                g_block_parents = it->next;
            it = it->next;
            free(dead);
            continue;
        }
        prev = it;
        it = it->next;
    }
}

void lr_llvm_compat_register_type_context(const lr_type_t *ty,
                                          const void *ctx) {
    lr_type_context_node_t *it;
    lr_type_context_node_t *node;
    if (!ty || !ctx)
        return;
    it = g_type_contexts;
    while (it) {
        if (it->type == ty) {
            it->context = ctx;
            return;
        }
        it = it->next;
    }
    node = (lr_type_context_node_t *)calloc(1, sizeof(*node));
    if (!node)
        return;
    node->type = ty;
    node->context = ctx;
    node->next = g_type_contexts;
    g_type_contexts = node;
}

const void *lr_llvm_compat_lookup_type_context(const lr_type_t *ty) {
    lr_type_context_node_t *it = g_type_contexts;
    if (!ty)
        return NULL;
    while (it) {
        if (it->type == ty)
            return it->context;
        it = it->next;
    }
    return NULL;
}

void lr_llvm_compat_register_vector_type(const lr_type_t *ty,
                                         const lr_type_t *element,
                                         unsigned num_elements,
                                         int scalable) {
    lr_vector_type_node_t *it;
    lr_vector_type_node_t *node;
    if (!ty || !element || num_elements == 0)
        return;
    it = g_vector_types;
    while (it) {
        if (it->type == ty) {
            it->info.element = element;
            it->info.num_elements = num_elements;
            it->info.scalable = scalable ? 1 : 0;
            return;
        }
        it = it->next;
    }
    node = (lr_vector_type_node_t *)calloc(1, sizeof(*node));
    if (!node)
        return;
    node->type = ty;
    node->info.element = element;
    node->info.num_elements = num_elements;
    node->info.scalable = scalable ? 1 : 0;
    node->next = g_vector_types;
    g_vector_types = node;
}

int lr_llvm_compat_lookup_vector_type(const lr_type_t *ty,
                                      lr_llvm_compat_vector_type_info_t *out_info) {
    lr_vector_type_node_t *it = g_vector_types;
    if (!ty || !out_info)
        return 0;
    while (it) {
        if (it->type == ty) {
            *out_info = it->info;
            return 1;
        }
        it = it->next;
    }
    return 0;
}

void lr_llvm_compat_unregister_type_contexts(const void *ctx) {
    lr_type_context_node_t *it = g_type_contexts;
    lr_type_context_node_t *prev = NULL;
    if (!ctx)
        return;
    while (it) {
        if (it->context == ctx) {
            lr_type_context_node_t *dead = it;
            const lr_type_t *dead_type = it->type;
            lr_vector_type_node_t *vit = g_vector_types;
            lr_vector_type_node_t *vprev = NULL;
            while (vit) {
                if (vit->type == dead_type) {
                    lr_vector_type_node_t *vdead = vit;
                    if (vprev)
                        vprev->next = vit->next;
                    else
                        g_vector_types = vit->next;
                    vit = vit->next;
                    free(vdead);
                    continue;
                }
                vprev = vit;
                vit = vit->next;
            }
            if (prev)
                prev->next = it->next;
            else
                g_type_contexts = it->next;
            it = it->next;
            free(dead);
            continue;
        }
        prev = it;
        it = it->next;
    }
}

void lr_llvm_compat_register_global_alias(const lc_module_compat_t *mod,
                                          const char *logical_name,
                                          const char *actual_name) {
    lr_global_alias_node_t *it;
    lr_global_alias_node_t *node;
    if (!mod || !logical_name || !logical_name[0] || !actual_name || !actual_name[0])
        return;
    it = g_global_aliases;
    while (it) {
        if (it->module == mod && strcmp(it->logical_name, logical_name) == 0) {
            char *dup = lr_strdup_safe(actual_name);
            if (!dup)
                return;
            free(it->actual_name);
            it->actual_name = dup;
            return;
        }
        it = it->next;
    }
    node = (lr_global_alias_node_t *)calloc(1, sizeof(*node));
    if (!node)
        return;
    node->module = mod;
    node->logical_name = lr_strdup_safe(logical_name);
    node->actual_name = lr_strdup_safe(actual_name);
    if (!node->logical_name || !node->actual_name) {
        free(node->logical_name);
        free(node->actual_name);
        free(node);
        return;
    }
    node->next = g_global_aliases;
    g_global_aliases = node;
}

int lr_llvm_compat_lookup_global_alias(const lc_module_compat_t *mod,
                                       const char *logical_name,
                                       char *out_name,
                                       size_t out_name_cap,
                                       size_t *out_name_len) {
    lr_global_alias_node_t *it = g_global_aliases;
    if (!mod || !logical_name || !logical_name[0])
        return 0;
    while (it) {
        if (it->module == mod && strcmp(it->logical_name, logical_name) == 0) {
            lr_copy_out_string(it->actual_name, out_name, out_name_cap, out_name_len);
            return 1;
        }
        it = it->next;
    }
    if (out_name_len)
        *out_name_len = 0;
    if (out_name && out_name_cap)
        out_name[0] = '\0';
    return 0;
}

void lr_llvm_compat_clear_global_aliases(const lc_module_compat_t *mod) {
    lr_global_alias_node_t *it = g_global_aliases;
    lr_global_alias_node_t *prev = NULL;
    if (!mod)
        return;
    while (it) {
        if (it->module == mod) {
            lr_global_alias_node_t *dead = it;
            if (prev)
                prev->next = it->next;
            else
                g_global_aliases = it->next;
            it = it->next;
            free(dead->logical_name);
            free(dead->actual_name);
            free(dead);
            continue;
        }
        prev = it;
        it = it->next;
    }
}

void lr_llvm_compat_global_value_set_linkage(const void *obj, int linkage) {
    lr_global_value_state_node_t *node = lr_ensure_global_value_state_node(obj);
    if (!node)
        return;
    node->linkage = linkage;
    node->has_linkage = 1;
}

int lr_llvm_compat_global_value_get_linkage(const void *obj, int *out_linkage) {
    lr_global_value_state_node_t *node = lr_find_global_value_state_node(obj);
    if (!node || !node->has_linkage || !out_linkage)
        return 0;
    *out_linkage = node->linkage;
    return 1;
}

void lr_llvm_compat_global_value_set_visibility(const void *obj, int visibility) {
    lr_global_value_state_node_t *node = lr_ensure_global_value_state_node(obj);
    if (!node)
        return;
    node->visibility = visibility;
    node->has_visibility = 1;
}

int lr_llvm_compat_global_value_get_visibility(const void *obj, int *out_visibility) {
    lr_global_value_state_node_t *node = lr_find_global_value_state_node(obj);
    if (!node || !node->has_visibility || !out_visibility)
        return 0;
    *out_visibility = node->visibility;
    return 1;
}

void lr_llvm_compat_global_value_set_unnamed_addr(const void *obj, int unnamed_addr) {
    lr_global_value_state_node_t *node = lr_ensure_global_value_state_node(obj);
    if (!node)
        return;
    node->unnamed_addr = unnamed_addr;
    node->has_unnamed_addr = 1;
}

int lr_llvm_compat_global_value_get_unnamed_addr(const void *obj, int *out_unnamed_addr) {
    lr_global_value_state_node_t *node = lr_find_global_value_state_node(obj);
    if (!node || !node->has_unnamed_addr || !out_unnamed_addr)
        return 0;
    *out_unnamed_addr = node->unnamed_addr;
    return 1;
}

void lr_llvm_compat_unregister_global_value_state(const void *obj) {
    lr_global_value_state_node_t *it = g_global_value_states;
    lr_global_value_state_node_t *prev = NULL;
    if (!obj)
        return;
    while (it) {
        if (it->obj == obj) {
            if (prev)
                prev->next = it->next;
            else
                g_global_value_states = it->next;
            free(it);
            return;
        }
        prev = it;
        it = it->next;
    }
}

enum {
    LR_LLVM_LINKAGE_AVAILABLE_EXTERNALLY = 1,
    LR_LLVM_LINKAGE_INTERNAL = 7,
    LR_LLVM_LINKAGE_PRIVATE = 8,
};

int lr_llvm_compat_is_local_global_linkage(int linkage) {
    return linkage == LR_LLVM_LINKAGE_INTERNAL || linkage == LR_LLVM_LINKAGE_PRIVATE;
}

size_t lr_llvm_compat_linkage_scoped_global_name(const lc_module_compat_t *compat,
                                                 const char *name,
                                                 int linkage,
                                                 char *out_name,
                                                 size_t out_name_cap) {
    char suffix[32];
    size_t n;
    const char *local_tag = ".__liric_local.";
    if (!name)
        name = "";
    if (!compat || !name[0] || !lr_llvm_compat_is_local_global_linkage(linkage) ||
        strstr(name, local_tag) != NULL) {
        lr_copy_out_string(name, out_name, out_name_cap, &n);
        return n;
    }
    snprintf(suffix, sizeof(suffix), "%" PRIxPTR, (uintptr_t)compat);
    n = strlen(name) + strlen(local_tag) + strlen(suffix);
    if (out_name && out_name_cap > 0) {
        size_t w = 0;
        out_name[0] = '\0';
        if (name[0]) {
            strncat(out_name, name, out_name_cap - 1);
            w = strlen(out_name);
        }
        if (w < out_name_cap - 1)
            strncat(out_name, local_tag, out_name_cap - 1 - w);
        w = strlen(out_name);
        if (w < out_name_cap - 1)
            strncat(out_name, suffix, out_name_cap - 1 - w);
    }
    return n;
}

void lr_llvm_compat_apply_global_linkage(lc_module_compat_t *compat,
                                         lc_value_t *global_value,
                                         int linkage) {
    lr_module_t *m;
    lr_global_t *g;
    if (!compat || !global_value || global_value->kind != LC_VAL_GLOBAL ||
        !global_value->global.name)
        return;
    m = lc_module_get_ir(compat);
    if (!m)
        return;
    for (g = m->first_global; g; g = g->next) {
        if (!g->name || strcmp(g->name, global_value->global.name) != 0)
            continue;
        g->is_local = lr_llvm_compat_is_local_global_linkage(linkage) ? true : false;
        if (g->is_local)
            g->is_external = false;
        if (linkage == LR_LLVM_LINKAGE_AVAILABLE_EXTERNALLY)
            g->is_external = true;
        return;
    }
}

const char *lr_llvm_compat_type_kind_name(unsigned type_kind) {
    switch (type_kind) {
    case LR_TYPE_VOID:
        return "void";
    case LR_TYPE_I1:
        return "i1";
    case LR_TYPE_I8:
        return "i8";
    case LR_TYPE_I16:
        return "i16";
    case LR_TYPE_I32:
        return "i32";
    case LR_TYPE_I64:
        return "i64";
    case LR_TYPE_FLOAT:
        return "float";
    case LR_TYPE_DOUBLE:
        return "double";
    case LR_TYPE_PTR:
        return "ptr";
    default:
        return "type";
    }
}

size_t lr_llvm_compat_cstr_len(const char *s) {
    if (!s)
        return 0;
    return strlen(s);
}

int lr_llvm_compat_bytes_equal(const void *lhs, const void *rhs, size_t n) {
    if (lhs == rhs)
        return 1;
    if (!lhs || !rhs)
        return 0;
    if (n == 0)
        return 1;
    return memcmp(lhs, rhs, n) == 0 ? 1 : 0;
}

static size_t lr_copy_literal(const char *s, char *out, size_t out_cap) {
    size_t n = s ? strlen(s) : 0;
    if (!out || out_cap == 0)
        return n;
    lr_copy_out_string(s, out, out_cap, NULL);
    return n;
}

size_t lr_llvm_compat_intrinsic_name(unsigned intrinsic_id,
                                     int overload_is_float,
                                     int overload_is_double,
                                     int overload_is_integer,
                                     unsigned overload_int_bits,
                                     unsigned powi_int_bits,
                                     char *out_name,
                                     size_t out_name_cap) {
    const char *base = NULL;
    char suffix[32];
    char ibits[16];

    suffix[0] = '\0';
    if (overload_is_float) {
        strcpy(suffix, "f32");
    } else if (overload_is_double) {
        strcpy(suffix, "f64");
    } else if (overload_is_integer) {
        snprintf(suffix, sizeof(suffix), "i%u", overload_int_bits);
    }

    switch (intrinsic_id) {
    case 1:
        base = "llvm.abs.";
        break;
    case 2:
        base = "llvm.copysign.";
        break;
    case 3:
        base = "llvm.cos.";
        break;
    case 4:
        base = "llvm.ctlz.";
        break;
    case 5:
        base = "llvm.ctpop.";
        break;
    case 6:
        base = "llvm.cttz.";
        break;
    case 7:
        base = "llvm.exp.";
        break;
    case 8:
        base = "llvm.exp2.";
        break;
    case 9:
        base = "llvm.fabs.";
        break;
    case 10:
        base = "llvm.floor.";
        break;
    case 11:
        base = "llvm.ceil.";
        break;
    case 12:
        base = "llvm.round.";
        break;
    case 13:
        base = "llvm.trunc.";
        break;
    case 14:
    case 15:
        base = "llvm.fma.";
        break;
    case 16:
        base = "llvm.log.";
        break;
    case 17:
        base = "llvm.log2.";
        break;
    case 18:
        base = "llvm.log10.";
        break;
    case 19:
        base = "llvm.maximum.";
        break;
    case 20:
        base = "llvm.maxnum.";
        break;
    case 21:
        base = "llvm.minimum.";
        break;
    case 22:
        base = "llvm.minnum.";
        break;
    case 26:
        base = "llvm.pow.";
        break;
    case 28:
        base = "llvm.sin.";
        break;
    case 29:
        base = "llvm.sqrt.";
        break;
    case 27:
        if (suffix[0] == '\0')
            return 0;
        if (powi_int_bits == 0)
            powi_int_bits = 32;
        snprintf(ibits, sizeof(ibits), "i%u", powi_int_bits);
        if (!out_name || out_name_cap == 0)
            return strlen("llvm.powi.") + strlen(suffix) + 1 + strlen(ibits);
        snprintf(out_name, out_name_cap, "llvm.powi.%s.%s", suffix, ibits);
        return strlen("llvm.powi.") + strlen(suffix) + 1 + strlen(ibits);
    default:
        base = lc_intrinsic_name(intrinsic_id);
        if (!base || !base[0])
            return 0;
        return lr_copy_literal(base, out_name, out_name_cap);
    }

    if (suffix[0] == '\0')
        return 0;
    if (!out_name || out_name_cap == 0)
        return strlen(base) + strlen(suffix);
    snprintf(out_name, out_name_cap, "%s%s", base, suffix);
    return strlen(base) + strlen(suffix);
}

static lr_block_t *lr_find_prev_block(lr_func_t *func, lr_block_t *target) {
    lr_block_t *b;
    if (!func || !target || func->first_block == target)
        return NULL;
    for (b = func->first_block; b && b->next; b = b->next) {
        if (b->next == target)
            return b;
    }
    return NULL;
}

static void lr_invalidate_function_block_caches(lr_func_t *func) {
    if (!func)
        return;
    func->block_array = NULL;
    func->linear_inst_array = NULL;
    func->block_inst_offsets = NULL;
    func->num_linear_insts = 0;
}

int lr_llvm_compat_block_erase(lr_block_t *block, uint32_t *out_removed_id) {
    lr_func_t *func;
    lr_block_t *prev;
    uint32_t removed_id;
    lr_block_t *it;
    if (!block || !block->func)
        return 0;

    func = block->func;
    if (lc_func_uses_block_id(func, block, block->id))
        return 0;

    prev = lr_find_prev_block(func, block);
    if (!prev && func->first_block != block)
        return 0;

    if (prev)
        prev->next = block->next;
    else
        func->first_block = block->next;

    if (func->last_block == block)
        func->last_block = prev;

    removed_id = block->id;
    block->func = NULL;
    block->next = NULL;

    if (func->num_blocks > 0u)
        func->num_blocks--;

    for (it = func->first_block; it; it = it->next) {
        if (it->id > removed_id)
            it->id--;
    }

    lc_func_remap_block_operands_after_erase(func, removed_id);
    func->is_decl = (func->first_block == NULL);
    lr_invalidate_function_block_caches(func);

    if (out_removed_id)
        *out_removed_id = removed_id;
    return 1;
}

int lr_llvm_compat_block_move_after(lr_block_t *block, lr_block_t *anchor) {
    lr_func_t *func;
    lr_block_t *prev;
    if (!block || !anchor || block == anchor)
        return 0;
    if (!block->func || block->func != anchor->func)
        return 0;

    func = block->func;
    if (anchor->next == block)
        return 0;

    prev = lr_find_prev_block(func, block);
    if (!prev && func->first_block != block)
        return 0;

    if (prev)
        prev->next = block->next;
    else
        func->first_block = block->next;
    if (func->last_block == block)
        func->last_block = prev;

    block->next = anchor->next;
    anchor->next = block;
    if (func->last_block == anchor)
        func->last_block = block;

    lr_invalidate_function_block_caches(func);
    return 1;
}

int lr_llvm_compat_block_move_before(lr_block_t *block, lr_block_t *anchor) {
    lr_func_t *func;
    lr_block_t *prev;
    lr_block_t *anchor_prev;
    if (!block || !anchor || block == anchor)
        return 0;
    if (!block->func || block->func != anchor->func)
        return 0;
    if (block->next == anchor)
        return 0;

    func = block->func;
    prev = lr_find_prev_block(func, block);
    if (!prev && func->first_block != block)
        return 0;

    if (prev)
        prev->next = block->next;
    else
        func->first_block = block->next;
    if (func->last_block == block)
        func->last_block = prev;

    anchor_prev = lr_find_prev_block(func, anchor);
    if (anchor_prev)
        anchor_prev->next = block;
    else
        func->first_block = block;
    block->next = anchor;

    lr_invalidate_function_block_caches(func);
    return 1;
}

unsigned lr_llvm_compat_function_block_count(const lr_func_t *func) {
    const lr_block_t *b;
    unsigned n = 0;
    for (b = func ? func->first_block : NULL; b; b = b->next)
        n++;
    return n;
}

int lr_llvm_compat_function_insert_block(lc_module_compat_t *mod,
                                         lr_func_t *func,
                                         lr_block_t *block,
                                         lr_block_t *insert_before) {
    lr_block_t *it;
    int attached = 0;
    if (!mod || !func || !block)
        return 0;
    if (block->func != func)
        return 0;

    for (it = func->first_block; it; it = it->next) {
        if (it == block) {
            attached = 1;
            break;
        }
    }
    if (!attached && lc_block_attach(mod, block) != 0)
        return 0;

    if (!insert_before || insert_before == block)
        return 1;
    if (insert_before->func != func)
        return 0;
    if (!lr_llvm_compat_block_move_before(block, insert_before))
        return 0;
    return 1;
}

struct lr_llvm_compat_object {
    uint64_t section_addr;
    uint64_t section_size;
    uint64_t section_index;
};

struct lr_llvm_compat_dwarf_unit {
    const char *compilation_dir;
};

struct lr_llvm_compat_dwarf_context {
    struct lr_llvm_compat_dwarf_unit unit;
};

int lr_llvm_compat_object_create(const char *path,
                                 lr_llvm_compat_object_t **out_obj) {
    lr_llvm_compat_object_t *obj = NULL;
    (void)path;
    if (!out_obj)
        return -1;
    obj = (lr_llvm_compat_object_t *)calloc(1, sizeof(*obj));
    if (!obj)
        return -1;
    obj->section_addr = 0;
    obj->section_size = UINT64_MAX;
    obj->section_index = 0;
    *out_obj = obj;
    return 0;
}

void lr_llvm_compat_object_destroy(lr_llvm_compat_object_t *obj) {
    free(obj);
}

size_t lr_llvm_compat_object_section_count(const lr_llvm_compat_object_t *obj) {
    if (!obj)
        return 0;
    return 1;
}

int lr_llvm_compat_object_section_get(const lr_llvm_compat_object_t *obj,
                                      size_t index,
                                      uint64_t *out_addr,
                                      uint64_t *out_size,
                                      uint64_t *out_index) {
    if (!obj || index != 0 || !out_addr || !out_size || !out_index)
        return -1;
    *out_addr = obj->section_addr;
    *out_size = obj->section_size;
    *out_index = obj->section_index;
    return 0;
}

int lr_llvm_compat_dwarf_context_create(const lr_llvm_compat_object_t *obj,
                                        lr_llvm_compat_dwarf_context_t **out_ctx) {
    lr_llvm_compat_dwarf_context_t *ctx = NULL;
    if (!obj || !out_ctx)
        return -1;
    ctx = (lr_llvm_compat_dwarf_context_t *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;
    ctx->unit.compilation_dir = "";
    *out_ctx = ctx;
    return 0;
}

void lr_llvm_compat_dwarf_context_destroy(lr_llvm_compat_dwarf_context_t *ctx) {
    free(ctx);
}

size_t lr_llvm_compat_dwarf_context_unit_count(const lr_llvm_compat_dwarf_context_t *ctx) {
    if (!ctx)
        return 0;
    return 0;
}

const lr_llvm_compat_dwarf_unit_t *lr_llvm_compat_dwarf_context_unit_at(
    const lr_llvm_compat_dwarf_context_t *ctx, size_t index) {
    if (!ctx || index != 0 || lr_llvm_compat_dwarf_context_unit_count(ctx) == 0)
        return NULL;
    return &ctx->unit;
}

const char *lr_llvm_compat_dwarf_unit_compilation_dir(const lr_llvm_compat_dwarf_unit_t *unit) {
    if (!unit || !unit->compilation_dir)
        return NULL;
    return unit->compilation_dir;
}

size_t lr_llvm_compat_dwarf_line_row_count(const lr_llvm_compat_dwarf_context_t *ctx,
                                           const lr_llvm_compat_dwarf_unit_t *unit) {
    (void)ctx;
    (void)unit;
    return 0;
}

int lr_llvm_compat_dwarf_line_row_get(const lr_llvm_compat_dwarf_context_t *ctx,
                                      const lr_llvm_compat_dwarf_unit_t *unit,
                                      size_t row_index,
                                      lr_llvm_compat_dwarf_row_t *out_row) {
    (void)ctx;
    (void)unit;
    (void)row_index;
    if (!out_row)
        return -1;
    memset(out_row, 0, sizeof(*out_row));
    return -1;
}

int lr_llvm_compat_dwarf_line_has_file_index(const lr_llvm_compat_dwarf_context_t *ctx,
                                             const lr_llvm_compat_dwarf_unit_t *unit,
                                             uint64_t file_index) {
    (void)ctx;
    (void)unit;
    return file_index != 0 ? 1 : 0;
}

int lr_llvm_compat_dwarf_line_get_file_name(const lr_llvm_compat_dwarf_context_t *ctx,
                                            const lr_llvm_compat_dwarf_unit_t *unit,
                                            uint64_t file_index,
                                            const char *comp_dir,
                                            int file_line_kind,
                                            char *out_name,
                                            size_t out_name_cap,
                                            size_t *out_name_len) {
    (void)ctx;
    (void)unit;
    (void)file_index;
    (void)comp_dir;
    (void)file_line_kind;
    if (!out_name || out_name_cap == 0)
        return -1;
    out_name[0] = '\0';
    if (out_name_len)
        *out_name_len = 0;
    return -1;
}

int lr_llvm_compat_symbolize_code(const char *binary_path,
                                  uint64_t address,
                                  uint64_t section_index,
                                  int demangle,
                                  char *out_file,
                                  size_t out_file_cap,
                                  char *out_func,
                                  size_t out_func_cap,
                                  uint32_t *out_line) {
    (void)binary_path;
    (void)address;
    (void)section_index;
    (void)demangle;
    if (out_file && out_file_cap > 0) {
        strncpy(out_file, "<invalid>", out_file_cap - 1);
        out_file[out_file_cap - 1] = '\0';
    }
    if (out_func && out_func_cap > 0) {
        strncpy(out_func, "??", out_func_cap - 1);
        out_func[out_func_cap - 1] = '\0';
    }
    if (out_line)
        *out_line = 0;
    return 0;
}
