#ifndef LIRIC_LL_PARSER_H
#define LIRIC_LL_PARSER_H

#include "ir.h"
#include "ll_lexer.h"

lr_module_t *lr_parse_ll_text(const char *src, size_t len,
                               lr_arena_t *arena, char *err, size_t errlen);

#endif
