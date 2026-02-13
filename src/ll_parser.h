#ifndef LIRIC_LL_PARSER_H
#define LIRIC_LL_PARSER_H

#include "ir.h"
#include "ll_lexer.h"
#include <liric/liric_session.h>

typedef int (*lr_parse_ll_func_cb_t)(lr_func_t *func, lr_module_t *mod,
                                     void *ctx);

lr_module_t *lr_parse_ll_text(const char *src, size_t len,
                               lr_arena_t *arena, char *err, size_t errlen);
lr_module_t *lr_parse_ll_text_streaming(const char *src, size_t len,
                                        lr_arena_t *arena,
                                        lr_parse_ll_func_cb_t on_func,
                                        void *ctx, char *err, size_t errlen);
int lr_parse_ll_to_session(const char *src, size_t len, lr_session_t *session,
                            char *err, size_t errlen);

#endif
