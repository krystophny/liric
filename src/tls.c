#include "ir.h"
#include <stdio.h>
#include <string.h>

/* GCC/compiler-rt emulated TLS ABI: size, alignment, runtime index,
 * initializer pointer. The runtime allocates, initializes and destroys one
 * object per thread, while all functions/TUs share the control symbol. */
static lr_global_t *tls_find_global(lr_module_t *m, const char *name) {
    for (lr_global_t *g = m->first_global; g; g = g->next)
        if (strcmp(g->name, name) == 0)
            return g;
    return NULL;
}

static char *tls_name(lr_module_t *m, const char *prefix, const char *name) {
    size_t n = strlen(prefix) + strlen(name) + 1;
    char *result = lr_arena_alloc(m->arena, n, 1);
    if (result)
        snprintf(result, n, "%s%s", prefix, name);
    return result;
}

int lr_tls_prepare_module(lr_module_t *m) {
    if (!m)
        return -1;
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (!g->tls_mode || (g->tls_control &&
            (!g->tls_control->is_external || g->is_external)))
            continue;
        char *name = tls_name(m, "__emutls_v.", g->name);
        if (!name)
            return -1;
        lr_global_t *control = tls_find_global(m, name);
        if (!control) {
            lr_type_t *fields[] = {m->type_i64, m->type_i64,
                                   m->type_i64, m->type_ptr};
            control = lr_global_create(m, name,
                lr_type_struct(m->arena, fields, 4, false, NULL), false);
            if (!control)
                return -1;
        }
        if (control->is_external || !control->init_data) {
            control->is_external = g->is_external;
            control->is_local = g->is_local;
            control->is_weak = g->is_weak;
            control->alignment = 8;
            control->is_tls_control = true;
            if (!g->is_external) {
                size_t size = lr_type_size(g->type);
                size_t align = g->alignment ? g->alignment : lr_type_align(g->type);
                if (!size)
                    size = 1;
                if (!align || (align & (align - 1)))
                    return -1;
                uint64_t descriptor[4] = {size, align, 0, 0};
                control->init_size = sizeof(descriptor);
                control->init_data = lr_arena_alloc(m->arena, sizeof(descriptor), 8);
                if (!control->init_data)
                    return -1;
                memcpy(control->init_data, descriptor, sizeof(descriptor));
                if (g->init_data || g->relocs) {
                    char *template_name = tls_name(m, "__emutls_t.", g->name);
                    if (!template_name)
                        return -1;
                    lr_global_t *template = lr_global_create(m, template_name,
                        g->type, true);
                    if (!template)
                        return -1;
                    template->is_local = true;
                    template->init_data = g->init_data;
                    template->init_size = g->init_size;
                    template->relocs = g->relocs;
                    template->alignment = align;
                    lr_reloc_t *reloc = lr_arena_new(m->arena, lr_reloc_t);
                    if (!reloc)
                        return -1;
                    reloc->offset = 24;
                    reloc->symbol_name = template->name;
                    control->relocs = reloc;
                }
            }
        }
        g->tls_control = control;
    }
    return 0;
}

int lr_tls_lower_function(lr_func_t *f) {
    lr_module_t *m = f ? f->module : NULL;
    if (!m || !f->first_block)
        return 0;
    bool any_tls = false;
    for (lr_global_t *g = m->first_global; g; g = g->next)
        any_tls |= g->tls_mode != 0;
    if (!any_tls)
        return 0;
    if (lr_tls_prepare_module(m) != 0)
        return -1;
    uint32_t symbols = m->num_symbols;
    lr_global_t **globals = lr_arena_array(m->arena, lr_global_t *, symbols);
    uint32_t *addresses = lr_arena_array(m->arena, uint32_t, symbols);
    if (!globals || !addresses)
        return -1;
    memset(globals, 0, sizeof(*globals) * symbols);
    memset(addresses, 0, sizeof(*addresses) * symbols);
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (!g->tls_mode)
            continue;
        uint32_t id = lr_module_intern_symbol(m, g->name);
        if (id >= symbols)
            return -1;
        globals[id] = g;
    }
    lr_inst_t *first = NULL, *last = NULL;
    for (lr_block_t *b = f->first_block; b; b = b->next) {
        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            for (uint32_t i = 0; i < inst->num_operands; i++) {
                lr_operand_t *op = &inst->operands[i];
                if (op->kind != LR_VAL_GLOBAL || op->global_id >= symbols)
                    continue;
                lr_global_t *g = globals[op->global_id];
                if (!g)
                    continue;
                uint32_t dest = addresses[op->global_id];
                if (!dest) {
                    lr_type_t *params[] = {m->type_ptr};
                    lr_func_t *getter = lr_func_declare(m, "__emutls_get_address",
                        m->type_ptr, params, 1, false);
                    if (!getter)
                        return -1;
                    getter->uses_llvm_abi = true;
                    lr_operand_t args[] = {
                        lr_op_global(lr_module_intern_symbol(m, getter->name), m->type_ptr),
                        lr_op_global(lr_module_intern_symbol(m, g->tls_control->name), m->type_ptr)
                    };
                    dest = lr_vreg_new(f);
                    lr_inst_t *call = lr_inst_create(m->arena, LR_OP_CALL,
                        m->type_ptr, dest, args, 2);
                    if (!call)
                        return -1;
                    call->call_external_abi = true;
                    call->call_fixed_args = 1;
                    if (last) last->next = call;
                    else first = call;
                    last = call;
                    addresses[op->global_id] = dest;
                }
                if (op->global_offset) {
                    lr_operand_t args[] = {lr_op_vreg(dest, m->type_ptr),
                        lr_op_imm_i64(op->global_offset, m->type_i64)};
                    dest = lr_vreg_new(f);
                    lr_inst_t *gep = lr_inst_create(m->arena, LR_OP_GEP,
                        m->type_i8, dest, args, 2);
                    if (!gep)
                        return -1;
                    last->next = gep;
                    last = gep;
                }
                *op = lr_op_vreg(dest, m->type_ptr);
            }
        }
    }
    if (first) {
        lr_block_t *entry = f->first_block;
        last->next = entry->first;
        entry->first = first;
        if (!entry->last)
            entry->last = last;
        entry->inst_array = NULL;
        f->linear_inst_array = NULL;
        f->block_inst_offsets = NULL;
        f->num_linear_insts = 0;
    }
    return 0;
}
