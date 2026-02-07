#ifndef LLVM_EXECUTIONENGINE_ORC_LLJIT_H
#define LLVM_EXECUTIONENGINE_ORC_LLJIT_H

#include <liric/liric.h>
#include "llvm/ADT/StringRef.h"

namespace llvm {

class Module;

namespace orc {

class LLJIT {
    lr_jit_t *jit_;

public:
    LLJIT() : jit_(lr_jit_create()) {}
    ~LLJIT() { if (jit_) lr_jit_destroy(jit_); }

    LLJIT(const LLJIT &) = delete;
    LLJIT &operator=(const LLJIT &) = delete;

    lr_jit_t *getJIT() const { return jit_; }

    inline int addModule(Module &M);

    void *lookup(StringRef Name) {
        return lr_jit_get_function(jit_, Name.data());
    }

    void addSymbol(StringRef Name, void *Addr) {
        lr_jit_add_symbol(jit_, Name.data(), Addr);
    }

    static const char *getHostTargetName() {
        return lr_jit_host_target_name();
    }
};

class LLJITBuilder {
public:
    LLJIT *create() { return new LLJIT(); }
};

} // namespace orc
} // namespace llvm

#include "llvm/IR/Module.h"

inline int llvm::orc::LLJIT::addModule(llvm::Module &M) {
    lc_module_finalize_phis(M.getCompat());
    return lr_jit_add_module(jit_, M.getIR());
}

#endif
