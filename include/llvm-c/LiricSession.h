#ifndef LLVM_C_LIRICSESSION_H
#define LLVM_C_LIRICSESSION_H

#include <liric/liric_compat.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LLVMLiricSessionState LLVMLiricSessionState;
typedef LLVMLiricSessionState *LLVMLiricSessionStateRef;

LLVMLiricSessionStateRef LLVMLiricSessionCreate(void);
void LLVMLiricSessionDispose(LLVMLiricSessionStateRef state);
int LLVMLiricSessionAddCompatModule(LLVMLiricSessionStateRef state,
                                    lc_module_compat_t *mod);
void LLVMLiricSessionAddSymbol(LLVMLiricSessionStateRef state,
                               const char *name, void *addr);
void *LLVMLiricSessionLookup(LLVMLiricSessionStateRef state, const char *name);
const char *LLVMLiricHostTargetName(void);

#ifdef __cplusplus
}
#endif

#endif
